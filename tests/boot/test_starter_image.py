"""Boot test: the starter image (the public download) in a virtual machine.

The starter image holds no ISO files.  Users write it to a USB stick with
balenaEtcher, Rufus or dd, which leaves the 3.5 GiB image at the start of a
bigger stick, then copy their own ISO files onto the DISTROBUNY partition.
Both situations are checked through the complete UEFI boot chain:

  * the empty image, attached as a 16 GiB USB stick: the runtime finds its
    data partition and the menu explains how to add images, on Home and in
    an empty folder;
  * the same layout with an ISO added to Desktop after the build, so the
    catalogue index on the drive (written for an empty drive) is out of
    date, as it is after a user copies files: the image is listed and the
    privileged runtime prepares its hand-off (dry run, no kexec);
  * growing DISTROBUNY to the end of the 16 GiB stick from the menu (the
    banner, an explicit confirmation), then a restart: the kernel no longer
    complains about the partition table and the menu offers nothing more;
  * a power cut in the middle of that (buny.expandcrash=N stops after N
    writes and resets the machine): the next start finishes the expansion
    before the menu appears;
  * a drive with the early 1.0.0 layout (ESP behind DISTROBUNY) and the current
    runtime: the menu explains why it cannot grow and nothing is written.

After every expansion the VM's disk is checked with fsck.exfat -n and
fsck.fat -n, and both GPT copies are validated.

The ISO is added by Distro Buny's own exFAT writer, not by Windows or macOS;
fragmented files, as other systems may leave them, are covered by
test_fragmented_boot.py.  Screenshots go to build/test-artifacts/starter/.

Image: $BUNY_STARTER_IMAGE or output/images/distrobuny-<VERSION>-starter.img.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / "VERSION").read_text().strip()
sys.path.insert(0, str(ROOT / "source" / "python"))
sys.path.insert(0, str(ROOT / "tests" / "boot"))

sys.path.insert(0, str(ROOT / "tests" / "unit" / "python"))

from distrobuny.build import image_build, pipeline  # noqa: E402
from distrobuny.image import gpt  # noqa: E402
from distrobuny.image.exfat import ExfatBuilder  # noqa: E402
from distrobuny.image.layout import Geometry, write_image  # noqa: E402
from distrobuny.qemu.vm import VM, VMConfig  # noqa: E402
from test_image import FSCK_EXFAT, FSCK_FAT, backup_gpt, sparse_extract  # noqa: E402
from test_vm_menu import ACT_BOOT, Menu  # noqa: E402

IMAGE = Path(os.environ.get("BUNY_STARTER_IMAGE", ROOT / "output" / "images" / f"distrobuny-{VERSION}-starter.img"))
ISO = ROOT / "Iso layout" / "Desktop" / "Not friendly" / "void-live-x86_64-20250202-base.iso"
WORK = ROOT / "build" / "vm" / "starter-test"
SHOTS = ROOT / "build" / "test-artifacts" / "starter"
STICK = 16 << 30
ACT_EXPAND = 21


def manifest() -> dict:
    return json.loads((ROOT / "output" / "manifests" / f"{IMAGE.name}.json").read_text())


class StarterMenu(Menu):
    def shot(self, name: str) -> None:
        self.vm.screenshot(SHOTS / f"{name}.png")


class StarterImageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not IMAGE.exists():
            raise unittest.SkipTest(f"{IMAGE} not built (tools/build/buny-build image --profile starter)")
        shutil.rmtree(WORK, ignore_errors=True)
        WORK.mkdir(parents=True)
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(WORK, ignore_errors=True)

    def boot(self, image: Path, name: str, disk_bytes: int | None = None, lab: str = "", keep_disk: bool = False) -> VM:
        cfg = VMConfig(image=image, workdir=WORK / name, lab_options=f"buny.lab=1 buny.dryrun=1 {lab}".strip(), name=name,
                       disk_bytes=disk_bytes, keep_disk=keep_disk)
        return VM(cfg)

    def ready(self, vm: VM, since_start: bool = False) -> StarterMenu:
        ready = vm.wait_for(r"BUNY-EVENT ui-ready isos=(\d+) display=\w+ (\d+)x(\d+)", 180, since_start=since_start)
        self.assertIsNotNone(ready, "the interface did not start:\n" + vm.serial()[-3000:])
        menu = StarterMenu(vm, (int(ready.group(2)), int(ready.group(3))))
        menu.items = menu.read_items(ready.start())
        return menu

    def confirm_expansion(self, vm: VM, menu: StarterMenu, shot: str | None = None) -> None:
        menu.click(action=ACT_EXPAND)
        self.assertIsNotNone(vm.wait_for(r'BUNY-EVENT ui dialog=\d+ title="Use the whole drive\?"', 15), vm.serial()[-2000:])
        if shot:
            time.sleep(1)  # let the dialog's animation finish
            menu.shot(shot)
        self.assertNotIn("expand-request", vm.serial(), "nothing happens before the user chooses Expand")
        vm.key("down")  # from Cancel (the initial focus) to "Expand DISTROBUNY"
        vm.key("ret")
        self.assertIsNotNone(vm.wait_for(r"BUNY-EVENT expand-request", 15), vm.serial()[-2000:])

    def check_disk(self, vm: VM, name: str) -> dict:
        """What the VM left on its stick: both GPT copies, fsck of both file systems."""
        raw = vm.export_disk(WORK / f"{name}.raw")
        vm.discard_disk()
        try:
            self.assertEqual(raw.stat().st_size, STICK)
            with open(raw, "rb") as f:
                disk = gpt.read_tables(f)
            self.assertEqual(disk.size_bytes, STICK, "the primary GPT must point at a backup at the end of the stick")
            backup_gpt(raw)
            parts = {p.name: p for p in disk.partitions}
            for p in disk.partitions:
                part = sparse_extract(raw, p.first_lba, p.last_lba - p.first_lba + 1, WORK / f"{name}-{p.name}.part")
                tool = [str(FSCK_EXFAT), "-n", "-v"] if p.name == "DISTROBUNY" else [FSCK_FAT, "-n"]
                r = subprocess.run(tool + [str(part)], capture_output=True, text=True)
                part.unlink()
                self.assertEqual(r.returncode, 0, f"{p.name}: " + r.stdout + r.stderr)
            return parts
        finally:
            raw.unlink(missing_ok=True)

    def test_empty_drive_on_a_bigger_stick(self):
        m = manifest()
        self.assertEqual(m["profile"], "starter")
        self.assertFalse([f for f in m["layout"]["files"] if f.lower().endswith(".iso")])
        with self.boot(IMAGE, "empty-16g", disk_bytes=16 << 30) as vm:
            ready = vm.wait_for(r"BUNY-EVENT ui-ready isos=(\d+) display=\w+ (\d+)x(\d+)", 120, since_start=True)
            self.assertIsNotNone(ready, "the interface did not start:\n" + vm.serial()[-3000:])
            self.assertEqual(ready.group(1), "0")
            # the interface's next events reach the serial port a moment after ui-ready
            for event in ('BUNY-EVENT ui hint="No ISO images on this drive yet"', "BUNY-EVENT ui expand offer=possible"):
                self.assertIsNotNone(vm.wait_for(re.escape(event), 15, since_start=True), event)
            log = vm.serial()
            self.assertRegex(log, rf"BUNY-EVENT data-partition device=\S+ fs=exfat uuid={m['data_fs']['uuid']}")
            self.assertIn('BUNY-EVENT ui hint="No ISO images on this drive yet"', log)
            # the stick is bigger than the image: the kernel notices, and the menu offers to grow DISTROBUNY
            self.assertIn("BUNY-EVENT kernel-gpt", log)
            self.assertRegex(log, r"BUNY-EVENT expand-plan state=possible code=possible disk=%d " % STICK)
            self.assertIn("BUNY-EVENT ui expand offer=possible", log)
            size = (int(ready.group(2)), int(ready.group(3)))
            menu = StarterMenu(vm, size)
            menu.items = menu.read_items(ready.start())
            vm.wait_for(r"BUNY-EVENT ui focus", 10)
            menu.shot("1-empty-home")
            menu.click(path="/Servers")
            menu.wait_screen("folder", "/Servers")
            self.assertIn('BUNY-EVENT ui hint="No ISO images in this folder yet"', vm.serial())
            menu.shot("2-empty-folder")

    def test_iso_copied_onto_the_drive_appears(self):
        if not ISO.exists():
            self.skipTest(f"{ISO} not available")
        # the starter layout and its (now stale) empty index, plus one ISO in Desktop
        build_id = manifest()["build_id"]
        collection = WORK / "collection"
        (collection / "Desktop").mkdir(parents=True)
        (collection / "Servers").mkdir()
        (collection / "Desktop" / ISO.name).symlink_to(ISO)
        data_files = pipeline.volume_files(pipeline.empty_collection(), build_id, "starter", None)
        image = WORK / "with-iso.img"
        image_build.build_image(collection, image, build_id, esp_tree=pipeline.ESP_TREE, data_files=data_files,
                                timestamp=1792713600, disk_bytes=int(pipeline.STARTER_GIB * (1 << 30)), growable=True)
        iso = f"/Desktop/{ISO.name}"
        with self.boot(image, "with-iso") as vm:
            ready = vm.wait_for(r"BUNY-EVENT ui-ready isos=(\d+) display=\w+ (\d+)x(\d+)", 120, since_start=True)
            self.assertIsNotNone(ready, "the interface did not start:\n" + vm.serial()[-3000:])
            self.assertEqual(ready.group(1), "1")
            self.assertNotIn("BUNY-EVENT ui hint=", vm.serial())
            menu = StarterMenu(vm, (int(ready.group(2)), int(ready.group(3))))
            menu.items = menu.read_items(ready.start())
            menu.click(path="/Desktop")
            menu.wait_screen("folder", "/Desktop")
            self.assertTrue(any(i["path"] == iso for i in menu.items), menu.items)
            menu.shot("3-copied-iso")
            menu.click(path=iso)
            menu.wait_screen("detail", iso)
            menu.click(action=ACT_BOOT)
            loaded = vm.wait_for(r'BUNY-EVENT boot-loaded dry_run=1 cmdline="([^"]*)"', 120)
            self.assertIsNotNone(loaded, "the runtime did not prepare the hand-off:\n" + vm.serial()[-3000:])
            self.assertRegex(vm.serial(), re.escape(f'boot-start iso="{iso}"'))

    def test_expanding_the_drive_from_the_menu(self):
        with self.boot(IMAGE, "expand", disk_bytes=STICK, keep_disk=True) as vm:
            menu = self.ready(vm, since_start=True)
            m = re.search(r"BUNY-EVENT expand-plan state=possible \S+ disk=\d+ data=(\d+) new=(\d+)", vm.serial())
            self.assertIsNotNone(m, vm.serial()[-2000:])
            data, new = int(m.group(1)), int(m.group(2))
            self.assertEqual(data, manifest()["geometry"]["data_partition"]["sectors"] * 512)
            menu.shot("4-expand-offer")
            self.confirm_expansion(vm, menu, "5-expand-confirm")
            done = vm.wait_for(r"BUNY-EVENT expand-done data=(\d+) partition=(\d+) writes=(\d+)", 60)
            self.assertIsNotNone(done, vm.serial()[-3000:])
            self.assertEqual(int(done.group(1)), new)
            self.assertEqual(int(done.group(2)), new, "the kernel must use the new partition size at once")
            self.assertGreater(new, STICK - (600 << 20))
            result = vm.wait_for(r'BUNY-EVENT expand-result status=(-?\d+)', 30)
            self.assertEqual(result.group(1), "0", vm.serial()[-2000:])
            self.assertIsNotNone(vm.wait_for(r'BUNY-EVENT ui dialog=\d+ title="The drive now uses all of its space"', 10))
            time.sleep(1)
            menu.shot("6-expanded")
            # restart: a fresh kernel reads the new layout
            vm.key("esc")
            vm.key("f10")
            self.assertIsNotNone(vm.wait_for(r'BUNY-EVENT ui dialog=\d+ title="Power"', 15))
            vm.mark()
            vm.key("ret")  # Restart
            self.assertIsNotNone(vm.wait_for(r"BUNY-EVENT power action=0", 15))
            start = vm.serial_position
            self.assertIsNotNone(vm.wait_for(r"BUNY-EVENT coldplug ", 180), "the machine did not restart")
            menu = self.ready(vm)
            log = vm.serial()[start:]
            self.assertNotIn("kernel-gpt", log, "the partition table must be correct for the whole stick")
            self.assertRegex(log, r"BUNY-EVENT expand-plan state=none")
            self.assertRegex(log, rf"BUNY-EVENT data-partition device=\S+ fs=exfat uuid=\S+ size={new}")
            self.assertNotIn("ui expand offer", log)
            menu.shot("7-after-restart")
        parts = self.check_disk(vm, "expand")
        self.assertEqual(parts["DISTROBUNY"].size_bytes, new)

    def test_power_cut_during_the_expansion(self):
        # 3: inside the partition table (primary entries written, header not);
        # 7: inside the file system (marked dirty, bitmap entry updated, boot regions not)
        for cut in (3, 7):
            with self.subTest(power_cut_after=cut), \
                    self.boot(IMAGE, f"expand-cut-{cut}", disk_bytes=STICK, lab=f"buny.expandcrash={cut}", keep_disk=True) as vm:
                menu = self.ready(vm, since_start=True)
                self.confirm_expansion(vm, menu)
                self.assertIsNotNone(vm.wait_for(rf"BUNY-EVENT expand-power-cut writes={cut}", 60), vm.serial()[-3000:])
                start = vm.serial_position
                # the machine restarts and Distro Buny finishes the job before the menu appears
                self.assertIsNotNone(vm.wait_for(r"BUNY-EVENT expand-plan state=resume", 180), vm.serial()[-3000:])
                done = vm.wait_for(r"BUNY-EVENT expand-done data=(\d+) partition=(\d+)", 60)
                self.assertIsNotNone(done, vm.serial()[-3000:])
                self.assertEqual(done.group(1), done.group(2))
                self.ready(vm)
                log = vm.serial()[start:]
                self.assertNotIn("ui expand offer", log)
                self.assertNotIn("expand-power-cut", log, "the automatic resume is not cut again")
                vm.stop()
                parts = self.check_disk(vm, f"expand-cut-{cut}")
                self.assertEqual(parts["DISTROBUNY"].size_bytes, int(done.group(1)))

    def old_layout_image(self, out: Path) -> Path:
        """The starter image as the first 1.0.0 builds laid it out: DISTROBUNY (plain exFAT,
        32 KiB clusters) at 1 MiB, the ESP behind it, with the current boot partition."""
        build_id, ts = manifest()["build_id"], 1792713600
        data_sectors, esp_sectors = (3070 << 20) // 512, (512 << 20) // 512
        geo = Geometry((1 << 20) + (data_sectors + esp_sectors) * 512 + (1 << 20), 2048, data_sectors,
                       2048 + data_sectors, esp_sectors)
        data = ExfatBuilder(data_sectors * 512, label="DISTROBUNY", serial=image_build.serial_for(build_id, "data"),
                            partition_offset_sectors=geo.data_start, timestamp=ts)
        for path, blob in pipeline.volume_files(pipeline.empty_collection(), build_id, "starter", None).items():
            data.add_file(path, data=blob)
        data.add_dir("Desktop")
        data.add_dir("Servers")
        esp = image_build.esp_builder_from_tree(pipeline.ESP_TREE, {}, geo, build_id, ts)
        write_image(out, geo, build_id, esp, data)
        return out

    def test_older_layout_is_refused(self):
        image = self.old_layout_image(WORK / "old-layout.img")
        with self.boot(image, "old-layout", disk_bytes=STICK) as vm:
            menu = self.ready(vm, since_start=True)
            log = vm.serial()
            self.assertIn("BUNY-EVENT expand-plan state=refused code=layout", log)
            self.assertIn("BUNY-EVENT ui expand offer=refused", log)
            menu.click(action=ACT_EXPAND)
            self.assertIsNotNone(vm.wait_for(r'BUNY-EVENT ui dialog=\d+ title="DISTROBUNY cannot grow on this drive"', 15))
            time.sleep(1)
            menu.shot("8-old-layout-refused")
            self.assertNotIn("expand-start", vm.serial())
        image.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
