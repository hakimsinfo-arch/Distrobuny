"""Drive discovery and safety assessment for the installer.

Everything is read from sysfs and procfs.  The roots are parameters, so the
rules can be tested against synthetic trees without touching real disks.

A drive is offered as an install target only when:
  * it is a whole physical disk (not a partition, loop, RAM, device-mapper,
    RAID or optical device) with a medium inserted and 512-byte sectors;
  * no file system on it (directly or through LVM/LUKS/RAID stacked on top)
    is mounted, nothing holds it, and it is not used for swap;
  * it is not part of the running system (/, /boot, /usr, ... never qualify,
    whatever options are given);
  * it is a removable USB or SD-card drive, unless --allow-fixed-disk;
  * it is not write-protected and is large enough for the content.
"""
from __future__ import annotations

import os
import stat
from dataclasses import dataclass, field
from pathlib import Path

from ..util import human_size

SYSTEM_MOUNTS = ("/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", "/nix", "/opt", "/srv", "/snap")
NOT_PHYSICAL = ("loop", "ram", "zram", "dm-", "md", "sr", "nbd", "fd", "zd", "drbd", "rbd", "mtdblock", "ubiblock")
LARGE_WARNING = 4 << 40


@dataclass
class Partition:
    name: str
    size: int
    devno: str


@dataclass
class Drive:
    name: str
    path: str
    devno: str
    size: int
    logical_block: int
    removable: bool
    read_only: bool
    transport: str
    vendor: str = ""
    model: str = ""
    serial: str = ""
    partitions: list[Partition] = field(default_factory=list)
    holders: list[str] = field(default_factory=list)       # e.g. ["dm-0 (on sdb2)"]
    mounts: list[str] = field(default_factory=list)        # mount points using this drive
    swap: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)      # reasons it must not be written
    warnings: list[str] = field(default_factory=list)

    @property
    def description(self) -> str:
        ident = " ".join(x for x in (self.vendor, self.model) if x) or "unknown model"
        return f"{ident}, {human_size(self.size)}, {self.transport.upper()}"

    @property
    def system(self) -> bool:
        return bool(self.swap) or any(m in SYSTEM_MOUNTS for m in self.mounts)

    @property
    def eligible(self) -> bool:
        return not self.problems

    def identity(self) -> tuple:
        """What must not change between confirmation and writing."""
        return (self.name, self.devno, self.size, self.vendor, self.model, self.serial)


def _read(path: Path, default: str = "") -> str:
    try:
        return path.read_text(errors="replace").strip()
    except OSError:
        return default


def _int(path: Path, default: int = 0) -> int:
    try:
        return int(_read(path) or default)
    except ValueError:
        return default


def _unescape(s: str) -> str:
    """Octal escapes used by /proc/self/mountinfo and /proc/swaps (\\040 for space)."""
    out, i = [], 0
    while i < len(s):
        esc = s[i + 1:i + 4]
        if s[i] == "\\" and len(esc) == 3 and all(ch in "01234567" for ch in esc):
            out.append(chr(int(esc, 8)))
            i += 4
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


class System:
    """A view of the block devices and their users (real or synthetic roots)."""

    def __init__(self, sys_root: Path = Path("/sys"), proc_root: Path = Path("/proc"), dev_root: Path = Path("/dev"),
                 live: bool | None = None):
        self.sys = Path(sys_root)
        self.proc = Path(proc_root)
        self.dev = Path(dev_root)
        self.live = live if live is not None else str(sys_root) == "/sys"
        self.cls = self.sys / "class" / "block"
        self.by_devno: dict[str, str] = {}
        for entry in self._names():
            devno = _read(self.cls / entry / "dev")
            if devno:
                self.by_devno[devno] = entry

    def _names(self) -> list[str]:
        try:
            return sorted(os.listdir(self.cls))
        except OSError:
            return []

    # --------------------------------------------------------- topology
    def is_partition(self, name: str) -> bool:
        return (self.cls / name / "partition").exists()

    def parent_disk(self, name: str) -> str:
        """The whole disk a partition belongs to (a disk is its own parent)."""
        if not self.is_partition(name):
            return name
        return Path(os.path.realpath(self.cls / name)).parent.name

    def lower(self, name: str, seen: set | None = None) -> set[str]:
        """Physical devices under a (possibly stacked) device: dm-0 -> {sdb2, sdb}."""
        seen = seen if seen is not None else set()
        if name in seen:
            return set()
        seen.add(name)
        out = {name, self.parent_disk(name)}
        try:
            slaves = os.listdir(self.cls / name / "slaves")
        except OSError:
            slaves = []
        for s in slaves:
            out |= self.lower(s, seen)
        return out

    def name_for_source(self, devno: str, source: str) -> str | None:
        if devno in self.by_devno:
            return self.by_devno[devno]
        if source.startswith("/dev/"):
            real = os.path.realpath(source) if self.live else source
            base = os.path.basename(real)
            if (self.cls / base).exists():
                return base
        return None

    # ----------------------------------------------------------- usage
    def mounts(self) -> list[tuple[str, str]]:
        """(block device name, mount point) for every mount backed by a block device."""
        out = []
        for line in _read(self.proc / "self" / "mountinfo").splitlines():
            left, _, right = line.partition(" - ")
            f = left.split()
            r = right.split()
            if len(f) < 5 or len(r) < 2:
                continue
            name = self.name_for_source(f[2], _unescape(r[1]))
            if name:
                out.append((name, _unescape(f[4])))
        if self.live:
            # belt and braces: the devices holding the running system, by st_dev
            found = {mp for _, mp in out}
            for mp in ("/", "/boot", "/boot/efi", "/efi", "/usr"):
                if mp in found or (mp != "/" and not os.path.ismount(mp)):
                    continue
                try:
                    st = os.stat(mp)
                except OSError:
                    continue
                devno = f"{os.major(st.st_dev)}:{os.minor(st.st_dev)}"
                if devno in self.by_devno:
                    out.append((self.by_devno[devno], mp))
        return out

    def swaps(self) -> list[str]:
        out = []
        for line in _read(self.proc / "swaps").splitlines()[1:]:
            parts = line.split()
            if parts and parts[0].startswith("/dev/"):
                name = self.name_for_source("", _unescape(parts[0]))
                if name:
                    out.append(name)
        return out

    # ---------------------------------------------------------- drives
    def _transport(self, name: str, real: str) -> str:
        if "/usb" in real:
            return "usb"
        if name.startswith("nvme"):
            return "nvme"
        if name.startswith("mmcblk"):
            kind = _read(self.cls / name / "device" / "type").upper()
            return "sd" if kind in ("SD", "SDIO") else "emmc"
        if "/virtio" in real:
            return "virtio"
        if "/ata" in real:
            return "sata"
        if "/firewire" in real or "/ieee1394" in real:
            return "firewire"
        if "/host" in real or "/scsi" in real:
            return "scsi"
        return "unknown"

    def _usb_serial(self, real: str) -> tuple[str, str, str]:
        """Manufacturer, product and serial from the USB device above a disk."""
        p = Path(real)
        for anc in [p, *p.parents]:
            if (anc / "idVendor").exists():
                return _read(anc / "manufacturer"), _read(anc / "product"), _read(anc / "serial")
            if anc == self.sys:
                break
        return "", "", ""

    def drive(self, name: str) -> Drive:
        base = self.cls / name
        real = os.path.realpath(base)
        transport = self._transport(name, real)
        dev = base / "device"
        vendor = _read(dev / "vendor")
        model = _read(dev / "model") or _read(dev / "name")
        serial = _read(dev / "serial")
        if transport == "usb":
            manu, prod, ser = self._usb_serial(real)
            vendor = vendor or manu
            model = model or prod
            serial = serial or ser
        d = Drive(
            name=name,
            path=str(self.dev / name),
            devno=_read(base / "dev"),
            size=_int(base / "size") * 512,
            logical_block=_int(base / "queue" / "logical_block_size", 512),
            removable=_read(base / "removable") == "1",
            read_only=_read(base / "ro") == "1",
            transport=transport,
            vendor=vendor,
            model=model,
            serial=serial,
        )
        for entry in sorted(os.listdir(base)):
            if (base / entry / "partition").exists():
                d.partitions.append(Partition(entry, _int(base / entry / "size") * 512, _read(base / entry / "dev")))
        return d

    def drives(self, include_virtual: bool = False) -> list[Drive]:
        """All whole disks, assessed for safety (problems/warnings filled in)."""
        out = []
        for name in self._names():
            if self.is_partition(name):
                continue
            if not include_virtual and name.startswith(NOT_PHYSICAL):
                continue
            out.append(self.drive(name))
        self.assess(out)
        return out

    def assess(self, drives: list[Drive], need_bytes: int = 0, allow_fixed: bool = False) -> None:
        mounts = self.mounts()
        swaps = self.swaps()
        for d in drives:
            members = {d.name} | {p.name for p in d.partitions}
            d.mounts = sorted({mp for dev, mp in mounts if self.lower(dev) & members})
            d.swap = sorted({s for s in swaps if self.lower(s) & members})
            holders = []
            for m in sorted(members):
                try:
                    hs = os.listdir(self.cls / m / "holders")
                except OSError:
                    hs = []
                holders += [f"{h} (on {m})" for h in hs]
            d.holders = holders
            d.problems, d.warnings = [], []
            p, w = d.problems, d.warnings
            if d.name.startswith(NOT_PHYSICAL):
                p.append("not a physical drive (loop, RAM, device-mapper, RAID or optical device)")
            if d.system:
                used = ", ".join(d.mounts + [f"swap on {s}" for s in d.swap])
                p.append(f"holds the running system ({used}) — never a valid target")
            elif d.mounts:
                p.append(f"has mounted file systems ({', '.join(d.mounts)}) — unmount them first")
            if d.holders and not d.system:
                p.append(f"is in use by {', '.join(d.holders)} (LVM, encryption or RAID) — release it first")
            if d.size == 0:
                p.append("no medium inserted")
            if d.read_only:
                p.append("is write-protected")
            if d.logical_block != 512:
                p.append(f"uses {d.logical_block}-byte sectors, which this version cannot write")
            if need_bytes and d.size and d.size < need_bytes:
                p.append(f"is too small: {human_size(need_bytes)} needed, {human_size(d.size)} available")
            fixed = d.transport not in ("usb", "sd")
            if fixed and not allow_fixed:
                p.append(f"is a fixed {d.transport.upper()} disk, not a removable USB/SD drive "
                         "(use --allow-fixed-disk only if you are certain)")
            elif fixed:
                w.append(f"fixed {d.transport.upper()} disk selected with --allow-fixed-disk")
            if d.transport == "usb" and not d.removable:
                w.append("USB disk that does not report removable media (an external SSD or hard disk?)")
            if d.size >= LARGE_WARNING:
                w.append(f"unusually large for a USB drive ({human_size(d.size)}) — check that it is the right disk")

    def resolve(self, spec: str) -> str:
        """Map 'sdb', '/dev/sdb' or a /dev/disk/by-* link to a whole-disk name."""
        name = spec
        if spec.startswith("/"):
            if self.live:
                real = os.path.realpath(spec)
                try:
                    st = os.stat(real)
                except OSError as exc:
                    raise LookupError(f"{spec}: {exc.strerror}") from exc
                if not stat.S_ISBLK(st.st_mode):
                    raise LookupError(f"{spec} is not a block device")
                devno = f"{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}"
                name = self.by_devno.get(devno, os.path.basename(real))
            else:
                name = os.path.basename(spec)
        if not (self.cls / name).exists():
            raise LookupError(f"no block device named {name}")
        if self.is_partition(name):
            raise LookupError(f"{name} is a partition; give the whole drive ({self.parent_disk(name)})")
        return name
