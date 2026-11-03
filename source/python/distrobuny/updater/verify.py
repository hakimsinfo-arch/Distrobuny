"""Integrity and authenticity verification for downloaded images.

Two independent checks protect every update:

  1. the image's SHA-256/SHA-512 digest must match the vendor's official
     checksum list;
  2. that checksum list (or the image itself, for vendors that sign images
     directly) must carry a valid OpenPGP signature from a key whose primary
     fingerprint is pinned in the update manifest.

Signatures are checked with gpg in a throw-away home directory containing
only the pinned vendor key, so the user's own keyring and trust settings
never influence the result.
"""
from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from ..util import WORKSPACE

KEY_DIR = WORKSPACE / "configuration" / "updates" / "keys"


class VerificationError(Exception):
    """A downloaded file failed an integrity or authenticity check."""


@dataclass
class ChecksumEntry:
    name: str
    digest: str
    algo: str


_GNU = re.compile(r"^([0-9a-fA-F]{64}|[0-9a-fA-F]{128})\s+\*?(\S.*)$")
_BSD = re.compile(r"^(SHA256|SHA512)\s*\((.+)\)\s*=\s*([0-9a-fA-F]+)$")


def parse_checksums(text: str) -> list[ChecksumEntry]:
    """Parse GNU ("<hex>  name" / "<hex> *name") and BSD ("SHA256 (name) = <hex>") lists."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("-----") or line.startswith("Hash:"):
            continue
        m = _GNU.match(line)
        if m:
            digest, name = m.group(1).lower(), m.group(2).strip()
            out.append(ChecksumEntry(name, digest, "sha256" if len(digest) == 64 else "sha512"))
            continue
        m = _BSD.match(line)
        if m:
            out.append(ChecksumEntry(m.group(2).strip(), m.group(3).lower(), m.group(1).lower()))
    return out


def file_digest(path: Path, algo: str, progress=None) -> str:
    h = hashlib.new(algo)
    size = path.stat().st_size
    done = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
            done += len(b)
            if progress:
                progress(done, size)
    return h.hexdigest()


def verify_digest(path: Path, expected: str, algo: str, progress=None) -> None:
    got = file_digest(path, algo, progress)
    if got.lower() != expected.lower():
        raise VerificationError(f"{path.name}: {algo.upper()} mismatch (expected {expected[:16]}…, got {got[:16]}…)")


def _gpg() -> str:
    g = shutil.which("gpg")
    if not g:
        raise VerificationError("gpg is required to verify signatures")
    return g


def verify_signature(data_file: Path, signature: Path | None, keyring: str, fingerprints: list[str],
                     key_dir: Path | None = None) -> str:
    """Verify a detached (signature given) or clear-signed (signature None) file.

    Returns the primary-key fingerprint that made the signature.  Raises
    VerificationError unless the signature is good *and* was made by one of
    the pinned fingerprints."""
    key = (key_dir or KEY_DIR) / keyring
    if not key.exists():
        raise VerificationError(f"pinned key file {key} is missing")
    pinned = {f.replace(" ", "").upper() for f in fingerprints}
    with tempfile.TemporaryDirectory(prefix="buny-gpg-") as home:
        base = [_gpg(), "--homedir", home, "--batch", "--no-tty", "--quiet", "--no-auto-key-retrieve",
                "--no-default-keyring", "--keyring", str(Path(home) / "vendor.kbx")]
        imp = subprocess.run(base + ["--import", str(key)], capture_output=True)
        if imp.returncode != 0:
            raise VerificationError(f"cannot import {keyring}: {imp.stderr.decode(errors='replace').strip()}")
        args = base + ["--status-fd", "1", "--verify"]
        args += [str(signature), str(data_file)] if signature else [str(data_file)]
        res = subprocess.run(args, capture_output=True)
        status = res.stdout.decode(errors="replace")
    valid = None
    for line in status.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "[GNUPG:]" and parts[1] == "VALIDSIG":
            # VALIDSIG <signing-key fpr> ... <primary-key fpr> is the last field
            valid = (parts[2].upper(), parts[-1].upper())
        if len(parts) >= 2 and parts[0] == "[GNUPG:]" and parts[1] in ("BADSIG", "ERRSIG", "EXPKEYSIG", "REVKEYSIG"):
            raise VerificationError(f"signature check failed: {' '.join(parts[1:3])}")
    if not valid:
        raise VerificationError("no valid signature found")
    if valid[0] not in pinned and valid[1] not in pinned:
        raise VerificationError(f"signed by {valid[1]}, which is not a pinned key for this source")
    return valid[1]


def clearsigned_body(path: Path) -> str:
    """The signed text of a clear-signed message (after verification)."""
    text = path.read_text(errors="replace")
    m = re.search(r"-----BEGIN PGP SIGNED MESSAGE-----\n(?:Hash:[^\n]*\n)*\n(.*?)\n-----BEGIN PGP SIGNATURE-----", text, re.S)
    if not m:
        raise VerificationError("not a clear-signed message")
    # dash-escaped lines start with "- "
    return "\n".join(line[2:] if line.startswith("- ") else line for line in m.group(1).splitlines())
