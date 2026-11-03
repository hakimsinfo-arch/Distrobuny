"""Installer tests: drive safety rules and writing onto (stand-in) drives.

Drive discovery runs against synthetic sysfs/procfs trees, so the refusal
rules are exercised for USB sticks, internal disks, LUKS/LVM stacks, swap,
card readers and 4Kn disks without touching real hardware.  Writing is
tested on regular files pre-filled with stale data (an old ISO-hybrid
signature, an old backup GPT) as a reused USB stick would be.
"""
from __future__ import annotations

import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.image import gpt  # noqa: E402
from distrobuny.image.exfat import ExfatBuilder  # noqa: E402
from distrobuny.image.fat32 import Fat32Builder  # noqa: E402
from distrobuny.image.layout import plan_geometry, plan_geometry_for_disk, volume_pieces, write_image  # noqa: E402
from distrobuny.image.pieces import Blob, Copy, Zero  # noqa: E402
from distrobuny.install import devices, writer  # noqa: E402

FSCK_EXFAT = ROOT / "build" / "host-tools" / "usr" / "sbin" / "fsck.exfat"
FSCK_FAT = shutil.which("fsck.fat", path="/usr/sbin:/sbin:" + os.environ.get("PATH", ""))
BUNYCTL = ROOT / "build" / "bin" / "bunyctl"
MiB = 1 << 20


# ------------------------------------------------------------ synthetic sysfs

class FakeSystem:
    def __init__(self, root: Path):
        self.root = root
        self.sys = root / "sys"
        self.proc = root / "proc"
        (self.sys / "class" / "block").mkdir(parents=True)
        (self.proc / "self").mkdir(parents=True)
        self.mountinfo = []
        self.swaps = []
        self.major = 8

    def _files(self, d: Path, values: dict) -> None:
        d.mkdir(parents=True, exist_ok=True)
        for k, v in values.items():
            (d / k).parent.mkdir(parents=True, exist_ok=True)
            (d / k).write_text(f"{v}\n")

    def disk(self, name: str, parent: str, size: int, removable=0, ro=0, lbs=512, dev_attrs=None, usb=None,
             parts=()) -> Path:
        """Create a disk below sys/devices/<parent>; parts is [(name, size)]."""
        devdir = self.sys / "devices" / parent
        if usb:
            usbdir = devdir.parent.parent  # .../usb2/2-1
            self._files(usbdir, {"idVendor": "0781", **usb})
        self._files(devdir, dev_attrs or {})
        blk = devdir / "block" / name
        minor = len(list((self.sys / "class" / "block").iterdir())) * 16
        self._files(blk, {"dev": f"{self.major}:{minor}", "size": size // 512, "removable": removable, "ro": ro,
                          "queue/logical_block_size": lbs})
        (blk / "holders").mkdir()
        (blk / "slaves").mkdir()
        os.symlink(os.path.relpath(devdir, blk), blk / "device")
        (self.sys / "class" / "block" / name).symlink_to(os.path.relpath(blk, self.sys / "class" / "block"))
        for i, (pname, psize) in enumerate(parts, 1):
            pd = blk / pname
            self._files(pd, {"dev": f"{self.major}:{minor + i}", "size": psize // 512, "partition": i})
            (pd / "holders").mkdir()
            (self.sys / "class" / "block" / pname).symlink_to(os.path.relpath(pd, self.sys / "class" / "block"))
        return blk

    def dm(self, name: str, devno: str, slaves: list[str]) -> None:
        d = self.sys / "devices" / "virtual" / "block" / name
        self._files(d, {"dev": devno, "size": 1000})
        (d / "slaves").mkdir()
        (d / "holders").mkdir()
        for s in slaves:
            (d / "slaves" / s).write_text("")
            for cand in (self.sys / "class" / "block").iterdir():
                if cand.name == s:
                    (cand.resolve() / "holders" / name).write_text("")
        (self.sys / "class" / "block" / name).symlink_to(os.path.relpath(d, self.sys / "class" / "block"))

    def mount(self, name: str, mp: str, fstype: str = "ext4", source: str | None = None) -> None:
        devno = (self.sys / "class" / "block" / name / "dev").read_text().strip()
        source = source or f"/dev/{name}"
        mp_esc = mp.replace(" ", "\\040")
        self.mountinfo.append(f"{len(self.mountinfo) + 20} 1 {devno} / {mp_esc} rw,relatime - {fstype} {source} rw")

    def system(self) -> devices.System:
        (self.proc / "self" / "mountinfo").write_text("\n".join(self.mountinfo) + "\n")
        (self.proc / "swaps").write_text("Filename\tType\tSize\tUsed\tPriority\n" +
                                         "".join(f"{s}\tpartition\t1000\t0\t-2\n" for s in self.swaps))
        return devices.System(self.sys, self.proc, Path("/dev"), live=False)


USB = "pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host6/target6:0:0/6:0:0:0"
USB2 = "pci0000:00/0000:00:14.0/usb2/2-2/2-2:1.0/host7/target7:0:0/7:0:0:0"
SATA = "pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0"
NVME = "pci0000:00/0000:00:1d.0/nvme/nvme0"


class DriveRulesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="buny-devices-test-"))
        self.f = FakeSystem(self.tmp)
        f = self.f
        f.disk("nvme0n1", NVME, 512 << 30, dev_attrs={"model": "Fast SSD", "serial": "N1"},
               parts=[("nvme0n1p1", 512 * MiB), ("nvme0n1p2", 500 << 30)])
        f.disk("sda", SATA, 1 << 40, dev_attrs={"vendor": "ATA", "model": "Data Disk"}, parts=[("sda1", 1 << 40)])
        f.disk("sdb", USB, 64 << 30, removable=1, dev_attrs={"vendor": "SanDisk", "model": "Ultra"},
               usb={"manufacturer": "SanDisk", "product": "Ultra USB 3.0", "serial": "4C530001"},
               parts=[("sdb1", 64 << 30)])
        f.disk("sdc", USB2, 32 << 30, removable=1, dev_attrs={"vendor": "Kingston", "model": "DataTraveler"},
               usb={"manufacturer": "Kingston", "product": "DT", "serial": "K2"}, parts=[("sdc1", 32 << 30)])
        f.mount("nvme0n1p2", "/")
        f.mount("nvme0n1p1", "/boot/efi", "vfat")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def drive(self, sysm, name, **kw):
        d = sysm.drive(name)
        sysm.assess([d], **kw)
        return d

    def test_unused_usb_stick_is_usable(self):
        d = self.drive(self.f.system(), "sdc")
        self.assertEqual(d.problems, [])
        self.assertEqual(d.transport, "usb")
        self.assertEqual(d.serial, "K2")
        self.assertEqual([p.name for p in d.partitions], ["sdc1"])

    def test_system_disk_is_refused_even_with_override(self):
        s = self.f.system()
        d = self.drive(s, "nvme0n1", allow_fixed=True)
        self.assertTrue(d.system)
        self.assertTrue(any("running system" in p for p in d.problems), d.problems)

    def test_mounted_stick_is_refused(self):
        self.f.mount("sdb1", "/media/user/MY STICK", "vfat")
        d = self.drive(self.f.system(), "sdb")
        self.assertTrue(any("/media/user/MY STICK" in p and "unmount" in p for p in d.problems), d.problems)

    def test_fixed_disk_needs_override(self):
        s = self.f.system()
        self.assertTrue(any("fixed SATA" in p for p in self.drive(s, "sda").problems))
        d = self.drive(s, "sda", allow_fixed=True)
        self.assertEqual(d.problems, [])
        self.assertTrue(d.warnings)

    def test_encrypted_home_on_usb_disk_counts_as_system(self):
        # /home is LUKS on a USB disk: dm-0 (mounted) -> sdb1 -> sdb
        self.f.dm("dm-0", "253:0", ["sdb1"])
        self.f.mount("dm-0", "/home", source="/dev/mapper/home")
        d = self.drive(self.f.system(), "sdb")
        self.assertTrue(d.system)
        self.assertIn("/home", d.mounts)

    def test_unmounted_lvm_member_is_refused(self):
        self.f.dm("dm-1", "253:1", ["sdc1"])
        d = self.drive(self.f.system(), "sdc")
        self.assertTrue(any("dm-1 (on sdc1)" in p for p in d.problems), d.problems)

    def test_swap_partition_counts_as_system(self):
        self.f.swaps.append("/dev/sdc1")
        d = self.drive(self.f.system(), "sdc")
        self.assertTrue(d.system)

    def test_card_reader_without_medium(self):
        self.f.disk("sdd", "pci0000:00/0000:00:14.0/usb2/2-3/2-3:1.0/host8/target8:0:0/8:0:0:0", 0, removable=1,
                    usb={"manufacturer": "Generic", "product": "Card Reader", "serial": "X"})
        d = self.drive(self.f.system(), "sdd")
        self.assertIn("no medium inserted", d.problems)

    def test_4kn_and_write_protected(self):
        self.f.disk("sde", "pci0000:00/0000:00:14.0/usb2/2-4/2-4:1.0/host9/target9:0:0/9:0:0:0", 8 << 40,
                    lbs=4096, ro=1, usb={"manufacturer": "WD", "product": "Elements", "serial": "W"})
        d = self.drive(self.f.system(), "sde")
        self.assertTrue(any("4096-byte sectors" in p for p in d.problems))
        self.assertIn("is write-protected", d.problems)
        self.assertTrue(any("unusually large" in w for w in d.warnings))

    def test_too_small(self):
        d = self.drive(self.f.system(), "sdc", need_bytes=100 << 30)
        self.assertTrue(any("too small" in p for p in d.problems))

    def test_resolve(self):
        s = self.f.system()
        self.assertEqual(s.resolve("/dev/sdc"), "sdc")
        with self.assertRaisesRegex(LookupError, "partition; give the whole drive .sdb."):
            s.resolve("sdb1")
        with self.assertRaises(LookupError):
            s.resolve("sdz")

    def test_listing_skips_virtual_devices(self):
        self.f.dm("dm-2", "253:2", [])
        names = [d.name for d in self.f.system().drives()]
        self.assertNotIn("dm-2", names)
        self.assertEqual(names, ["nvme0n1", "sda", "sdb", "sdc"])


# --------------------------------------------------------------- writing

FILES = {
    "Desktop/arch/archlinux.iso": os.urandom(3 * MiB + 5),
    "Servers/my server.iso": os.urandom(700 * 1024),
}


def builders(geo, ts=1767225600.0):
    data = ExfatBuilder(geo.data_sectors * 512, label="DISTROBUNY", serial=0x1234ABCD,
                        partition_offset_sectors=geo.data_start, timestamp=ts)
    for name, blob in FILES.items():
        data.add_file(name, data=blob, mtime=ts)
    esp = Fat32Builder(geo.esp_sectors * 512, label="BUNY-EFI", serial=0x55667788, hidden_sectors=geo.esp_start,
                       timestamp=ts)
    esp.add_file("EFI/BOOT/BOOTX64.EFI", data=b"MZ" + bytes(5000))
    return data, esp


def dirty_file(path: Path, size: int) -> None:
    """A 'used USB stick': random bytes plus an ISO9660 signature and an old GPT at the end."""
    with open(path, "wb") as f:
        f.truncate(size)
        rnd = random.Random(7)
        for off in (0, 32768, size // 2, size - 4 * MiB):
            f.seek(off)
            f.write(rnd.randbytes(4 * MiB))
        f.seek(0x8001)
        f.write(b"CD001")
        f.seek(size - 512)
        f.write(b"EFI PART")


def fsck_partitions(test: unittest.TestCase, img: Path, disk: gpt.Disk, tmp: Path) -> None:
    for p in disk.partitions:
        part = tmp / f"{p.name}.part"
        with open(img, "rb") as src, open(part, "wb") as dst:
            src.seek(p.offset)
            dst.write(src.read(p.size_bytes))
        if p.name == "DISTROBUNY" and FSCK_EXFAT.exists():
            r = subprocess.run([str(FSCK_EXFAT), "-n", str(part)], capture_output=True, text=True)
            test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        if p.name == "BUNY-EFI" and FSCK_FAT:
            r = subprocess.run([FSCK_FAT, "-n", str(part)], capture_output=True, text=True)
            test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        part.unlink()


class WriterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-install-test-"))
        cls.image = cls.tmp / "distrobuny.img"
        geo = plan_geometry(48 * MiB, 64 * MiB)
        data, esp = builders(geo)
        write_image(cls.image, geo, "install-test", esp, data)
        cls.geo = geo

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def target(self, name: str, size: int) -> writer.Target:
        path = self.tmp / name
        dirty_file(path, size)
        return writer.Target(str(path), os.open(path, os.O_RDWR), size, False)

    def test_paint_matches_a_simple_model(self):
        rnd = random.Random(1)
        size = 4096
        plan = writer.Plan(size)
        model = bytearray(size)
        for i in range(300):
            s = rnd.randrange(size - 1)
            e = rnd.randrange(s + 1, min(size, s + 600) + 1)
            blob = bytes(rnd.randrange(1, 256) for _ in range(e - s)) if i % 3 else bytes(e - s)
            plan.add(Blob(s, blob) if any(blob) else Zero(s, e - s))
            model[s:e] = blob
        out = bytearray(size)
        for s, e, src, base in plan.segs:
            out[s:e] = bytes(e - s) if src is None else src[base:base + e - s]
        covered = set()
        for s, e, _, _ in plan.segs:
            self.assertTrue(covered.isdisjoint(range(s, e)))
            covered.update(range(s, e))
        for i in covered:
            self.assertEqual(out[i], model[i], f"byte {i}")

    def test_image_onto_larger_dirty_drive(self):
        src = writer.SourceImage.open(self.image)
        size = src.size * 3 + 512 * 7  # odd size: backup GPT must land on the real last sector
        t = self.target("larger.img", size)
        plan, disk = writer.plan_from_image(src, size)
        writer.write_plan(t, plan)
        writer.verify_plan(t, plan)
        t.close()
        with open(t.path, "rb") as f:
            got = gpt.read_tables(f)
            f.seek(size - 512)
            self.assertEqual(f.read(8), b"EFI PART", "backup GPT header at the end of the drive")
            f.seek(0x8001)
            self.assertEqual(f.read(5), bytes(5), "stale ISO9660 signature cleared")
        self.assertEqual(got.size_bytes, size)
        orig = writer.SourceImage.open(self.image).disk
        self.assertEqual([(p.name, p.first_lba, p.last_lba) for p in got.partitions],
                         [(p.name, p.first_lba, p.last_lba) for p in orig.partitions])
        self.assertNotEqual(got.disk_guid, orig.disk_guid, "every drive gets fresh GUIDs")
        self.assertTrue(all(a.guid != b.guid for a, b in zip(got.partitions, orig.partitions)))
        # partition contents are byte-identical to the image
        with open(self.image, "rb") as a, open(t.path, "rb") as b:
            for p in orig.partitions:
                a.seek(p.offset)
                b.seek(p.offset)
                self.assertEqual(a.read(p.size_bytes), b.read(p.size_bytes), p.name)
        fsck_partitions(self, Path(t.path), got, self.tmp)

    def test_verification_detects_bad_media(self):
        src = writer.SourceImage.open(self.image)
        t = self.target("bad.img", src.size)
        plan, _ = writer.plan_from_image(src, src.size)
        writer.write_plan(t, plan)
        # a drive that silently drops writes (fake capacity) returns old data
        s, e, _, _ = max((x for x in plan.segs if isinstance(x[2], Path)), key=lambda x: x[1] - x[0])
        os.pwrite(t.fd, b"\x00\xff" * 8, (s + e) // 2)
        with self.assertRaisesRegex(writer.InstallError, "verification failed"):
            writer.verify_plan(t, plan)
        t.close()

    def test_refusals(self):
        src = writer.SourceImage.open(self.image)
        with self.assertRaisesRegex(writer.InstallError, "too small"):
            writer.plan_from_image(src, src.size - MiB)
        junk = self.tmp / "junk.img"
        junk.write_bytes(os.urandom(MiB))
        with self.assertRaisesRegex(writer.InstallError, "cannot read|not a Distro Buny"):
            writer.SourceImage.open(junk)
        with self.assertRaisesRegex(writer.InstallError, "already exists"):
            writer.Target.open_file(self.image, src.size)

    def test_create_fills_the_drive(self):
        size = 300 * MiB + 512 * 3
        geo = plan_geometry_for_disk(size, 64 * MiB)
        self.assertEqual(geo.total_bytes, size)
        self.assertGreater(geo.data_sectors * 512, 230 * MiB)
        data, esp = builders(geo)
        disk, pieces, layout = volume_pieces(geo, "create-test", esp, data)
        t = self.target("created.img", size)
        plan = writer.plan_from_pieces(size, disk.partitions, pieces)
        # the partition table is written after everything else
        last = plan.ordered()[-1]
        self.assertTrue(last[0] >= size - 34 * 512 or last[0] < 34 * 512)
        writer.write_plan(t, plan)
        writer.verify_plan(t, plan)
        t.close()
        with open(t.path, "rb") as f:
            got = gpt.read_tables(f)
            for name, blob in FILES.items():
                off, n = layout["/" + name]
                f.seek(geo.data_start * 512 + off)
                self.assertEqual(f.read(n), blob, name)
        tail = size // 512 - (max(p.last_lba for p in got.partitions) + 1)
        self.assertTrue(2048 <= tail < 4096, f"{tail} sectors left after the last partition")
        fsck_partitions(self, Path(t.path), got, self.tmp)
        if BUNYCTL.exists():
            r = subprocess.run([str(BUNYCTL), "map", "--image", t.path, "--iso-path", "/Servers/my server.iso"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(str(len(FILES["Servers/my server.iso"])), r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
