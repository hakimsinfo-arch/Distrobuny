"""Deterministic FAT32 volume writer for the EFI System Partition.

Writes a FAT32 file system (Microsoft "FAT: General Overview of On-Disk
Format") with BPB, FSInfo, backup boot sector, two FATs, 8.3 short names and
VFAT long file names.  Used for the ESP, which UEFI firmware reads to start
the boot chain.
"""
from __future__ import annotations

import os
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..util import BuildError
from .pieces import Blob, Copy, execute

SECTOR = 512
RESERVED = 32
EOC = 0x0FFFFFFF


@dataclass
class _Node:
    name: str
    is_dir: bool
    data: bytes | None = None
    source: Path | None = None
    size: int = 0
    mtime: float = 0
    children: list["_Node"] = field(default_factory=list)
    first_cluster: int = 0
    clusters: int = 0
    short: bytes = b""
    lossless: bool = False  # the 8.3 name represents the name exactly (no LFN needed)


def _dos_datetime(epoch: float) -> tuple[int, int]:
    t = time.gmtime(max(epoch, 315532800))
    date = ((max(t.tm_year, 1980) - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tm


_SHORT_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def _short_candidate(name: str) -> tuple[str, str, bool]:
    """Return (base, ext, lossless) for an 8.3 name."""
    upper = name.upper()
    if "." in upper and not upper.startswith("."):
        base, ext = upper.rsplit(".", 1)
    else:
        base, ext = upper, ""
    cb = "".join(c for c in base if c in _SHORT_OK)
    ce = "".join(c for c in ext if c in _SHORT_OK)
    lossless = cb == base and ce == ext and 0 < len(base) <= 8 and len(ext) <= 3 and name == upper
    return cb[:8], ce[:3], lossless


def _lfn_checksum(short: bytes) -> int:
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


class Fat32Builder:
    def __init__(self, volume_bytes: int, label: str = "BUNY-EFI", serial: int = 0x0B0B0B0B,
                 cluster_size: int | None = None, hidden_sectors: int = 0, timestamp: float | None = None):
        self.volume_bytes = volume_bytes - volume_bytes % SECTOR
        if cluster_size is None:
            # FAT32 requires at least 65525 clusters: use the largest cluster
            # size (up to 4 KiB) that keeps a comfortable margin above that.
            cluster_size = 4096
            while cluster_size > SECTOR and self.volume_bytes // cluster_size < 70000:
                cluster_size //= 2
        self.label = label.upper()[:11]
        self.serial = serial & 0xFFFFFFFF
        self.cluster_size = cluster_size
        self.hidden = hidden_sectors
        self.ts = timestamp if timestamp is not None else time.time()
        self.root = _Node("", True, mtime=self.ts)

    def _dir_for(self, parts):
        node = self.root
        for p in parts:
            c = next((x for x in node.children if x.name.lower() == p.lower()), None)
            if c is None:
                c = _Node(p, True, mtime=self.ts)
                node.children.append(c)
            elif not c.is_dir:
                raise BuildError(f"{p} is a file")
            node = c
        return node

    def add_dir(self, path: str) -> None:
        self._dir_for([p for p in path.split("/") if p])

    def add_file(self, path: str, source: Path | None = None, data: bytes | None = None) -> None:
        parts = [p for p in path.split("/") if p]
        parent = self._dir_for(parts[:-1])
        if any(c.name.lower() == parts[-1].lower() for c in parent.children):
            raise BuildError(f"duplicate path {path}")
        if source is not None:
            size = os.path.getsize(source)
            parent.children.append(_Node(parts[-1], False, source=Path(source), size=size, mtime=self.ts))
        else:
            parent.children.append(_Node(parts[-1], False, data=data or b"", size=len(data or b""), mtime=self.ts))

    # ----------------------------------------------------------- geometry
    def _layout(self) -> None:
        spc = self.cluster_size // SECTOR
        total = self.volume_bytes // SECTOR
        # iterate FAT size until stable
        fat_sectors = 1
        while True:
            data_sectors = total - RESERVED - 2 * fat_sectors
            clusters = data_sectors // spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        self.fat_sectors = fat_sectors
        self.cluster_count = clusters
        if clusters < 65525:
            raise BuildError("ESP too small for FAT32 with this cluster size")
        self.data_start = RESERVED + 2 * fat_sectors

    def _assign_short_names(self, d: _Node) -> None:
        used = set()
        for c in d.children:
            base, ext, lossless = _short_candidate(c.name)
            if lossless and (base, ext) not in used:
                c.short = base.ljust(8).encode() + ext.ljust(3).encode()
                used.add((base, ext))
                c.lossless = True
                continue
            c.lossless = False
            base = base or "FILE"
            for n in range(1, 1000000):
                tail = f"~{n}"
                b = base[: 8 - len(tail)] + tail
                if (b, ext) not in used:
                    used.add((b, ext))
                    c.short = b.ljust(8).encode() + ext.ljust(3).encode()
                    break
        for c in d.children:
            if c.is_dir:
                self._assign_short_names(c)

    def _entries_for(self, c: _Node) -> int:
        return 1 if c.lossless else 1 + (len(c.name) + 12) // 13

    def _allocate(self) -> None:
        self._assign_short_names(self.root)
        nxt = 2
        order, queue = [], [self.root]
        while queue:
            d = queue.pop(0)
            order.append(d)
            queue.extend(c for c in d.children if c.is_dir)
        cs = self.cluster_size
        for d in order:
            n = (0 if d is self.root else 2) + (1 if d is self.root else 0)
            n += sum(self._entries_for(c) for c in d.children) + 1
            d.clusters = max(1, (n * 32 + cs - 1) // cs)
            d.first_cluster = nxt
            nxt += d.clusters
        self.files = []
        for d in order:
            for c in d.children:
                if not c.is_dir:
                    c.clusters = (c.size + cs - 1) // cs
                    c.first_cluster = nxt if c.clusters else 0
                    nxt += c.clusters
                    self.files.append(c)
        self.dirs = order
        self.next_free = nxt
        if nxt - 2 > self.cluster_count:
            raise BuildError("ESP content does not fit")

    def _boot_sector(self) -> bytes:
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"DISTRBNY"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, self.cluster_size // SECTOR, RESERVED, 2, 0, 0, 0xF8, 0, 63,
                         255, self.hidden, self.volume_bytes // SECTOR)
        struct.pack_into("<IHHIHH12sBBBI11s8s", bs, 36, self.fat_sectors, 0, 0, 2, 1, 6, b"", 0x80, 0, 0x29,
                         self.serial, self.label.ljust(11).encode(), b"FAT32   ")
        bs[510:512] = b"\x55\xAA"
        return bytes(bs)

    def _fsinfo(self) -> bytes:
        fs = bytearray(SECTOR)
        free = self.cluster_count - (self.next_free - 2)
        struct.pack_into("<I", fs, 0, 0x41615252)
        struct.pack_into("<III", fs, 484, 0x61417272, free, self.next_free)
        struct.pack_into("<I", fs, 508, 0xAA550000)
        return bytes(fs)

    def _fat(self) -> bytes:
        fat = bytearray(self.fat_sectors * SECTOR)
        struct.pack_into("<II", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF)
        for node in self.dirs + self.files:
            for i in range(node.clusters):
                c = node.first_cluster + i
                struct.pack_into("<I", fat, c * 4, EOC if i == node.clusters - 1 else c + 1)
        return bytes(fat)

    def _dirent(self, short: bytes, attr: int, cluster: int, size: int, mtime: float) -> bytes:
        date, tm = _dos_datetime(mtime)
        return struct.pack("<11sBBBHHHHHHHI", short, attr, 0, 0, tm, date, date, cluster >> 16, tm, date, cluster & 0xFFFF, size)

    def _lfn(self, name: str, short: bytes) -> bytes:
        cs = _lfn_checksum(short)
        units = list(name.encode("utf-16-le"))
        chars = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
        chunks = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        out = bytearray()
        for idx in range(len(chunks), 0, -1):
            chunk = chunks[idx - 1]
            full = chunk + ([0x0000] if len(chunk) < 13 else [])
            full += [0xFFFF] * (13 - len(full))
            seq = idx | (0x40 if idx == len(chunks) else 0)
            e = bytearray(32)
            e[0] = seq
            struct.pack_into("<5H", e, 1, *full[0:5])
            e[11] = 0x0F
            e[13] = cs
            struct.pack_into("<6H", e, 14, *full[5:11])
            struct.pack_into("<2H", e, 28, *full[11:13])
            out += e
        return bytes(out)

    def _dir_bytes(self, d: _Node, parent: _Node | None) -> bytes:
        out = bytearray()
        if d is self.root:
            out += self._dirent(self.label.ljust(11).encode(), 0x08, 0, 0, self.ts)
        else:
            out += self._dirent(b".          ", 0x10, d.first_cluster, 0, d.mtime)
            pc = 0 if parent is self.root else parent.first_cluster
            out += self._dirent(b"..         ", 0x10, pc, 0, d.mtime)
        for c in d.children:
            if not c.lossless:
                out += self._lfn(c.name, c.short)
            attr = 0x10 if c.is_dir else 0x20
            out += self._dirent(c.short, attr, c.first_cluster, 0 if c.is_dir else c.size, c.mtime)
        return bytes(out.ljust(d.clusters * self.cluster_size, b"\0"))

    def pieces(self, base: int = 0) -> list:
        """Describe the volume at byte offset `base` as pieces (see image.pieces)."""
        self._layout()
        self._allocate()
        boot = self._boot_sector()
        fsinfo = self._fsinfo()
        fat = self._fat()
        out = [
            Blob(base, boot),
            Blob(base + SECTOR, fsinfo),
            Blob(base + 6 * SECTOR, boot),
            Blob(base + 7 * SECTOR, fsinfo),
            Blob(base + RESERVED * SECTOR, fat),
            Blob(base + (RESERVED + self.fat_sectors) * SECTOR, fat),
        ]

        def coff(c: int) -> int:
            return base + (self.data_start + (c - 2) * (self.cluster_size // SECTOR)) * SECTOR

        parents = {}

        def mark(d):
            for c in d.children:
                if c.is_dir:
                    parents[id(c)] = d
                    mark(c)

        mark(self.root)
        for d in self.dirs:
            out.append(Blob(coff(d.first_cluster), self._dir_bytes(d, parents.get(id(d)))))
        for node in self.files:
            if not node.size:
                continue
            if node.data is not None:
                out.append(Blob(coff(node.first_cluster), bytes(node.data)))
            else:
                out.append(Copy(coff(node.first_cluster), node.size, Path(node.source), 0, node.name))
        return out

    def write(self, f, base: int = 0) -> None:
        execute(f.fileno(), self.pieces(base))
