"""Compatibility matrix: boot every ISO through the complete Distro Buny chain.

For each image a fresh VM (UEFI firmware, image attached as a USB stick through
a throw-away snapshot) boots shim -> GRUB -> runtime -> interface, and the
interface starts the image exactly as a user would (lab "autoboot" option,
passed through QEMU fw_cfg so the image under test is never modified).

Verdicts are strict:
  PASS     the distribution's own user space came up from the ISO (login
           prompt, graphical/multi-user target, or its installer started),
           and - for the virtual medium - the medium was attached and verified
  FAIL     a classified failure (kexec refused, medium not found, distribution
           fell into an emergency shell, kernel panic, timeout ...)
  BLOCKED  Distro Buny reports the image as unsupported (no attempt made)
Evidence (matched log lines, screenshots, serial log) is kept for every run.
"""
from __future__ import annotations

import json
import re
import subprocess
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .vm import VM, VMConfig

# Evidence that the distribution's own user space is running, by adapter family.
SUCCESS = {
    "default": [r"login:", r"Reached target [^\n]*(Graphical Interface|Multi-User System|graphical\.target)"],
    "anaconda": [r"Starting installer, one moment", r"anaconda[^\n]*(started|for Fedora)", r"Reached target [^\n]*Multi-User"],
    # linuxrc: the installer itself, after it loaded the installation system from the medium
    "linuxrc": [r"\*\*\* Starting YaST \*\*\*", r"Started [^\n]*Agama", r"Reached target [^\n]*Multi-User"],
    # KIWI self-install images stop at their own "Destroying ALL data on <disk>"
    # question (the VM never answers it, and its disk is a throw-away snapshot).
    "kiwi": [r"login:", r"Reached target [^\n]*(Multi-User|Graphical)", r"Started [^\n]*Agama",
             r"Destroying ALL data on"],
    # Ubuntu Server: the subiquity installer service runs from the ISO's snaps.
    # (its serial console then shows "Continue in rich mode")
    "casper": [r"Started [^\n]*subiquity\.subiquity-(server|service)", r"Continue in rich mode"],
    # Shepherd (PID 1 from the store on the ISO) stops logging to the serial
    # console once syslog runs; its root-file-system service is the last
    # reliable sign there.  The installer itself only draws on tty1.
    "guix": [r"login:", r"This is the GNU system", r"shepherd\[1\]: Service root-file-system has been started"],
    # antiX/MX live-init reports when it hands over to the live system's init
    # (which then only draws on the screen)
    "antix": [r"login:", r"The initrd init program took [0-9.]+ seconds"],
    # the Slackware installer's own greeting, its first question, its login prompt
    "slackware": [r"login:", r"Welcome to the Slackware Linux installation disk", r"Enter 1 to select a keyboard map"],
    # debian-installer: network detection starts only after cdrom-detect mounted
    # the medium and anna loaded the installer's components from it
    "debian-installer": [r"Detecting network hardware", r"Detect network hardware", r"Configure the network"],
}
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
# Signs that the boot failed, checked after the hand-off.
FAILURE = [
    (r"Kernel panic - not syncing[^\n]*", "kernel panic"),
    (r"(Entering emergency mode|You are in emergency mode)[^\n]*", "distribution emergency shell"),
    (r"dracut-initqueue[^\n]*timeout[^\n]*", "dracut timed out waiting for the medium"),
    (r"(Could not find the ISO|Unable to find a live file system)[^\n]*", "live system not found"),
    (r"ERROR: (device|Device) '[^']*' not found[^\n]*", "medium not found by initramfs"),
    (r"distrobuny-medium: [^\n]*(giving up|could not|does not contain)[^\n]*", "virtual medium failed"),
    (r"(BusyBox v[^\n]*built-in shell|\(initramfs\) $)", "initramfs shell"),
    (r"Failed to load installer component[^\n]*", "the Debian installer could not load its components"),
    (r"(Retry mounting installation media|Incorrect installation media detected)[^\n]*", "the Debian installer did not find its medium"),
]


@dataclass
class Result:
    path: str
    name: str = ""
    adapter: str = ""
    label: str = ""
    size: int = 0
    verdict: str = "NOT TESTED"
    reason: str = ""
    evidence: list[str] = field(default_factory=list)
    timings: dict = field(default_factory=dict)
    screenshots: list[str] = field(default_factory=list)
    secure_boot: bool = False
    started: str = ""
    seconds: float = 0.0


def slug(path: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", path.strip("/"))[:120]


def run_one(image: Path, entry: dict, outdir: Path, secure_boot: bool = False, memory: int = 4096,
            success_timeout: float = 420, extra_options: str = "", settle: float = 40) -> Result:
    path = entry["path"]
    meta = entry.get("meta", {})
    r = Result(path=path, name=entry.get("display", ""), adapter=meta.get("adapter") or "", label=meta.get("label") or "",
               size=entry.get("size", 0), secure_boot=secure_boot, started=time.strftime("%Y-%m-%d %H:%M:%S"))
    work = outdir / slug(path)
    work.mkdir(parents=True, exist_ok=True)
    if meta.get("level") not in ("prepared", "verified"):
        r.verdict = "BLOCKED"
        r.reason = meta.get("reason") or "not supported by Distro Buny"
        return r
    cfg = VMConfig(image=image, workdir=work, secure_boot=secure_boot, memory_mb=memory,
                   lab_options=f'buny.lab=1 "buny.autoboot={path}" {extra_options}'.strip(), name=slug(path)[:40])
    t0 = time.time()

    def stamp(key):
        r.timings[key] = round(time.time() - t0, 1)

    def shot(name):
        p = work / f"{name}.png"
        try:
            vm.screenshot(p)
            r.screenshots.append(str(p))
        except Exception:
            pass

    vm = VM(cfg)
    try:
        vm.start()
        if not vm.wait_for(r"BUNY-EVENT ui-ready", 120):
            r.verdict, r.reason = "FAIL", "the Distro Buny interface did not start"
            shot("no-ui")
            return r
        stamp("ui_ready")
        m = vm.wait_for(r"BUNY-EVENT (boot-loaded|boot-load-failed|boot-error|autoboot-missing)[^\n]*", 240)
        if not m or "boot-loaded" not in m.group(0):
            r.verdict = "FAIL"
            r.reason = m.group(0) if m else "the hand-off was never prepared"
            time.sleep(1)
            shot("error")
            return r
        r.evidence.append(m.group(0)[:400])
        stamp("kernel_loaded")
        if not vm.wait_for(r"BUNY-EVENT handoff", 30):
            r.verdict, r.reason = "FAIL", "the interface did not hand off"
            return r
        stamp("handoff")
        virtual = "rdinit=/distrobuny/preinit" in m.group(0)
        deadline = time.time() + success_timeout
        pats = SUCCESS.get(r.adapter, []) + SUCCESS["default"]
        rx_ok = re.compile("|".join(f"(?:{p})" for p in pats))
        attached = False
        shot_taken = False
        handoff_pos = vm.serial_position
        while time.time() < deadline and vm.alive():
            text = ANSI.sub("", vm.serial()[handoff_pos:])  # live-init and others colour their output
            if virtual and not attached:
                am = re.search(r"distrobuny-medium: attached[^\n]*", text)
                if am:
                    attached = True
                    r.evidence.append(am.group(0).strip())
                    stamp("medium_attached")
            for frx, why in FAILURE:
                fm = re.search(frx, text)
                if fm:
                    r.verdict, r.reason = "FAIL", f"{why}: {fm.group(0).strip()[:200]}"
                    time.sleep(3)
                    shot("failure")
                    return r
            om = rx_ok.search(text)
            if om and (attached or not virtual):
                r.evidence.append(om.group(0).strip()[:200])
                stamp("userspace")
                r.verdict = "PASS"
                r.reason = "distribution user space reached" + (" via the virtual medium" if virtual else "")
                break
            if not shot_taken and time.time() - t0 > r.timings["handoff"] + 25:
                shot("early-boot")
                shot_taken = True
            time.sleep(1)
        if r.verdict != "PASS":
            r.verdict = "FAIL"
            r.reason = "timed out waiting for the distribution" + ("" if attached or not virtual else " (virtual medium never attached)")
            shot("timeout")
            return r
        # let graphical sessions and installers settle, then capture what the user would see
        time.sleep(settle)
        shot("final")
        return r
    finally:
        r.seconds = round(time.time() - t0, 1)
        vm.stop()
        (work / "result.json").write_text(json.dumps(asdict(r), indent=2) + "\n")


def image_index(image: Path, bunyctl: Path) -> dict | None:
    """The catalog stored on an image (/.distrobuny/index.json), read through
    the partition table and the runtime's own exFAT mapper."""
    from ..image import gpt

    with open(image, "rb") as f:
        try:
            disk = gpt.read_tables(f)
        except ValueError:
            return None
        part = next((p for p in disk.partitions if p.name == "DISTROBUNY"), None)
        if not part:
            return None
        r = subprocess.run([str(bunyctl), "map", "--image", str(image), "--iso-path", "/.distrobuny/index.json"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None
        lines = r.stdout.split("\n")
        size = int(lines[0].split()[1])
        data = b""
        for line in lines[1:]:
            if line.startswith("extent "):
                off, length = map(int, line.split()[1:3])
                f.seek(part.offset + off)
                data += f.read(length)
        if len(data) != size:
            return None
    return json.loads(data)


def flatten(index: dict) -> list[dict]:
    out = []

    def walk(n):
        if n["kind"] == "iso":
            out.append(n)
        for c in n.get("children", []):
            walk(c)

    walk(index["root"])
    return out
