"""Brand and interface assets for the runtime.

  * The official bunny logo is converted from its white-background PNG into a
    transparent mark by un-blending white: alpha = darkness relative to the
    brand colour.  Edges keep their anti-aliasing.
  * Distribution marks come from Simple Icons (the collection is CC0; some
    marks carry their own licence, listed in assets/distro-icons/ATTRIBUTION.md;
    all remain trademarks of their owners).  They are stored as SVG path data and drawn
    as vectors by the interface at any size, in each brand's official colour.
  * Images are stored as BIMG: a tiny header plus premultiplied BGRA pixels,
    the native layout of the framebuffer, so the runtime needs no decoder.
"""
from __future__ import annotations

import json
import re
import shutil
import struct
from pathlib import Path

from ..util import WORKSPACE, BuildError, step

ASSETS = WORKSPACE / "assets"
BRAND_GREEN = (156, 198, 111)  # sampled from the official logo

# interface icon id -> Simple Icons slug
SIMPLE_ICON_MAP = {
    "ubuntu": "ubuntu", "kubuntu": "kubuntu", "xubuntu": "xubuntu", "lubuntu": "lubuntu",
    "ubuntu-mate": "ubuntumate", "linuxmint": "linuxmint", "pop-os": "popos", "zorin": "zorin",
    "elementary": "elementary", "kde-neon": "kdeneon", "debian": "debian", "kali": "kalilinux",
    "parrot": "parrotsecurity", "mx": "mxlinux", "tails": "tails", "fedora": "fedora", "rocky": "rockylinux",
    "almalinux": "almalinux", "centos": "centos", "opensuse": "opensuse", "archlinux": "archlinux",
    "endeavouros": "endeavouros", "cachyos": "cachyos", "manjaro": "manjaro", "omarchy": "omarchy",
    "garuda": "garudalinux", "artix": "artixlinux", "nixos": "nixos", "void": "voidlinux", "gentoo": "gentoo",
    "alpine": "alpinelinux", "slackware": "slackware", "solus": "solus",
}

# Distributions without an official mark in Simple Icons get a monogram badge
# in their family's colour (never an invented logo).
MONOGRAMS = {
    "ubuntu-budgie": ("ubuntu", "UB"), "ubuntu-kylin": ("ubuntu", "UK"), "ubuntu-studio": ("ubuntu", "US"),
    "ubuntu-unity": ("ubuntu", "UU"), "ubuntu-cinnamon": ("ubuntu", "UC"), "edubuntu": ("ubuntu", "Ed"),
    "bazzite": ("fedora", "Bz"), "nobara": ("fedora", "Nb"), "guix": (None, "Gx"), "generic": (None, ""),
    "antix": ("debian", "aX"), "blackarch": ("archlinux", "BA"),
}
NEUTRAL = "6E7868"


def _pil():
    try:
        from PIL import Image
    except ImportError as exc:
        raise BuildError("Python Imaging Library (python3-pil) is required to process the logo") from exc
    return Image


def logo_mark(src: Path, out_png: Path, size: int = 1024) -> None:
    """Turn the official white-background logo into a transparent mark."""
    Image = _pil()
    im = Image.open(src).convert("RGB")
    w, h = im.size
    px = im.load()
    out = Image.new("RGBA", (w, h))
    po = out.load()
    br, bg, bb = BRAND_GREEN
    # The "white" background of the supplied PNG is 250-254 with slight
    # noise; measure it from the corners so noise does not become alpha.
    corners = [px[x, y] for x in (2, w - 3) for y in (2, h - 3)]
    paper = min(sum(c) / 3 for c in corners)
    noise_floor = 0.07
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            # how far each channel moved from the paper toward the brand colour
            a = max((paper - r) / (paper - br), (paper - g) / (paper - bg), (paper - b) / (paper - bb))
            a = max(0.0, min(1.0, (a - noise_floor) / (1.0 - noise_floor)))
            po[x, y] = (br, bg, bb, int(round(a * 255)))
    bbox = out.getbbox()
    if not bbox:
        raise BuildError("the logo appears to be empty")
    out = out.crop(bbox)
    cw, ch = out.size
    side = max(cw, ch)
    square = Image.new("RGBA", (side, side), (br, bg, bb, 0))
    square.paste(out, ((side - cw) // 2, (side - ch) // 2))
    square = square.resize((size, size), Image.LANCZOS)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    square.save(out_png, optimize=True)


def write_bimg(png: Path, out: Path, sizes: list[int]) -> None:
    """Write a multi-resolution BIMG: 'BIMG', version, count, then per level w, h, BGRA premultiplied."""
    Image = _pil()
    im = Image.open(png).convert("RGBA")
    blob = bytearray(b"BIMG" + struct.pack("<II", 1, len(sizes)))
    for s in sizes:
        lvl = im.resize((s, s), Image.LANCZOS) if im.size != (s, s) else im
        data = bytearray(s * s * 4)
        raw = lvl.tobytes()
        for i in range(0, len(raw), 4):
            r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
            data[i] = (b * a + 127) // 255
            data[i + 1] = (g * a + 127) // 255
            data[i + 2] = (r * a + 127) // 255
            data[i + 3] = a
        blob += struct.pack("<II", s, s) + data
    out.write_bytes(bytes(blob))


def compile_icons(out: Path) -> int:
    si = ASSETS / "distro-icons" / "simple-icons"
    meta = json.loads((si / "icons.json").read_text())
    lines = ["# Distro Buny icon set (generated).  id|kind|RRGGBB|title|data",
             "# svg: Simple Icons path data (24x24 viewBox); licences per mark in ICON-ATTRIBUTION.md;"
             " trademarks belong to their owners",
             "# mono: monogram badge text"]
    for icon_id, slug in sorted(SIMPLE_ICON_MAP.items()):
        svg = (si / f"{slug}.svg").read_text()
        m = re.search(r'<path d="([^"]+)"', svg)
        if not m:
            raise BuildError(f"{slug}.svg has no path")
        hexcol = meta[slug]["hex"]
        lines.append(f"{icon_id}|svg|{hexcol}|{meta[slug]['title']}|{m.group(1)}")
    for icon_id, (vendor, text) in sorted(MONOGRAMS.items()):
        col = meta[SIMPLE_ICON_MAP[vendor]]["hex"] if vendor else NEUTRAL
        lines.append(f"{icon_id}|mono|{col}||{text}")
    out.write_text("\n".join(lines) + "\n")
    return len(lines) - 3


def prepare_sources() -> None:
    """Regenerate derived source assets that are kept in assets/ (run when the logo changes)."""
    step("processing the official logo")
    logo_mark(WORKSPACE / "Distro buny logo.png", ASSETS / "branding" / "distrobuny-mark.png")


def install_runtime_assets(share: Path) -> None:
    mark = ASSETS / "branding" / "distrobuny-mark.png"
    if not mark.exists():
        prepare_sources()
    (share / "fonts").mkdir(parents=True, exist_ok=True)
    for f in sorted((ASSETS / "fonts").glob("Inter-*.ttf")):
        shutil.copy2(f, share / "fonts" / f.name)
    write_bimg(mark, share / "logo.bimg", [512, 256, 128, 64])
    n = compile_icons(share / "icons.txt")
    step(f"installed fonts, logo and {n} distribution icons")
