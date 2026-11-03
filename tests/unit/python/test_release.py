"""Release folder checks (source/python/distrobuny/build/release.py).

verify_release() is what `buny-build release` runs last and what
`buny-build publish` runs on the published copy.  These tests build small
release folders and break them in the ways a hand-assembled release breaks:
a stale checksum in SOURCE-CODE.txt (the problem found in the first exported
1.0.0 release), a changed or unlisted file, a wrong unpacked-image hash,
unfinished text, incomplete third-party source.  The last test checks the
real output/release when it exists.
"""
from __future__ import annotations

import hashlib
import lzma
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.build import release  # noqa: E402
from distrobuny.util import BuildError  # noqa: E402

PAIRS = [("hello", "2.10-3")]  # one fake source package instead of Ubuntu's


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class VerifyReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="buny-release-test-"))
        self._rows = release.source_rows
        release.source_rows = lambda: ([], PAIRS)
        self.dir = self.make()

    def tearDown(self):
        release.source_rows = self._rows
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make(self, name: str = "rel") -> Path:
        d = self.tmp / name
        legal = d / release.LEGAL
        (legal / "third-party").mkdir(parents=True)
        self.image = b"DISTRO BUNY TEST IMAGE" * 1000
        (d / "distrobuny-9.9.9-starter.img.xz").write_bytes(lzma.compress(self.image, format=lzma.FORMAT_XZ))
        tar = b"source tarball"
        (legal / "distrobuny-9.9.9-source.tar.xz").write_bytes(tar)
        (legal / "LICENSES.txt").write_text("Distro Buny: GPL-3.0-or-later\n")
        (legal / "SOURCE-CODE.txt").write_text(
            f"1. Distro Buny\n     distrobuny-9.9.9-source.tar.xz\n     SHA-256 {sha(tar)}\n")
        orig = b"upstream"
        (legal / "third-party" / "hello_2.10.orig.tar.gz").write_bytes(orig)
        (legal / "third-party" / "hello_2.10-3.dsc").write_text(
            f"Source: hello\nChecksums-Sha256:\n {sha(orig)} {len(orig)} hello_2.10.orig.tar.gz\n")
        (d / "Distro Buny 9.9.9 - User Guide.pdf").write_bytes(b"%PDF-1.4 test")
        release.write_sums(d, "distrobuny-9.9.9-starter.img", sha(self.image))
        return d

    def broken(self, pattern: str) -> None:
        with self.assertRaises(BuildError) as cm:
            release.verify_release(self.dir)
        self.assertRegex(str(cm.exception), pattern)

    def test_a_consistent_folder_passes(self):
        sums = release.verify_release(self.dir)
        self.assertIn("distrobuny-9.9.9-starter.img", sums, "the unpacked image is listed")
        self.assertIn(f"{release.LEGAL}/third-party/hello_2.10-3.dsc", sums, "files in sub-folders are listed")

    def test_stale_source_checksum_in_the_notice(self):
        notice = self.dir / release.LEGAL / "SOURCE-CODE.txt"
        notice.write_text(notice.read_text().replace(sha(b"source tarball"), "5861d4fa" + "0" * 56))
        release.write_sums(self.dir, "distrobuny-9.9.9-starter.img", sha(self.image))  # SHA256SUMS itself is fresh
        self.broken(r"SOURCE-CODE.txt:3: quotes SHA-256 5861d4fa")

    def test_changed_file(self):
        (self.dir / release.LEGAL / "distrobuny-9.9.9-source.tar.xz").write_bytes(b"rebuilt later")
        self.broken(r"distrobuny-9.9.9-source.tar.xz: SHA-256 is")

    def test_unlisted_and_missing_files(self):
        (self.dir / "notes.txt").write_text("forgotten")
        (self.dir / "Distro Buny 9.9.9 - User Guide.pdf").unlink()
        self.broken(r"User Guide.pdf: listed in SHA256SUMS but not in the folder")
        self.broken(r"notes.txt: not listed in SHA256SUMS")

    def test_wrong_unpacked_image_hash(self):
        sums = self.dir / "SHA256SUMS"
        sums.write_text(sums.read_text().replace(sha(self.image), "0" * 64))
        self.broken(r"distrobuny-9.9.9-starter.img: SHA-256 is")

    def test_unfinished_text(self):
        notice = self.dir / release.LEGAL / "SOURCE-CODE.txt"
        notice.write_text(notice.read_text() + "Write to: [CONTACT ADDRESS: fill in before publishing]\n")
        release.write_sums(self.dir, "distrobuny-9.9.9-starter.img", sha(self.image))
        self.broken(r"unfinished text")

    def test_incomplete_third_party_source(self):
        (self.dir / release.LEGAL / "third-party" / "hello_2.10.orig.tar.gz").unlink()
        release.write_sums(self.dir, "distrobuny-9.9.9-starter.img", sha(self.image))
        self.broken(r"third-party: hello_2.10.orig.tar.gz \(hello_2.10-3.dsc\) is missing")

    def test_publish_verifies_the_copy_and_leaves_foreign_files_alone(self):
        dest = self.tmp / "published"
        (dest / release.LEGAL).mkdir(parents=True)
        # a file of the release placed there beforehand (same bytes) is accepted
        shutil.copy2(self.dir / release.LEGAL / "LICENSES.txt", dest / release.LEGAL / "LICENSES.txt")
        release.publish(self.dir, dest)
        self.assertEqual(release.verify_release(dest), release.verify_release(self.dir))
        other = self.tmp / "other"
        other.mkdir()
        (other / "notes.txt").write_text("someone else's file")
        with self.assertRaises(BuildError):
            release.publish(self.dir, other)
        self.assertEqual((other / "notes.txt").read_text(), "someone else's file", "a refusal touches nothing")


@unittest.skipUnless((release.RELEASE / "SHA256SUMS").exists(), "no output/release yet (buny-build release)")
class BuiltReleaseTest(unittest.TestCase):
    def test_output_release_is_consistent(self):
        sums = release.verify_release(release.RELEASE, unpack=False)
        version = (ROOT / "VERSION").read_text().strip()
        self.assertIn(f"distrobuny-{version}-starter.img.xz", sums, "the release is for the current VERSION")


if __name__ == "__main__":
    unittest.main(verbosity=2)
