"""Shared helpers for Distro Buny host tooling.

Only the Python standard library is used so the tools run on any recent
Linux distribution without extra packages.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[3]
NAMESPACE = uuid.UUID("6f1d3b2a-8c4e-4b8e-9b6e-62756e790001")  # Distro Buny UUID namespace

_COLOR = sys.stderr.isatty() and os.environ.get("NO_COLOR") is None


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _COLOR else text


def info(msg: str) -> None:
    print(f"{_c('32', '==>')} {msg}", file=sys.stderr, flush=True)


def step(msg: str) -> None:
    print(f"  {_c('36', '->')} {msg}", file=sys.stderr, flush=True)


def warn(msg: str) -> None:
    print(f"{_c('33', 'warning:')} {msg}", file=sys.stderr, flush=True)


def error(msg: str) -> None:
    print(f"{_c('31', 'error:')} {msg}", file=sys.stderr, flush=True)


class BuildError(RuntimeError):
    """A build step failed with a message meant for the user."""


def run(cmd, *, cwd=None, env=None, capture=False, check=True, input=None) -> subprocess.CompletedProcess:
    """Run a command with a clear error message on failure."""
    try:
        res = subprocess.run(
            [str(c) for c in cmd],
            cwd=cwd,
            env=env,
            input=input,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
        )
    except FileNotFoundError as exc:
        raise BuildError(f"command not found: {cmd[0]}") from exc
    if check and res.returncode != 0:
        detail = ""
        if capture and res.stderr:
            detail = ": " + res.stderr.decode(errors="replace").strip()[-2000:]
        raise BuildError(f"{' '.join(str(c) for c in cmd)} failed with status {res.returncode}{detail}")
    return res


def sha256_file(path: Path, chunk: int = 4 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def stable_uuid(*parts: str) -> uuid.UUID:
    """Deterministic UUID derived from build inputs (reproducible images)."""
    return uuid.uuid5(NAMESPACE, "/".join(parts))


def source_date_epoch() -> int:
    """Build timestamp: SOURCE_DATE_EPOCH if set (reproducible builds), else now."""
    val = os.environ.get("SOURCE_DATE_EPOCH")
    return int(val) if val and val.isdigit() else int(time.time())


def human_size(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return f"{n:.1f} TiB"


def write_json(path: Path, data) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=False) + "\n")
    os.replace(tmp, path)


def which(name: str) -> str | None:
    return shutil.which(name, path=os.environ.get("PATH", "") + ":/usr/sbin:/sbin")


def copy_range(src_fd: int, dst_fd: int, count: int, src_off: int, dst_off: int) -> None:
    """Copy bytes between files efficiently (copy_file_range, falling back to pread/pwrite)."""
    done = 0
    use_cfr = hasattr(os, "copy_file_range")
    while done < count:
        n = min(count - done, 1 << 30)
        if use_cfr:
            try:
                w = os.copy_file_range(src_fd, dst_fd, n, src_off + done, dst_off + done)
            except OSError:
                use_cfr = False
                continue
            if w == 0:
                raise BuildError("unexpected end of file while copying")
            done += w
        else:
            buf = os.pread(src_fd, min(n, 8 << 20), src_off + done)
            if not buf:
                raise BuildError("unexpected end of file while copying")
            os.pwrite(dst_fd, buf, dst_off + done)
            done += len(buf)
