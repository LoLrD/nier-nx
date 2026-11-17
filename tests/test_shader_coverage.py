import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "shaders" / "shader_coverage.py"
sys.path.insert(0, str(ROOT / "tools" / "shaders"))

import shader_coverage


def container(vertex: bool, words: list[int], fetch_addresses: tuple[int, ...] = ()) -> bytes:
    virtual = bytearray(64)
    header = 16
    microcode = b"".join(struct.pack(">I", w) for w in words)
    struct.pack_into(">I", virtual, 0, 0x102A1100 | int(vertex))
    struct.pack_into(">I", virtual, 4, len(virtual))
    struct.pack_into(">I", virtual, 8, len(microcode))
    struct.pack_into(">I", virtual, 24, header)
    struct.pack_into(">II", virtual, header, 0, len(microcode))
    if vertex:
        struct.pack_into(">II", virtual, header + 24, 0, len(fetch_addresses))
        for i, address in enumerate(fetch_addresses):
            struct.pack_into(">I", virtual, header + 36 + 4 * i, (5 << 12) | address)
    return bytes(virtual) + microcode


def ucode(words: list[int]) -> bytes:
    return b"".join(struct.pack(">I", w) for w in words)


BASE = [0x11111111, 0x22222222, 0x33333333, 0x00012345, 0x80000ABC, 0x80000001]


class CoverageTest(unittest.TestCase):
    def run_case(self, containers: dict[str, bytes], discovered: dict[str, bytes]) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            cdir = Path(tmp) / "containers"
            ddir = Path(tmp) / "discovered"
            cdir.mkdir()
            ddir.mkdir()
            for name, data in containers.items():
                (cdir / name).write_bytes(data)
            for name, data in discovered.items():
                (ddir / name).write_bytes(data)
            return shader_coverage.coverage(cdir, ddir)

    def test_pixel_exact_match(self) -> None:
        result = self.run_case({"ps_a.bin": container(False, BASE)}, {"ps_1.ucode": ucode(BASE)})
        self.assertEqual(result["ps"], {"discovered": 1, "matched": 1})
        self.assertEqual(result["matches"], {"ps_1.ucode": "ps_a.bin"})

    def test_pixel_one_word_differs_unmatched(self) -> None:
        changed = BASE[:2] + [0x33333334] + BASE[3:]
        result = self.run_case({"ps_a.bin": container(False, BASE)}, {"ps_1.ucode": ucode(changed)})
        self.assertEqual(result["unmatched"], ["ps_1.ucode"])

    def test_patched_vertex_fetch_matches(self) -> None:
        patched = BASE[:3] + [BASE[3] | 0xFFF00000, 0x80000000 | 0x7FFF1234, 0x80000000 | 0x12345678]
        result = self.run_case({"vs_a.bin": container(True, BASE, (1,))}, {"vs_1.ucode": ucode(patched)})
        self.assertEqual(result["vs"], {"discovered": 1, "matched": 1})

    def test_alu_difference_does_not_match(self) -> None:
        changed = [0x11111112] + BASE[1:]
        result = self.run_case({"vs_a.bin": container(True, BASE, (1,))}, {"vs_1.ucode": ucode(changed)})
        self.assertEqual(result["unmatched"], ["vs_1.ucode"])

    def test_exit_code_reports_unmatched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cdir = Path(tmp) / "c"
            ddir = Path(tmp) / "d"
            cdir.mkdir()
            ddir.mkdir()
            (ddir / "ps_1.ucode").write_bytes(ucode(BASE))
            done = subprocess.run([sys.executable, str(TOOL), "--containers", str(cdir), "--discovered", str(ddir)],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
