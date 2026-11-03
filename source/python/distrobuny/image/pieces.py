"""Disk writes described as data.

The image builders describe their output as a list of pieces (bytes to put
at an offset, or a range of a file to copy there).  The same description
drives image files, direct installs onto drives and read-back verification.
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from ..util import copy_range


@dataclass
class Blob:
    offset: int
    data: bytes

    @property
    def length(self) -> int:
        return len(self.data)


@dataclass
class Copy:
    offset: int
    length: int
    source: Path
    source_offset: int = 0
    label: str = ""


@dataclass
class Zero:
    offset: int
    length: int


def execute(fd: int, pieces: list, progress=None) -> None:
    """Write pieces into an open file (simple path used for image files)."""
    total = sum(p.length for p in pieces if isinstance(p, Copy))
    done = 0
    zero = bytes(1 << 20)
    for p in pieces:
        if isinstance(p, Blob):
            os.pwrite(fd, p.data, p.offset)
        elif isinstance(p, Zero):
            for off in range(0, p.length, len(zero)):
                os.pwrite(fd, zero[: min(len(zero), p.length - off)], p.offset + off)
        else:
            with open(p.source, "rb") as src:
                copy_range(src.fileno(), fd, p.length, p.source_offset, p.offset)
            done += p.length
            if progress:
                progress(done, total, p.label or str(p.source))
