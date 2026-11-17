import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "control_file_test"


class ControlFileTest(unittest.TestCase):
    def test_parse_settings(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             str(ROOT / "tests" / "cpp" / "control_file_test.cpp"), "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
