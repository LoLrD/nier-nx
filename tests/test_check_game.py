import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "tools" / "check_game.py"
UNPACK = ROOT / "tools" / "unpack_dumps.sh"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class CheckGameTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "assets"
        (self.root / "game_root").mkdir(parents=True)
        (self.root / "content" / "535107E8" / "00000002").mkdir(parents=True)
        self.xex = self.root / "game_root" / "default.xex"
        self.dlc = self.root / "content" / "535107E8" / "00000002" / "PACKAGE"
        self.xex.write_bytes(b"XEX2-good")
        self.dlc.write_bytes(b"LIVE-dlc")
        self.lock = Path(self.tmp.name) / "game.lock"
        self.lock.write_text(
            'title_id = "535107E8"\n'
            f'xex_sha256 = "{sha256(b"XEX2-good")}"\n'
            'xex_version = "1.0"\n'
            f'iso_sha256 = "{"0" * 64}"\n'
            f'dlc_15_nightmares_sha256 = "{sha256(b"LIVE-dlc")}"\n'
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_check(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(CHECK), "--root", str(self.root), "--lock", str(self.lock)],
            capture_output=True,
            text=True,
        )

    def test_matching_hashes_ok(self) -> None:
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wrong_xex_exit2_message(self) -> None:
        self.xex.write_bytes(b"XEX2-other-edition")
        result = self.run_check()
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            f"default.xex does not match app/game.lock (expected {sha256(b'XEX2-good')}, "
            f"got {sha256(b'XEX2-other-edition')}): this port supports only Nier (USA, Europe) disc",
            result.stderr,
        )

    def test_wrong_dlc_exit2(self) -> None:
        self.dlc.write_bytes(b"LIVE-other")
        result = self.run_check()
        self.assertEqual(result.returncode, 2)
        self.assertIn("15 Nightmares", result.stderr)

    def test_missing_xex_exit3(self) -> None:
        self.xex.unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 3)
        self.assertIn("default.xex", result.stderr)

    def test_iso_hash_mismatch_exit2(self) -> None:
        iso = Path(self.tmp.name) / "Nier.iso"
        iso.write_bytes(b"damaged")
        result = subprocess.run(
            [sys.executable, str(CHECK), "--lock", str(self.lock), "--iso", str(iso)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("Nier.iso does not match app/game.lock", result.stderr)


class UnpackDumpsTest(unittest.TestCase):
    def test_refuses_when_disk_space_is_short(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                ["bash", str(UNPACK), "/nonexistent/game.7z", "/nonexistent/dlc.zip"],
                env={"PATH": "/usr/bin:/bin:/opt/homebrew/bin", "NIER_DATA": tmp, "MIN_FREE_GB": "999999"},
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("not enough free space", result.stderr)


if __name__ == "__main__":
    unittest.main()
