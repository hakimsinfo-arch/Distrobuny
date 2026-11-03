"""Distro Buny build pipeline steps."""
from __future__ import annotations

import json
import os
import platform
import re
import shutil
import tempfile
from pathlib import Path

from .. import util
from ..util import WORKSPACE, BuildError, info, step
from . import image_build

BUILD = WORKSPACE / "build"
OUTPUT = WORKSPACE / "output"
ESP_TREE = BUILD / "esp"
BUNYCTL = BUILD / "bin" / "bunyctl"

DEV_PROFILE = [r"/archlinux-x86_64\.iso$", r"void-live", r"Gentoo-amd64-minimal", r"debian-.*netinst", r"guix-system"]
# The starter image (the public download) holds no ISO images.  3.5 GiB fits
# every stick sold as "4 GB" (3.6-3.8 GiB); compressed it is about the size
# of the boot partition, because the empty data partition is all zeros.
STARTER_GIB = 3.5


def version() -> str:
    return (WORKSPACE / "VERSION").read_text().strip()


# ------------------------------------------------------------------ doctor

REQUIRED_TOOLS = {
    "gcc": "C compiler (runtime, interface, bunyctl)",
    "make": "build driver",
    "python3": "build pipeline",
    "qemu-system-x86_64": "virtual machine tests",
    "zstd": "initramfs compression",
    "cpio": "inspecting initramfs archives (tests)",
    "sbsign": "Secure Boot signing (optional)",
    "openssl": "key generation (optional)",
    "apt-get": "fetching the signed runtime kernel and boot chain packages",
    "dpkg-deb": "unpacking Debian packages",
    "gpg": "updater signature verification",
    "qemu-img": "virtual machine test disks",
    "fsck.fat": "FAT32 checks in the image tests",
    "genisoimage": "ISO fixtures for the interface tests",
    "xz": "release image compression",
    "soffice": "release PDFs (LibreOffice)",
}

# Ubuntu packages that provide what doctor() checks, for its install hint.
UBUNTU_PACKAGES = {
    "gcc": "build-essential", "make": "build-essential", "python3": "python3",
    "qemu-system-x86_64": "qemu-system-x86", "zstd": "zstd", "cpio": "cpio", "sbsign": "sbsigntool",
    "openssl": "openssl", "apt-get": "apt", "dpkg-deb": "dpkg", "gpg": "gnupg", "qemu-img": "qemu-utils",
    "fsck.fat": "dosfstools", "genisoimage": "genisoimage", "xz": "xz-utils",
    "soffice": "libreoffice-writer-nogui", "PIL": "python3-pil",
    "/usr/share/OVMF/OVMF_CODE_4M.fd": "ovmf", "/usr/lib/x86_64-linux-gnu/libc.a": "libc6-dev",
}


def doctor() -> int:
    info(f"Distro Buny {version()} build environment on {platform.platform()}")
    missing = []
    for tool, why in REQUIRED_TOOLS.items():
        path = util.which(tool)
        if not path:
            missing.append(tool)
        print(f"  [{'ok ' if path else 'MISSING'}] {tool:<22} {why}")
    try:
        import PIL  # noqa: F401  (the logo and icons are processed with Pillow)
        ok = True
    except ImportError:
        ok = False
        missing.append("PIL")
    print(f"  [{'ok ' if ok else 'MISSING'}] {'Python Pillow':<22} logo and icon processing (runtime build)")
    for f, why in [("/usr/share/OVMF/OVMF_CODE_4M.fd", "UEFI firmware for VM tests"),
                   ("/usr/lib/x86_64-linux-gnu/libc.a", "static libc for runtime binaries"),
                   ("/dev/kvm", "hardware virtualisation for fast VM tests")]:
        ok = os.path.exists(f)
        if not ok:
            missing.append(f)
        print(f"  [{'ok ' if ok else 'MISSING'}] {f:<22} {why}")
    st = os.statvfs(WORKSPACE)
    free = st.f_bavail * st.f_frsize
    print(f"  free space in workspace: {util.human_size(free)}")
    packages = sorted({UBUNTU_PACKAGES[m] for m in missing if m in UBUNTU_PACKAGES})
    if packages:
        print(f"  on Ubuntu: sudo apt install {' '.join(packages)}")
    if "/dev/kvm" in missing:
        print("  /dev/kvm: enable virtualisation in the firmware settings, then add yourself to the kvm group")
    return 0 if not missing else 1


# ------------------------------------------------------------------ bunyctl

def make_host_tools() -> None:
    step("building host tools (make host)")
    util.run(["make", "-C", WORKSPACE, "host", f"-j{os.cpu_count() or 2}"], capture=True)


def catalog_index(iso_root: Path, data_uuid: str, includes: list[str] | None = None) -> bytes:
    """Generate .distrobuny/index.json for the image with the real data partition UUID."""
    make_host_tools()
    with tempfile.TemporaryDirectory(prefix="buny-", dir=BUILD) as tmp:
        index = Path(tmp) / "index.json"
        util.run([BUNYCTL, "scan", "--rules", WORKSPACE / "configuration/distributions/distributions.conf",
                  "--uuid", data_uuid, "--fs", "exfat", "--index", index, iso_root], capture=True)
        data = json.loads(index.read_text())
    if includes:
        _prune(data["root"], includes)
    return (json.dumps(data, separators=(",", ":")) + "\n").encode()


def _prune(node: dict, includes: list[str]) -> bool:
    if node["kind"] == "iso":
        return any(re.search(p, node["path"]) for p in includes)
    node["children"] = [c for c in node.get("children", []) if _prune(c, includes)]
    return bool(node["children"]) or node["path"] == "/"


# ------------------------------------------------------------------ images

def include_filter(pats: list[str]):
    return (lambda rel: any(re.search(p, rel) for p in pats)) if pats else None


def volume_files(iso_root: Path, build_id: str, profile: str, includes: list[str] | None) -> dict[str, bytes]:
    """The files every Distro Buny data partition carries besides the ISO images."""
    data_uuid = image_build.fs_uuid_from_serial(image_build.serial_for(build_id, "data"))
    step(f"cataloguing ISO images (data partition UUID {data_uuid})")
    index = catalog_index(iso_root, data_uuid, includes or None)
    volume = {
        "product": "Distro Buny",
        "version": version(),
        "build_id": build_id,
        "profile": profile,
        "created": util.source_date_epoch(),
    }
    return {
        ".distrobuny/volume.json": (json.dumps(volume, indent=2) + "\n").encode(),
        ".distrobuny/index.json": index,
        "README.txt": README_TXT.encode(),
    }


def empty_collection() -> Path:
    """An ISO collection with only the standard folders (the starter image)."""
    root = BUILD / "starter-collection"
    shutil.rmtree(root, ignore_errors=True)
    for d in ("Desktop", "Servers"):
        (root / d).mkdir(parents=True)
    return root


def build_disk_image(profile: str, iso_root: Path, out: Path | None, includes: list[str], free_gib: float | None,
                     with_runtime: bool, build_id: str | None, size_gib: float | None = None) -> Path:
    BUILD.mkdir(exist_ok=True)
    (OUTPUT / "images").mkdir(parents=True, exist_ok=True)
    (OUTPUT / "manifests").mkdir(parents=True, exist_ok=True)
    (OUTPUT / "checksums").mkdir(parents=True, exist_ok=True)
    if profile == "starter":
        if includes:
            raise BuildError("the starter profile holds no ISO images; --include does not apply")
        iso_root = empty_collection()
        size_gib = size_gib or STARTER_GIB
    if not iso_root.is_dir():
        raise BuildError(f"ISO collection {iso_root} does not exist")
    pats = list(includes)
    if profile == "dev" and not pats:
        pats = DEV_PROFILE
    build_id = build_id or f"distrobuny-{version()}-{profile}"
    out = out or OUTPUT / "images" / f"{build_id}.img"
    if with_runtime and not (ESP_TREE / "EFI" / "BOOT" / "BOOTX64.EFI").exists():
        raise BuildError("the boot chain has not been built yet (run: buny-build esp)")
    info(f"building {profile} image {out.name}")
    data_files = volume_files(iso_root, build_id, profile, pats)
    free = int((free_gib if free_gib is not None else (2 if profile != "full" else 8)) * (1 << 30))
    flt = include_filter(pats)
    esp_files = {} if with_runtime else {"README.txt": b"Distro Buny data-only test image: no boot chain.\n"}
    disk_bytes = int(size_gib * (1 << 30)) // (1 << 20) * (1 << 20) if size_gib else None
    # images are written to sticks of any size: the runtime can grow their data partition
    manifest = image_build.build_image(iso_root, out, build_id, esp_tree=ESP_TREE if with_runtime else None,
                                       esp_files=esp_files, data_files=data_files, free_bytes=free, iso_filter=flt,
                                       timestamp=util.source_date_epoch(), disk_bytes=disk_bytes, growable=True)
    manifest["version"] = version()
    manifest["profile"] = profile
    manifest["with_runtime"] = with_runtime
    step("computing image checksum (SHA-256)")
    digest = util.sha256_file(out)
    manifest["sha256"] = digest
    (OUTPUT / "checksums" / f"{out.name}.sha256").write_text(f"{digest}  {out.name}\n")
    image_build.write_manifest(OUTPUT / "manifests" / f"{out.name}.json", manifest)
    info(f"image ready: {out} ({util.human_size(out.stat().st_size)}), sha256 {digest[:16]}…")
    return out


def refresh_image_esp(image: Path, build_id: str | None) -> None:
    if not (ESP_TREE / "EFI" / "BOOT" / "BOOTX64.EFI").exists():
        raise BuildError("the boot chain has not been built yet (run: buny-build esp)")
    m = re.match(r"(distrobuny-.*)\.img$", image.name)
    build_id = build_id or (m.group(1) if m else image.stem)
    image_build.refresh_esp(image, build_id, ESP_TREE, timestamp=util.source_date_epoch())
    # keep the published checksum and manifest true to the image
    manifest_path = OUTPUT / "manifests" / f"{image.name}.json"
    if image.resolve().parent == (OUTPUT / "images").resolve() and manifest_path.exists():
        step("recomputing the image checksum (SHA-256)")
        digest = util.sha256_file(image)
        manifest = json.loads(manifest_path.read_text())
        manifest["sha256"] = digest
        manifest["esp_refreshed"] = util.source_date_epoch()
        image_build.write_manifest(manifest_path, manifest)
        (OUTPUT / "checksums" / f"{image.name}.sha256").write_text(f"{digest}  {image.name}\n")
        info(f"sha256 {digest}")


README_TXT = """Distro Buny — portable Linux multiboot drive

Add .iso files to the Desktop or Servers folder (or to sub-folders you create
inside them).  Folders become categories in the boot menu automatically; the
menu refreshes the next time the drive boots.

Do not edit or delete the hidden .distrobuny folder.

To update images safely, use the Distro Buny maintenance tool
(tools/maintenance/buny-update) from a Linux computer.
"""
