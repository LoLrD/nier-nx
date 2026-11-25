import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CHECK = Path(__file__).resolve().parents[1] / "tools" / "codegen" / "check_generated.py"


class CheckGeneratedTest(unittest.TestCase):
    def run_check(self, files: dict[str, str]) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in files.items():
                (Path(tmp) / name).write_text(text)
            return subprocess.run([sys.executable, str(CHECK), "--gen", tmp], capture_output=True, text=True)

    def test_clean_tree_passes(self) -> None:
        result = self.run_check({"nier_recomp.0.cpp": "int x;\n"})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fatal_branch_fails_and_is_listed(self) -> None:
        result = self.run_check({
            "nier_recomp.0.cpp": "int x;\n",
            "nier_recomp.1.cpp": '\tif (!ctx.cr6.eq) REX_FATAL("Unresolved branch from 0x8226045C to 0x82260468");\n',
        })
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unresolved branch from 0x8226045C to 0x82260468", result.stderr)


if __name__ == "__main__":
    unittest.main()
