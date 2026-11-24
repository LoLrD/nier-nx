import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "npc_lod_test"


class NpcLodTest(unittest.TestCase):
    def test_bins_scale_and_report(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             "-I", str(ROOT / "sdk" / "thirdparty" / "fmt" / "include"), "-DFMT_HEADER_ONLY",
             str(ROOT / "tests" / "cpp" / "npc_lod_test.cpp"), str(ROOT / "app" / "src" / "nier_npc_lod.cpp"),
             "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
