"""Integration tests: the real boot menu driven by keyboard and mouse.

Builds a small ISO collection with genisoimage (an Ubuntu-style and an
Arch-style image, nested folders, names with spaces, a hidden folder and a
damaged file), then runs the actual interface program (build/bin/buny-ui-host,
the same code as the runtime's buny-ui) on an offscreen display.  Scripted
key presses and pointer clicks go through the normal input handlers; the
program reports what it shows as BUNY-EVENT lines, which the tests check.
Without the runtime's privileged half the program can only prepare a boot
(dry run): it never starts a kernel.

Screenshots of every scenario are written to build/test-artifacts/ui/.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "build" / "bin" / "buny-ui-host"
SHARE = ROOT / "build" / "runtime-root" / "usr" / "share" / "distrobuny"
SHOTS = ROOT / "build" / "test-artifacts" / "ui"


def fake_kernel(version: str) -> bytes:
    """Enough of a bzImage header for kernel identification (boot protocol 2.15)."""
    k = bytearray(600 * 1024)
    k[0x202:0x206] = b"HdrS"
    k[0x206:0x208] = b"\x0f\x02"
    k[0x236] = 0x01
    k[0x20e:0x210] = b"\x00\x02"
    k[0x400:0x400 + len(version)] = version.encode()
    return bytes(k)


def make_iso(out: Path, label: str, files: dict[str, bytes | str]) -> None:
    with tempfile.TemporaryDirectory() as d:
        for rel, data in files.items():
            p = Path(d) / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data.encode() if isinstance(data, str) else data)
        out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["genisoimage", "-quiet", "-R", "-J", "-joliet-long", "-V", label, "-o", str(out), d],
                       check=True, stderr=subprocess.DEVNULL)


UBUNTU = {
    "casper/vmlinuz": fake_kernel("6.9.0-test-generic (buildd@lcy) #1 SMP"),
    "casper/initrd": "initrd-placeholder",
    ".disk/info": 'Ubuntu 26.04.1 LTS "Test" - Release amd64 (20260101)\n',
    "boot/grub/grub.cfg": 'menuentry "Try or Install Ubuntu" {\n linux /casper/vmlinuz --- quiet splash\n initrd /casper/initrd\n}\n',
    "boot/grub/loopback.cfg": 'menuentry "Try or Install Ubuntu" {\n linux /casper/vmlinuz iso-scan/filename=${iso_path} '
                              '--- quiet splash\n initrd /casper/initrd\n}\n',
}
ARCH = {
    "arch/boot/x86_64/vmlinuz-linux": fake_kernel("6.10.1-arch1-1 (linux@archlinux) #1"),
    "arch/boot/x86_64/initramfs-linux.img": "x",
    "loader/entries/01-archiso-linux.conf": "title Arch Linux install medium\nlinux /arch/boot/x86_64/vmlinuz-linux\n"
                                            "initrd /arch/boot/x86_64/initramfs-linux.img\n"
                                            "options archisobasedir=arch archisosearchuuid=2026-01-01-00-00-00-00\n",
}

SELF_INSTALL = {
    "boot/x86_64/loader/linux": fake_kernel("6.12.0-160000.35-default (geeko@buildhost) #1"),
    "boot/x86_64/loader/initrd": "x",
    "boot/grub2/grub.cfg": 'menuentry "Install openSUSE Leap Micro" {\n linux /boot/x86_64/loader/linux cdinst=1 '
                           'root=install:CDLABEL=INSTALL\n initrd /boot/x86_64/loader/initrd\n}\n',
}


class UiNavigationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not UI.exists() or not (SHARE / "distributions.conf").exists():
            raise unittest.SkipTest("build the interface and runtime first (make host; buny-build runtime)")
        if not shutil.which("genisoimage"):
            raise unittest.SkipTest("genisoimage not installed")
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-ui-test-"))
        cls.data = cls.tmp / "data"
        make_iso(cls.data / "Desktop" / "Ubuntu Family" / "ubuntu-26.04.1-desktop-amd64.iso", "Ubuntu 26.04.1 LTS amd64", UBUNTU)
        make_iso(cls.data / "Desktop" / "Ubuntu Family" / "Older" / "ubuntu-24.04-desktop-amd64.iso", "Ubuntu 24.04 LTS amd64", UBUNTU)
        make_iso(cls.data / "Desktop" / "archlinux-x86_64.iso", "ARCH_202601", ARCH)
        make_iso(cls.data / "Servers" / "my server image.iso", "ARCH_202601", ARCH)
        (cls.data / "Desktop" / "damaged.iso").write_bytes(os.urandom(300 * 1024))
        make_iso(cls.data / "Servers" / "leap-micro-SelfInstall.iso", "INSTALL", SELF_INSTALL)
        # an Ubuntu-style image around a real Canonical-signed kernel (the runtime's own)
        signed = dict(UBUNTU, **{"casper/vmlinuz": (ROOT / "build" / "runtime" / "vmlinuz").read_bytes()})
        make_iso(cls.data / "Servers" / "signed-ubuntu-server.iso", "Ubuntu-Server 26.04.1 LTS amd64", signed)
        make_iso(cls.data / ".distrobuny" / "staging" / "hidden.iso", "HIDDEN", ARCH)  # never listed
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_ui(self, name: str, steps: list[str], size: str = "1280x800", extra_env: dict | None = None) -> list[str]:
        """Run the menu with a script; returns the BUNY-EVENT lines it printed."""
        script = []
        for s in steps:
            script.append(s)
            if not s.startswith(("wait:", "shot:")):
                script.append("wait:350")  # let transitions settle like a person would
        script += [f"shot:{SHOTS / (name + '.png')}", "quit"]
        env = dict(os.environ, BUNY_DISPLAY=f"offscreen:{size}", BUNY_DATA=str(self.data), BUNY_SHARE=str(SHARE),
                   BUNY_LAB="1", BUNY_SCRIPT=";".join(["wait:900"] + script), BUNY_STATE=str(self.tmp / "state"))
        env.update(extra_env or {})
        r = subprocess.run([str(UI)], env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        self.assertNotIn("script-missing", r.stderr, r.stderr[-3000:])
        return [l[len("BUNY-EVENT "):] for l in r.stderr.splitlines() if l.startswith("BUNY-EVENT ")]

    def screens(self, events: list[str]) -> list[str]:
        out = []
        for e in events:
            m = re.match(r'ui screen=(\w+) path="([^"]*)"', e)
            if m:
                out.append(f"{m.group(1)}:{m.group(2)}")
        return out

    def test_discovery(self):
        ev = self.run_ui("discovery", [])
        ready = next(e for e in ev if e.startswith("ui-ready"))
        # 7 images (including the damaged one), the hidden staging folder ignored
        self.assertIn("isos=7", ready)
        self.assertFalse([e for e in ev if e.startswith("ui hint=")], ev)

    def test_keyboard_hierarchy_and_back(self):
        ev = self.run_ui("keyboard", ["key:Enter", "key:Enter", "key:Enter", "key:Esc", "key:Backspace", "key:Esc"])
        self.assertEqual(self.screens(ev), [
            "folder:/Desktop", "folder:/Desktop/Ubuntu Family", "folder:/Desktop/Ubuntu Family/Older",
            "folder:/Desktop/Ubuntu Family", "folder:/Desktop", "home:",
        ])

    def test_keyboard_moves_focus_spatially(self):
        ev = self.run_ui("focus", ["key:Right", "key:Enter"])
        self.assertIn("folder:/Servers", self.screens(ev))

    def test_mouse_hierarchy_breadcrumbs_and_back(self):
        ev = self.run_ui("mouse", ["clickon:/Desktop", "clickon:/Desktop/Ubuntu Family", "clickon:/Desktop/Ubuntu Family/Older",
                                   "clickon:@crumb:1", "clickon:@back", "clickon:/Servers"])
        self.assertEqual(self.screens(ev), [
            "folder:/Desktop", "folder:/Desktop/Ubuntu Family", "folder:/Desktop/Ubuntu Family/Older",
            "folder:/Desktop", "home:", "folder:/Servers",
        ])

    def test_boot_preparation_through_the_menu(self):
        iso = "/Desktop/Ubuntu Family/ubuntu-26.04.1-desktop-amd64.iso"
        ev = self.run_ui("boot-ubuntu", ["clickon:/Desktop", "clickon:/Desktop/Ubuntu Family", f"clickon:{iso}", "wait:600",
                                         "clickon:@boot", "wait:1500"])
        self.assertIn(f"detail:{iso}", self.screens(ev))
        ready = [e for e in ev if e.startswith("boot-ready")]
        self.assertEqual(len(ready), 1, ev)
        self.assertIn("adapter=casper", ready[0])
        self.assertIn("dry_run=1", ready[0])
        self.assertIn("live-media=/dev/loop200", ready[0])
        # kernel parameters stay in front of the installer's "---"
        self.assertRegex(ready[0], r"live-media=/dev/loop200 [^\"]*---\"")

    def test_names_with_spaces_boot_through_the_virtual_medium(self):
        iso = "/Servers/my server image.iso"
        ev = self.run_ui("boot-spaces", ["clickon:/Servers", f"clickon:{iso}", "wait:600", "clickon:@boot", "wait:1500"])
        ready = [e for e in ev if e.startswith("boot-ready")]
        self.assertEqual(len(ready), 1, ev)
        self.assertIn("adapter=archiso", ready[0])
        self.assertIn("archisosearchuuid=", ready[0])

    def test_damaged_image_cannot_start(self):
        iso = "/Desktop/damaged.iso"
        ev = self.run_ui("damaged", ["clickon:/Desktop", f"clickon:{iso}", "wait:800", "key:Enter", "wait:800"])
        self.assertIn(f"detail:{iso}", self.screens(ev))
        self.assertFalse([e for e in ev if e.startswith("boot-ready")], ev)

    def test_self_installing_image_needs_confirmation(self):
        iso = "/Servers/leap-micro-SelfInstall.iso"
        # Boot opens a warning whose default choice (Enter) cancels
        ev = self.run_ui("self-install-warning", ["clickon:/Servers", f"clickon:{iso}", "wait:600", "clickon:@boot",
                                                  "wait:300", "shot:" + str(SHOTS / "self-install-dialog.png"),
                                                  "key:Enter", "wait:800"])
        self.assertTrue(any(e.startswith("ui dialog=") and "erases a disk" in e for e in ev), ev)
        self.assertFalse([e for e in ev if e.startswith("boot-start")], ev)
        # "Start anyway" goes ahead
        ev = self.run_ui("self-install-confirmed", ["clickon:/Servers", f"clickon:{iso}", "wait:600", "clickon:@boot",
                                                    "wait:300", "key:Down", "key:Enter", "wait:1500"])
        ready = [e for e in ev if e.startswith("boot-ready")]
        self.assertEqual(len(ready), 1, ev)
        self.assertIn("adapter=kiwi", ready[0])

    def badges(self, runtime: dict) -> dict:
        rt = self.tmp / "runtime.json"
        rt.write_text(json.dumps(runtime))
        ev = self.run_ui("secure-boot-" + str(len(runtime.get("trusted_keys", []))) + runtime.get("secure_boot", ""),
                         ["wait:1500", "clickon:/Servers", "wait:300"], extra_env={"BUNY_RUNTIME": str(rt)})
        out = {}
        for e in ev:
            m = re.match(r'ui item .* path="([^"]*\.iso)" badge="([^"]*)"', e)
            if m:
                out[m.group(1)] = m.group(2)
        return out

    def test_secure_boot_trust_prediction(self):
        signed, unsigned = "/Servers/signed-ubuntu-server.iso", "/Servers/my server image.iso"
        canonical = {"secure_boot": "enabled", "trusted_keys": ["Canonical Ltd. Master Certificate Authority",
                                                                "Microsoft Corporation UEFI CA 2011"]}
        b = self.badges(canonical)
        self.assertEqual(b[signed], "Ready")                       # Canonical-signed: kexec will accept it
        self.assertEqual(b[unsigned], "Needs Secure Boot off")     # unsigned kernel
        b = self.badges({"secure_boot": "enabled", "trusted_keys": ["Microsoft Corporation UEFI CA 2011"]})
        self.assertEqual(b[signed], "Needs Secure Boot off")       # signed, but by a CA this kernel does not trust
        b = self.badges({"secure_boot": "disabled"})
        self.assertEqual(b[signed], "Ready")
        self.assertEqual(b[unsigned], "Ready")

    def test_settings_dark_theme(self):
        ev = self.run_ui("settings", ["clickon:@settings", "key:Right", "wait:400", "shot:" + str(SHOTS / "settings-dark.png"),
                                      "key:Esc", "clickon:/Desktop"])
        self.assertIn("settings:", self.screens(ev))
        self.assertIn("setting index=0 theme=dark", ev)
        self.assertIn("folder:/Desktop", self.screens(ev))

    def test_search(self):
        ev = self.run_ui("search", ["clickon:@search", "type:24.04", "wait:500", "key:Down", "key:Enter"])
        self.assertIn('ui search query="24.04"', ev)
        self.assertIn("detail:/Desktop/Ubuntu Family/Older/ubuntu-24.04-desktop-amd64.iso", self.screens(ev))

    def test_small_and_large_displays(self):
        for size in ("800x600", "3840x2160"):
            ev = self.run_ui(f"display-{size}", ["key:Enter"], size=size)
            self.assertIn("folder:/Desktop", self.screens(ev))
            self.assertTrue(any(e.startswith("ui-ready") and size in e for e in ev), ev)


class EmptyDriveTest(unittest.TestCase):
    """A drive without images (the starter image): the menu says how to add some."""

    run_ui = UiNavigationTest.run_ui
    screens = UiNavigationTest.screens

    @classmethod
    def setUpClass(cls):
        if not UI.exists() or not (SHARE / "distributions.conf").exists():
            raise unittest.SkipTest("build the interface and runtime first (make host; buny-build runtime)")
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-ui-empty-"))
        cls.data = cls.tmp / "data"
        for d in ("Desktop/My distributions", "Servers"):
            (cls.data / d).mkdir(parents=True)
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_home_explains_how_to_add_images(self):
        ev = self.run_ui("empty-home", [])
        self.assertIn("isos=0", next(e for e in ev if e.startswith("ui-ready")))
        self.assertIn('ui hint="No ISO images on this drive yet"', ev)
        # the standard folders can still be opened
        self.assertTrue(any(e.startswith("ui item") and 'path="/Servers"' in e for e in ev), ev)

    def test_empty_folders_explain_how_to_add_images(self):
        ev = self.run_ui("empty-folder", ["key:Enter", "key:Enter"])
        self.assertEqual(self.screens(ev), ["folder:/Desktop", "folder:/Desktop/My distributions"])
        self.assertEqual(ev.count('ui hint="No ISO images in this folder yet"'), 2, ev)

    def test_small_display(self):
        ev = self.run_ui("empty-home-800x600", [], size="800x600")
        self.assertIn('ui hint="No ISO images on this drive yet"', ev)
        self.assertFalse([e for e in ev if e.startswith("ui expand")], "no expansion offer without the runtime's word")


ALPINE = {  # a family no adapter knows: its initramfs finds the medium by itself
    "boot/vmlinuz-lts": fake_kernel("6.12.40-0-lts (buildozer@build) #1-Alpine SMP"),
    "boot/initramfs-lts": "x",
    "boot/grub/grub.cfg": 'menuentry "Linux lts" {\n linux /boot/vmlinuz-lts modules=loop,squashfs,sd-mod,usb-storage quiet\n'
                          ' initrd /boot/initramfs-lts\n}\n',
}
AUTOINSTALL = dict(UBUNTU, **{  # a customised Ubuntu Server that installs without asking
    "boot/grub/grub.cfg": 'menuentry "Install Ubuntu Server" {\n linux /casper/vmlinuz autoinstall ds=nocloud --- quiet\n'
                          ' initrd /casper/initrd\n}\n',
})


ANTIX = {  # antiX / MX Linux: live-init skips loop devices in its scan
    "antiX/vmlinuz": fake_kernel("5.10.256-antix.1-amd64-smp (root@antix) #1"),
    "antiX/initrd.gz": "x",
    "antiX/linuxfs": "x",
    "boot/grub/grub.cfg": 'menuentry "antiX-26.1 x64" {\n linux /antiX/vmlinuz quiet from=usb\n initrd /antiX/initrd.gz\n}\n',
}
BLACKARCH = {  # archiso of 2023: label-only lookup
    "blackarch/boot/x86_64/vmlinuz-linux": fake_kernel("6.2.10-arch1-1 (linux@archlinux) #1"),
    "blackarch/boot/x86_64/initramfs-linux.img": "x",
    "loader/entries/01-archiso-x86_64-linux.conf": "title BlackArch Linux\nlinux /blackarch/boot/x86_64/vmlinuz-linux\n"
                                                    "initrd /blackarch/boot/x86_64/initramfs-linux.img\n"
                                                    "options archisobasedir=blackarch archisolabel=BLACKARCH_202304\n",
}


SLACKWARE = {  # the installer only needs the DVD for its packages
    "kernels/huge.s/bzImage": fake_kernel("5.15.19 (root@hive64) #1 SMP PREEMPT"),
    "isolinux/initrd.img": "x",
    "slackware64/PACKAGES.TXT": "x",
    "EFI/BOOT/grub.cfg": 'menuentry "Slackware 15.0 huge.s kernel" {\n linux /kernels/huge.s/bzImage load_ramdisk=1 ro\n'
                         ' initrd /isolinux/initrd.img\n}\n',
}


class NewFamiliesTest(unittest.TestCase):
    """Boot rules added for antiX/MX Linux and older archiso (BlackArch), each
    verified with the real ISO in a VM (documentation/COMPATIBILITY.md)."""

    run_ui = UiNavigationTest.run_ui

    @classmethod
    def setUpClass(cls):
        if not UI.exists() or not (SHARE / "distributions.conf").exists():
            raise unittest.SkipTest("build the interface and runtime first (make host; buny-build runtime)")
        if not shutil.which("genisoimage"):
            raise unittest.SkipTest("genisoimage not installed")
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-ui-families-"))
        cls.data = cls.tmp / "data"
        make_iso(cls.data / "Desktop" / "antiX-26.1_x64-full.iso", "antiX-Live", ANTIX)
        make_iso(cls.data / "Desktop" / "blackarch-linux-netinst.iso", "BLACKARCH_202304", BLACKARCH)
        make_iso(cls.data / "Desktop" / "slackware64-15.0-install-dvd.iso", "SlackDVD", SLACKWARE)
        (cls.data / "Servers").mkdir()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def ready(self, iso: str) -> str:
        ev = self.run_ui("family-" + Path(iso).stem, ["clickon:/Desktop", "wait:600", f"clickon:{iso}", "wait:300",
                                                     "clickon:@boot", "wait:1500"])
        ready = [e for e in ev if e.startswith("boot-ready")]
        self.assertEqual(len(ready), 1, ev)
        return ready[0]

    def test_antix_is_pointed_at_the_virtual_medium(self):
        ready = self.ready("/Desktop/antiX-26.1_x64-full.iso")
        self.assertIn("adapter=antix", ready)
        self.assertIn("bdev=/dev/loop200", ready)
        self.assertNotIn("from=", ready, "a from= filter would hide the loop device")
        self.assertIn("efi=noruntime", ready, "kernel 5.10 crashes in UEFI runtime services after kexec")

    def test_slackware_installer(self):
        ready = self.ready("/Desktop/slackware64-15.0-install-dvd.iso")
        self.assertIn("adapter=slackware", ready)
        self.assertNotIn("efi=noruntime", ready, "kernel 5.15 keeps UEFI runtime services")

    def test_label_only_archiso_names_the_device(self):
        ready = self.ready("/Desktop/blackarch-linux-netinst.iso")
        self.assertIn("adapter=archiso", ready)
        self.assertIn("archisodevice=/dev/loop200", ready)


class UnknownAndUnattendedTest(unittest.TestCase):
    """Images Distro Buny has no adapter for start best effort, after a
    confirmation; images that install without asking are marked like
    self-installers."""

    run_ui = UiNavigationTest.run_ui

    @classmethod
    def setUpClass(cls):
        if not UI.exists() or not (SHARE / "distributions.conf").exists():
            raise unittest.SkipTest("build the interface and runtime first (make host; buny-build runtime)")
        if not shutil.which("genisoimage"):
            raise unittest.SkipTest("genisoimage not installed")
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-ui-unknown-"))
        cls.data = cls.tmp / "data"
        make_iso(cls.data / "Desktop" / "alpine-standard-3.22.0-x86_64.iso", "alpine-std 3.22.0 x86_64", ALPINE)
        make_iso(cls.data / "Servers" / "ubuntu-autoinstall.iso", "Ubuntu-Server 26.04.1 LTS amd64", AUTOINSTALL)
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def badge(self, ev: list[str], iso: str) -> str:
        return next(re.search(r'badge="([^"]*)"', e).group(1) for e in ev if e.startswith("ui item") and f'path="{iso}"' in e)

    def test_unknown_family_starts_best_effort_after_a_confirmation(self):
        iso = "/Desktop/alpine-standard-3.22.0-x86_64.iso"
        ev = self.run_ui("best-effort", ["clickon:/Desktop", "wait:600", f"clickon:{iso}", "wait:300", "clickon:@boot",
                                         "wait:300", "shot:" + str(SHOTS / "best-effort-dialog.png"), "key:Enter", "wait:500"])
        self.assertEqual(self.badge(ev, iso), "Best effort")
        self.assertTrue(any(e.startswith("ui dialog=") and "does not know this system" in e for e in ev), ev)
        self.assertFalse([e for e in ev if e.startswith("boot-start")], "Enter on the initial focus cancels")
        ev = self.run_ui("best-effort-started", ["clickon:/Desktop", "wait:600", f"clickon:{iso}", "wait:300", "clickon:@boot",
                                                 "wait:300", "key:Down", "key:Enter", "wait:1500"])
        ready = [e for e in ev if e.startswith("boot-ready")]
        self.assertEqual(len(ready), 1, ev)
        self.assertIn("adapter=best-effort", ready[0])
        self.assertIn("modules=loop,squashfs,sd-mod,usb-storage", ready[0], "the image's own arguments are kept")

    def test_unattended_installer_is_marked(self):
        iso = "/Servers/ubuntu-autoinstall.iso"
        ev = self.run_ui("autoinstall", ["clickon:/Servers", "wait:600", f"clickon:{iso}", "wait:300", "clickon:@boot",
                                         "wait:300", "key:Enter", "wait:500"])
        self.assertEqual(self.badge(ev, iso), "Erases a disk")
        self.assertTrue(any(e.startswith("ui dialog=") and "erases a disk" in e for e in ev), ev)
        self.assertFalse([e for e in ev if e.startswith("boot-start")], ev)


ACT_EXPAND = 21


class ExpandOfferTest(unittest.TestCase):
    """The starter image written onto a bigger stick: Home offers to grow DISTROBUNY.

    The offer comes from the privileged runtime (runtime.json "expand"); here
    it is supplied directly, and without the runtime the confirmed request
    can only be reported (expand-request).  The expansion itself is tested on
    images (tests/unit/python/test_image.py) and in a virtual machine
    (tests/boot/test_starter_image.py).
    """

    run_ui = UiNavigationTest.run_ui
    screens = UiNavigationTest.screens

    @classmethod
    def setUpClass(cls):
        EmptyDriveTest.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def runtime(self, **expand) -> dict:
        info = {"state": "possible", "code": "possible", "disk_bytes": 16_000_000_000, "data_bytes": 2_683_305_984,
                "new_data_bytes": 15_461_908_480, "limited": False, "reason": ""}
        info.update(expand)
        path = self.tmp / "runtime.json"
        path.write_text(json.dumps({"version": "test", "lab": True, "expand": info,
                                    "data": {"device": "/dev/sda1", "fs": "exfat", "label": "DISTROBUNY", "size": info["data_bytes"],
                                             "free": info["data_bytes"] - (4 << 20)}}))
        return {"BUNY_RUNTIME": str(path)}

    def test_home_offers_to_use_the_whole_drive(self):
        ev = self.run_ui("expand-offer", [], extra_env=self.runtime())
        self.assertIn("ui expand offer=possible", ev)
        self.assertTrue([e for e in ev if e.startswith("ui item") and f"action={ACT_EXPAND} " in e], ev)
        self.assertIn('ui hint="No ISO images on this drive yet"', ev)

    def test_expansion_needs_a_deliberate_confirmation(self):
        ev = self.run_ui("expand-dialog", ["clickon:@expand", "key:Enter",  # Enter on the initial focus: Cancel
                                           "clickon:@expand", "shot:" + str(SHOTS / "expand-confirm.png"),
                                           "key:Down", "key:Enter"], extra_env=self.runtime())
        dialogs = [e for e in ev if e.startswith("ui dialog=")]
        self.assertEqual(len(dialogs), 2, ev)
        self.assertIn('title="Use the whole drive?"', dialogs[0])
        self.assertEqual(ev.count("expand-request"), 1, "only the explicit choice asks the runtime")
        self.assertGreater(ev.index("expand-request"), ev.index(dialogs[1]))

    def test_mouse_confirmation(self):
        ev = self.run_ui("expand-mouse", ["clickon:@expand", "clickon:@cancel", "clickon:@expand", "clickon:@confirm-expand"],
                         extra_env=self.runtime())
        self.assertEqual(ev.count("expand-request"), 1, ev)

    def test_refused_drive_explains_why(self):
        reason = "On this drive the boot partition lies behind DISTROBUNY (an older Distro Buny image)."
        ev = self.run_ui("expand-refused", ["clickon:@expand"],
                         extra_env=self.runtime(state="refused", code="layout", reason=reason))
        self.assertIn("ui expand offer=refused", ev)
        self.assertTrue(any('title="DISTROBUNY cannot grow on this drive"' in e for e in ev), ev)
        self.assertNotIn("expand-request", ev)

    def test_small_display(self):
        ev = self.run_ui("expand-offer-800x600", [], size="800x600", extra_env=self.runtime())
        self.assertIn("ui expand offer=possible", ev)
        items = [e for e in ev if e.startswith("ui item")]
        banner = next(e for e in items if f"action={ACT_EXPAND} " in e)
        x, y, w, h = (int(re.search(rf"{k}=(-?\d+)", banner).group(1)) for k in ("x", "y", "w", "h"))
        self.assertTrue(0 <= x and x + w <= 800 and 0 <= y and y + h <= 600, banner)
        for e in items:  # nothing overlaps the banner
            if e is banner:
                continue
            ox, oy, ow, oh = (int(re.search(rf"{k}=(-?\d+)", e).group(1)) for k in ("x", "y", "w", "h"))
            self.assertFalse(ox < x + w and x < ox + ow and oy < y + h and y < oy + oh, f"{e} overlaps {banner}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
