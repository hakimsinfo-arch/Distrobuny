"""Fragment a file inside the exFAT data partition of a Distro Buny image.

Test helper: real drives end up with fragmented ISO files when users copy
images onto a full or long-used drive.  Distro Buny's builder always writes
contiguous files, so to test the fragmented path this rewrites one file
of an image *copy* into several out-of-order runs of clusters linked by a
FAT chain, exactly as a file system driver would leave it:

  * the file's clusters are split into N runs; all but the first are moved
    to the highest free space of the cluster heap (the end, or just in front
    of the allocation bitmap on growable volumes), in reverse order;
  * the FAT chain links the runs, the stream extension's NoFatChain flag is
    cleared and the entry set checksum recomputed;
  * the allocation bitmap marks the new clusters used and the old ones free.

The result is checked with the reference fsck.exfat by the caller.
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.image import gpt  # noqa: E402
from distrobuny.image.exfat import entry_set_checksum  # noqa: E402

EOC = 0xFFFFFFFF


class Volume:
    def __init__(self, fd: int, base: int):
        self.fd, self.base = fd, base
        bs = os.pread(fd, 512, base)
        if bs[3:11] != b"EXFAT   ":
            raise ValueError("not an exFAT volume")
        (self.fat_offset, self.fat_length, self.heap_offset, self.cluster_count, self.root_cluster) = \
            struct.unpack_from("<IIIII", bs, 80)
        self.cluster_size = 1 << (bs[108] + bs[109])
        root = self.read_chain(self.root_cluster)
        self.bitmap_cluster = self.bitmap_bytes = None
        for i in range(0, len(root), 32):
            if root[i] == 0x81:
                self.bitmap_cluster, self.bitmap_bytes = struct.unpack_from("<IQ", root, i + 20)
        if self.bitmap_cluster is None:
            raise ValueError("no allocation bitmap")

    def coff(self, c: int) -> int:
        return self.base + self.heap_offset * 512 + (c - 2) * self.cluster_size

    def fat_get(self, c: int) -> int:
        return struct.unpack("<I", os.pread(self.fd, 4, self.base + self.fat_offset * 512 + c * 4))[0]

    def fat_set(self, c: int, v: int) -> None:
        os.pwrite(self.fd, struct.pack("<I", v), self.base + self.fat_offset * 512 + c * 4)

    def read_chain(self, first: int, contiguous_len: int | None = None) -> bytes:
        out, c = bytearray(), first
        if contiguous_len is not None:
            return os.pread(self.fd, contiguous_len, self.coff(first))
        while 2 <= c < 0xFFFFFFF7:
            out += os.pread(self.fd, self.cluster_size, self.coff(c))
            c = self.fat_get(c)
        return bytes(out)

    def find(self, path: str) -> tuple[int, int, int, int]:
        """(directory cluster, byte offset of the entry set in that directory, first cluster, length)."""
        dir_cluster, dir_contig = self.root_cluster, None
        parts = [p for p in path.split("/") if p]
        for depth, name in enumerate(parts):
            data = self.read_chain(dir_cluster, dir_contig)
            i = 0
            found = None
            while i < len(data):
                t = data[i]
                if t == 0:
                    break
                if t == 0x85:
                    n = data[i + 1]
                    es = data[i:i + 32 * (n + 1)]
                    flags, nlen = es[33], es[35]
                    first, length = struct.unpack_from("<IQ", es, 32 + 20)
                    units = b"".join(es[32 * k + 2:32 * k + 32] for k in range(2, n + 1))
                    if units[:nlen * 2].decode("utf-16-le") == name:
                        found = (dir_cluster, i, first, length, flags)
                        break
                    i += 32 * (n + 1)
                else:
                    i += 32
            if not found:
                raise FileNotFoundError(path)
            if depth == len(parts) - 1:
                if not found[4] & 0x02:
                    raise ValueError(f"{path} is already fragmented")
                return found[:4]
            dir_cluster = found[2]
            dir_contig = found[3] if found[4] & 0x02 else None
        raise FileNotFoundError(path)

    def bitmap(self) -> bytearray:
        return bytearray(os.pread(self.fd, self.bitmap_bytes, self.coff(self.bitmap_cluster)))

    def write_bitmap(self, bm: bytearray) -> None:
        os.pwrite(self.fd, bytes(bm), self.coff(self.bitmap_cluster))


def fragment(image: Path, iso_path: str, runs: int = 3) -> list[tuple[int, int]]:
    """Split `iso_path` into `runs` cluster runs; returns them as (first cluster, count)."""
    fd = os.open(image, os.O_RDWR)
    try:
        with open(image, "rb") as f:
            part = next(p for p in gpt.read_tables(f).partitions if p.name == "DISTROBUNY")
        v = Volume(fd, part.offset)
        dir_cluster, off, first, length = v.find(iso_path)
        nclus = (length + v.cluster_size - 1) // v.cluster_size
        bm = v.bitmap()
        used = lambda c: bm[(c - 2) // 8] >> ((c - 2) % 8) & 1  # noqa: E731
        # the highest free space of the heap, enough for everything after the first run
        per = nclus // runs
        sizes = [per] * (runs - 1) + [nclus - per * (runs - 1)]
        moved = sum(sizes[1:])
        dest, free = None, 0
        for c in range(v.cluster_count + 1, 1, -1):
            free = 0 if used(c) else free + 1
            if free == moved:
                dest = c
                break
        if dest is None:
            raise ValueError("not enough free space in the volume to fragment the file")
        # place runs 2..N there in reverse order: [run N][run N-1]...[run 2]
        placed: list[tuple[int, int]] = [(first, sizes[0])]
        src = first + sizes[0]
        layout = {}
        cursor = dest
        for k in range(runs - 1, 0, -1):
            layout[k] = cursor
            cursor += sizes[k]
        for k in range(1, runs):
            placed.append((layout[k], sizes[k]))
        for k in range(1, runs):
            n = sizes[k]
            for j in range(0, n, 64):
                cnt = min(64, n - j)
                data = os.pread(fd, cnt * v.cluster_size, v.coff(src + j))
                os.pwrite(fd, data, v.coff(layout[k] + j))
            src += n
        # FAT chain through all runs
        chain = [c for start, n in placed for c in range(start, start + n)]
        for a, b in zip(chain, chain[1:]):
            v.fat_set(a, b)
        v.fat_set(chain[-1], EOC)
        # bitmap: old tail clusters free, new ones used
        def setbit(c, val):
            i, b = (c - 2) // 8, (c - 2) % 8
            bm[i] = (bm[i] | (1 << b)) if val else (bm[i] & ~(1 << b))
        for c in range(first + sizes[0], first + nclus):
            setbit(c, 0)
        for start, n in placed[1:]:
            for c in range(start, start + n):
                setbit(c, 1)
        v.write_bitmap(bm)
        # clear NoFatChain and fix the entry set checksum
        dir_data = bytearray(v.read_chain(dir_cluster))
        n = dir_data[off + 1]
        es = bytearray(dir_data[off:off + 32 * (n + 1)])
        es[33] &= ~0x02
        struct.pack_into("<H", es, 2, entry_set_checksum(bytes(es)))
        # write back into the right cluster of the directory chain
        c, pos = dir_cluster, off
        while pos >= v.cluster_size:
            c = v.fat_get(c)
            pos -= v.cluster_size
        if pos + len(es) > v.cluster_size:
            raise ValueError("entry set crosses a cluster boundary (not handled by this helper)")
        os.pwrite(fd, bytes(es), v.coff(c) + pos)
        os.fsync(fd)
        return placed
    finally:
        os.close(fd)


if __name__ == "__main__":
    img, path = Path(sys.argv[1]), sys.argv[2]
    runs = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    for start, n in fragment(img, path, runs):
        print(f"run: clusters {start}..{start + n - 1} ({n})")
