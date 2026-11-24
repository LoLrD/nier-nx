import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "frame_pacing_test"


class FramePacingTest(unittest.TestCase):
    def test_param_rewrite_and_counts(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             "-I", str(ROOT / "sdk" / "thirdparty" / "fmt" / "include"), "-DFMT_HEADER_ONLY",
             str(ROOT / "tests" / "cpp" / "frame_pacing_test.cpp"), str(ROOT / "app" / "src" / "nier_frame_pacing.cpp"),
             "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
