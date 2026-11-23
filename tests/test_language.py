import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "language_test"


class LanguageTest(unittest.TestCase):
    def test_translated_files_take_the_space_of_other_languages(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             str(ROOT / "tests" / "cpp" / "language_test.cpp"), str(ROOT / "app" / "src" / "cpk_toc.cpp"),
             str(ROOT / "app" / "src" / "read_patches.cpp"), str(ROOT / "app" / "src" / "language_patches.cpp"),
             "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
