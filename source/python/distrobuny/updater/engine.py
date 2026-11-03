"""The Distro Buny update engine.

Workflow for one image:

  inventory  -> which source manages the installed file, and its version
  check      -> discover the newest release from the vendor's *signed*
                checksum list (the signature is verified before anything is
                reported, so tampered metadata cannot announce an update)
  download   -> resumable download into <root>/.distrobuny/staging
  verify     -> digest from the signed list (+ image signature where the
                vendor signs images); a failed check deletes the download
  apply      -> only after explicit approval: the verified image is moved
                into the image's folder with an atomic rename, the old image
                is removed only after the new one is in place, and the boot
                menu index is refreshed.

A working image is never overwritten by an unverified file.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..util import WORKSPACE
from . import http, verify

SOURCES = WORKSPACE / "configuration" / "updates" / "sources.json"
BUNYCTL = WORKSPACE / "build" / "bin" / "bunyctl"
RULES = WORKSPACE / "configuration" / "distributions" / "distributions.conf"


class UpdateError(Exception):
    pass


# ------------------------------------------------------------- versions

def version_key(v: str) -> tuple:
    """Natural ordering: '26.04.1' > '26.04', '44-1.10' > '44-1.7', dates compare numerically."""
    parts = re.findall(r"\d+|[A-Za-z]+", v or "")
    return tuple((0, int(p)) if p.isdigit() else (1, p.lower()) for p in parts)


def is_lts_series(series: str) -> bool:
    m = re.match(r"(\d{2})\.04$", series)
    return bool(m) and int(m.group(1)) % 2 == 0


# ------------------------------------------------------------- manifest

@dataclass
class Source:
    id: str
    name: str
    raw: dict
    keys: dict

    def get(self, key, default=None):
        return self.raw.get(key, default)


def load_sources(path: Path = SOURCES) -> list[Source]:
    data = json.loads(path.read_text())
    templates = data.get("templates", {})
    keys = data.get("keys", {})
    out = []
    for s in data["sources"]:
        merged = dict(templates.get(s.get("template"), {}))
        merged.update(s)
        if "pick" not in merged and "installed" in merged:
            merged["pick"] = merged["installed"]
        for required in ("id", "name", "installed", "pick", "download"):
            if required not in merged:
                raise UpdateError(f"source {s.get('id', '?')}: missing '{required}'")
        if "checksums" not in merged and "checksums_index" not in merged:
            raise UpdateError(f"source {merged['id']}: no checksum list")
        sig = merged.get("signature")
        if sig and sig.get("key") not in keys:
            raise UpdateError(f"source {merged['id']}: key {sig.get('key')} is not pinned")
        out.append(Source(merged["id"], merged["name"], merged, keys))
    return out


def fill(template: str, ctx: dict) -> str:
    return re.sub(r"\{(\w+)\}", lambda m: str(ctx.get(m.group(1), m.group(0))), template)


# ------------------------------------------------------------ inventory

@dataclass
class Installed:
    path: str                  # relative to the data root, e.g. /Desktop/ubuntu/x.iso
    file: str
    size: int
    version: str | None
    source: Source | None
    groups: dict = field(default_factory=dict)
    meta: dict = field(default_factory=dict)


def scan(root: Path) -> list[dict]:
    if not BUNYCTL.exists():
        raise UpdateError("bunyctl is not built (run: make host)")
    res = subprocess.run([str(BUNYCTL), "scan", "--json", "--rules", str(RULES), str(root)], capture_output=True)
    if res.returncode != 0:
        raise UpdateError(f"scanning {root} failed: {res.stderr.decode(errors='replace').strip()}")
    data = json.loads(res.stdout)
    out = []

    def walk(n):
        if n["kind"] == "iso":
            out.append(n)
        for c in n.get("children", []):
            walk(c)

    walk(data["root"])
    return out


def inventory(root: Path, sources: list[Source]) -> list[Installed]:
    items = []
    for n in scan(root):
        inst = Installed(n["path"], n["name"], n.get("size", 0), None, None, meta=n.get("meta", {}))
        for s in sources:
            m = re.match(s.get("installed"), n["name"])
            if m:
                inst.source = s
                inst.groups = {k: v for k, v in m.groupdict().items() if v}
                inst.version = inst.groups.get("version")
                if not inst.version or s.get("installed_version_from") == "metadata":
                    inst.version = inst.meta.get("version") or inst.version
                break
        items.append(inst)
    return items


# ---------------------------------------------------------------- check

@dataclass
class Candidate:
    source: Source
    version: str
    file: str
    url: str
    digest: str
    algo: str
    signer: str
    signature_url: str | None = None   # image-detached signatures
    size: int | None = None
    series: str | None = None


def _discover_series(src: Source, installed: Installed, ctx: dict) -> str | None:
    spec = src.get("series")
    if not spec:
        return None
    html = http.fetch_text(fill(spec["index"], ctx))
    found = sorted(set(re.findall(spec["regex"], html)), key=version_key)
    if not found:
        raise UpdateError(f"{src.name}: no releases found at {fill(spec['index'], ctx)}")
    if spec.get("policy") == "track" and installed.version:
        cur = re.match(r"\d{2}\.\d{2}", installed.version)
        if cur and is_lts_series(cur.group(0)):
            lts = [s for s in found if is_lts_series(s)]
            return lts[-1] if lts else cur.group(0)
    return found[-1]


def check(inst: Installed, workdir: Path) -> Candidate | None:
    """Return the newest verified release for an installed image, or None if up to date."""
    src = inst.source
    if not src:
        raise UpdateError(f"no trusted update source is configured for {inst.file}")
    ctx = {"flavor": src.get("flavor", "")}
    ctx.update(inst.groups)
    series = _discover_series(src, inst, ctx)
    if series:
        ctx["series"] = series
    if src.get("checksums_index"):
        ci = src.get("checksums_index")
        listing = http.fetch_text(fill(ci["url"], ctx))
        names = sorted(set(re.findall(ci["regex"], listing)), key=version_key)
        if not names:
            raise UpdateError(f"{src.name}: no checksum list found for release {series}")
        ctx["checksums"] = fill(ci["url"], ctx) + names[-1]
    else:
        ctx["checksums"] = fill(src.get("checksums"), ctx)

    workdir.mkdir(parents=True, exist_ok=True)
    sums = workdir / f"{src.id}-checksums"
    http.fetch_bytes(ctx["checksums"], sums)
    sig = src.get("signature")
    signer = "unsigned (HTTPS only)"
    body = sums.read_text(errors="replace")
    if sig and sig["type"] in ("detached", "clearsigned"):
        sig_file = None
        if sig["type"] == "detached":
            sig_file = workdir / f"{src.id}-checksums.sig"
            http.fetch_bytes(fill(sig["url"], ctx), sig_file)
        signer = verify.verify_signature(sums, sig_file, sig["key"], src.keys[sig["key"]])
        if sig["type"] == "clearsigned":
            body = verify.clearsigned_body(sums)

    # values captured from the installed name (e.g. the Mint edition) narrow the pick pattern
    pick = re.compile(fill(src.get("pick"), {k: re.escape(v) for k, v in inst.groups.items()}))
    best = None
    for e in verify.parse_checksums(body):
        m = pick.match(e.name.lstrip("*"))
        if not m:
            continue
        ver = m.group("version")
        if best is None or version_key(ver) > version_key(best[0]):
            best = (ver, e)
    if not best:
        raise UpdateError(f"{src.name}: the checksum list names no matching image")
    ver, entry = best
    if inst.version and version_key(ver) <= version_key(inst.version):
        return None
    ctx["file"] = entry.name.lstrip("*")
    url = fill(src.get("download"), ctx)
    cand = Candidate(src, ver, ctx["file"], url, entry.digest, entry.algo, signer, series=series)
    if sig and sig["type"] == "image-detached":
        cand.signature_url = fill(sig["url"], ctx)
        cand.signer = "image signature (verified after download)"
    cand.size = http.remote_size(url)
    return cand


# ------------------------------------------------------------- download

def is_volume(root: Path) -> bool:
    return (root / ".distrobuny" / "volume.json").exists()


def metadata_dir(root: Path) -> Path:
    """Scratch space for release metadata.  Read-only commands never write into root."""
    import hashlib
    base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "distrobuny-update"
    d = base / hashlib.sha256(str(root.resolve()).encode()).hexdigest()[:16]
    d.mkdir(parents=True, exist_ok=True)
    return d


def staging_dir(root: Path) -> Path:
    """Downloads are staged on the drive itself so the final move is an atomic rename."""
    if not is_volume(root):
        raise UpdateError(f"{root} is not a Distro Buny data partition (no .distrobuny/volume.json); "
                          "updates are only installed onto Distro Buny drives")
    d = root / ".distrobuny" / "staging"
    d.mkdir(parents=True, exist_ok=True)
    return d


def download_and_verify(root: Path, cand: Candidate, progress=None, verify_progress=None) -> Path:
    st = os.statvfs(root)
    free = st.f_bavail * st.f_frsize
    if cand.size and free < cand.size + (256 << 20):
        raise UpdateError(f"not enough free space on the drive: {cand.size} bytes needed, {free} available")
    part = staging_dir(root) / (cand.file + ".part")
    http.download(cand.url, part, cand.size, progress)
    try:
        verify.verify_digest(part, cand.digest, cand.algo, verify_progress)
        sig = cand.source.get("signature")
        if cand.signature_url:
            sig_file = part.with_name(cand.file + ".sig")
            http.fetch_bytes(cand.signature_url, sig_file)
            cand.signer = verify.verify_signature(part, sig_file, sig["key"], cand.source.keys[sig["key"]])
            sig_file.unlink()
    except verify.VerificationError:
        # never keep a file that failed verification
        part.unlink(missing_ok=True)
        part.with_name(part.name + ".json").unlink(missing_ok=True)
        raise
    verified = part.with_name(cand.file + ".verified")
    os.replace(part, verified)
    part.with_name(part.name + ".json").unlink(missing_ok=True)
    return verified


# ---------------------------------------------------------------- apply

def apply(root: Path, inst: Installed, cand: Candidate, verified: Path, remove_old: bool = True, log=None) -> Path:
    """Place a verified image next to the one it updates.  Requires the caller to have obtained approval."""
    old = root / inst.path.lstrip("/")
    target = old.parent / cand.file
    if target.exists() and target != old:
        raise UpdateError(f"{target} already exists")
    os.replace(verified, target)          # atomic within the data partition
    os.sync()
    if remove_old and old.exists() and old != target:
        old.unlink()
        os.sync()
    record = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "source": cand.source.id, "from": inst.path,
              "from_version": inst.version, "to": str(target.relative_to(root)), "to_version": cand.version,
              "sha": cand.digest, "signer": cand.signer}
    logf = root / ".distrobuny" / "updates.log"
    with open(logf, "a") as f:
        f.write(json.dumps(record) + "\n")
    if log:
        log(record)
    refresh_index(root)
    return target


def refresh_index(root: Path) -> bool:
    """Regenerate the boot menu's metadata index if root is a Distro Buny volume."""
    meta = root / ".distrobuny"
    if not (meta / "volume.json").exists():
        return False
    uuid = fs_uuid(root)
    with tempfile.TemporaryDirectory() as tmp:
        idx = Path(tmp) / "index.json"
        args = [str(BUNYCTL), "scan", "--rules", str(RULES), "--fs", "exfat", "--index", str(idx), str(root)]
        if uuid:
            args[2:2] = ["--uuid", uuid]
        subprocess.run(args, capture_output=True, check=True)
        shutil.copyfile(idx, meta / "index.json.tmp")
        os.replace(meta / "index.json.tmp", meta / "index.json")
    return True


def fs_uuid(root: Path) -> str | None:
    """UUID of the file system mounted at (or containing) root, without privileges."""
    root = root.resolve()
    best, dev = "", None
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        parts = line.split()
        mnt = parts[4].replace("\\040", " ")
        src = parts[parts.index("-") + 2]
        if (str(root) + "/").startswith(mnt.rstrip("/") + "/") and len(mnt) > len(best):
            best, dev = mnt, src
    if not dev or not dev.startswith("/dev/"):
        return None
    by_uuid = Path("/dev/disk/by-uuid")
    real = os.path.realpath(dev)
    for link in by_uuid.iterdir() if by_uuid.exists() else []:
        if os.path.realpath(link) == real:
            return link.name
    return None


def find_data_root() -> Path | None:
    """Locate a mounted Distro Buny data partition (label DISTROBUNY with .distrobuny/volume.json)."""
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        mnt = Path(line.split()[4].replace("\\040", " "))
        if (mnt / ".distrobuny" / "volume.json").exists():
            return mnt
    return None
