import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "switch_prompts"))

import typeface

FACE = "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf"


def drawing(text: str) -> tuple:
    face = typeface.Face(FACE, 40)
    image = Image.new("L", (240, 80), 0)
    face.text(image, (10.25, 50.5), text, "ls")
    return image.tobytes(), face.getlength(text), face.getbbox(text, "ls")


@unittest.skipUnless(Path(FACE).exists(), "needs the macOS fonts")
class TypefaceTest(unittest.TestCase):
    def tearDown(self):
        typeface.use(None)

    def test_stored_drawing_matches_the_font(self):
        with tempfile.TemporaryDirectory() as folder:
            typeface.use(Path(folder))
            recorded = drawing("Ниер")
            typeface.finish()
            with mock.patch.dict(os.environ, {"NIER_LANG_FONTS": "stored"}):
                typeface.use(Path(folder))
                self.assertEqual(drawing("Ниер"), recorded)

    def test_text_that_was_never_drawn_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            typeface.use(Path(folder))
            drawing("Ниер")
            typeface.finish()
            with mock.patch.dict(os.environ, {"NIER_LANG_FONTS": "stored"}):
                typeface.use(Path(folder))
                with self.assertRaises(SystemExit):
                    drawing("Кайнэ")

    def test_without_a_store_the_font_is_used_directly(self):
        self.assertEqual(drawing("Ниер")[1:], drawing("Ниер")[1:])


if __name__ == "__main__":
    unittest.main()
