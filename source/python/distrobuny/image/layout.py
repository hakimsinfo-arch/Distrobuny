"""Distro Buny disk image layout.

    LBA 0        protective MBR (+ optional BIOS boot code)
    LBA 1-33     primary GPT
    1 MiB        partition 2  BUNY-EFI    FAT32   EFI System Partition (boot chain + runtime)
    ...          partition 3  BIOS-BOOT   (optional) GRUB core image for legacy BIOS
    ...          partition 1  DISTROBUNY  exFAT   ISO collection (Windows/macOS/Linux writable)
    end          backup GPT

The data partition is the first entry of the table because some operating
systems only mount the first partition of removable media, and that is the
partition users add ISO files to.  On the disk it comes last, so that a drive
written from an image onto a bigger stick can grow it to the end of the stick
without moving anything else (see source/filesystem/expand.c).  All
partitions are 1 MiB aligned.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from ..util import BuildError, stable_uuid
from . import gpt
from .exfat import ExfatBuilder
from .fat32 import Fat32Builder
from .pieces import Blob, execute

MiB = 1 << 20
DATA_NAME = "DISTROBUNY"
ESP_NAME = "BUNY-EFI"
BIOS_NAME = "BIOS-BOOT"


@dataclass
class Geometry:
    total_bytes: int
    data_start: int    # LBA
    data_sectors: int
    esp_start: int
    esp_sectors: int
    bios_start: int = 0
    bios_sectors: int = 0

    def as_dict(self) -> dict:
        return {
            "total_bytes": self.total_bytes,
            "data_partition": {"start_lba": self.data_start, "sectors": self.data_sectors, "number": 1},
            "esp_partition": {"start_lba": self.esp_start, "sectors": self.esp_sectors, "number": 2},
            "bios_boot_partition": {"start_lba": self.bios_start, "sectors": self.bios_sectors} if self.bios_sectors else None,
        }


def plan_geometry(data_bytes: int, esp_bytes: int, bios_boot: bool = False) -> Geometry:
    def align(n: int) -> int:
        return (n + MiB - 1) // MiB * MiB

    data_bytes = align(data_bytes)
    esp_bytes = align(esp_bytes)
    bios_bytes = MiB if bios_boot else 0
    start = MiB
    total = start + esp_bytes + bios_bytes + data_bytes + MiB
    data_start = start + esp_bytes + bios_bytes
    g = Geometry(total, data_start // 512, data_bytes // 512, start // 512, esp_bytes // 512)
    if bios_boot:
        g.bios_start = g.esp_start + g.esp_sectors
        g.bios_sectors = bios_bytes // 512
    return g


def plan_geometry_for_disk(total_bytes: int, esp_bytes: int, bios_boot: bool = False) -> Geometry:
    """Geometry that fills a whole disk of `total_bytes` (the data partition takes the rest)."""
    bios_bytes = MiB if bios_boot else 0
    data_bytes = (total_bytes - 2 * MiB - esp_bytes - bios_bytes) // MiB * MiB
    if data_bytes < 64 * MiB:
        raise BuildError(f"the disk is too small ({total_bytes} bytes)")
    g = plan_geometry(data_bytes, esp_bytes, bios_boot)
    g.total_bytes = total_bytes  # backup GPT at the real end of the disk
    return g


def make_disk(geo: Geometry, build_id: str) -> gpt.Disk:
    disk = gpt.Disk(geo.total_bytes, stable_uuid(build_id, "disk"))
    disk.partitions.append(gpt.Partition(DATA_NAME, gpt.TYPE_MS_BASIC_DATA, stable_uuid(build_id, "data"),
                                         geo.data_start, geo.data_start + geo.data_sectors - 1))
    disk.partitions.append(gpt.Partition(ESP_NAME, gpt.TYPE_ESP, stable_uuid(build_id, "esp"),
                                         geo.esp_start, geo.esp_start + geo.esp_sectors - 1))
    if geo.bios_sectors:
        disk.partitions.append(gpt.Partition(BIOS_NAME, gpt.TYPE_BIOS_BOOT, stable_uuid(build_id, "bios"),
                                             geo.bios_start, geo.bios_start + geo.bios_sectors - 1))
    return disk


def volume_pieces(geo: Geometry, build_id: str, esp: Fat32Builder, data: ExfatBuilder,
                  boot_code: bytes = b"", bios_core: bytes = b"") -> tuple[gpt.Disk, list, dict]:
    """Describe a whole disk as pieces: file systems first, partition table last.

    Writing the table last means an interrupted write leaves a disk without
    a valid GPT rather than one with half-written file systems.
    """
    if data.volume_bytes != geo.data_sectors * 512:
        raise BuildError("internal error: exFAT size does not match the partition")
    disk = make_disk(geo, build_id)
    out = list(esp.pieces(geo.esp_start * 512))
    if bios_core:
        if len(bios_core) > geo.bios_sectors * 512:
            raise BuildError("BIOS core image does not fit the BIOS boot partition")
        out.append(Blob(geo.bios_start * 512, bios_core))
    data_pieces, layout = data.pieces(geo.data_start * 512)
    out += data_pieces
    out += [Blob(off, blob) for off, blob in gpt.table_blobs(disk, boot_code, bootable_pmbr=bool(boot_code))]
    return disk, out, layout


def write_volume(f, geo: Geometry, build_id: str, esp: Fat32Builder, data: ExfatBuilder,
                 boot_code: bytes = b"", bios_core: bytes = b"", progress=None) -> tuple[gpt.Disk, dict]:
    """Write all structures into an open file of geo.total_bytes; returns the table and layout map."""
    disk, out, layout = volume_pieces(geo, build_id, esp, data, boot_code, bios_core)
    execute(f.fileno(), out, progress)
    f.flush()
    os.fsync(f.fileno())
    return disk, layout


def write_image(path: Path, geo: Geometry, build_id: str, esp: Fat32Builder, data: ExfatBuilder,
                boot_code: bytes = b"", bios_core: bytes = b"", progress=None) -> dict:
    """Create the image file and write all structures.  Returns the data layout map."""
    tmp = path.with_suffix(path.suffix + ".partial")
    with open(tmp, "wb") as f:
        f.truncate(geo.total_bytes)  # sparse: unwritten space costs nothing
        disk, layout = write_volume(f, geo, build_id, esp, data, boot_code, bios_core, progress)
    os.replace(tmp, path)
    return {
        "disk_guid": str(disk.disk_guid),
        "partitions": [
            {"name": p.name, "type": str(p.type_guid), "guid": str(p.guid), "first_lba": p.first_lba, "last_lba": p.last_lba}
            for p in disk.partitions
        ],
        "files": {k: {"offset": v[0], "size": v[1]} for k, v in layout.items()},
    }
