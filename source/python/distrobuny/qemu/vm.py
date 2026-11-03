"""QEMU virtual machine driver for Distro Buny tests.

Every VM runs on a *snapshot* of the disk image (QEMU -snapshot semantics via
a temporary qcow2 overlay), so tests never modify the image under test, and
nothing touches physical devices.  The VM is controlled over QMP:
screenshots, keyboard and pointer injection, power control.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

OVMF_CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
OVMF_VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
OVMF_CODE_SB = Path("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")
OVMF_VARS_MS = Path("/usr/share/OVMF/OVMF_VARS_4M.ms.fd")  # Microsoft keys enrolled, Secure Boot on

QCODES = {
    "Up": "up", "Down": "down", "Left": "left", "Right": "right", "Enter": "ret", "Esc": "esc",
    "Backspace": "backspace", "Tab": "tab", "Home": "home", "PageDown": "pgdn", "PageUp": "pgup",
    "Space": "spc", "F10": "f10", "Shift": "shift",
}


@dataclass
class VMConfig:
    image: Path
    workdir: Path
    memory_mb: int = 4096
    cpus: int = 4
    secure_boot: bool = False
    kernel: Path | None = None        # direct kernel boot (lab runtime tests)
    initrd: Path | None = None
    append: str = ""
    usb_storage: bool = True          # attach the image as a USB stick (realistic) instead of virtio
    resolution: tuple[int, int] | None = None
    extra: list[str] = field(default_factory=list)
    name: str = "buny"
    lab_options: str = ""             # buny.* options passed through QEMU fw_cfg (no image changes)
    disk_bytes: int | None = None     # a bigger drive with the image at its start (as dd/Etcher leave it)
    keep_disk: bool = False           # keep what the VM wrote after it stops (see VM.export_disk)


class QMP:
    def __init__(self, path: Path, timeout: float = 30):
        deadline = time.time() + timeout
        self.sock = None
        while time.time() < deadline:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(str(path))
                self.sock = s
                break
            except OSError:
                s.close()
                time.sleep(0.1)
        if not self.sock:
            raise RuntimeError("cannot connect to QMP")
        self.f = self.sock.makefile("rwb", buffering=0)
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self) -> dict:
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name: str, **args) -> dict:
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\n").encode())
        resp = self._read()
        if "error" in resp:
            raise RuntimeError(f"QMP {name}: {resp['error']}")
        return resp.get("return", {})

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class VM:
    def __init__(self, cfg: VMConfig):
        self.cfg = cfg
        cfg.workdir.mkdir(parents=True, exist_ok=True)
        self.serial_path = cfg.workdir / "serial.log"
        self.qmp_path = Path(tempfile.mkdtemp(prefix="buny-qmp-")) / "qmp.sock"
        self.proc: subprocess.Popen | None = None
        self.qmp: QMP | None = None
        self._serial_pos = 0
        self.start_time = 0.0

    # ----------------------------------------------------------- lifecycle
    def command_line(self) -> list[str]:
        c = self.cfg
        vars_copy = c.workdir / "OVMF_VARS.fd"
        shutil.copy(OVMF_VARS_MS if c.secure_boot else OVMF_VARS, vars_copy)
        code = OVMF_CODE_SB if c.secure_boot else OVMF_CODE
        overlay = c.workdir / "overlay.qcow2"
        if overlay.exists():
            overlay.unlink()
        subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw", "-b", str(c.image.resolve()), str(overlay)]
                       + ([str(c.disk_bytes)] if c.disk_bytes else []), check=True)
        args = [
            "qemu-system-x86_64", "-name", c.name, "-machine", "q35,smm=on" if c.secure_boot else "q35",
            "-enable-kvm", "-cpu", "host", "-m", str(c.memory_mb), "-smp", str(c.cpus),
            "-global", "ICH9-LPC.disable_s3=1",
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-tablet,bus=xhci.0",
            "-vga", "std",
            "-display", "none",
            "-serial", f"file:{self.serial_path}",
            "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
        ]
        if c.secure_boot:
            args += ["-global", "driver=cfi.pflash01,property=secure,value=on"]
        if c.usb_storage:
            args += ["-drive", f"id=bunydisk,file={overlay},format=qcow2,if=none,cache=unsafe",
                     "-device", "usb-storage,bus=xhci.0,drive=bunydisk,removable=on,bootindex=1"]
        else:
            args += ["-drive", f"file={overlay},format=qcow2,if=virtio,cache=unsafe"]
        if c.kernel:
            args += ["-kernel", str(c.kernel)]
            if c.initrd:
                args += ["-initrd", str(c.initrd)]
            if c.append:
                args += ["-append", c.append]
        if c.resolution:
            args += ["-global", f"VGA.xres={c.resolution[0]}", "-global", f"VGA.yres={c.resolution[1]}"]
        if c.lab_options:
            opts = c.workdir / "lab-options.txt"
            opts.write_text(c.lab_options)
            args += ["-fw_cfg", f"name=opt/distrobuny/cmdline,file={opts}"]
        args += c.extra
        return args

    def start(self) -> None:
        if self.serial_path.exists():
            self.serial_path.unlink()
        cmd = self.command_line()
        (self.cfg.workdir / "qemu-command.txt").write_text(" ".join(cmd) + "\n")
        self._out = open(self.cfg.workdir / "qemu.out", "wb")
        self.proc = subprocess.Popen(cmd, stdout=self._out, stderr=subprocess.STDOUT, start_new_session=True)
        self.start_time = time.time()
        self.qmp = QMP(self.qmp_path)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self) -> None:
        if self.qmp:
            try:
                self.qmp.cmd("quit")
            except Exception:
                pass
            self.qmp.close()
            self.qmp = None
        if self.proc:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        if getattr(self, "_out", None):
            self._out.close()
            self._out = None
        shutil.rmtree(self.qmp_path.parent, ignore_errors=True)
        overlay = self.cfg.workdir / "overlay.qcow2"
        if overlay.exists() and not self.cfg.keep_disk:
            overlay.unlink()

    def export_disk(self, out: Path) -> Path:
        """The VM's disk as a raw (sparse) image, after stop() with keep_disk."""
        subprocess.run(["qemu-img", "convert", "-O", "raw", str(self.cfg.workdir / "overlay.qcow2"), str(out)], check=True)
        return out

    def discard_disk(self) -> None:
        (self.cfg.workdir / "overlay.qcow2").unlink(missing_ok=True)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    # -------------------------------------------------------------- serial
    def serial(self) -> str:
        try:
            return self.serial_path.read_bytes().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_for(self, pattern: str, timeout: float, since_start: bool = False) -> re.Match | None:
        """Wait for a regex in the serial log (new output only, unless since_start)."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        pos = 0 if since_start else self._serial_pos
        while time.time() < deadline:
            text = self.serial()
            m = rx.search(text, pos)
            if m:
                self._serial_pos = m.end()
                return m
            if not self.alive():
                return None
            time.sleep(0.25)
        return None

    @property
    def serial_position(self) -> int:
        """Offset in the serial log up to which output has been consumed."""
        return self._serial_pos

    def mark(self) -> None:
        """Only consider serial output from now on."""
        self._serial_pos = len(self.serial())

    # ------------------------------------------------------------ control
    def screenshot(self, path: Path) -> Path:
        self.qmp.cmd("screendump", filename=str(path), format="png")
        return path

    def key(self, name: str, hold: float = 0.05) -> None:
        q = QCODES.get(name, name)
        ev = lambda down: {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}
        self.qmp.cmd("input-send-event", events=[ev(True)])
        time.sleep(hold)
        self.qmp.cmd("input-send-event", events=[ev(False)])

    def type_text(self, text: str) -> None:
        for ch in text:
            if ch == " ":
                self.key("spc")
            elif ch.isalnum():
                self.key(ch.lower())

    def pointer(self, x: int, y: int, screen: tuple[int, int]) -> None:
        """Move the absolute pointer (usb-tablet) to screen coordinates."""
        ax = int(x * 32767 / max(1, screen[0] - 1))
        ay = int(y * 32767 / max(1, screen[1] - 1))
        self.qmp.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ])

    def click(self, x: int, y: int, screen: tuple[int, int], button: str = "left") -> None:
        self.pointer(x, y, screen)
        time.sleep(0.1)
        self.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": button}}])
        time.sleep(0.08)
        self.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": button}}])

    def scroll(self, notches: int) -> None:
        button = "wheel-down" if notches > 0 else "wheel-up"
        for _ in range(abs(notches)):
            self.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": button}}])
            self.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": button}}])
