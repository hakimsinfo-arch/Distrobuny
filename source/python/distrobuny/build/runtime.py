"""Build the Distro Buny runtime: signed kernel + generated initramfs."""
from __future__ import annotations

import gzip
import json
import os
import re
import shutil
import stat
from pathlib import Path

from .. import util
from ..util import WORKSPACE, BuildError, info, step
from . import assets, components

OUT = WORKSPACE / "build" / "runtime"
ROOTFS = WORKSPACE / "build" / "runtime-root"

# Driver directories (relative to lib/modules/<release>/kernel) the runtime
# needs: storage controllers, USB, input (keyboards, mice, touchpads incl. the
# I2C-HID stack of modern laptops), file systems for the data partition and
# virtio for virtual machines.  GPU drivers are deliberately excluded: the
# interface uses the firmware framebuffer (simpledrm), which leaves graphics
# hardware untouched for the distribution that is booted next.
MODULE_DIRS = [
    "drivers/usb/host", "drivers/usb/storage", "drivers/usb/common", "drivers/usb/core", "drivers/usb/misc",
    "drivers/usb/typec", "drivers/hid", "drivers/input/mouse", "drivers/input/keyboard", "drivers/input/serio",
    "drivers/input/tablet", "drivers/input/touchscreen", "drivers/input/misc", "drivers/input/rmi4",
    "drivers/ata", "drivers/nvme", "drivers/mmc", "drivers/scsi", "drivers/block", "drivers/virtio",
    "drivers/i2c/busses", "drivers/i2c", "drivers/mfd", "drivers/pinctrl/intel", "drivers/pinctrl/pinctrl-amd",
    "drivers/platform/x86/amd", "drivers/thunderbolt", "drivers/md", "drivers/firmware/efi",
    "drivers/firmware/qemu_fw_cfg.ko",
    "fs/exfat", "fs/fat", "fs/ntfs3", "fs/isofs", "fs/udf", "fs/nls", "fs/efivarfs", "fs/fuse",
    "drivers/char/virtio_console", "drivers/acpi",
]
EXCLUDE = re.compile(r"/(gpu|sound|media|net|wireless|bluetooth|infiniband|staging)/|nvidia|amdgpu|i915|nouveau|radeon")

BUSYBOX_APPLETS = [
    "sh", "ash", "cat", "ls", "mount", "umount", "mkdir", "dmesg", "ps", "kill", "sleep", "echo", "grep", "less",
    "blkid", "losetup", "reboot", "poweroff", "halt", "sync", "uname", "free", "df", "du", "head", "tail", "vi",
    "hexdump", "lsmod", "insmod", "rmmod", "modprobe", "find", "stat", "true", "false", "test", "[", "env", "cp",
    "mv", "rm", "ln", "chmod", "chown", "dd", "md5sum", "sha256sum", "wc", "sed", "awk", "tr", "cut", "sort",
    "uptime", "top", "clear", "setsid", "cttyhack", "switch_root",
]


def _module_closure(moddir: Path) -> list[str]:
    deps: dict[str, list[str]] = {}
    for line in (moddir / "modules.dep").read_text().splitlines():
        if ":" not in line:
            continue
        mod, rest = line.split(":", 1)
        deps[mod] = rest.split()
    wanted = set()
    for mod in deps:
        rel = mod[len("kernel/"):] if mod.startswith("kernel/") else mod
        if EXCLUDE.search("/" + rel):
            continue
        if any(rel.startswith(d + "/") or rel.startswith(d + ".") or rel == d for d in MODULE_DIRS):
            wanted.add(mod)
    # dependency closure
    stack = list(wanted)
    while stack:
        m = stack.pop()
        for d in deps.get(m, []):
            if d not in wanted:
                wanted.add(d)
                stack.append(d)
    return sorted(wanted)


def _write_module_indexes(moddir: Path, dest: Path, mods: list[str]) -> None:
    keep = set(mods)
    lines = [ln for ln in (moddir / "modules.dep").read_text().splitlines() if ln.split(":", 1)[0] in keep]
    (dest / "modules.dep").write_text("\n".join(lines) + "\n")
    names = {re.sub(r"\.ko(\..*)?$", "", Path(m).name).replace("-", "_") for m in mods}
    alias_lines = []
    for ln in (moddir / "modules.alias").read_text().splitlines():
        parts = ln.split()
        if len(parts) == 3 and parts[0] == "alias" and parts[2].replace("-", "_") in names:
            alias_lines.append(ln)
    (dest / "modules.alias").write_text("\n".join(alias_lines) + "\n")
    shutil.copy2(moddir / "modules.builtin", dest / "modules.builtin")
    if (moddir / "modules.builtin.modinfo").exists():
        shutil.copy2(moddir / "modules.builtin.modinfo", dest / "modules.builtin.modinfo")


def _cpio_newc(root: Path, out_f, mtime: int) -> None:
    """Deterministic newc archive of a directory tree (sorted, root-owned)."""
    ino = 0

    def put(name: str, mode: int, data: bytes = b"", nlink: int = 1) -> None:
        nonlocal ino
        ino += 1
        nb = name.encode() + b"\0"
        hdr = "070701" + "".join(f"{v:08X}" for v in (ino, mode, 0, 0, nlink, mtime, len(data), 0, 0, 0, 0, len(nb), 0))
        out_f.write(hdr.encode() + nb)
        out_f.write(b"\0" * ((4 - (110 + len(nb)) % 4) % 4))
        out_f.write(data)
        out_f.write(b"\0" * ((4 - len(data) % 4) % 4))

    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for n in dirnames + sorted(filenames):
            entries.append(Path(dirpath) / n)
    entries.sort(key=lambda p: str(p.relative_to(root)))
    for p in entries:
        rel = str(p.relative_to(root))
        st = os.lstat(p)
        perm = stat.S_IMODE(st.st_mode)
        if stat.S_ISLNK(st.st_mode):
            put(rel, stat.S_IFLNK | 0o777, os.readlink(p).encode())
        elif stat.S_ISDIR(st.st_mode):
            put(rel, stat.S_IFDIR | perm, nlink=2)
        else:
            put(rel, stat.S_IFREG | perm, p.read_bytes())
    put("TRAILER!!!", 0)


def build_runtime(compress: str = "zstd") -> dict:
    """Build vmlinuz + initramfs.img into build/runtime/.  Returns a summary."""
    info("building the Distro Buny runtime")
    rel = components.kernel_release()
    kimg = components.extract("linux-image")
    kmods = components.extract("linux-modules")
    vmlinuz = kimg / "boot" / f"vmlinuz-{rel}"
    moddir = kmods / "lib" / "modules" / rel
    if not vmlinuz.exists() or not moddir.exists():
        raise BuildError(f"kernel {rel} not found in the extracted packages")
    if not (moddir / "modules.dep").exists():
        # the package's postinst normally runs depmod; do it inside the build tree
        depmod = util.which("depmod")
        if not depmod:
            raise BuildError("depmod (kmod) is required to index the kernel modules")
        step("indexing kernel modules (depmod)")
        util.run([depmod, "-b", kmods, rel], capture=True)

    step("compiling runtime programs (static)")
    util.run(["make", "-C", WORKSPACE, "runtime", f"-j{os.cpu_count() or 2}"], capture=True)
    bindir = WORKSPACE / "build" / "runtime-bin"

    if ROOTFS.exists():
        shutil.rmtree(ROOTFS)
    for d in ["bin", "sbin", "dev", "proc", "sys", "run", "tmp", "root", "etc", "usr/bin", "usr/lib/distrobuny",
              "usr/share/distrobuny", f"lib/modules/{rel}"]:
        (ROOTFS / d).mkdir(parents=True, exist_ok=True)
    os.chmod(ROOTFS / "tmp", 0o1777)
    shutil.copy2(bindir / "buny-init", ROOTFS / "init")
    shutil.copy2(bindir / "buny-ui", ROOTFS / "usr/bin/buny-ui")
    shutil.copy2(bindir / "buny-preinit", ROOTFS / "usr/lib/distrobuny/preinit")
    for p in [ROOTFS / "init", ROOTFS / "usr/bin/buny-ui", ROOTFS / "usr/lib/distrobuny/preinit"]:
        os.chmod(p, 0o755)

    # pinned static BusyBox from the Ubuntu archive, so the image does not depend on the build host
    busybox = components.extract("busybox-static") / "usr" / "bin" / "busybox"
    if busybox.exists():
        shutil.copy2(busybox, ROOTFS / "bin/busybox")
        os.chmod(ROOTFS / "bin/busybox", 0o755)
        for a in BUSYBOX_APPLETS:
            link = ROOTFS / "bin" / a
            if not link.exists():
                link.symlink_to("busybox")
    else:
        util.warn("no static busybox found: the runtime will have no emergency shell")

    step("selecting kernel modules")
    mods = _module_closure(moddir)
    dest = ROOTFS / "lib" / "modules" / rel
    for m in mods:
        target = dest / m
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(moddir / m, target)
    _write_module_indexes(moddir, dest, mods)

    share = ROOTFS / "usr/share/distrobuny"
    shutil.copy2(WORKSPACE / "configuration/distributions/distributions.conf", share / "distributions.conf")
    compat = WORKSPACE / "configuration/distributions/compatibility.json"
    if compat.exists():
        shutil.copy2(compat, share / "compatibility.json")
    assets.install_runtime_assets(share)

    (ROOTFS / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\nbuny:x:100:100:Distro Buny interface:/tmp:/bin/false\n")
    (ROOTFS / "etc/group").write_text("root:x:0:\ntty:x:5:\nbuny:x:100:\n")
    (ROOTFS / "etc/hostname").write_text("distrobuny\n")
    (ROOTFS / "etc/os-release").write_text(
        f'NAME="Distro Buny Runtime"\nID=distrobuny\nVERSION="{(WORKSPACE / "VERSION").read_text().strip()}"\n')

    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(vmlinuz, OUT / "vmlinuz")
    raw = OUT / "initramfs.cpio"
    mtime = util.source_date_epoch()
    with open(raw, "wb") as f:
        _cpio_newc(ROOTFS, f, mtime)
    img = OUT / "initramfs.img"
    step(f"compressing initramfs ({compress})")
    if compress == "zstd":
        util.run(["zstd", "-q", "-f", "-19", "-T0", raw, "-o", img])
    else:
        with open(raw, "rb") as src, gzip.GzipFile(img, "wb", mtime=0) as dst:
            shutil.copyfileobj(src, dst)
    raw.unlink()
    summary = {
        "kernel_release": rel,
        "vmlinuz_sha256": util.sha256_file(OUT / "vmlinuz"),
        "initramfs_sha256": util.sha256_file(img),
        "initramfs_bytes": img.stat().st_size,
        "modules": len(mods),
    }
    (OUT / "runtime.json").write_text(json.dumps(summary, indent=2) + "\n")
    info(f"runtime ready: kernel {rel}, {len(mods)} modules, initramfs {util.human_size(img.stat().st_size)}")
    return summary
