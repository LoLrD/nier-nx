import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "check_repo_clean.py"


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


def run_check(repo: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT)], cwd=repo, capture_output=True, text=True)


class CheckRepoCleanTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        git(self.repo, "init", "-q")
        (self.repo / "README.md").write_text("x")
        git(self.repo, "add", "README.md")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_clean_repo_passes(self) -> None:
        result = run_check(self.repo)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tracked_xex_fails(self) -> None:
        xex = self.repo / "assets" / "game_root" / "default.xex"
        xex.parent.mkdir(parents=True)
        xex.write_bytes(b"XEX2")
        git(self.repo, "add", "-f", str(xex))
        result = run_check(self.repo)
        self.assertEqual(result.returncode, 1)
        self.assertIn("assets/game_root/default.xex", result.stdout)

    def test_tracked_generated_code_fails(self) -> None:
        gen = self.repo / "app" / "generated" / "default" / "nier_recomp.0.cpp"
        gen.parent.mkdir(parents=True)
        gen.write_text("int x;")
        git(self.repo, "add", "-f", str(gen))
        result = run_check(self.repo)
        self.assertEqual(result.returncode, 1)
        self.assertIn("app/generated/default/nier_recomp.0.cpp", result.stdout)


if __name__ == "__main__":
    unittest.main()
