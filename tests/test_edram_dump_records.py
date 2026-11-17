import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "edram_dump_records_test"


class EdramDumpRecordsTest(unittest.TestCase):
    def test_records_follow_edram_overlaps(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++20", "-Wall", "-Werror", "-I", str(ROOT / "sdk" / "include" / "rex" / "graphics" / "vulkan"),
             str(ROOT / "tests" / "cpp" / "edram_dump_records_test.cpp"), "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
