"""Assemble the public release folder (output/release) from the starter image.

Everything in the folder may go on a public download page, as one unit:

  distrobuny-<v>-starter.img.xz          the starter image (no ISO files), xz-compressed
  SHA256SUMS                             every file of the folder, and the unpacked image
  Distro Buny <v> - User Guide.pdf       using Distro Buny (documentation/release/user-guide.html)
  Distro Buny <v> - How it was made.pdf  overview of the design (documentation/release/how-it-was-made.html)
  Branding/                              the Distro Buny logo and mark, and the 1.0.1 trailer
  Licenses and source code/
    SOURCE-CODE.txt                      what the GPL/LGPL source is and where it is in this folder
    LICENSES.txt, licenses/              the notices and licence texts that are also on the drive
    distrobuny-<v>-source.tar.xz         Distro Buny's own source code (GPL-3.0-or-later)
    third-party/                         the complete Ubuntu source packages of the shipped binaries

The third-party source is shipped with the image ("accompanied by the
complete corresponding source"), so no written offer is needed.  Fetch it
once with `buny-build sources`.  verify_release() checks a finished folder:
every file listed with its correct SHA-256, nothing unlisted, every checksum
quoted in a document matching, no unfinished text.

Images with ISO files are refused: those files belong to their publishers.
"""
from __future__ import annotations

import hashlib
import io
import json
import lzma
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

from .. import util
from ..util import WORKSPACE, BuildError, info, step
from . import components

OUTPUT = WORKSPACE / "output"
RELEASE = OUTPUT / "release"
SOURCES = WORKSPACE / "build" / "third-party-sources"
LEGAL = "Licenses and source code"
DOCUMENTS = {"user-guide.html": "User Guide", "how-it-was-made.html": "How it was made"}
BRANDING = {"Distro buny logo.png": WORKSPACE / "Distro buny logo.png",
            "distrobuny-mark.png": WORKSPACE / "assets" / "branding" / "distrobuny-mark.png",
            # the owner's corrected 1.0.1 trailer (media/ is not part of the source archive)
            "Distro Buny 1.0.1 Trailer.mp4": WORKSPACE / "media" / "Distro Buny 1.0.1 Trailer.mp4"}
# text that must never reach a published folder
UNFINISHED = re.compile(r"fill in before publishing|\[CONTACT|PLACEHOLDER|@[A-Z_]+@|\bTODO\b", re.I)

# Distro Buny's source: these top-level entries, without build products and test-run logs
SOURCE_ENTRIES = ["Makefile", "VERSION", "README.md", "CHANGELOG.md", "COPYING", "Distro buny logo.png", "assets", "configuration",
                  "documentation", "source", "tests", "tools"]
SOURCE_EXCLUDE = {"tests/compatibility/runs"}  # VM logs and screenshots of test runs


def version() -> str:
    return (WORKSPACE / "VERSION").read_text().strip()


# ------------------------------------------------------------------ checks

def check_image(image: Path) -> dict:
    manifest_path = OUTPUT / "manifests" / f"{image.name}.json"
    if not manifest_path.exists():
        raise BuildError(f"{manifest_path} is missing: build the image with buny-build image --profile starter")
    m = json.loads(manifest_path.read_text())
    if m.get("profile") != "starter":
        raise BuildError(f"{image.name} is a '{m.get('profile')}' image; only the starter image (no ISO files) "
                         "may be published")
    isos = [f for f in m["layout"]["files"] if f.lower().endswith(".iso")]
    if isos:
        raise BuildError(f"{image.name} contains ISO files ({', '.join(isos[:3])}); refusing to publish it")
    if not m.get("with_runtime"):
        raise BuildError(f"{image.name} has no boot chain")
    step("verifying the image checksum")
    digest = util.sha256_file(image)
    if digest != m["sha256"]:
        raise BuildError(f"{image.name} does not match its manifest (sha256 {digest}, manifest {m['sha256']}); "
                         "rebuild it")
    m["sha256"] = digest
    return m


def esp_files(image: Path, m: dict, dest: Path) -> dict:
    """Copy the licence notices out of the image's own boot partition (so they match the image)."""
    tools = WORKSPACE / "build" / "host-tools" / "usr" / "bin"
    if not (tools / "mcopy").exists():
        components.test_tools()
    offset = m["geometry"]["esp_partition"]["start_lba"] * 512
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    src = f"{image}@@{offset}"
    util.run([tools / "mcopy", "-n", "-i", src, "::/distrobuny/LICENSES.txt", dest / "LICENSES.txt"], env=env, capture=True)
    util.run([tools / "mcopy", "-n", "-s", "-i", src, "::/distrobuny/licenses", dest], env=env, capture=True)
    build = util.run([tools / "mtype", "-i", src, "::/distrobuny/build.json"], env=env, capture=True).stdout
    build = json.loads(build.decode())
    if build.get("lab"):
        raise BuildError(f"{image.name} was built with lab automation (buny-build esp --lab); rebuild the ESP "
                         "without --lab and the image")
    return build


# ------------------------------------------------------------------ artefacts

def compress_image(image: Path, out: Path) -> str:
    step(f"compressing {image.name} (xz)")
    tmp = out.with_suffix(".xz.partial")
    with open(tmp, "wb") as f:
        res = subprocess.run(["xz", "-T0", "-6", "--block-size=64MiB", "-c", str(image)], stdout=f)
    if res.returncode != 0:
        raise BuildError(f"xz failed with status {res.returncode}")
    step("checking the compressed image (decompress and compare SHA-256)")
    proc = subprocess.Popen(["xz", "-dc", str(tmp)], stdout=subprocess.PIPE)
    h = hashlib.sha256()
    for chunk in iter(lambda: proc.stdout.read(4 << 20), b""):
        h.update(chunk)
    if proc.wait() != 0:
        raise BuildError(f"xz cannot decompress {tmp}")
    tmp.rename(out)
    return h.hexdigest()


def source_tarball(out: Path, epoch: int) -> None:
    """Distro Buny's own source as a reproducible tar.xz (sorted, fixed owner and time)."""
    step(f"packing the Distro Buny source ({out.name})")
    prefix = f"distrobuny-{version()}"

    def norm(ti: tarfile.TarInfo) -> tarfile.TarInfo:
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        ti.mtime = epoch
        ti.mode = 0o755 if ti.isdir() or ti.mode & 0o100 else 0o644
        return ti

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        def add(path: Path) -> None:
            rel = path.relative_to(WORKSPACE)
            tar.add(path, arcname=f"{prefix}/{rel}", recursive=False, filter=norm)
            if path.is_dir() and not path.is_symlink():
                for child in sorted(path.iterdir()):
                    if child.name != "__pycache__" and str(child.relative_to(WORKSPACE)) not in SOURCE_EXCLUDE:
                        add(child)
        tar.add(WORKSPACE, arcname=prefix, recursive=False, filter=norm)
        for entry in SOURCE_ENTRIES:
            add(WORKSPACE / entry)
    out.write_bytes(lzma.compress(buf.getvalue(), preset=9))


def source_rows() -> tuple[list[dict], list[tuple[str, str]]]:
    """Notice rows and the unique (source package, version) pairs to fetch."""
    rows = components.shipped()
    libc, libc_version = components.libc_source()
    rows.append({"component": "libc6-dev", "name": f"GNU C Library {components.upstream_version(libc_version)}",
                 "license": "LGPL-2.1-or-later", "source": {libc: libc_version},
                 "binary": f"libc6-dev {libc_version} (statically linked into buny-init, buny-ui, buny-preinit)"})
    pairs: list[tuple[str, str]] = []
    for r in rows:
        for pair in r["source"].items():
            if pair not in pairs:
                pairs.append(pair)
    exfat = components.load()["packages"]["exfatprogs"]["source"]
    pairs += [p for p in exfat.items() if p not in pairs]
    return rows, pairs


def source_notice(rows: list[dict], tar_name: str, tar_sha: str, image_name: str, image_sha: str, epoch: int) -> str:
    table = []
    for r in rows:
        srcs = list(r["source"].items())
        for i, (name, ver) in enumerate(srcs):
            left = f"{r['name']:<28}{r['license']:<20}" if i == 0 else " " * 48
            note = "  (signing only)" if i and "signed" in name else ""
            table.append(f"   {left}{name} {ver}{note}")
        table.append(f"   {'':<48}binary: {r['binary']}")
    # the examples use the first GPL component
    ex = next(iter(next(r for r in rows if r["license"].startswith("GPL"))["source"].items()))
    return SOURCE_NOTICE.format(
        v=version(), rule="=" * len(f"Distro Buny {version()} — source code"), image=image_name, image_sha=image_sha,
        tar=tar_name, tar_sha=tar_sha, epoch=epoch, table="\n".join(table), legal=LEGAL,
        lp_example=f"https://launchpad.net/ubuntu/+source/{ex[0]}/{ex[1]}", ex_dsc=dsc_name(*ex))


def get_sources_sh(pairs: list[tuple[str, str]]) -> str:
    calls = "\n".join(f"get {name} {ver}" for name, ver in pairs)
    return GET_SOURCES.replace("@VERSION@", version()).replace("@CALLS@", calls)


# ------------------------------------------------------ third-party source

def dsc_name(pkg: str, ver: str) -> str:
    return f"{pkg}_{ver.split(':', 1)[-1]}.dsc"


def dsc_files(dsc: Path) -> list[tuple[str, str]]:
    """(sha256, file name) of every file a .dsc lists."""
    out, inside = [], False
    for line in dsc.read_text(errors="replace").splitlines():
        if line.startswith("Checksums-Sha256:"):
            inside = True
            continue
        if inside and line.startswith(" "):
            f = line.split()
            if len(f) == 3:
                out.append((f[0], f[2]))
        elif inside:
            break
    return out


def check_third_party(folder: Path, pairs: list[tuple[str, str]]) -> list[str]:
    """Every source package of `pairs` is complete in `folder`; returns the problems."""
    problems = []
    for pkg, ver in pairs:
        dsc = folder / dsc_name(pkg, ver)
        if not dsc.is_file():
            problems.append(f"{dsc.name} is missing")
            continue
        listed = dsc_files(dsc)
        if not listed:
            problems.append(f"{dsc.name} lists no files")
        for digest, name in listed:
            f = folder / name
            if not f.is_file():
                problems.append(f"{name} ({dsc.name}) is missing")
            elif util.sha256_file(f) != digest:
                problems.append(f"{name} does not match {dsc.name}")
    return problems


def fetch_sources(dest: Path = SOURCES) -> Path:
    """Download the third-party source packages from Launchpad (verified against their .dsc)."""
    _, pairs = source_rows()
    dest.mkdir(parents=True, exist_ok=True)
    script = dest.parent / "get-sources.sh"
    script.write_text(get_sources_sh(pairs))
    step(f"downloading {len(pairs)} source packages from Launchpad into {dest}")
    util.run(["sh", script, dest])
    problems = check_third_party(dest, pairs)
    if problems:
        raise BuildError("the third-party source is incomplete: " + "; ".join(problems))
    return dest


def copy_third_party(src: Path, dest: Path, pairs: list[tuple[str, str]]) -> None:
    problems = check_third_party(src, pairs)
    if problems:
        raise BuildError("the third-party source in " + str(src) + " is incomplete (run: buny-build sources): " +
                         "; ".join(problems[:5]))
    dest.mkdir(parents=True)
    for pkg, ver in pairs:
        dsc = src / dsc_name(pkg, ver)
        for name in [dsc.name] + [n for _, n in dsc_files(dsc)]:
            if not (dest / name).exists():
                shutil.copy2(src / name, dest / name)


# --------------------------------------------------------------- documents

def render_documents(out: Path, values: dict[str, str]) -> list[Path]:
    """The release's PDF documents, from documentation/release/*.html (LibreOffice)."""
    soffice = util.which("soffice") or util.which("libreoffice")
    if not soffice:
        raise BuildError("LibreOffice (soffice) is needed to turn documentation/release/*.html into the release PDFs")
    src = WORKSPACE / "documentation" / "release"
    made = []
    with tempfile.TemporaryDirectory(prefix="buny-docs-", dir=WORKSPACE / "build") as tmp:
        work = Path(tmp) / "release"
        work.mkdir()
        shutil.copytree(WORKSPACE / "documentation" / "screenshots", Path(tmp) / "screenshots")
        for name, title in DOCUMENTS.items():
            text = (src / name).read_text()
            for k, v in values.items():
                text = text.replace(f"@{k}@", v)
            left = sorted(set(re.findall(r"@[A-Z_]+@", text)))
            if left:
                raise BuildError(f"documentation/release/{name}: no value for {', '.join(left)}")
            (work / name).write_text(text)
            step(f"rendering {title} (PDF)")
            util.run([soffice, "--headless", "--convert-to", "pdf:writer_web_pdf_Export", "--outdir", work, work / name],
                     capture=True)
            pdf = out / f"Distro Buny {values['VERSION']} - {title}.pdf"
            shutil.move(work / name.replace(".html", ".pdf"), pdf)
            made.append(pdf)
    return made


# ------------------------------------------------------------ verification

def write_sums(folder: Path, image_name: str, image_sha: str) -> None:
    """SHA256SUMS: every file of the folder (relative paths), and the image once unpacked."""
    rows = {image_name: image_sha}
    for f in sorted(folder.rglob("*")):
        if f.is_file() and f.name != "SHA256SUMS":
            rows[str(f.relative_to(folder))] = util.sha256_file(f)
    (folder / "SHA256SUMS").write_text("".join(f"{d}  {n}\n" for n, d in rows.items()))


def xz_sha256(path: Path) -> str:
    proc = subprocess.Popen(["xz", "-dc", str(path)], stdout=subprocess.PIPE)
    h = hashlib.sha256()
    for chunk in iter(lambda: proc.stdout.read(4 << 20), b""):
        h.update(chunk)
    if proc.wait() != 0:
        raise BuildError(f"xz cannot decompress {path}")
    return h.hexdigest()


def verify_release(folder: Path, unpack: bool = True) -> dict[str, str]:
    """Check a release folder; raises BuildError listing every inconsistency.

    * every file is listed in SHA256SUMS with its correct SHA-256, and every
      listed file exists, except the unpacked image, which is checked by
      decompressing its .xz (unpack=False skips that slow step);
    * every SHA-256 quoted in the folder's own text files is in SHA256SUMS
      next to the same file name, if the line names one;
    * the third-party source packages are complete (their .dsc checksums);
    * no unfinished text (placeholders) is left in the folder's own documents.
    """
    folder = Path(folder)
    problems: list[str] = []
    sums_file = folder / "SHA256SUMS"
    if not sums_file.is_file():
        raise BuildError(f"{folder} has no SHA256SUMS")
    sums: dict[str, str] = {}
    for line in sums_file.read_text().splitlines():
        m = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not m:
            problems.append(f"SHA256SUMS: malformed line {line!r}")
            continue
        sums[m.group(2)] = m.group(1)
    for name, want in sums.items():
        f = folder / name
        if f.is_file():
            have = util.sha256_file(f)
        elif name.endswith(".img") and (folder / f"{name}.xz").is_file():
            if not unpack:
                continue
            step(f"checking {name} (decompressing {name}.xz)")
            have = xz_sha256(folder / f"{name}.xz")
        else:
            problems.append(f"{name}: listed in SHA256SUMS but not in the folder")
            continue
        if have != want:
            problems.append(f"{name}: SHA-256 is {have}, SHA256SUMS says {want}")
    for f in sorted(folder.rglob("*")):
        rel = str(f.relative_to(folder))
        if f.is_file() and f != sums_file and rel not in sums:
            problems.append(f"{rel}: not listed in SHA256SUMS")
    by_hash = {d: n for n, d in sums.items()}
    own_texts = [p for p in (folder / LEGAL / "SOURCE-CODE.txt", folder / LEGAL / "LICENSES.txt") if p.is_file()]
    if not (folder / LEGAL / "SOURCE-CODE.txt").is_file():
        problems.append(f"{LEGAL}/SOURCE-CODE.txt is missing")
    for t in own_texts:
        text = t.read_text()
        for n, line in enumerate(text.splitlines(), 1):
            for h in re.findall(r"\b[0-9a-f]{64}\b", line):
                if h not in by_hash:
                    problems.append(f"{t.name}:{n}: quotes SHA-256 {h[:16]}…, which matches no file of the release")
            if UNFINISHED.search(line):
                problems.append(f"{t.name}:{n}: unfinished text: {line.strip()[:80]}")
        # "file ... SHA-256 hash" on consecutive lines must agree
        for m in re.finditer(r"([\w.+~-]+\.(?:tar\.xz|img\.xz|img))\s+SHA-256 ([0-9a-f]{64})", text):
            named = [n for n in sums if n == m.group(1) or n.endswith("/" + m.group(1))]
            if named and sums[named[0]] != m.group(2):
                problems.append(f"{t.name}: quotes {m.group(2)[:16]}… for {m.group(1)}, SHA256SUMS has {sums[named[0]][:16]}…")
    third = folder / LEGAL / "third-party"
    if third.is_dir():
        _, pairs = source_rows()
        problems += [f"third-party: {p}" for p in check_third_party(third, pairs)]
    else:
        problems.append(f"{LEGAL}/third-party (the complete third-party source) is missing")
    if problems:
        raise BuildError(f"the release folder {folder} is inconsistent:\n  " + "\n  ".join(problems))
    return sums


def publish(release: Path, dest: Path) -> Path:
    """Copy a verified release folder to `dest` and verify the copy.

    `dest` may only contain files that are already part of this release (same
    path, same SHA-256), for example branding someone placed there by hand;
    anything else is left alone and refused."""
    sums = verify_release(release, unpack=False)
    dest = Path(dest)
    if dest.exists():
        foreign = []
        for f in sorted(dest.rglob("*")):
            rel = str(f.relative_to(dest))
            if f.is_file() and (rel not in sums or util.sha256_file(f) != sums[rel]):
                foreign.append(rel)
        if foreign:
            raise BuildError(f"{dest} holds files that are not part of this release ({', '.join(foreign[:5])}): "
                             "move them, or the previous release, out of it first")
    dest.mkdir(parents=True, exist_ok=True)
    for item in sorted(release.rglob("*")):
        target = dest / item.relative_to(release)
        if item.is_dir():
            target.mkdir(exist_ok=True)
        elif not target.exists():
            shutil.copy2(item, target)
    step(f"verifying the published copy in {dest}")
    verify_release(dest)
    info(f"published {release.name} to {dest}")
    return dest


# ------------------------------------------------------------------ main

def build_release(image: Path, out: Path | None = None, sources: Path | None = None) -> Path:
    out = out or RELEASE
    sources = sources or SOURCES
    image = image.resolve()
    info(f"preparing the release of {image.name} in {out}")
    components.check_sources()
    m = check_image(image)
    rows, pairs = source_rows()
    problems = check_third_party(sources, pairs)
    if problems:
        raise BuildError(f"the third-party source in {sources} is incomplete (run: buny-build sources): " +
                         "; ".join(problems[:5]))
    if out.exists():
        shutil.rmtree(out)
    legal = out / LEGAL
    legal.mkdir(parents=True)
    epoch = util.source_date_epoch()
    esp_files(image, m, legal)

    xz_name = f"{image.name}.xz"
    if compress_image(image, out / xz_name) != m["sha256"]:
        raise BuildError("the compressed image does not decompress to the original")
    tar_name = f"distrobuny-{version()}-source.tar.xz"
    source_tarball(legal / tar_name, epoch)
    tar_sha = util.sha256_file(legal / tar_name)
    step("copying the third-party source packages")
    copy_third_party(sources, legal / "third-party", pairs)
    (legal / "SOURCE-CODE.txt").write_text(source_notice(rows, tar_name, tar_sha, image.name, m["sha256"], epoch))
    render_documents(out, {"VERSION": version(), "IMAGE_XZ": xz_name, "IMAGE_SHA": m["sha256"]})
    (out / "Branding").mkdir()
    for name, src in BRANDING.items():
        shutil.copy2(src, out / "Branding" / name)
    step("writing SHA256SUMS")
    write_sums(out, image.name, m["sha256"])
    for p in sorted(out.rglob("*")):
        if p.is_file():
            os.utime(p, (epoch, epoch))
    step("verifying the release folder")
    verify_release(out)
    total = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    info(f"release ready: {out} ({util.human_size(total)}); image {xz_name} "
         f"{util.human_size((out / xz_name).stat().st_size)}, sha256 {util.sha256_file(out / xz_name)[:16]}…")
    return out


# ------------------------------------------------------------------ texts

SOURCE_NOTICE = """Distro Buny {v} — source code
{rule}

The starter image {image}
(SHA-256 {image_sha} once unpacked)
contains free software published under the GNU General Public License (GPL)
and the GNU Lesser General Public License (LGPL).  You may copy, change and
share that software under those licences, and you are entitled to its
complete corresponding source code.  This release ships all of it, in the
folder "{legal}".  The licence texts are in its licenses
folder, and in distrobuny/licenses on the drive's BUNY-EFI partition.


1. Distro Buny (GPL-3.0-or-later)
   buny-init, buny-ui, buny-preinit and the build and maintenance tools.

     {tar}
     SHA-256 {tar_sha}

   documentation/BUILD_INSTRUCTIONS.md in it explains how to rebuild the
   image (this one was built on Ubuntu 24.04 with SOURCE_DATE_EPOCH={epoch}).


2. Third-party software: unmodified binary packages from Ubuntu 24.04 LTS
   ("noble").  Its corresponding source is these Ubuntu source packages, in
   exactly these versions:

   Component                   Licence             Ubuntu source package and version
{table}

   * A "-signed" source package only adds Microsoft's or Canonical's Secure
     Boot signature to binaries built from the source package above it.
   * The GNU C Library is linked statically into buny-init, buny-ui and
     buny-preinit.  To use them with a modified C library, rebuild them from
     the Distro Buny source in section 1 (make runtime).
   * shim is BSD-licensed; it is listed for completeness.
   * The exFAT up-case table that the image writer puts on the data partition
     is the table defined by the Microsoft exFAT specification, in the bytes
     of exfatprogs 1.2.2; its source package is included as well.


3. Where the third-party source is

   The folder third-party next to this file holds every source package listed
   above exactly as Ubuntu publishes it: the .dsc file, the upstream tarball
   and Ubuntu's changes.  SHA256SUMS in the release folder lists every file;
   each .dsc also lists the SHA-256 of its own files.  Unpack a package with

       dpkg-source -x {ex_dsc}

   The same files are on Launchpad, which keeps every published version of
   every Ubuntu package, e.g.
       {lp_example}

   If you pass this image on, pass this folder on with it.
"""

GET_SOURCES = r"""#!/bin/sh
# Distro Buny @VERSION@: download the exact Ubuntu source packages of the
# third-party software in the image from Launchpad (buny-build sources),
# and verify every file against the SHA-256 sums in its .dsc file.
#
#   sh get-sources.sh [DIRECTORY]      (default: ./sources)
set -eu

dest=${1:-sources}
mkdir -p "$dest"
cd "$dest"

fetch() {  # URL FILE
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2.part" "$1"
    else
        wget -q -O "$2.part" "$1"
    fi
    mv "$2.part" "$2"
}

get() {  # SOURCE-PACKAGE VERSION
    pkg=$1 ver=$2
    dsc="${pkg}_${ver#*:}.dsc"
    base="https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/$pkg/$ver"
    echo "== $pkg $ver"
    [ -s "$dsc" ] || fetch "$base/$dsc" "$dsc"
    awk '/^Checksums-Sha256:/ {f = 1; next} /^[^ ]/ {f = 0} f && NF == 3 {print $1, $3}' "$dsc" |
    while read -r sum file; do
        # files from an earlier run are kept if they are intact
        if ! echo "$sum  $file" | sha256sum -c --quiet - >/dev/null 2>&1; then
            fetch "$base/$file" "$file"
            echo "$sum  $file" | sha256sum -c --quiet -
        fi
    done
}

@CALLS@

echo "all source packages downloaded and verified in $(pwd)"
"""
