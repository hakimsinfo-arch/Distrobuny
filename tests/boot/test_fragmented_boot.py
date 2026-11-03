"""Boot test: an ISO stored in several out-of-order fragments still boots.

Takes a sparse copy of the dev image, splits Void Linux's ISO into three
cluster runs linked by a FAT chain (see fragment_exfat.py), checks the
volume with the reference fsck.exfat and the content through the runtime's
own extent mapper, then boots it in QEMU/OVMF.  The runtime must assemble the
fragments with device-mapper and present them as /dev/loop200.

Needs output/images/distrobuny-<VERSION>-dev.img (buny-build image --profile dev),
QEMU and OVMF; takes about two minutes with KVM.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / "VERSION").read_text().strip()
sys.path.insert(0, str(ROOT / "source" / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from distrobuny.image import gpt  # noqa: E402
from fragment_exfat import fragment  # noqa: E402

DEV = ROOT / "output" / "images" / f"distrobuny-{VERSION}-dev.img"
WORK = ROOT / "build" / "vm" / "fragmented"
ISO = "/Desktop/Not friendly/void-live-x86_64-20250202-base.iso"
BUNYCTL = ROOT / "build" / "bin" / "bunyctl"
FSCK = ROOT / "build" / "host-tools" / "usr" / "sbin" / "fsck.exfat"


class FragmentedBootTest(unittest.TestCase):
    def test_fragmented_iso_boots(self):
        if not DEV.exists() or not (ROOT / "Iso layout" / ISO.lstrip("/")).exists():
            self.skipTest("needs the dev image and the Void ISO")
        WORK.mkdir(parents=True, exist_ok=True)
        img = WORK / "frag.img"
        subprocess.run(["cp", "--sparse=always", str(DEV), str(img)], check=True)
        runs = fragment(img, ISO, 3)
        self.assertEqual(len(runs), 3)

        # the runtime's mapper sees three extents whose bytes are the ISO
        out = subprocess.run([str(BUNYCTL), "map", "--image", str(img), "--iso-path", ISO],
                             capture_output=True, text=True, check=True).stdout.split("\n")
        extents = [tuple(map(int, l.split()[1:3])) for l in out if l.startswith("extent ")]
        self.assertEqual(len(extents), 3)
        with open(img, "rb") as f:
            part = next(p for p in gpt.read_tables(f).partitions if p.name == "DISTROBUNY")
            h = hashlib.sha256()
            for off, n in extents:
                f.seek(part.offset + off)
                while n:
                    b = f.read(min(n, 8 << 20))
                    h.update(b)
                    n -= len(b)
        orig = hashlib.sha256((ROOT / "Iso layout" / ISO.lstrip("/")).read_bytes()).hexdigest()
        self.assertEqual(h.hexdigest(), orig)

        # the volume is still consistent for the reference implementation
        if FSCK.exists():
            data = WORK / "data.part"
            subprocess.run(["dd", f"if={img}", f"of={data}", "bs=1M", f"skip={part.offset >> 20}",
                            f"count={part.size_bytes >> 20}", "conv=sparse", "status=none"], check=True)
            r = subprocess.run([str(FSCK), "-n", str(data)], capture_output=True, text=True)
            data.unlink()
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

        # and it boots
        run = WORK / "run"
        shutil.rmtree(run, ignore_errors=True)
        subprocess.run([str(ROOT / "tools" / "development" / "buny-test-matrix"), "--image", str(img), "--filter",
                        "void-live", "--jobs", "1", "--out", str(run)], check=True, timeout=1200)
        result = json.loads((run / "results.json").read_text())[0]
        self.assertEqual(result["verdict"], "PASS", result["reason"])
        serial = next(run.glob("*/serial.log")).read_text(errors="replace")
        self.assertIn("assembled /dev/mapper/distrobuny-medium from 3 extents", serial)
        self.assertIn('attached /dev/loop200: "VOID_LIVE"', serial)


if __name__ == "__main__":
    unittest.main(verbosity=2)
