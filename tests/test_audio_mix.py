import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "audio_mix_test"


class AudioMixTest(unittest.TestCase):
    def test_mix_add_matches_scalar_fma(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-O2", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             str(ROOT / "tests" / "cpp" / "audio_mix_test.cpp"), str(ROOT / "app" / "src" / "nier_audio_mix.cpp"),
             "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
