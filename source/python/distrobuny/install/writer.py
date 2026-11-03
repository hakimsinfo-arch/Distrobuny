"""Writing Distro Buny onto a drive (or a file standing in for one).

Two ways to install:
  * image mode copies a built image (buny-build image) onto the target.  The
    partition table is regenerated with fresh GUIDs and placed at the real
    end of the target, so every drive is distinct and larger drives get a
    valid backup GPT;
  * create mode lays the volume out directly on the target from an ISO
    folder, with the data partition filling the whole drive.  No intermediate
    image (and no host disk space) is needed.

Both reduce to a list of non-overlapping segments (zeros, bytes, ranges of a
source file).  Zones where an old partition table or file system signature
could survive (first and last MiB of the disk, first 8 MiB of every
partition) are cleared explicitly.  The partition table is written last and
everything is read back and compared afterwards.
"""
from __future__ import annotations

import errno
import fcntl
import os
import stat
import struct
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

from ..image import gpt
from ..image.layout import DATA_NAME, ESP_NAME
from ..image.pieces import Blob, Copy, Zero
from ..util import human_size

MiB = 1 << 20
CHUNK = 8 * MiB
SYNC_EVERY = 512 * MiB
PARTITION_WIPE = 8 * MiB

BLKGETSIZE64 = 0x80081272
BLKSSZGET = 0x1268
BLKFLSBUF = 0x1261
BLKRRPART = 0x125F


class InstallError(RuntimeError):
    """An install step failed with a message meant for the user."""


# --------------------------------------------------------------------- targets

class Target:
    """An open install target: a block device held exclusively, or a regular file."""

    def __init__(self, path: str, fd: int, size: int, is_block: bool):
        self.path, self.fd, self.size, self.is_block = path, fd, size, is_block

    @classmethod
    def open_device(cls, path: str, devno: str, expect_size: int) -> "Target":
        # O_EXCL on a block device is an exclusive claim: the kernel refuses it
        # while any partition is mounted or held, and nobody can mount the
        # drive while we hold it (between confirmation and writing).
        try:
            fd = os.open(path, os.O_RDWR | os.O_EXCL | os.O_CLOEXEC)
        except OSError as exc:
            if exc.errno in (errno.EACCES, errno.EPERM):
                raise InstallError(f"permission denied opening {path}: run buny-install as root (sudo)") from exc
            if exc.errno == errno.EBUSY:
                raise InstallError(f"{path} is busy: something has it mounted or open; unmount it and retry") from exc
            if exc.errno == errno.EROFS:
                raise InstallError(f"{path} is write-protected") from exc
            raise InstallError(f"cannot open {path}: {exc.strerror}") from exc
        try:
            st = os.fstat(fd)
            if not stat.S_ISBLK(st.st_mode):
                raise InstallError(f"{path} is not a block device")
            got = f"{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}"
            if got != devno:
                raise InstallError(f"{path} changed identity ({got}, expected {devno}); not writing")
            size = struct.unpack("<Q", fcntl.ioctl(fd, BLKGETSIZE64, bytes(8)))[0]
            ssz = struct.unpack("<i", fcntl.ioctl(fd, BLKSSZGET, bytes(4)))[0]
            if size != expect_size:
                raise InstallError(f"{path} reports {size} bytes, expected {expect_size}; was the drive swapped?")
            if ssz != 512:
                raise InstallError(f"{path} uses {ssz}-byte sectors, which this version cannot write")
        except BaseException:
            os.close(fd)
            raise
        return cls(path, fd, size, True)

    @classmethod
    def open_file(cls, path: Path, size: int, overwrite: bool = False) -> "Target":
        """A regular file standing in for a drive (tests, virtual machine disks)."""
        if path.exists():
            if not path.is_file():
                raise InstallError(f"{path} exists and is not a regular file")
            if not overwrite:
                raise InstallError(f"{path} already exists (pass --overwrite-file to replace it)")
        if size % 512:
            raise InstallError("the target size must be a multiple of 512 bytes")
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o644)
        os.ftruncate(fd, size)
        return cls(str(path), fd, size, False)

    def close(self, reread: bool = True) -> None:
        if self.fd < 0:
            return
        if self.is_block and reread:
            try:
                fcntl.ioctl(self.fd, BLKRRPART)
            except OSError:
                pass  # the kernel re-reads the table when the drive is next plugged in
        os.close(self.fd)
        self.fd = -1


# --------------------------------------------------------------- source image

@dataclass
class SourceImage:
    path: Path
    size: int
    disk: gpt.Disk
    mbr: bytes

    @classmethod
    def open(cls, path: Path) -> "SourceImage":
        try:
            size = path.stat().st_size
            with open(path, "rb") as f:
                mbr = f.read(512)
                disk = gpt.read_tables(f)
                names = [p.name for p in disk.partitions]
                if DATA_NAME not in names or ESP_NAME not in names:
                    raise InstallError(f"{path} is not a Distro Buny image (partitions: {', '.join(names) or 'none'})")
                data = next(p for p in disk.partitions if p.name == DATA_NAME)
                esp = next(p for p in disk.partitions if p.name == ESP_NAME)
                f.seek(data.offset + 3)
                if f.read(8) != b"EXFAT   ":
                    raise InstallError(f"{path}: the data partition holds no exFAT file system")
                f.seek(esp.offset + 82)
                if f.read(8) != b"FAT32   ":
                    raise InstallError(f"{path}: the EFI system partition holds no FAT32 file system")
        except (OSError, ValueError) as exc:
            raise InstallError(f"cannot read {path}: {exc}") from exc
        if disk.size_bytes != size:
            raise InstallError(f"{path}: the partition table does not match the file size (truncated image?)")
        return cls(path, size, disk, mbr)

    def data_extents(self) -> list[tuple[int, int]]:
        """(offset, length) of the allocated parts of a sparse image."""
        out = []
        fd = os.open(self.path, os.O_RDONLY | os.O_CLOEXEC)
        try:
            off = 0
            while off < self.size:
                try:
                    start = os.lseek(fd, off, os.SEEK_DATA)
                except OSError:
                    break  # ENXIO: no data after off
                end = os.lseek(fd, start, os.SEEK_HOLE)
                out.append((start, min(end, self.size) - start))
                off = end
        finally:
            os.close(fd)
        return out


# --------------------------------------------------------------------- planning

class Plan:
    """Non-overlapping segments in target order; later paint() calls win."""

    def __init__(self, size: int):
        self.size = size
        self.segs: list[tuple[int, int, object, int]] = []  # (start, end, source, source_base)
        self.last: set[int] = set()  # ids of sources written after everything else

    def paint(self, start: int, end: int, source, base: int = 0) -> None:
        if not (0 <= start < end <= self.size):
            raise InstallError(f"internal error: segment {start}-{end} outside the target")
        out = []
        for s, e, src, b in self.segs:
            if e <= start or s >= end:
                out.append((s, e, src, b))
                continue
            if s < start:
                out.append((s, start, src, b))
            if e > end:
                out.append((end, e, src, b + (end - s)))
        out.append((start, end, source, base))
        out.sort(key=lambda x: x[0])
        self.segs = out

    def add(self, piece, last: bool = False) -> None:
        if isinstance(piece, Zero):
            if piece.length:
                self.paint(piece.offset, piece.offset + piece.length, None)
        elif isinstance(piece, Blob):
            self.paint(piece.offset, piece.offset + piece.length, piece.data)
            if last:
                self.last.add(id(piece.data))
        else:
            if piece.length:
                self.paint(piece.offset, piece.offset + piece.length, piece.source, piece.source_offset)

    def clear_zones(self, partitions: list[gpt.Partition]) -> None:
        """Zero where stale partition tables and file system signatures could survive."""
        self.add(Zero(0, min(MiB, self.size)))
        tail = max(0, self.size - MiB)
        self.add(Zero(tail, self.size - tail))
        for p in partitions:
            self.add(Zero(p.offset, min(PARTITION_WIPE, p.size_bytes)))

    @property
    def total(self) -> int:
        return sum(e - s for s, e, _, _ in self.segs)

    def ordered(self) -> list:
        early = [x for x in self.segs if id(x[2]) not in self.last]
        late = [x for x in self.segs if id(x[2]) in self.last]
        return early + late


def plan_from_image(src: SourceImage, target_size: int) -> tuple[Plan, gpt.Disk]:
    if target_size < src.size:
        raise InstallError(f"the target is too small: the image needs {human_size(src.size)}, "
                           f"the target has {human_size(target_size)}")
    disk = gpt.Disk(target_size, uuid.uuid4(),
                    [gpt.Partition(p.name, p.type_guid, uuid.uuid4(), p.first_lba, p.last_lba, p.attributes)
                     for p in src.disk.partitions])
    plan = Plan(target_size)
    plan.clear_zones(disk.partitions)
    # the image's own GPT copies are replaced by the regenerated table
    head = (2 + gpt.ENTRY_SECTORS) * 512
    tail = src.size - (1 + gpt.ENTRY_SECTORS) * 512
    if target_size > src.size:
        plan.add(Zero(tail, src.size - tail))
    for off, length in src.data_extents():
        s, e = max(off, head), min(off + length, tail)
        if s < e:
            plan.add(Copy(s, e - s, src.path, s))
    boot_code = src.mbr[:440] if any(src.mbr[:440]) else b""
    bootable = src.mbr[446] == 0x80
    for off, blob in gpt.table_blobs(disk, boot_code, bootable):
        plan.add(Blob(off, blob), last=True)
    return plan, disk


def plan_from_pieces(size: int, partitions: list[gpt.Partition], pieces: list) -> Plan:
    plan = Plan(size)
    plan.clear_zones(partitions)
    for p in pieces:
        plan.add(p)
    # the partition table pieces are the Blobs that land in the first/last 34 sectors
    edge = (2 + gpt.ENTRY_SECTORS) * 512
    for p in pieces:
        if isinstance(p, Blob) and (p.offset < edge or p.offset >= size - edge):
            plan.last.add(id(p.data))
    return plan


# -------------------------------------------------------------------- execution

class Progress:
    def __init__(self, label: str, total: int, out=None):
        self.label, self.total, self.done = label, total, 0
        self.t0 = time.time()
        self.shown = 0.0
        self.out = out

    def add(self, n: int) -> None:
        self.done += n
        now = time.time()
        if self.out and (now - self.shown > 0.5 or self.done >= self.total):
            self.shown = now
            rate = self.done / max(now - self.t0, 1e-3)
            eta = (self.total - self.done) / rate if rate else 0
            pct = self.done * 100 // max(self.total, 1)
            self.out(f"\r  {self.label} {pct:3d}%  {human_size(self.done)} / {human_size(self.total)}  "
                     f"{rate / MiB:6.1f} MiB/s  ETA {int(eta) // 60}:{int(eta) % 60:02d} ")


def _source_reader(cache: dict, source) -> int:
    if source not in cache:
        cache[source] = os.open(source, os.O_RDONLY | os.O_CLOEXEC)
    return cache[source]


def _expected(cache: dict, source, base: int, delta: int, n: int) -> bytes:
    if source is None:
        return bytes(n)
    if isinstance(source, bytes):
        return source[base + delta: base + delta + n]
    data = os.pread(_source_reader(cache, source), n, base + delta)
    if len(data) != n:
        raise InstallError(f"{source} ended unexpectedly while reading")
    return data


def write_plan(t: Target, plan: Plan, out=None) -> None:
    """Write all segments, data first and the partition table last."""
    prog = Progress("writing  ", plan.total, out)
    cache: dict = {}
    since_sync = 0
    try:
        for s, e, src, base in plan.ordered():
            pos = s
            while pos < e:
                n = min(CHUNK, e - pos)
                buf = _expected(cache, src, base, pos - s, n)
                w = os.pwrite(t.fd, buf, pos)
                if w != n:
                    raise InstallError(f"short write at byte {pos} of {t.path}")
                pos += n
                prog.add(n)
                since_sync += n
                if t.is_block and since_sync >= SYNC_EVERY:
                    # keep the page cache (and the progress figure) honest
                    os.fdatasync(t.fd)
                    os.posix_fadvise(t.fd, 0, 0, os.POSIX_FADV_DONTNEED)
                    since_sync = 0
        os.fsync(t.fd)
    except OSError as exc:
        raise InstallError(f"write error on {t.path} at about {human_size(prog.done)}: {exc.strerror}. "
                           "The drive is incomplete and will not boot; check it and try again.") from exc
    finally:
        for fd in cache.values():
            os.close(fd)
    if out:
        out("\n")


def verify_plan(t: Target, plan: Plan, out=None) -> None:
    """Read everything back from the target and compare it with what was meant to be written."""
    if t.is_block:
        os.posix_fadvise(t.fd, 0, 0, os.POSIX_FADV_DONTNEED)
        try:
            fcntl.ioctl(t.fd, BLKFLSBUF)  # drop cached blocks so we read the medium
        except OSError:
            pass
    prog = Progress("verifying", plan.total, out)
    cache: dict = {}
    try:
        for s, e, src, base in plan.segs:
            pos = s
            while pos < e:
                n = min(CHUNK, e - pos)
                got = os.pread(t.fd, n, pos)
                if got != _expected(cache, src, base, pos - s, n):
                    raise InstallError(f"verification failed: data at byte {pos} of {t.path} differs from what was "
                                       "written. The drive may be faulty or counterfeit (fake capacity).")
                pos += n
                prog.add(n)
    except OSError as exc:
        raise InstallError(f"read error on {t.path} during verification: {exc.strerror}") from exc
    finally:
        for fd in cache.values():
            os.close(fd)
    if out:
        out("\n")
