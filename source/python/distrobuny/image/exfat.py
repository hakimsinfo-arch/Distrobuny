"""Deterministic exFAT volume writer.

Implements the exFAT file system as described in Microsoft's "exFAT File
System Specification" (2019): Main and Backup Boot regions with boot checksum,
one FAT, allocation bitmap, up-case table, root directory with volume label,
and File / Stream Extension / File Name directory entry sets.

Design choices for Distro Buny:
  * every file is allocated contiguously and marked NoFatChain, so the boot
    runtime can map an ISO to a single extent of the partition (FAT chains are
    written as well, for readers that ignore the flag);
  * the build is reproducible: allocation order is the natural sort order of
    names, timestamps come from the source files, the serial is supplied;
  * file contents are copied with copy_file_range and holes are left sparse,
    so building a 100+ GB image needs no extra temporary space;
  * a volume can be made *growable* (grow_limit): the space between the FAT
    and the cluster heap is reserved for a FAT of grow_limit bytes, and the
    allocation bitmap takes the last clusters of the heap, so that the
    runtime can later extend the volume in place to the end of a bigger
    drive (source/filesystem/expand.c).  The result is an ordinary exFAT
    volume: the gap and the bitmap position are both allowed by the
    specification.

The volume is written into an existing file at a byte offset (the partition
start inside a disk image) and needs no privileges.
"""
from __future__ import annotations

import os
import stat
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..util import BuildError
from .pieces import Blob, Copy, execute

SECTOR_SHIFT = 9
SECTOR = 1 << SECTOR_SHIFT
DIR_ENTRY = 32
EOC = 0xFFFFFFFF

ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20


def default_cluster_size(volume_bytes: int) -> int:
    """Cluster size policy (matches common formatters; large clusters suit ISO files)."""
    if volume_bytes < 256 << 20:
        return 4096
    if volume_bytes < 32 << 30:
        return 32 << 10
    return 128 << 10


# Growable volumes (images written to sticks of unknown size): the cluster
# size a formatter would choose for the grown volume, and the largest size
# the FAT reserve allows (64 MiB of FAT at 128 KiB clusters).
GROWABLE_CLUSTER_SIZE = 128 << 10
GROW_LIMIT = 2 << 40
MAX_CLUSTERS = 0xFFFFFFF5


# --------------------------------------------------------------- up-case table

UPCASE_FILE = Path(__file__).with_name("data") / "exfat-upcase-table.bin"
UPCASE_CHECKSUM = 0xE619D30D  # published in the exFAT specification, section 7.2.5.1


def table_checksum(data: bytes) -> int:
    cs = 0
    for b in data:
        cs = (((cs << 31) | (cs >> 1)) + b) & 0xFFFFFFFF
    return cs


def decompress_upcase(table: bytes) -> list[int]:
    """Expand a (possibly compressed) up-case table into 65536 mappings.

    In the compressed form the value 0xFFFF is followed by the number of
    consecutive code points that map to themselves.
    """
    units = struct.unpack(f"<{len(table) // 2}H", table)
    out: list[int] = []
    i = 0
    while i < len(units) and len(out) < 0x10000:
        v = units[i]
        if v == 0xFFFF and i + 1 < len(units):
            run = units[i + 1]
            out.extend(range(len(out), min(len(out) + run, 0x10000)))
            i += 2
        else:
            out.append(v)
            i += 1
    out.extend(range(len(out), 0x10000))
    return out


_UPCASE: tuple[bytes, list[int]] | None = None


def upcase_table() -> tuple[bytes, list[int]]:
    """The on-disk table bytes and its decompressed mapping."""
    global _UPCASE
    if _UPCASE is None:
        raw = UPCASE_FILE.read_bytes()
        if table_checksum(raw) != UPCASE_CHECKSUM:
            raise BuildError(f"{UPCASE_FILE} is corrupt (checksum mismatch)")
        _UPCASE = (raw, decompress_upcase(raw))
    return _UPCASE


def name_hash(name_utf16: list[int]) -> int:
    mapping = upcase_table()[1]
    h = 0
    for c in name_utf16:
        u = mapping[c] if c < 0x10000 else c
        for byte in (u & 0xFF, u >> 8):
            h = (((h << 15) | (h >> 1)) + byte) & 0xFFFF
    return h


def entry_set_checksum(entries: bytes) -> int:
    cs = 0
    for i, b in enumerate(entries):
        if i in (2, 3):
            continue
        cs = (((cs << 15) | (cs >> 1)) + b) & 0xFFFF
    return cs


def boot_checksum(sectors: bytes) -> int:
    cs = 0
    for i, b in enumerate(sectors):
        if i in (106, 107, 112):  # VolumeFlags and PercentInUse are excluded
            continue
        cs = (((cs << 31) | (cs >> 1)) + b) & 0xFFFFFFFF
    return cs


def exfat_timestamp(epoch: float) -> tuple[int, int, int]:
    """(timestamp, 10ms increment, utc offset byte) for an epoch time, in UTC."""
    t = time.gmtime(max(epoch, 315532800))  # not before 1980-01-01
    year = min(max(t.tm_year, 1980), 2107)
    ts = (
        (t.tm_sec // 2)
        | (t.tm_min << 5)
        | (t.tm_hour << 11)
        | (t.tm_mday << 16)
        | (t.tm_mon << 21)
        | ((year - 1980) << 25)
    )
    inc = (t.tm_sec % 2) * 100
    return ts, inc, 0x80  # offset valid, UTC+0


def utf16_units(name: str) -> list[int]:
    raw = name.encode("utf-16-le")
    return [raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2)]


INVALID_NAME_CHARS = set('"*/:<>?\\|') | {chr(c) for c in range(0x20)}


def validate_name(name: str) -> None:
    if not name or name in (".", ".."):
        raise BuildError(f"invalid file name {name!r}")
    if any(ch in INVALID_NAME_CHARS for ch in name):
        raise BuildError(f"file name {name!r} contains characters exFAT does not allow")
    if len(utf16_units(name)) > 255:
        raise BuildError(f"file name {name!r} is longer than 255 UTF-16 code units")


# ------------------------------------------------------------------ the tree

@dataclass
class Node:
    name: str
    is_dir: bool
    source: Path | None = None          # file contents
    data: bytes | None = None           # inline contents (small generated files)
    size: int = 0
    mtime: float = 0.0
    children: list["Node"] = field(default_factory=list)
    first_cluster: int = 0
    clusters: int = 0
    dir_bytes: int = 0

    def child(self, name: str) -> "Node | None":
        for c in self.children:
            if c.name.lower() == name.lower():
                return c
        return None


def _natural_key(s: str):
    import re
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


class ExfatBuilder:
    """Collects a file tree and writes it as an exFAT volume."""

    def __init__(self, volume_bytes: int, label: str = "DISTROBUNY", serial: int = 0x12345678,
                 cluster_size: int | None = None, partition_offset_sectors: int = 0, timestamp: float | None = None,
                 grow_limit: int | None = None):
        if volume_bytes < 8 << 20:
            raise BuildError("exFAT volume must be at least 8 MiB")
        self.volume_bytes = volume_bytes - volume_bytes % SECTOR
        self.grow_limit = grow_limit if grow_limit and grow_limit > self.volume_bytes else None
        self.label = label
        self.serial = serial & 0xFFFFFFFF
        self.cluster_size = cluster_size or default_cluster_size(volume_bytes)
        if self.cluster_size & (self.cluster_size - 1) or not SECTOR <= self.cluster_size <= 32 << 20:
            raise BuildError("cluster size must be a power of two between 512 B and 32 MiB")
        self.partition_offset = partition_offset_sectors
        self.timestamp = timestamp if timestamp is not None else time.time()
        self.root = Node("", True, mtime=self.timestamp)
        if len(utf16_units(label)) > 11:
            raise BuildError("exFAT volume labels are limited to 11 characters")

    # ---------------------------------------------------------------- input
    def _dir_for(self, parts: list[str]) -> Node:
        node = self.root
        for p in parts:
            validate_name(p)
            c = node.child(p)
            if c is None:
                c = Node(p, True, mtime=self.timestamp)
                node.children.append(c)
            elif not c.is_dir:
                raise BuildError(f"{p} is both a file and a directory")
            node = c
        return node

    def add_dir(self, path: str, mtime: float | None = None) -> None:
        d = self._dir_for([p for p in path.split("/") if p])
        if mtime is not None:
            d.mtime = mtime

    def add_file(self, path: str, source: Path | None = None, data: bytes | None = None, mtime: float | None = None) -> None:
        parts = [p for p in path.split("/") if p]
        parent = self._dir_for(parts[:-1])
        name = parts[-1]
        validate_name(name)
        if parent.child(name):
            raise BuildError(f"duplicate path {path}")
        if source is not None:
            st = os.stat(source)
            if not stat.S_ISREG(st.st_mode):
                raise BuildError(f"{source} is not a regular file")
            node = Node(name, False, source=Path(source), size=st.st_size, mtime=mtime if mtime is not None else st.st_mtime)
        else:
            node = Node(name, False, data=data or b"", size=len(data or b""), mtime=mtime if mtime is not None else self.timestamp)
        parent.children.append(node)

    def add_tree(self, src: Path, dest: str = "/", filter_fn=None) -> list[Path]:
        """Add a host directory tree.  Returns the files added."""
        added = []
        for dirpath, dirnames, filenames in os.walk(src, followlinks=False):
            dirnames.sort(key=_natural_key)
            rel = Path(dirpath).relative_to(src)
            base = "/".join(p for p in (dest.strip("/"), str(rel) if str(rel) != "." else "") if p)
            if base:
                self.add_dir(base, os.stat(dirpath).st_mtime)
            for fn in sorted(filenames, key=_natural_key):
                full = Path(dirpath) / fn
                if filter_fn and not filter_fn(full):
                    continue
                self.add_file(f"{base}/{fn}" if base else fn, source=full)
                added.append(full)
        return added

    # ------------------------------------------------------------- geometry
    def _layout(self) -> None:
        cs = self.cluster_size
        self.spc_shift = (cs // SECTOR).bit_length() - 1
        vol_sectors = self.volume_bytes // SECTOR
        # FAT starts at 1 MiB (or at sector 24 for tiny volumes), cluster heap aligned to 4 MiB
        self.fat_offset = 2048 if vol_sectors > 65536 else 128
        # first estimate of cluster count, then FAT length, then exact heap offset
        approx_clusters = (vol_sectors - self.fat_offset) * SECTOR // cs
        fat_bytes = (approx_clusters + 2) * 4
        self.fat_length = (fat_bytes + SECTOR - 1) // SECTOR
        heap_align = max(cs // SECTOR, 8192 if vol_sectors > 4 * 8192 else 64)
        fat_room = self.fat_length
        if self.grow_limit:
            # room for the FAT of the largest volume this one may grow to
            max_clusters = min(self.grow_limit // cs, MAX_CLUSTERS)
            fat_room = max(fat_room, ((max_clusters + 2) * 4 + SECTOR - 1) // SECTOR)
        heap = self.fat_offset + fat_room
        heap = (heap + heap_align - 1) // heap_align * heap_align
        self.heap_offset = heap
        self.cluster_count = (vol_sectors - heap) // (cs // SECTOR)
        if self.cluster_count < 16:
            raise BuildError("volume too small for the chosen cluster size")
        if (self.cluster_count + 2) * 4 > self.fat_length * SECTOR:
            raise BuildError("internal error: FAT too small")

    def _clusters_for(self, nbytes: int) -> int:
        return max(1, (nbytes + self.cluster_size - 1) // self.cluster_size)

    def _dir_size(self, d: Node, is_root: bool) -> int:
        n = 0
        if is_root:
            n += 3  # volume label, allocation bitmap, up-case table
        for c in d.children:
            n += 2 + (len(utf16_units(c.name)) + 14) // 15
        n += 1  # keep room for an end marker
        return n * DIR_ENTRY

    def _allocate(self) -> None:
        nxt = 2
        self.bitmap_bytes = (self.cluster_count + 7) // 8
        bitmap_clusters = self._clusters_for(self.bitmap_bytes)
        if self.grow_limit:
            # the last clusters of the heap: growing appends free clusters right behind it
            self.bitmap_cluster = self.cluster_count + 2 - bitmap_clusters
            self.tail_clusters = bitmap_clusters
        else:
            self.bitmap_cluster = nxt
            nxt += bitmap_clusters
            self.tail_clusters = 0
        self.upcase = upcase_table()[0]
        self.upcase_cluster = nxt
        nxt += self._clusters_for(len(self.upcase))
        # directories first (breadth first) so metadata is together at the start
        order = []
        queue = [self.root]
        while queue:
            d = queue.pop(0)
            d.children.sort(key=lambda c: (not c.is_dir, _natural_key(c.name)))
            order.append(d)
            queue.extend(c for c in d.children if c.is_dir)
        for d in order:
            d.dir_bytes = self._dir_size(d, d is self.root)
            d.clusters = self._clusters_for(d.dir_bytes)
            d.first_cluster = nxt
            nxt += d.clusters
        # files in traversal order, each contiguous
        self.files = []
        for d in order:
            for c in d.children:
                if c.is_dir:
                    continue
                if c.size == 0:
                    c.first_cluster = 0
                    c.clusters = 0
                else:
                    c.clusters = self._clusters_for(c.size)
                    c.first_cluster = nxt
                    nxt += c.clusters
                self.files.append(c)
        self.dirs = order
        self.head_clusters = nxt - 2
        self.used_clusters = self.head_clusters + self.tail_clusters
        if self.used_clusters > self.cluster_count:
            need = self.used_clusters * self.cluster_size
            raise BuildError(f"the volume is too small: content needs {need} bytes in clusters, "
                             f"volume has {self.cluster_count * self.cluster_size}")

    # ---------------------------------------------------------------- output
    def _boot_region(self) -> bytes:
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x76\x90"
        bs[3:11] = b"EXFAT   "
        percent = min(100, self.used_clusters * 100 // self.cluster_count)
        struct.pack_into(
            "<QQIIIIIIHHBBBB",
            bs,
            64,
            self.partition_offset,
            self.volume_bytes // SECTOR,
            self.fat_offset,
            self.fat_length,
            self.heap_offset,
            self.cluster_count,
            self.root.first_cluster,
            self.serial,
            0x0100,            # file system revision 1.00
            0,                 # volume flags
            SECTOR_SHIFT,
            self.spc_shift,
            1,                 # number of FATs
            0x80,              # drive select
        )
        bs[112] = percent
        bs[120:510] = b"\xF4" * 390  # boot code: hlt
        bs[510:512] = b"\x55\xAA"
        region = bytearray(bs)
        for _ in range(8):  # extended boot sectors
            ext = bytearray(SECTOR)
            ext[508:512] = b"\x00\x00\x55\xAA"
            region += ext
        region += bytes(SECTOR)  # OEM parameters
        region += bytes(SECTOR)  # reserved
        cs = boot_checksum(bytes(region))
        region += struct.pack("<I", cs) * (SECTOR // 4)
        assert len(region) == 12 * SECTOR
        return bytes(region)

    def _fat(self) -> bytearray:
        fat = bytearray(self.fat_length * SECTOR)
        struct.pack_into("<II", fat, 0, 0xFFFFFFF8, 0xFFFFFFFF)

        def chain(first: int, count: int) -> None:
            for i in range(count):
                c = first + i
                struct.pack_into("<I", fat, c * 4, EOC if i == count - 1 else c + 1)

        chain(self.bitmap_cluster, self._clusters_for(self.bitmap_bytes))
        chain(self.upcase_cluster, self._clusters_for(len(self.upcase)))
        for d in self.dirs:
            chain(d.first_cluster, d.clusters)
        for f in self.files:
            if f.clusters:
                chain(f.first_cluster, f.clusters)
        return fat

    def _bitmap(self) -> bytearray:
        bm = bytearray(self.bitmap_bytes)
        full, rest = divmod(self.head_clusters, 8)
        bm[:full] = b"\xFF" * full
        if rest:
            bm[full] = (1 << rest) - 1
        for c in range(self.bitmap_cluster, self.bitmap_cluster + self.tail_clusters):
            bm[(c - 2) // 8] |= 1 << ((c - 2) % 8)
        return bm

    def _entry_set(self, node: Node) -> bytes:
        units = utf16_units(node.name)
        n_name = (len(units) + 14) // 15
        attrs = ATTR_DIRECTORY if node.is_dir else ATTR_ARCHIVE
        ts, inc, utc = exfat_timestamp(node.mtime)
        primary = bytearray(DIR_ENTRY)
        struct.pack_into("<BBHH2xIIIBBBBB", primary, 0, 0x85, 1 + n_name, 0, attrs, ts, ts, ts, inc, inc, utc, utc, utc)
        if node.is_dir:
            length = node.clusters * self.cluster_size
            flags = 0x01  # allocation possible, FAT chain used for directories
        else:
            length = node.size
            flags = 0x03 if node.clusters else 0x01  # NoFatChain for contiguous files
        stream = bytearray(DIR_ENTRY)
        struct.pack_into("<BBxBH2xQ4xIQ", stream, 0, 0xC0, flags, len(units), name_hash(units), length,
                         node.first_cluster, length)
        names = bytearray()
        for i in range(n_name):
            e = bytearray(DIR_ENTRY)
            e[0] = 0xC1
            chunk = units[i * 15:(i + 1) * 15]
            for k, u in enumerate(chunk):
                struct.pack_into("<H", e, 2 + 2 * k, u)
            names += e
        es = primary + stream + names
        struct.pack_into("<H", es, 2, entry_set_checksum(bytes(es)))
        return bytes(es)

    def _dir_bytes(self, d: Node) -> bytes:
        out = bytearray()
        if d is self.root:
            label = utf16_units(self.label)
            e = bytearray(DIR_ENTRY)
            e[0] = 0x83
            e[1] = len(label)
            for k, u in enumerate(label):
                struct.pack_into("<H", e, 2 + 2 * k, u)
            out += e
            e = bytearray(DIR_ENTRY)
            struct.pack_into("<BB18xIQ", e, 0, 0x81, 0, self.bitmap_cluster, self.bitmap_bytes)
            out += e
            e = bytearray(DIR_ENTRY)
            struct.pack_into("<B3xI12xIQ", e, 0, 0x82, table_checksum(self.upcase), self.upcase_cluster, len(self.upcase))
            out += e
        for c in d.children:
            out += self._entry_set(c)
        size = d.clusters * self.cluster_size
        return bytes(out.ljust(size, b"\0"))

    def cluster_offset(self, cluster: int) -> int:
        """Byte offset of a cluster from the start of the volume."""
        return self.heap_offset * SECTOR + (cluster - 2) * self.cluster_size

    def pieces(self, base: int = 0) -> tuple[list, dict]:
        """Describe the volume at byte offset `base` as pieces (see image.pieces).

        Also returns a map of file paths to (offset_in_volume, size) for the
        build manifest and for tests.  Every metadata structure is written in
        full (zero padded), so the result does not depend on what the target
        contained before.
        """
        self._layout()
        self._allocate()
        boot = self._boot_region()
        out = [
            Blob(base, boot),
            Blob(base + 12 * SECTOR, boot),
            Blob(base + self.fat_offset * SECTOR, bytes(self._fat())),
            Blob(base + self.cluster_offset(self.bitmap_cluster), bytes(self._bitmap())),
            Blob(base + self.cluster_offset(self.upcase_cluster), self.upcase),
        ]
        for d in self.dirs:
            out.append(Blob(base + self.cluster_offset(d.first_cluster), self._dir_bytes(d)))
        layout = {}
        paths = self._paths()
        for node in self.files:
            path = paths[id(node)]
            if node.size:
                off = base + self.cluster_offset(node.first_cluster)
                if node.source is not None:
                    out.append(Copy(off, node.size, Path(node.source), 0, path))
                else:
                    out.append(Blob(off, bytes(node.data)))
                layout[path] = (self.cluster_offset(node.first_cluster), node.size)
            else:
                layout[path] = (0, 0)
        return out, layout

    def write(self, f, base: int = 0, progress=None) -> dict:
        """Write the volume into open file `f` at byte offset `base`; returns the layout map."""
        out, layout = self.pieces(base)
        execute(f.fileno(), out, progress)
        return layout

    def _paths(self) -> dict:
        out = {}

        def walk(d: Node, prefix: str) -> None:
            for c in d.children:
                p = f"{prefix}/{c.name}"
                out[id(c)] = p
                if c.is_dir:
                    walk(c, p)

        walk(self.root, "")
        return out
