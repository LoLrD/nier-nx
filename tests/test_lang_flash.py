import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import flash  # noqa: E402


class FlashTest(unittest.TestCase):
    def test_wrapping_keeps_words_and_paragraphs(self) -> None:
        lines = flash.wrap("Первый абзац текста\n\nВторой", 12, len)
        self.assertEqual(lines, ["Первый абзац", "текста", "", "Второй"])

    def test_the_text_block_rotation_is_found(self) -> None:
        angle = math.radians(-10)
        u, v = np.meshgrid(np.linspace(0, 200, 81), np.linspace(0, 60, 25))
        x = u * math.cos(angle) - v * math.sin(angle)
        y = u * math.sin(angle) + v * math.cos(angle)
        box = flash.oriented_box(np.stack([x.ravel(), y.ravel()], 1))
        self.assertAlmostEqual(box[0], -10, delta=0.3)
        self.assertAlmostEqual(box[2] - box[1], 200, delta=4)
        self.assertAlmostEqual(box[4] - box[3], 60, delta=4)

    def test_a_redacted_word_is_found_in_its_line(self) -> None:
        self.assertEqual(flash.word_span("под кодом «Нуар» для", "«Нуар»", len), (10, 16))
        self.assertIsNone(flash.word_span("без слова", "«Нуар»", len))


if __name__ == "__main__":
    unittest.main()
