import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "out" / "tests" / "switch_prompts_test"
SOURCE = ROOT / "app" / "src" / "nier_switch_prompts.cpp"


def text_edits() -> list:
    block = SOURCE.read_text().split("constexpr TextEdit kTextEdits[] = {", 1)[1].split("};", 1)[0]
    literals = [m.replace("\\t", "\t").replace("\\r", "\r") for m in re.findall(r'"((?:[^"\\]|\\.)*)"', block)]
    return [tuple(literals[i:i + 3]) for i in range(0, len(literals), 3)]


def columns(line: str) -> list:
    parts = line.rstrip("\r").split("\t")
    return parts[1:] if len(parts) > 1 else parts


class SwitchPromptsTest(unittest.TestCase):
    def test_patches_and_cpk_toc(self) -> None:
        BINARY.parent.mkdir(parents=True, exist_ok=True)
        build = subprocess.run(
            ["clang++", "-std=c++23", "-Wall", "-Werror", "-I", str(ROOT / "app" / "src"),
             str(ROOT / "tests" / "cpp" / "switch_prompts_test.cpp"), str(ROOT / "app" / "src" / "cpk_toc.cpp"),
             str(ROOT / "app" / "src" / "read_patches.cpp"), "-o", str(BINARY)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


class TextEditsTest(unittest.TestCase):
    def test_every_edit_keeps_byte_length_and_columns(self) -> None:
        edits = text_edits()
        self.assertEqual(len(edits), 7)
        for _, original, replacement in edits:
            self.assertEqual(len(original.encode()), len(replacement.encode()), replacement)
            self.assertEqual(original.count("\t"), replacement.count("\t"), replacement)
            self.assertEqual(original.endswith("\r"), replacement.endswith("\r"), replacement)

    def test_every_column_still_has_words(self) -> None:
        for _, _, replacement in text_edits():
            for column in columns(replacement):
                self.assertIn("<ICON=", column)
                self.assertRegex(re.sub(r"<ICON=\d+>", "", column), r"\w\w", column)

    def test_padding_is_spread_over_the_columns(self) -> None:
        for _, _, replacement in text_edits():
            pads = [len(column) - len(column.rstrip(" ")) for column in columns(replacement)]
            self.assertLessEqual(max(pads) - min(pads), 1, replacement)


if __name__ == "__main__":
    unittest.main()
