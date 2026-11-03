"""Updater tests: discovery, download (with resume), verification and safe replacement.

Runs a local HTTP server (with Range support) serving synthetic "images", a
checksum list and OpenPGP signatures made with a throw-away key, so every
failure path can be exercised without touching real ISOs or the network.
"""
from __future__ import annotations

import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "source" / "python"))

from distrobuny.updater import engine, http as uhttp, verify  # noqa: E402


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """Static files with HTTP Range support; can stop mid-response to simulate drops."""
    cut_after = None  # bytes to send before dropping the connection (class-level switch)

    def log_message(self, *a):
        pass

    def do_GET(self):
        path = Path(self.translate_path(self.path))
        if not path.is_file():
            self.send_error(404)
            return
        data = path.read_bytes()
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[6:].split("-")[0])
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        else:
            self.send_response(200)
        body = data[start:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if RangeHandler.cut_after is not None:
            self.wfile.write(body[: RangeHandler.cut_after])
            RangeHandler.cut_after = None
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        self.wfile.write(body)

    def do_HEAD(self):
        path = Path(self.translate_path(self.path))
        if not path.is_file():
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(path.stat().st_size))
        self.end_headers()


def gpg(home: Path, *args, input: bytes | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["gpg", "--homedir", str(home), "--batch", "--yes", "--pinentry-mode", "loopback",
                           "--passphrase", "", *args], input=input, capture_output=True, check=True)


class UpdaterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="buny-updater-test-"))
        cls.www = cls.tmp / "www"
        cls.www.mkdir()
        cls.gnupg = cls.tmp / "gnupg"
        cls.gnupg.mkdir(mode=0o700)
        cls.keys = cls.tmp / "keys"
        cls.keys.mkdir()
        # the vendor's signing key and an impostor's key
        for name in ("vendor", "impostor"):
            gpg(cls.gnupg, "--quick-generate-key", f"Test {name} <{name}@example.invalid>", "ed25519", "sign", "never")
        listing = gpg(cls.gnupg, "--with-colons", "--list-keys").stdout.decode()
        fprs = [l.split(":")[9] for l in listing.splitlines() if l.startswith("fpr")]
        cls.fpr = {"vendor": fprs[0], "impostor": fprs[1]}
        (cls.keys / "vendor.asc").write_bytes(gpg(cls.gnupg, "--armor", "--export", cls.fpr["vendor"]).stdout)
        # releases: 1.0 is installed, 1.1 is offered
        cls.new_data = os.urandom(3 * 1024 * 1024 + 123)
        (cls.www / "testos-1.1-amd64.iso").write_bytes(cls.new_data)
        digest = hashlib.sha256(cls.new_data).hexdigest()
        (cls.www / "SHA256SUMS").write_text(f"{'0' * 64}  testos-1.0-amd64.iso\n{digest}  testos-1.1-amd64.iso\n")
        cls.sign("vendor")
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                                     lambda *a, **k: RangeHandler(*a, directory=str(cls.www), **k))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def sign(cls, who):
        sig = cls.www / "SHA256SUMS.gpg"
        sig.unlink(missing_ok=True)
        gpg(cls.gnupg, "--local-user", cls.fpr[who], "--detach-sign", "-o", str(sig), str(cls.www / "SHA256SUMS"))

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.sign("vendor")
        RangeHandler.cut_after = None
        self.root = Path(tempfile.mkdtemp(prefix="buny-root-", dir=self.tmp))
        (self.root / ".distrobuny").mkdir()
        (self.root / ".distrobuny" / "volume.json").write_text("{}")
        (self.root / "Desktop" / "testos").mkdir(parents=True)
        self.old = self.root / "Desktop" / "testos" / "testos-1.0-amd64.iso"
        self.old.write_bytes(b"old image" * 1000)
        self.source = engine.Source("testos", "TestOS", {
            "installed": r"^testos-(?P<version>\d+\.\d+)-amd64\.iso$",
            "pick": r"^testos-(?P<version>\d+\.\d+)-amd64\.iso$",
            "checksums": f"{self.base}/SHA256SUMS",
            "signature": {"type": "detached", "url": "{checksums}.gpg", "key": "vendor.asc"},
            "download": f"{self.base}/{{file}}",
        }, {"vendor.asc": [self.fpr["vendor"]]})
        self.inst = engine.Installed("/Desktop/testos/testos-1.0-amd64.iso", self.old.name, self.old.stat().st_size, "1.0",
                                     self.source, {"version": "1.0"})
        self.saved_key_dir = verify.KEY_DIR
        verify.KEY_DIR = self.keys

    def tearDown(self):
        verify.KEY_DIR = self.saved_key_dir

    def test_check_finds_newer_verified_release(self):
        c = engine.check(self.inst, self.tmp / "meta")
        self.assertIsNotNone(c)
        self.assertEqual(c.version, "1.1")
        self.assertEqual(c.signer, self.fpr["vendor"])
        self.assertEqual(c.size, len(self.new_data))

    def test_current_version_reports_no_update(self):
        inst = engine.Installed(self.inst.path, "testos-1.1-amd64.iso", 0, "1.1", self.source, {"version": "1.1"})
        self.assertIsNone(engine.check(inst, self.tmp / "meta"))

    def test_impostor_signature_is_rejected(self):
        self.sign("impostor")
        with self.assertRaises(verify.VerificationError):
            engine.check(self.inst, self.tmp / "meta")

    def test_download_verify_apply_replaces_atomically(self):
        c = engine.check(self.inst, self.tmp / "meta")
        verified = engine.download_and_verify(self.root, c)
        self.assertTrue(self.old.exists(), "old image must survive until the new one is approved")
        target = engine.apply(self.root, self.inst, c, verified, remove_old=True)
        self.assertEqual(target.read_bytes(), self.new_data)
        self.assertFalse(self.old.exists())
        log = (self.root / ".distrobuny" / "updates.log").read_text().splitlines()
        self.assertEqual(json.loads(log[-1])["to_version"], "1.1")

    def test_keep_old(self):
        c = engine.check(self.inst, self.tmp / "meta")
        verified = engine.download_and_verify(self.root, c)
        engine.apply(self.root, self.inst, c, verified, remove_old=False)
        self.assertTrue(self.old.exists())

    def test_corrupted_download_is_discarded_and_original_kept(self):
        c = engine.check(self.inst, self.tmp / "meta")
        c.digest = "f" * 64
        with self.assertRaises(verify.VerificationError):
            engine.download_and_verify(self.root, c)
        self.assertEqual(list((self.root / ".distrobuny" / "staging").glob("*.part")), [])
        self.assertTrue(self.old.exists())

    def test_interrupted_download_resumes(self):
        c = engine.check(self.inst, self.tmp / "meta")
        part = self.root / ".distrobuny" / "staging" / (c.file + ".part")
        part.parent.mkdir(parents=True, exist_ok=True)
        RangeHandler.cut_after = 1024 * 1024
        with self.assertRaises(uhttp.DownloadError):
            uhttp.download(c.url, part, c.size, retries=0)
        self.assertEqual(part.stat().st_size, 1024 * 1024)
        # the next attempt continues from 1 MiB and the result verifies
        verified = engine.download_and_verify(self.root, c)
        self.assertEqual(verified.read_bytes(), self.new_data)

    def test_downloads_refuse_non_volume_roots(self):
        plain = Path(tempfile.mkdtemp(dir=self.tmp))
        with self.assertRaises(engine.UpdateError):
            engine.staging_dir(plain)

    def test_non_https_urls_are_refused(self):
        with self.assertRaises(uhttp.DownloadError):
            uhttp.fetch_text("http://example.com/SHA256SUMS")

    def test_checksum_formats(self):
        e = verify.parse_checksums("SHA256 (Fedora-X.iso) = " + "ab" * 32 + "\n" + "cd" * 32 + " *ubuntu.iso\n")
        self.assertEqual([(x.name, x.algo) for x in e], [("Fedora-X.iso", "sha256"), ("ubuntu.iso", "sha256")])

    def test_version_ordering(self):
        k = engine.version_key
        self.assertGreater(k("26.04.1"), k("26.04"))
        self.assertGreater(k("44-1.10"), k("44-1.7"))
        self.assertGreater(k("2026.11.01"), k("2026.10.01"))
        self.assertTrue(engine.is_lts_series("26.04"))
        self.assertFalse(engine.is_lts_series("25.04"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
