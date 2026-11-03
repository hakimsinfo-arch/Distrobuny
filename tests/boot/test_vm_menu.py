"""Boot test: the real menu in a virtual machine, driven like a person would.

Boots a Distro Buny image in QEMU with UEFI firmware (OVMF) through the
complete chain (shim → GRUB → runtime kernel → buny-init → buny-ui), then
uses the virtual USB keyboard and the virtual USB tablet (an absolute
pointer, like a touchscreen or a VM mouse) through QEMU's input events. The
runtime reports what it shows on the serial console (lab mode, enabled by
QEMU's fw_cfg, so the image is not modified).

Checks: the interface appears, keyboard navigation into a category and back,
mouse clicks on real cards, right-click back, breadcrumbs, the power dialog,
and preparing a real hand-off for an ISO through the privileged runtime in
dry-run mode (everything up to, but not including, the kexec).
Screenshots go to build/test-artifacts/vm-menu/.

Image: $BUNY_TEST_IMAGE or output/images/distrobuny-<VERSION>-dev.img.
"""
from __future__ import annotations

import os
import re
import shutil
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / "VERSION").read_text().strip()
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.qemu.vm import VM, VMConfig  # noqa: E402

IMAGE = Path(os.environ.get("BUNY_TEST_IMAGE", ROOT / "output" / "images" / f"distrobuny-{VERSION}-dev.img"))
WORK = ROOT / "build" / "vm" / "menu-test"
SHOTS = ROOT / "build" / "test-artifacts" / "vm-menu"
ACT_BOOT = 5
ACT_POWER = 3


class Menu:
    def __init__(self, vm: VM, size: tuple[int, int]):
        self.vm, self.size = vm, size
        self.items: list[dict] = []

    def wait_screen(self, kind: str, path: str = "", timeout: float = 15) -> None:
        start = self.vm.serial_position
        m = self.vm.wait_for(rf'BUNY-EVENT ui screen={kind} path="{re.escape(path)}"', timeout)
        if not m:
            raise AssertionError(f"screen {kind} {path!r} did not appear; serial tail:\n{self.vm.serial()[-1500:]}")
        self.items = self.read_items(start)

    def read_items(self, start: int) -> list[dict]:
        # the item list follows the screen event within a frame or two
        self.vm.wait_for(r"BUNY-EVENT ui item ", 5)
        import time
        time.sleep(0.5)
        text = self.vm.serial()[start:]
        last = [m for m in re.finditer(r"BUNY-EVENT ui screen=", text)]
        text = text[last[-1].start():] if last else text
        out = []
        for m in re.finditer(r'ui item x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) kind=(\d+) action=(\d+) path="([^"]*)"', text):
            x, y, w, h, kind, action = map(int, m.groups()[:6])
            out.append({"x": x, "y": y, "w": w, "h": h, "kind": kind, "action": action, "path": m.group(7)})
        return out

    def click(self, path: str | None = None, action: int | None = None, button: str = "left") -> None:
        for it in self.items:
            if (path is not None and it["path"] == path) or (action is not None and it["action"] == action):
                self.vm.mark()
                self.vm.click(it["x"] + it["w"] // 2, it["y"] + it["h"] // 2, self.size, button)
                return
        raise AssertionError(f"no item {path or action} on screen: {self.items}")

    def shot(self, name: str) -> None:
        self.vm.screenshot(SHOTS / f"{name}.png")


class VmMenuTest(unittest.TestCase):
    def test_menu_in_a_virtual_machine(self):
        if not IMAGE.exists():
            self.skipTest(f"{IMAGE} not built")
        shutil.rmtree(WORK, ignore_errors=True)
        WORK.mkdir(parents=True)
        SHOTS.mkdir(parents=True, exist_ok=True)
        cfg = VMConfig(image=IMAGE, workdir=WORK, lab_options="buny.lab=1 buny.dryrun=1", name="menu-test")
        with VM(cfg) as vm:
            m = vm.wait_for(r"BUNY-EVENT ui-ready isos=(\d+) display=(\w+) (\d+)x(\d+) keyboards=(\d+) pointers=(\d+)",
                            120, since_start=True)
            self.assertIsNotNone(m, "the interface did not start:\n" + vm.serial()[-3000:])
            isos, backend, w, h, kbd, ptr = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), \
                int(m.group(5)), int(m.group(6))
            self.assertGreater(isos, 0)
            self.assertEqual(backend, "drm", "the interface should drive the display through DRM/KMS")
            self.assertGreaterEqual(kbd, 1)
            self.assertGreaterEqual(ptr, 1)
            menu = Menu(vm, (w, h))
            menu.items = menu.read_items(m.start())
            self.assertTrue(any(i["path"] == "/Desktop" for i in menu.items), menu.items)
            vm.wait_for(r"BUNY-EVENT ui focus", 10)
            menu.shot("1-home")

            # keyboard: the focused category opens with Enter, Esc goes back
            vm.mark()
            vm.key("ret")
            menu.wait_screen("folder", "/Desktop")
            menu.shot("2-desktop-keyboard")
            vm.mark()
            vm.key("esc")
            menu.wait_screen("home")

            # mouse: click the Servers card, right-click returns
            menu.click(path="/Servers")
            menu.wait_screen("folder", "/Servers")
            menu.shot("3-servers-mouse")
            vm.mark()
            vm.click(w // 2, h - 20, (w, h), "right")
            menu.wait_screen("home")

            # nested folders with the mouse, then a breadcrumb back to Desktop
            menu.click(path="/Desktop")
            menu.wait_screen("folder", "/Desktop")
            folders = [i for i in menu.items if i["kind"] == 1]  # IT_FOLDER
            self.assertTrue(folders, "the Desktop page should list sub-folders")
            sub = folders[0]["path"]
            menu.click(path=sub)
            menu.wait_screen("folder", sub)
            menu.shot("4-subfolder")
            crumbs = [i for i in menu.items if i["kind"] == 4 and i["path"] == "/Desktop"]  # IT_CRUMB
            self.assertTrue(crumbs, menu.items)
            vm.mark()
            vm.click(crumbs[0]["x"] + crumbs[0]["w"] // 2, crumbs[0]["y"] + crumbs[0]["h"] // 2, (w, h))
            menu.wait_screen("folder", "/Desktop")

            # an image's page, then Start: the privileged runtime prepares the
            # whole hand-off (extents, medium check, pre-init, kexec_file_load
            # arguments) but stops before loading because of buny.dryrun=1
            menu.click(path=sub)
            menu.wait_screen("folder", sub)
            isos_here = [i for i in menu.items if i["kind"] == 2]  # IT_ISO
            self.assertTrue(isos_here)
            iso = isos_here[0]["path"]
            menu.click(path=iso)
            menu.wait_screen("detail", iso)
            menu.shot("5-detail")
            menu.click(action=ACT_BOOT)
            m = vm.wait_for(r'BUNY-EVENT boot-loaded dry_run=1 cmdline="([^"]*)"', 120)
            self.assertIsNotNone(m, "the runtime did not prepare the hand-off:\n" + vm.serial()[-3000:])
            self.assertIn("rdinit=/distrobuny/preinit", m.group(1))
            vm.wait_for(r"BUNY-EVENT boot-ready", 30)
            menu.wait_screen("detail", iso)  # the dry run returns to the image page
            menu.shot("6-prepared")

            # back out with the keyboard one level at a time, then the power dialog
            for kind, path in (("folder", sub), ("folder", "/Desktop"), ("home", "")):
                vm.mark()
                vm.key("esc")
                menu.wait_screen(kind, path)
            vm.mark()
            vm.key("f10")
            self.assertIsNotNone(vm.wait_for(r'BUNY-EVENT ui dialog=\d+ title="Power"', 15))
            menu.shot("7-power-dialog")

    def test_secure_boot_badges_in_a_virtual_machine(self):
        """With Secure Boot enforced, the runtime reads the kernel's trusted keys
        and marks images whose kernels it will refuse before anyone tries them."""
        if not IMAGE.exists():
            self.skipTest(f"{IMAGE} not built")
        work = WORK.with_name("menu-test-sb")
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        cfg = VMConfig(image=IMAGE, workdir=work, secure_boot=True, lab_options="buny.lab=1 buny.dryrun=1",
                       name="menu-test-sb")
        with VM(cfg) as vm:
            m = vm.wait_for(r"BUNY-EVENT ui-ready isos=\d+ display=\w+ (\d+)x(\d+)", 120, since_start=True)
            self.assertIsNotNone(m, vm.serial()[-3000:])
            self.assertIsNotNone(vm.wait_for(r"BUNY-EVENT trusted-keys count=[1-9]", 5, since_start=True))
            menu = Menu(vm, (int(m.group(1)), int(m.group(2))))
            menu.items = menu.read_items(m.start())
            menu.click(path="/Desktop")
            menu.wait_screen("folder", "/Desktop")
            badges = {}
            for folder in ("/Desktop/Fedora", "/Desktop/ubuntu", "/Desktop/Arch"):
                if not any(i["path"] == folder for i in menu.items):
                    continue
                start = vm.serial_position
                menu.click(path=folder)
                menu.wait_screen("folder", folder)
                text = vm.serial()[start:]
                badges.update(dict(re.findall(r'ui item [^\n]*path="([^"]*\.iso)" badge="([^"]*)"', text)))
                menu.shot("sb-" + folder.rsplit("/", 1)[1].lower())
                vm.mark()
                vm.key("esc")
                menu.wait_screen("folder", "/Desktop")
            self.assertTrue(badges, "no image cards were reported")
            for path, badge in badges.items():
                if "/Fedora/" in path or "/Arch/" in path:
                    # Fedora: signed by Fedora's CA, not trusted here; Arch family: unsigned
                    self.assertEqual(badge, "Needs Secure Boot off", path)
                elif "/ubuntu/" in path:
                    # Canonical-signed and booted in the lab with Secure Boot on
                    self.assertEqual(badge, "Verified", path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
