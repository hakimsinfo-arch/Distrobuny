"""HTTP access for the updater: small metadata fetches and resumable downloads."""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

USER_AGENT = "DistroBuny-Updater/1.0"
ALLOWED_SCHEMES = ("https://", "http://127.0.0.1", "http://localhost")


class DownloadError(Exception):
    pass


def _check_url(url: str) -> None:
    # Production sources must use HTTPS; plain HTTP is accepted only for local test servers.
    if not url.startswith(ALLOWED_SCHEMES):
        raise DownloadError(f"refusing non-HTTPS URL {url}")


def _open(url: str, headers: dict | None = None, timeout: float = 60):
    _check_url(url)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    return urllib.request.urlopen(req, timeout=timeout)


def fetch_text(url: str, limit: int = 16 << 20, timeout: float = 60) -> str:
    try:
        with _open(url, timeout=timeout) as r:
            data = r.read(limit + 1)
    except (urllib.error.URLError, OSError) as exc:
        raise DownloadError(f"{url}: {exc}") from exc
    if len(data) > limit:
        raise DownloadError(f"{url}: response too large")
    return data.decode("utf-8", errors="replace")


def fetch_bytes(url: str, dest: Path, limit: int = 16 << 20) -> Path:
    try:
        with _open(url) as r:
            data = r.read(limit + 1)
    except (urllib.error.URLError, OSError) as exc:
        raise DownloadError(f"{url}: {exc}") from exc
    if len(data) > limit:
        raise DownloadError(f"{url}: response too large")
    dest.write_bytes(data)
    return dest


def reserve_space(fd: int, size: int) -> None:
    """Reserve blocks without changing the file size (FALLOC_FL_KEEP_SIZE).

    On the Distro Buny data partition this encourages the file system to place
    the image contiguously, so it can be presented as a single-extent virtual
    medium.  Unsupported file systems simply skip the reservation."""
    import ctypes

    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_long, ctypes.c_long]
        libc.fallocate(fd, 0x01, 0, size)
    except (OSError, AttributeError):
        pass


def remote_size(url: str) -> int | None:
    try:
        _check_url(url)
        req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=60) as r:
            n = r.headers.get("Content-Length")
            return int(n) if n else None
    except (urllib.error.URLError, OSError, ValueError):
        return None


def download(url: str, part: Path, expected_size: int | None = None, progress=None, retries: int = 5) -> Path:
    """Download into `part`, resuming an earlier partial download with an HTTP Range request.

    A sidecar file `<part>.json` records the URL so a resume never mixes data
    from different files.  Returns the completed `part` path (not yet verified).
    """
    meta = part.with_name(part.name + ".json")
    if part.exists() and meta.exists():
        try:
            if json.loads(meta.read_text()).get("url") != url:
                part.unlink()
        except (OSError, ValueError):
            part.unlink()
    elif part.exists():
        part.unlink()
    meta.write_text(json.dumps({"url": url, "expected_size": expected_size, "started": time.time()}))
    attempt = 0
    while True:
        have = part.stat().st_size if part.exists() else 0
        if expected_size is not None and have == expected_size:
            return part
        if expected_size is not None and have > expected_size:
            part.unlink()
            have = 0
        headers = {"Range": f"bytes={have}-"} if have else {}
        try:
            with _open(url, headers, timeout=120) as r:
                if have and r.status != 206:
                    # server ignored the range: start over
                    have = 0
                    mode = "wb"
                else:
                    mode = "ab" if have else "wb"
                total = expected_size
                if total is None:
                    cl = r.headers.get("Content-Length")
                    total = (int(cl) + have) if cl else None
                with open(part, mode) as f:
                    if mode == "wb" and total:
                        reserve_space(f.fileno(), total)
                    done = have
                    while True:
                        chunk = r.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                        done += len(chunk)
                        if progress:
                            progress(done, total)
                    f.flush()
                    os.fsync(f.fileno())
            size = part.stat().st_size
            if expected_size is not None and size != expected_size:
                raise DownloadError(f"download ended at {size} of {expected_size} bytes")
            return part
        except (urllib.error.URLError, OSError, DownloadError) as exc:
            attempt += 1
            if attempt > retries:
                raise DownloadError(f"{url}: {exc} (the partial download is kept and will resume)") from exc
            time.sleep(min(30, 2 ** attempt))
