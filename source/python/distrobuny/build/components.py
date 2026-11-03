"""Fetch and verify pinned third-party boot components (configuration/boot/components.json)."""
from __future__ import annotations

import json
import re
import shutil
import urllib.request
from pathlib import Path

from .. import util
from ..util import WORKSPACE, BuildError, step

CONFIG = WORKSPACE / "configuration" / "boot" / "components.json"
CACHE = WORKSPACE / "build" / "cache" / "debs"
EXTRACT = WORKSPACE / "build" / "components"


def load() -> dict:
    return json.loads(CONFIG.read_text())


def fetch(name: str) -> Path:
    """Return the verified .deb for a component, downloading it if needed."""
    cfg = load()
    pkg = cfg["packages"][name]
    CACHE.mkdir(parents=True, exist_ok=True)
    deb = CACHE / Path(pkg["path"]).name
    if not deb.exists():
        url = pkg.get("archive", cfg["archive"]) + pkg["path"]
        step(f"downloading {deb.name}")
        tmp = deb.with_suffix(".partial")
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
        except OSError as exc:
            raise BuildError(f"cannot download {url}: {exc}") from exc
        tmp.rename(deb)
    digest = util.sha256_file(deb)
    if digest != pkg["sha256"]:
        raise BuildError(f"{deb.name}: SHA-256 mismatch (got {digest}, pinned {pkg['sha256']}); refusing to use it")
    return deb


def extract(name: str) -> Path:
    """Extract a verified component into build/components/<name> (cached by checksum)."""
    deb = fetch(name)
    dest = EXTRACT / name
    stamp = dest / ".sha256"
    want = load()["packages"][name]["sha256"]
    if stamp.exists() and stamp.read_text().strip() == want:
        return dest
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    util.run(["dpkg-deb", "-x", deb, dest])
    stamp.write_text(want + "\n")
    return dest


TEST_TOOLS = ("exfatprogs", "mtools")
HOST_TOOLS = WORKSPACE / "build" / "host-tools"


def test_tools() -> Path:
    """Unpack the pinned reference file-system tools used by the tests into build/host-tools."""
    HOST_TOOLS.mkdir(parents=True, exist_ok=True)
    for name in TEST_TOOLS:
        src = extract(name)
        for item in src.iterdir():
            if item.name == ".sha256":
                continue
            util.run(["cp", "-a", "--", str(item), str(HOST_TOOLS)])
    step(f"reference tools ready in {HOST_TOOLS} (fsck.exfat, mtools)")
    return HOST_TOOLS


def kernel_release() -> str:
    return load()["kernel_release"]


# ------------------------------------------------------------ licence notices

def upstream_version(version: str) -> str:
    """'1:1.36.1-6ubuntu3.1' -> '1.36.1'"""
    v = version.split(":", 1)[-1]
    return v.rsplit("-", 1)[0] if "-" in v else v


def shipped() -> list[dict]:
    """The components on every drive, with their licence and Ubuntu source packages."""
    out = []
    for key, pkg in load()["packages"].items():
        if not pkg.get("shipped"):
            continue
        version = next(iter(pkg["source"].values()))
        out.append({"component": key, "name": f"{pkg['name']} {upstream_version(version)}",
                    "license": re.split(r"[;(]", pkg["license"])[0].strip(), "source": pkg["source"],
                    "binary": Path(pkg["path"]).name})
    return out


def libc_source() -> tuple[str, str]:
    """glibc source package and version linked statically into the runtime programs (the build host's libc6-dev)."""
    r = util.run(["dpkg-query", "-W", "-f=${source:Package} ${source:Version}", "libc6-dev"], capture=True, check=False)
    parts = r.stdout.decode().split() if r.returncode == 0 else []
    if len(parts) != 2:
        raise BuildError("cannot determine the C library version (dpkg-query libc6-dev); it is needed for the licence notice")
    return parts[0], parts[1]


def check_sources() -> None:
    """The 'source' entries must match the source package recorded in each shipped .deb."""
    for c in shipped():
        res = util.run(["dpkg-deb", "-f", fetch(c["component"]), "Package", "Source", "Version"], capture=True)
        fields = dict(ln.split(": ", 1) for ln in res.stdout.decode().splitlines() if ": " in ln)
        m = re.fullmatch(r"(\S+)(?: \((\S+)\))?", fields.get("Source", fields["Package"]))
        name, version = m.group(1), m.group(2) or fields["Version"]
        if c["source"].get(name) != version:
            raise BuildError(f"configuration/boot/components.json: {c['component']} was built from {name} {version}, "
                             f"but its 'source' entry says {c['source']}")
