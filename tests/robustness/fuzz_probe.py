"""Robustness test: the ISO and boot-configuration parsers against corrupted images.

The menu reads untrusted ISO files (ISO 9660 + Rock Ridge/Joliet, El Torito,
GRUB, systemd-boot and isolinux configurations, kernel headers).  This test
builds bunyctl with AddressSanitizer and UndefinedBehaviorSanitizer, makes
small valid ISOs of several families, corrupts them (random bytes in the
volume descriptors, directory records and file data; mangled boot
configurations), and runs `bunyctl probe` on each.  Any sanitizer report,
crash or hang is a failure; a clean "cannot read" refusal is the expected
outcome for broken input.

  python3 tests/robustness/fuzz_probe.py [ITERATIONS] [SEED]

Not part of the quick unit tests: it rebuilds bunyctl in build/asan.
"""
from __future__ import annotations

import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from test_ui_navigation import ALPINE, ARCH, SELF_INSTALL, UBUNTU, make_iso  # noqa: E402

ASAN = ROOT / "build" / "asan"
BUNYCTL = ASAN / "bin" / "bunyctl"
RULES = ROOT / "configuration" / "distributions" / "distributions.conf"


def build() -> None:
    flags = "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    # relative paths: make cannot handle the space in the workspace path
    subprocess.run(["make", "BUILD=build/asan", f"CFLAGS={flags}", f"-j{os.cpu_count() or 2}", "build/asan/bin/bunyctl"],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def mangle_text(data: bytes, rnd: random.Random) -> bytes:
    """Boot configurations: cut, duplicate and corrupt lines and quotes."""
    lines = data.split(b"\n")
    for _ in range(rnd.randint(1, 6)):
        op = rnd.randrange(5)
        i = rnd.randrange(len(lines))
        if op == 0:
            lines[i] = lines[i][: rnd.randrange(len(lines[i]) + 1)]
        elif op == 1:
            lines.insert(i, lines[i] * rnd.randint(2, 50))
        elif op == 2:
            lines[i] += rnd.choice([b'"', b"'", b"{", b"}", b"$", b"${", b"\\", b"menuentry", b"source /x", b"\x00", b"\xff" * 300])
        elif op == 3:
            lines[i] = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 80)))
        else:
            del lines[i]
            if not lines:
                lines = [b""]
    return b"\n".join(lines)


def main() -> int:
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if not shutil.which("genisoimage"):
        print("genisoimage not installed")
        return 2
    build()
    rnd = random.Random(seed)
    tmp = Path(tempfile.mkdtemp(prefix="buny-fuzz-"))
    families = {"ubuntu": UBUNTU, "arch": ARCH, "kiwi": SELF_INSTALL, "alpine": ALPINE}
    seeds = {}
    for name, files in families.items():
        p = tmp / f"{name}.iso"
        make_iso(p, name.upper(), files)
        seeds[name] = (files, p.read_bytes())
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    failures, refused = [], 0
    case = tmp / "case.iso"
    for n in range(iterations):
        name = rnd.choice(list(seeds))
        files, image = seeds[name]
        if rnd.random() < 0.3:
            # mangled boot configuration inside a valid ISO 9660 structure
            cfgs = {k: v for k, v in files.items() if k.endswith((".cfg", ".conf"))}
            mutated = dict(files)
            for k in cfgs:
                v = cfgs[k].encode() if isinstance(cfgs[k], str) else cfgs[k]
                mutated[k] = mangle_text(v, rnd)
            make_iso(case, name.upper(), mutated)
        else:
            data = bytearray(image)
            # the interesting structures: volume descriptors (sector 16+), path tables, directories
            hot = [(16 * 2048, 20 * 2048), (0, len(data))]
            for _ in range(rnd.randint(1, 40)):
                lo, hi = rnd.choice(hot)
                pos = rnd.randrange(lo, min(hi, len(data)))
                data[pos] = rnd.randrange(256) if rnd.random() < 0.8 else rnd.choice([0, 0xFF, 0x7F, 0x80])
            if rnd.random() < 0.1:
                data = data[: rnd.randrange(len(data))]  # truncated file
            case.write_bytes(bytes(data))
        try:
            r = subprocess.run([str(BUNYCTL), "probe", "--rules", str(RULES), "--entries", str(case)],
                               capture_output=True, text=True, errors="replace", timeout=20, env=env)
        except subprocess.TimeoutExpired:
            failures.append((n, name, "timeout"))
            shutil.copy(case, tmp / f"fail-{n}.iso")
            continue
        report = r.stderr
        if r.returncode < 0 or "AddressSanitizer" in report or "runtime error:" in report or "LeakSanitizer" in report:
            failures.append((n, name, report.strip().splitlines()[0] if report.strip() else f"signal {-r.returncode}"))
            shutil.copy(case, tmp / f"fail-{n}.iso")
        elif r.returncode != 0:
            refused += 1
    print(f"{iterations} corrupted images, seed {seed}: {len(failures)} failures, {refused} refused cleanly, "
          f"{iterations - refused - len(failures)} parsed")
    for f in failures[:20]:
        print("FAIL", *f)
    if failures:
        print(f"failing inputs kept in {tmp}")
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
