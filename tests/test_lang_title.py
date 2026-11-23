import struct
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import title  # noqa: E402


class TitleTest(unittest.TestCase):
    def test_a_resized_sprite_keeps_its_center(self) -> None:
        self.assertEqual(title.centered((224.0, 184.0, 78.0, 26.0), 128, 30), (199.0, 182.0, 128.0, 30.0))

    def test_the_size_field_holds_width_and_height_minus_one(self) -> None:
        self.assertEqual(title.size_field(0x32036, 128, 30), (0x32036 & ~0x3FFFFFF) | 127 | (29 << 13))
        self.assertEqual(title.size_field(0xC0032036, 55, 26), 0xC0032036)

    def test_the_group_size_is_the_largest_that_fits_every_label(self) -> None:
        size = title.fit_size(lambda size: [size * 5, size * 7], [100, 100], start=80)
        self.assertEqual(size, 14)

    def test_line_centers_follow_the_original_rows(self) -> None:
        alpha = np.zeros((20, 8), np.uint8)
        alpha[2:5, 1] = 255
        alpha[10:15, 3] = 200
        alpha[17, 2] = 50
        self.assertEqual(title.line_centers(alpha), [3.5, 12.5])

    def test_frame_sizes_find_the_frames_of_one_sprite(self) -> None:
        record = (struct.pack(">I", 0x40) + b"\xc6\x1c\x3c\x00" * 11 + struct.pack(">4f", 0, 0, 34, 14)
                  + struct.pack(">I", 0x7800) + b"\xff" * 4)
        other = struct.pack(">I", 0x48) + record[4:]
        data = b"\0" * 8 + record + other
        self.assertEqual(title.frame_sizes(data, range(0, len(data)), 0x40, 34, 14), [8 + 0x38])

    def test_layer_sizes_find_the_layer_by_name_and_size(self) -> None:
        data = b"xx" + struct.pack(">HH", 34, 14) + b"chain\0" + struct.pack(">HH", 34, 14) + b"chain_de\0"
        self.assertEqual(title.layer_sizes(data, "chain", 34, 14), [2])

    def test_an_anchored_sprite_keeps_its_origin_and_resizes_every_record(self) -> None:
        index = bytearray(64)
        sprite = {"rect_at": 0, "rect": (1118.0, 134.0, 34.0, 14.0), "frames": [16], "layers": [40]}
        title.move_sprite(index, sprite, 65, 14, anchored=True)
        self.assertEqual(struct.unpack_from(">4f", index, 0), (1118.0, 134.0, 65.0, 14.0))
        self.assertEqual(struct.unpack_from(">2f", index, 16), (65.0, 14.0))
        self.assertEqual(struct.unpack_from(">HH", index, 40), (65, 14))

    def test_word_centers_ignore_the_gaps_between_letters(self) -> None:
        alpha = np.zeros((4, 60), np.uint8)
        alpha[:, [2, 3, 5, 6, 8]] = 255
        alpha[:, [40, 42, 43]] = 255
        self.assertEqual(title.line_centers(alpha.T, gap=4), [5.5, 42.0])


if __name__ == "__main__":
    unittest.main()
