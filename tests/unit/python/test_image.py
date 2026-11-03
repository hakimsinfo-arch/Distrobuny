"""Image writer tests: GPT, FAT32 (ESP) and exFAT (data partition).

Generated volumes are checked with independent implementations: dosfstools'
fsck.fat, exfatprogs' fsck.exfat (fetched into build/host-tools by the build
tools) and Distro Buny's own C exFAT reader (bunyctl map), plus a
reproducibility check.

ExpandTest covers growing DISTROBUNY when an image was written to a bigger
stick (source/filesystem/expand.c, through bunyctl expand on image files):
the result is checked with fsck.exfat and fsck.fat, both GPT copies, the
files' bytes and the untouched ESP, including after a simulated power cut
at every single write.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.image import gpt  # noqa: E402
from distrobuny.image.exfat import ExfatBuilder, entry_set_checksum, name_hash, upcase_table, utf16_units  # noqa: E402
from distrobuny.image.fat32 import Fat32Builder  # noqa: E402
from distrobuny.image.layout import Geometry, plan_geometry, write_image  # noqa: E402

FSCK_EXFAT = ROOT / "build" / "host-tools" / "usr" / "sbin" / "fsck.exfat"
FSCK_FAT = shutil.which("fsck.fat", path="/usr/sbin:/sbin:" + os.environ.get("PATH", ""))
BUNYCTL = ROOT / "build" / "bin" / "bunyctl"

FILES = {
    "Desktop/ubuntu/ubuntu-26.04.1-desktop-amd64.iso": os.urandom(5 * 1024 * 1024 + 17),
    "Desktop/Not friendly/arch with spaces.iso": os.urandom(300 * 1024),
    "Desktop/Ünïcødé Ordner/Größe.iso": os.urandom(70 * 1024),
    "Servers/empty.iso": b"",
    ".distrobuny/index.json": b"{}\n",
}
ESP_BLOB = os.urandom(100000)


def build(path: Path, ts: float = 1767225600.0) -> dict:
    geo = plan_geometry(64 << 20, 256 << 20)
    data = ExfatBuilder(geo.data_sectors * 512, label="DISTROBUNY", serial=0x11223344,
                        partition_offset_sectors=geo.data_start, timestamp=ts)
    for name, blob in FILES.items():
        data.add_file(name, data=blob, mtime=ts)
    data.add_dir("Servers/Empty Folder", mtime=ts)
    esp = Fat32Builder(geo.esp_sectors * 512, label="BUNY-EFI", serial=0x55667788, hidden_sectors=geo.esp_start,
                       timestamp=ts)
    esp.add_file("EFI/BOOT/BOOTX64.EFI", data=b"MZ" + bytes(4000))
    esp.add_file("EFI/BOOT/grub.cfg", data=b"set timeout=0\n")
    esp.add_file("distrobuny/a-long-file-name.img", data=ESP_BLOB)
    info = write_image(path, geo, "unit-test", esp, data)
    return {"geo": geo, "info": info}


def extract(img: Path, first_lba: int, sectors: int, out: Path) -> Path:
    with open(img, "rb") as src, open(out, "wb") as dst:
        src.seek(first_lba * 512)
        left = sectors * 512
        while left:
            b = src.read(min(left, 4 << 20))
            dst.write(b)
            left -= len(b)
    return out


class ImageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-image-test-"))
        cls.img = cls.tmp / "test.img"
        cls.built = build(cls.img)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_gpt_roundtrip(self):
        with open(self.img, "rb") as f:
            disk = gpt.read_tables(f)
        names = [p.name for p in disk.partitions]
        self.assertEqual(names, ["DISTROBUNY", "BUNY-EFI"])
        geo = self.built["geo"]
        self.assertEqual(disk.partitions[0].first_lba, geo.data_start)
        self.assertEqual(disk.partitions[1].type_guid, gpt.TYPE_ESP)
        for p in disk.partitions:
            self.assertEqual(p.first_lba % 2048, 0, "partitions must be 1 MiB aligned")

    def test_esp_passes_fsck_fat(self):
        if not FSCK_FAT:
            self.skipTest("fsck.fat not installed")
        geo = self.built["geo"]
        part = extract(self.img, geo.esp_start, geo.esp_sectors, self.tmp / "esp.part")
        r = subprocess.run([FSCK_FAT, "-n", "-v", str(part)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_data_passes_fsck_exfat(self):
        if not FSCK_EXFAT.exists():
            self.skipTest("fsck.exfat not available (run tools/build/buny-build doctor)")
        geo = self.built["geo"]
        part = extract(self.img, geo.data_start, geo.data_sectors, self.tmp / "data.part")
        r = subprocess.run([str(FSCK_EXFAT), "-n", "-v", str(part)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("clean", r.stdout)

    def test_c_reader_agrees_with_writer(self):
        if not BUNYCTL.exists():
            self.skipTest("bunyctl not built")
        files = self.built["info"]["files"]
        data_off = self.built["geo"].data_start * 512
        with open(self.img, "rb") as f:
            for name, blob in FILES.items():
                if not blob:
                    continue
                out = subprocess.run([str(BUNYCTL), "map", "--image", str(self.img), "--iso-path", "/" + name],
                                     capture_output=True, text=True)
                self.assertEqual(out.returncode, 0, out.stderr)
                lines = out.stdout.split("\n")
                self.assertEqual(int(lines[0].split()[1]), len(blob))
                extents = [tuple(map(int, l.split()[1:])) for l in lines if l.startswith("extent")]
                self.assertEqual(len(extents), 1, "files must be contiguous")
                self.assertEqual(extents[0][0], files["/" + name]["offset"])
                f.seek(data_off + extents[0][0])
                self.assertEqual(f.read(len(blob)), blob, f"content of {name}")

    def test_reproducible(self):
        a, b = self.tmp / "r1.img", self.tmp / "r2.img"
        build(a)
        build(b)
        # file contents in FILES are random per process but identical between the two builds
        ha = hashlib.sha256(a.read_bytes()).hexdigest()
        hb = hashlib.sha256(b.read_bytes()).hexdigest()
        self.assertEqual(ha, hb)

    def test_exfat_spec_primitives(self):
        raw, mapping = upcase_table()
        self.assertEqual(len(raw), 5836)
        self.assertEqual(mapping[ord("a")], ord("A"))
        self.assertEqual(mapping[0x00E9], 0x00C9)  # é -> É
        self.assertEqual(name_hash(utf16_units("file.iso")), name_hash(utf16_units("FILE.ISO")))
        es = bytearray(64)
        es[0] = 0x85
        es[2:4] = b"\xff\xff"  # the checksum field itself is excluded
        self.assertEqual(entry_set_checksum(bytes(es)), entry_set_checksum(bytes(es[:2]) + b"\x00\x00" + bytes(es[4:])))

    def test_data_partition_is_last_but_first_in_the_table(self):
        geo = self.built["geo"]
        self.assertGreater(geo.data_start, geo.esp_start + geo.esp_sectors - 1)
        self.assertEqual((geo.data_start + geo.data_sectors) * 512, geo.total_bytes - (1 << 20))


# ----------------------------------------------------------- growing DISTROBUNY

GROW_FILES = {
    "Desktop/ubuntu/ubuntu-26.04.1-desktop-amd64.iso": os.urandom(3 * 1024 * 1024 + 5),
    "Servers/debian.iso": os.urandom(700 * 1024),
    "README.txt": b"Distro Buny\n",
}


def build_growable(path: Path, cluster_size: int = 4096, grow_limit: int | None = 1 << 30, old_layout: bool = False,
                   data_bytes: int = 64 << 20) -> tuple[Geometry, dict]:
    """A small image laid out like the starter image (or like early 1.0.0 builds, data partition first)."""
    if old_layout:
        esp_start = 2048 + data_bytes // 512
        geo = Geometry(2048 * 512 + data_bytes + (64 << 20) + (1 << 20), 2048, data_bytes // 512, esp_start, (64 << 20) // 512)
    else:
        geo = plan_geometry(data_bytes, 64 << 20)
    data = ExfatBuilder(geo.data_sectors * 512, label="DISTROBUNY", serial=0x0453FFB8, cluster_size=cluster_size,
                        partition_offset_sectors=geo.data_start, timestamp=1767225600.0, grow_limit=grow_limit)
    for name, blob in GROW_FILES.items():
        data.add_file(name, data=blob, mtime=1767225600.0)
    esp = Fat32Builder(geo.esp_sectors * 512, label="BUNY-EFI", serial=0x55667788, hidden_sectors=geo.esp_start,
                       timestamp=1767225600.0)
    esp.add_file("EFI/BOOT/BOOTX64.EFI", data=b"MZ" + bytes(4000))
    esp.add_file("distrobuny/blob.bin", data=ESP_BLOB)
    return geo, write_image(path, geo, "grow-test", esp, data)


def sparse_extract(img: Path, first_lba: int, sectors: int, out: Path) -> Path:
    """Copy a partition out of a sparse image, skipping holes (images of 200 GiB)."""
    start, end = first_lba * 512, (first_lba + sectors) * 512
    # unbuffered: SEEK_DATA/SEEK_HOLE move the descriptor's offset behind a
    # buffered reader's back, which then reads the wrong bytes (seen with small
    # holes on tmpfs, Ubuntu 26.04's /tmp)
    with open(img, "rb", buffering=0) as src, open(out, "wb") as dst:
        dst.truncate(end - start)
        pos = start
        while pos < end:
            try:
                data = os.lseek(src.fileno(), pos, os.SEEK_DATA)
            except OSError:
                break
            if data >= end:
                break
            hole = min(os.lseek(src.fileno(), data, os.SEEK_HOLE), end)
            src.seek(data)
            dst.seek(data - start)
            left = hole - data
            while left:
                b = src.read(min(left, 4 << 20))
                dst.write(b)
                left -= len(b)
            pos = hole
    return out


def backup_gpt(img: Path) -> tuple[bytes, bytes]:
    """The backup header (checked: at the last LBA, CRCs) and its entry array."""
    size = img.stat().st_size
    last = size // 512 - 1
    with open(img, "rb") as f:
        f.seek(last * 512)
        hdr = f.read(512)
        if hdr[:8] != b"EFI PART":
            raise AssertionError("no backup GPT header at the end of the disk")
        hsize, crc = struct.unpack_from("<II", hdr, 12)
        my_lba, alt_lba, _, last_usable = struct.unpack_from("<QQQQ", hdr, 24)
        check = bytearray(hdr[:hsize])
        struct.pack_into("<I", check, 16, 0)
        if zlib.crc32(bytes(check)) & 0xFFFFFFFF != crc:
            raise AssertionError("backup GPT header CRC mismatch")
        entries_lba, n, esize, ecrc = struct.unpack_from("<QIII", hdr, 72)
        if my_lba != last or alt_lba != 1 or last_usable != last - 33 or entries_lba != last - 32:
            raise AssertionError(f"backup GPT header fields: my {my_lba} alt {alt_lba} last usable {last_usable}")
        f.seek(entries_lba * 512)
        entries = f.read(n * esize)
        if zlib.crc32(entries) & 0xFFFFFFFF != ecrc:
            raise AssertionError("backup GPT entry array CRC mismatch")
    return hdr, entries


def region_digest(img: Path, offset: int, length: int) -> str:
    h = hashlib.sha256()
    with open(img, "rb") as f:
        f.seek(offset)
        while length:
            b = f.read(min(length, 4 << 20))
            h.update(b)
            length -= len(b)
    return h.hexdigest()


@unittest.skipUnless(BUNYCTL.exists(), "bunyctl not built")
class ExpandTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-expand-test-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def expand(self, img: Path, *args: str, rc: int | None = 0) -> dict:
        r = subprocess.run([str(BUNYCTL), "expand", "--image", str(img), *args], capture_output=True, text=True)
        if rc is not None:
            self.assertEqual(r.returncode, rc, r.stdout + r.stderr)
        out = {"rc": r.returncode, "stderr": r.stderr}
        for line in r.stdout.splitlines():
            key, _, rest = line.partition(" ")
            if key == "reason":
                out["reason"] = rest
                continue
            fields = rest.split()
            if key == "state":
                out["state"] = fields[0]
                fields = fields[1:]
            else:
                out[key] = True
            for kv in fields:
                k, _, v = kv.partition("=")
                out[k] = v
        return out

    def assert_healthy(self, img: Path, geo: Geometry, esp_digest: str) -> gpt.Partition:
        """Both GPT copies, fsck of both file systems, the files and the ESP."""
        with open(img, "rb") as f:
            disk = gpt.read_tables(f)
        self.assertEqual(disk.size_bytes, img.stat().st_size, "the primary GPT must point at a backup at the end")
        _, entries = backup_gpt(img)
        with open(img, "rb") as f:
            f.seek(2 * 512)
            self.assertEqual(f.read(len(entries)), entries, "both GPT copies must list the same partitions")
        parts = {p.name: p for p in disk.partitions}
        data, esp = parts["DISTROBUNY"], parts["BUNY-EFI"]
        self.assertEqual([p.name for p in disk.partitions][0], "DISTROBUNY")
        self.assertEqual((esp.first_lba, esp.last_lba), (geo.esp_start, geo.esp_start + geo.esp_sectors - 1))
        self.assertEqual(region_digest(img, esp.offset, esp.size_bytes), esp_digest, "the ESP must not change")
        if FSCK_EXFAT.exists():
            part = sparse_extract(img, data.first_lba, data.last_lba - data.first_lba + 1, self.tmp / "data.part")
            r = subprocess.run([str(FSCK_EXFAT), "-n", "-v", str(part)], capture_output=True, text=True)
            part.unlink()
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("clean", r.stdout)
        if FSCK_FAT:
            part = sparse_extract(img, esp.first_lba, esp.last_lba - esp.first_lba + 1, self.tmp / "esp.part")
            r = subprocess.run([FSCK_FAT, "-n", str(part)], capture_output=True, text=True)
            part.unlink()
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for name, blob in GROW_FILES.items():
            out = subprocess.run([str(BUNYCTL), "map", "--image", str(img), "--iso-path", "/" + name],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            off, n = map(int, [l for l in out.stdout.split("\n") if l.startswith("extent")][0].split()[1:])
            with open(img, "rb") as f:
                f.seek(data.offset + off)
                self.assertEqual(f.read(n), blob, name)
        return data

    def test_grows_to_the_end_of_a_bigger_stick(self):
        img = self.tmp / "grow.img"
        geo, _ = build_growable(img)
        esp_digest = region_digest(img, geo.esp_start * 512, geo.esp_sectors * 512)
        self.assertEqual(self.expand(img, "--check")["state"], "none", "an image on a stick of its own size")
        os.truncate(img, 320 << 20)  # the image at the start of a bigger stick
        plan = self.expand(img, "--check")
        self.assertEqual(plan["state"], "possible")
        self.assertEqual(int(plan["data"]), geo.data_sectors * 512)
        done = self.expand(img)
        self.assertIn("expanded", done)
        data = self.assert_healthy(img, geo, esp_digest)
        self.assertEqual(data.first_lba, geo.data_start)
        self.assertEqual((data.last_lba + 1) % 2048, 0, "the partition ends on a 1 MiB boundary")
        self.assertGreater(data.last_lba, (320 << 20) // 512 - 34 - 2048, "the partition reaches the end")
        self.assertEqual(int(done["data"]), data.size_bytes, "the file system fills the partition")
        self.assertEqual(int(plan["bitmap"].split("+")[1]), 1, "the allocation bitmap grew by one cluster")
        self.assertEqual(self.expand(img, "--check")["state"], "none", "nothing left to do")

    def test_power_cut_at_every_write(self):
        pristine = self.tmp / "pristine.img"
        geo, info = build_growable(pristine)
        os.truncate(pristine, 320 << 20)
        esp_digest = region_digest(pristine, geo.esp_start * 512, geo.esp_sectors * 512)
        reference = self.tmp / "reference.img"
        shutil.copyfile(pristine, reference)
        writes = int(self.expand(reference)["writes"])
        self.assertGreater(writes, 8)
        want = region_digest(reference, 0, reference.stat().st_size)
        files = {name: hashlib.sha256(blob).hexdigest() for name, blob in GROW_FILES.items()}
        layout = {name: (info["files"]["/" + name]["offset"], len(GROW_FILES[name])) for name in GROW_FILES}
        for n in range(1, writes):
            with self.subTest(power_cut_after=n):
                img = self.tmp / "cut.img"
                shutil.copyfile(pristine, img)
                self.assertEqual(self.expand(img, "--stop-after", str(n), rc=3)["state"], "possible")
                # the drive still starts and no file changed
                self.assertEqual(region_digest(img, geo.esp_start * 512, geo.esp_sectors * 512), esp_digest)
                for name, (off, size) in layout.items():
                    self.assertEqual(region_digest(img, geo.data_start * 512 + off, size), files[name], name)
                state = self.expand(img, "--check")["state"]
                self.assertIn(state, ("possible", "resume"), f"after {n} writes")
                self.expand(img)
                self.assertEqual(region_digest(img, 0, img.stat().st_size), want,
                                 f"finishing after a power cut at write {n} must give the uninterrupted result")
        self.assert_healthy(reference, geo, esp_digest)

    def test_torn_boot_region_is_finished_from_the_backup(self):
        pristine = self.tmp / "torn-src.img"
        geo, _ = build_growable(pristine)
        os.truncate(pristine, 320 << 20)
        esp_digest = region_digest(pristine, geo.esp_start * 512, geo.esp_sectors * 512)
        done, cut = self.tmp / "torn-done.img", self.tmp / "torn.img"
        shutil.copyfile(pristine, done)
        writes = int(self.expand(done)["writes"])
        shutil.copyfile(pristine, cut)
        self.expand(cut, "--stop-after", str(writes - 1), rc=3)  # everything but the main boot region
        base = geo.data_start * 512
        with open(done, "rb") as f:
            f.seek(base)
            new_main = f.read(12 * 512)
        with open(cut, "r+b") as f:  # half of the 6 KiB write reached the stick
            f.seek(base)
            f.write(new_main[:3 * 512])
        self.assertEqual(self.expand(cut, "--check")["state"], "resume")
        self.expand(cut)
        self.assertEqual(region_digest(cut, 0, cut.stat().st_size), region_digest(done, 0, done.stat().st_size))
        self.assert_healthy(cut, geo, esp_digest)

    def test_firmware_restored_the_primary_gpt(self):
        """UEFI firmware (EDK2) rebuilds a torn primary GPT from the backup before Distro Buny starts."""
        img = self.tmp / "fw.img"
        geo, _ = build_growable(img)
        os.truncate(img, 320 << 20)
        esp_digest = region_digest(img, geo.esp_start * 512, geo.esp_sectors * 512)
        self.expand(img, "--stop-after", "3", rc=3)  # backup copy and primary entries written, primary header not
        with open(img, "rb") as f:
            with self.assertRaises(ValueError):
                gpt.read_tables(f)
        hdr, entries = backup_gpt(img)
        last = img.stat().st_size // 512 - 1
        prim = bytearray(hdr)
        struct.pack_into("<QQ", prim, 24, 1, last)   # MyLBA, AlternateLBA
        struct.pack_into("<Q", prim, 72, 2)          # PartitionEntryLBA
        struct.pack_into("<I", prim, 16, 0)
        struct.pack_into("<I", prim, 16, zlib.crc32(bytes(prim[:struct.unpack_from("<I", prim, 12)[0]])) & 0xFFFFFFFF)
        with open(img, "r+b") as f:
            f.seek(512)
            f.write(bytes(prim))
            f.write(entries)
        self.assertEqual(self.expand(img, "--check")["state"], "resume", "an enlarged partition is an interrupted expansion")
        self.expand(img)
        self.assert_healthy(img, geo, esp_digest)

    def test_real_cluster_size_on_a_200_gib_stick(self):
        """128 KiB clusters, FAT room for 2 TiB: the bitmap outgrows its first cluster."""
        img = self.tmp / "big.img"
        geo, _ = build_growable(img, cluster_size=128 << 10, grow_limit=2 << 40, data_bytes=256 << 20)
        esp_digest = region_digest(img, geo.esp_start * 512, geo.esp_sectors * 512)
        os.truncate(img, 200 << 30)
        plan = self.expand(img, "--check")
        self.assertEqual(plan["state"], "possible")
        self.assertEqual(plan["bitmap"].split("+")[1], "1")
        self.expand(img)
        data = self.assert_healthy(img, geo, esp_digest)
        self.assertGreater(data.size_bytes, (200 << 30) - (300 << 20))

    def test_volume_that_was_not_made_to_grow(self):
        """A plain volume (bitmap in front) grows only as far as its bitmap's cluster reaches."""
        img = self.tmp / "plain.img"
        geo, _ = build_growable(img, grow_limit=None)
        esp_digest = region_digest(img, geo.esp_start * 512, geo.esp_sectors * 512)
        os.truncate(img, 320 << 20)
        plan = self.expand(img, "--check")
        self.assertEqual((plan["state"], plan["limited"]), ("possible", "1"))
        self.expand(img)
        data = self.assert_healthy(img, geo, esp_digest)
        self.assertLessEqual(data.size_bytes, 8 * 4096 * 4096 + (64 << 20))

    def refused(self, img: Path, code: str) -> None:
        before = region_digest(img, 0, img.stat().st_size)
        plan = self.expand(img)
        self.assertEqual((plan["state"], plan["code"]), ("refused", code), plan)
        self.assertTrue(plan.get("reason"))
        self.assertEqual(region_digest(img, 0, img.stat().st_size), before, "a refusal writes nothing")

    def test_refusals(self):
        img = self.tmp / "old.img"
        build_growable(img, old_layout=True)
        os.truncate(img, 320 << 20)
        self.refused(img, "layout")

        img = self.tmp / "dirty.img"
        geo, _ = build_growable(img)
        with open(img, "r+b") as f:  # VolumeFlags.VolumeDirty, as Windows leaves it when unplugged
            f.seek(geo.data_start * 512 + 106)
            f.write(b"\x02\x00")
        os.truncate(img, 320 << 20)
        self.refused(img, "dirty")

        img = self.tmp / "reformatted.img"
        geo, _ = build_growable(img)
        with open(img, "r+b") as f:
            f.seek(geo.data_start * 512)
            f.write(bytes(24 * 512))
        os.truncate(img, 320 << 20)
        self.refused(img, "not-exfat")

        r = subprocess.run([str(BUNYCTL), "expand", "--image", "/dev/null"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2, "bunyctl only expands image files, never devices")


if __name__ == "__main__":
    unittest.main(verbosity=2)
