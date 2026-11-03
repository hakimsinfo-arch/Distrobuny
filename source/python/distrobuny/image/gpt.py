"""GUID Partition Table writer and reader (UEFI specification, chapter 5).

Writes a protective MBR, the primary header and entry array at LBA 1-33 and
the backup copies at the end of the disk.  GUIDs are supplied by the caller so
images are reproducible.
"""
from __future__ import annotations

import struct
import uuid
import zlib
from dataclasses import dataclass, field

SECTOR = 512
ENTRY_SIZE = 128
NUM_ENTRIES = 128
ENTRY_SECTORS = NUM_ENTRIES * ENTRY_SIZE // SECTOR  # 32

TYPE_ESP = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
TYPE_MS_BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
TYPE_BIOS_BOOT = uuid.UUID("21686148-6449-6e6f-744e-656564454649")
TYPE_LINUX_DATA = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")

ATTR_REQUIRED = 1 << 0          # platform required (firmware must not remove)
ATTR_LEGACY_BIOS_BOOTABLE = 1 << 2
ATTR_MS_NO_DRIVE_LETTER = 1 << 63


@dataclass
class Partition:
    name: str
    type_guid: uuid.UUID
    guid: uuid.UUID
    first_lba: int
    last_lba: int            # inclusive
    attributes: int = 0

    @property
    def size_bytes(self) -> int:
        return (self.last_lba - self.first_lba + 1) * SECTOR

    @property
    def offset(self) -> int:
        return self.first_lba * SECTOR


@dataclass
class Disk:
    size_bytes: int
    disk_guid: uuid.UUID
    partitions: list[Partition] = field(default_factory=list)

    @property
    def sectors(self) -> int:
        return self.size_bytes // SECTOR

    @property
    def first_usable(self) -> int:
        return 2 + ENTRY_SECTORS

    @property
    def last_usable(self) -> int:
        return self.sectors - 2 - ENTRY_SECTORS


def _entries(disk: Disk) -> bytes:
    out = bytearray(NUM_ENTRIES * ENTRY_SIZE)
    for i, p in enumerate(disk.partitions):
        name = p.name.encode("utf-16-le")[:72].ljust(72, b"\0")
        struct.pack_into(
            "<16s16sQQQ72s",
            out,
            i * ENTRY_SIZE,
            p.type_guid.bytes_le,
            p.guid.bytes_le,
            p.first_lba,
            p.last_lba,
            p.attributes,
            name,
        )
    return bytes(out)


def _header(disk: Disk, current: int, backup: int, entries_lba: int, entries_crc: int) -> bytes:
    hdr = bytearray(SECTOR)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        hdr,
        0,
        b"EFI PART",
        0x00010000,       # revision 1.0
        92,               # header size
        0,                # header CRC (filled below)
        0,
        current,
        backup,
        disk.first_usable,
        disk.last_usable,
        disk.disk_guid.bytes_le,
        entries_lba,
        NUM_ENTRIES,
        ENTRY_SIZE,
        entries_crc,
    )
    crc = zlib.crc32(bytes(hdr[:92])) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 16, crc)
    return bytes(hdr)


def protective_mbr(disk: Disk, boot_code: bytes = b"", bootable: bool = False) -> bytes:
    mbr = bytearray(SECTOR)
    if boot_code:
        mbr[: min(len(boot_code), 440)] = boot_code[:440]
    struct.pack_into("<I", mbr, 440, zlib.crc32(disk.disk_guid.bytes) & 0xFFFFFFFF)  # disk signature
    size = min(disk.sectors - 1, 0xFFFFFFFF)
    # one partition of type 0xEE covering the disk; CHS values per UEFI spec
    struct.pack_into("<B3sB3sII", mbr, 446, 0x80 if bootable else 0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, size)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def table_blobs(disk: Disk, boot_code: bytes = b"", bootable_pmbr: bool = False) -> list[tuple[int, bytes]]:
    """The protective MBR and both GPT copies as (byte offset, data) pieces.

    Entries keep the caller's order, which need not be the order on disk
    (Distro Buny's data partition is entry 1 but lies behind the ESP).
    """
    for p in disk.partitions:
        if p.first_lba < disk.first_usable or p.last_lba > disk.last_usable or p.first_lba > p.last_lba:
            raise ValueError(f"partition {p.name} is outside the usable area")
    by_lba = sorted(disk.partitions, key=lambda p: p.first_lba)
    for a, b in zip(by_lba, by_lba[1:]):
        if a.last_lba >= b.first_lba:
            raise ValueError(f"partitions {a.name} and {b.name} overlap")
    entries = _entries(disk)
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    last = disk.sectors - 1
    return [
        (0, protective_mbr(disk, boot_code, bootable_pmbr)),
        (SECTOR, _header(disk, 1, last, 2, ecrc)),
        (2 * SECTOR, entries),
        ((last - ENTRY_SECTORS) * SECTOR, entries),
        (last * SECTOR, _header(disk, last, 1, last - ENTRY_SECTORS, ecrc)),
    ]


def write_tables(f, disk: Disk, boot_code: bytes = b"", bootable_pmbr: bool = False) -> None:
    """Write the protective MBR and both GPT copies into an open file."""
    for off, data in table_blobs(disk, boot_code, bootable_pmbr):
        os_write(f, off, data)


def os_write(f, offset: int, data: bytes) -> None:
    f.seek(offset)
    f.write(data)


def read_tables(f) -> Disk:
    """Parse and validate a GPT (used by tests and the installer)."""
    f.seek(SECTOR)
    hdr = f.read(SECTOR)
    if hdr[:8] != b"EFI PART":
        raise ValueError("no GPT header")
    fields = struct.unpack_from("<8sIIIIQQQQ16sQIII", hdr, 0)
    hsize = fields[2]
    crc = fields[3]
    check = bytearray(hdr[:hsize])
    struct.pack_into("<I", check, 16, 0)
    if zlib.crc32(bytes(check)) & 0xFFFFFFFF != crc:
        raise ValueError("GPT header CRC mismatch")
    backup_lba = fields[6]
    disk_guid = uuid.UUID(bytes_le=fields[9])
    entries_lba, n, esize, ecrc = fields[10], fields[11], fields[12], fields[13]
    f.seek(entries_lba * SECTOR)
    raw = f.read(n * esize)
    if zlib.crc32(raw) & 0xFFFFFFFF != ecrc:
        raise ValueError("GPT entry array CRC mismatch")
    disk = Disk((backup_lba + 1) * SECTOR, disk_guid)
    for i in range(n):
        e = raw[i * esize:(i + 1) * esize]
        tg, pg, first, last, attrs, name = struct.unpack_from("<16s16sQQQ72s", e, 0)
        if tg == b"\0" * 16:
            continue
        disk.partitions.append(
            Partition(name.decode("utf-16-le").rstrip("\0"), uuid.UUID(bytes_le=tg), uuid.UUID(bytes_le=pg), first, last, attrs)
        )
    return disk
