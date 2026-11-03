"""Assemble a Distro Buny disk image from an ISO collection and an ESP tree."""
from __future__ import annotations

import hashlib
import json
import os
import time
from pathlib import Path

from ..image import gpt
from ..image.exfat import GROW_LIMIT, GROWABLE_CLUSTER_SIZE, MAX_CLUSTERS, ExfatBuilder, default_cluster_size
from ..image.fat32 import Fat32Builder
from ..image.layout import ESP_NAME, Geometry, plan_geometry, plan_geometry_for_disk, write_image
from ..util import BuildError, human_size, info, step, warn

ESP_SIZE = 512 << 20
METADATA_MARGIN = 64 << 20


def serial_for(build_id: str, what: str) -> int:
    return int.from_bytes(hashlib.sha256(f"{build_id}/{what}".encode()).digest()[:4], "little")


def fs_uuid_from_serial(serial: int) -> str:
    return f"{serial >> 16:04X}-{serial & 0xFFFF:04X}"


def collect_isos(root: Path) -> list[tuple[str, Path]]:
    """Return (path on data partition, host path) for every *.iso below root."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for fn in sorted(filenames):
            if fn.lower().endswith(".iso") and not fn.startswith("."):
                host = Path(dirpath) / fn
                rel = "/" + str(host.relative_to(root))
                out.append((rel, host))
    return out


def plan_data_size(isos: list[tuple[str, Path]], extra: list[tuple[str, bytes]], free_bytes: int,
                   cluster_size: int | None, growable: bool = False) -> tuple[int, int]:
    content = sum(p.stat().st_size for _, p in isos) + sum(len(b) for _, b in extra)
    guess = content + free_bytes + METADATA_MARGIN
    cs = cluster_size or (GROWABLE_CLUSTER_SIZE if growable else default_cluster_size(guess))
    clustered = sum((p.stat().st_size + cs - 1) // cs * cs for _, p in isos) + len(extra) * cs
    # FAT (4 bytes/cluster), bitmap and directories, plus alignment
    overhead = (clustered // cs) * 4 + (clustered // cs) // 8 + 32 * cs + (8 << 20)
    if growable:
        overhead += min(GROW_LIMIT // cs, MAX_CLUSTERS) * 4  # FAT reserve for growing in place
    return clustered + overhead + free_bytes + METADATA_MARGIN, cs


def esp_builder_from_tree(tree: Path | None, files: dict[str, bytes], geo: Geometry, build_id: str, ts: float) -> Fat32Builder:
    esp = Fat32Builder(geo.esp_sectors * 512, label="BUNY-EFI", serial=serial_for(build_id, "esp"),
                       hidden_sectors=geo.esp_start, timestamp=ts)
    if tree and tree.exists():
        for dirpath, dirnames, filenames in os.walk(tree):
            dirnames.sort()
            rel = Path(dirpath).relative_to(tree)
            for fn in sorted(filenames):
                dest = "/".join(p for p in (str(rel) if str(rel) != "." else "", fn) if p)
                esp.add_file(dest, source=Path(dirpath) / fn)
    for dest, data in sorted(files.items()):
        esp.add_file(dest, data=data)
    return esp


class Volume:
    """A planned Distro Buny disk: geometry plus the two file system builders."""

    def __init__(self, geo: Geometry, esp: Fat32Builder, data: ExfatBuilder, build_id: str, serial: int, cs: int,
                 growable: bool = False):
        self.geo, self.esp, self.data = geo, esp, data
        self.build_id, self.serial, self.cluster_size = build_id, serial, cs
        self.growable = growable

    def describe(self) -> dict:
        return {
            "build_id": self.build_id,
            "geometry": self.geo.as_dict(),
            "data_fs": {"type": "exfat", "label": "DISTROBUNY", "uuid": fs_uuid_from_serial(self.serial),
                        "serial": f"{self.serial:08X}", "cluster_size": self.cluster_size,
                        "grow_limit": GROW_LIMIT if self.growable else None},
            "esp_fs": {"type": "vfat", "label": "BUNY-EFI",
                       "uuid": fs_uuid_from_serial(serial_for(self.build_id, "esp"))},
        }


def plan_volume(iso_root: Path, build_id: str, esp_tree: Path | None = None, esp_files: dict[str, bytes] | None = None,
                data_files: dict[str, bytes] | None = None, free_bytes: int = 2 << 30, cluster_size: int | None = None,
                iso_filter=None, bios_boot: bool = False, timestamp: float | None = None,
                disk_bytes: int | None = None, growable: bool = False) -> Volume:
    """Lay out a volume.  With disk_bytes the data partition fills a disk of that size.

    A growable data partition can later be extended in place to the end of a
    bigger drive by the runtime (images that users write with dd, Etcher or
    Rufus); a drive built for its disk (the installer) does not need it.
    """
    ts = timestamp if timestamp is not None else time.time()
    isos = collect_isos(iso_root)
    if iso_filter:
        isos = [(r, p) for r, p in isos if iso_filter(r)]
    extra = sorted((data_files or {}).items())
    data_bytes, cs = plan_data_size(isos, extra, free_bytes, cluster_size, growable)
    if disk_bytes is None:
        geo = plan_geometry(data_bytes, ESP_SIZE, bios_boot=bios_boot)
    else:
        geo = plan_geometry_for_disk(disk_bytes, ESP_SIZE, bios_boot=bios_boot)
        if geo.data_sectors * 512 < data_bytes - free_bytes:
            raise BuildError(f"the selected images need {human_size(data_bytes - free_bytes)} but the disk only has "
                             f"room for {human_size(geo.data_sectors * 512)}")
        if cluster_size is None and not growable:
            cs = default_cluster_size(geo.data_sectors * 512)
    serial = serial_for(build_id, "data")
    info(f"layout: {len(isos)} ISO files ({human_size(sum(p.stat().st_size for _, p in isos))}), "
         f"data partition {human_size(geo.data_sectors * 512)}, ESP {human_size(geo.esp_sectors * 512)}, "
         f"total {human_size(geo.total_bytes)}")
    data = ExfatBuilder(geo.data_sectors * 512, label="DISTROBUNY", serial=serial, cluster_size=cs,
                        partition_offset_sectors=geo.data_start, timestamp=ts,
                        grow_limit=GROW_LIMIT if growable else None)
    for rel, host in isos:
        data.add_file(rel, source=host)
    for rel, blob in extra:
        data.add_file(rel, data=blob)
    # keep the standard category folders even when empty
    for top in sorted({r.split("/")[1] for r, _ in isos} | {"Desktop", "Servers"}):
        data.add_dir(top)
    esp = esp_builder_from_tree(esp_tree, esp_files or {}, geo, build_id, ts)
    return Volume(geo, esp, data, build_id, serial, cs, growable)


def progress_printer():
    t0 = time.time()
    last = [0.0]

    def progress(done, total, path):
        now = time.time()
        if now - last[0] > 2 or done == total:
            last[0] = now
            rate = done / max(now - t0, 0.001)
            step(f"data {human_size(done)} / {human_size(total)} ({rate / (1 << 20):.0f} MiB/s) {Path(path).name}")

    return progress


def build_image(iso_root: Path, out: Path, build_id: str, esp_tree: Path | None = None,
                esp_files: dict[str, bytes] | None = None, data_files: dict[str, bytes] | None = None,
                free_bytes: int = 2 << 30, cluster_size: int | None = None, iso_filter=None,
                boot_code: bytes = b"", bios_core: bytes = b"", timestamp: float | None = None,
                disk_bytes: int | None = None, growable: bool = False) -> dict:
    """With disk_bytes the image has exactly that size and the data partition fills it."""
    vol = plan_volume(iso_root, build_id, esp_tree, esp_files, data_files, free_bytes, cluster_size, iso_filter,
                      bios_boot=bool(bios_core), timestamp=timestamp, disk_bytes=disk_bytes, growable=growable)
    st = os.statvfs(out.parent)
    if st.f_bavail * st.f_frsize < vol.geo.total_bytes * 1.02:
        raise BuildError(f"not enough free space in {out.parent} for a {human_size(vol.geo.total_bytes)} image")
    t0 = time.time()
    layout = write_image(out, vol.geo, build_id, vol.esp, vol.data, boot_code=boot_code, bios_core=bios_core,
                         progress=progress_printer())
    elapsed = time.time() - t0
    info(f"wrote {out} in {elapsed:.1f}s")
    return {"image": str(out), **vol.describe(), "layout": layout, "seconds": round(elapsed, 1)}


def refresh_esp(image: Path, build_id: str, esp_tree: Path | None, esp_files: dict[str, bytes] | None = None,
                timestamp: float | None = None) -> None:
    """Rewrite only the ESP of an existing image (fast iteration on the boot chain)."""
    with open(image, "r+b") as f:
        disk = gpt.read_tables(f)
        part = next((p for p in disk.partitions if p.name == ESP_NAME), None)
        if not part:
            raise BuildError(f"{image} has no {ESP_NAME} partition")
        geo = Geometry(disk.size_bytes, 0, 0, part.first_lba, part.last_lba - part.first_lba + 1)
        esp = esp_builder_from_tree(esp_tree, esp_files or {}, geo, build_id, timestamp or time.time())
        size = geo.esp_sectors * 512
        # clear the old file system so the result does not depend on history
        if not punch_hole(f.fileno(), part.first_lba * 512, size):
            warn("cannot punch a hole in the image; overwriting the ESP with zeros")
            zero = bytes(1 << 20)
            for off in range(0, size, len(zero)):
                os.pwrite(f.fileno(), zero[: min(len(zero), size - off)], part.first_lba * 512 + off)
        esp.write(f, part.first_lba * 512)
        f.flush()
        os.fsync(f.fileno())
    info(f"refreshed the ESP of {image}")


def punch_hole(fd: int, offset: int, length: int) -> bool:
    """Deallocate a byte range (it then reads as zeros).  False if unsupported."""
    import ctypes

    FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE = 0x01, 0x02
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_long, ctypes.c_long]
        return libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length) == 0
    except (OSError, AttributeError):
        return False


def write_manifest(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n")
