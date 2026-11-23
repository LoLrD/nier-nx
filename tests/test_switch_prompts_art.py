import sys
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "switch_prompts"))

import xenos_dxt  # noqa: E402


def flat_image(width: int, height: int, alpha_bits: int) -> Image.Image:
    image = Image.new("RGBA", (width, height))
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (132, 130, 132), (255, 255, 255)]
    alphas = [255, 0, 136, 68] if alpha_bits == 4 else [255, 0, 128, 64]
    for by in range(height // 4):
        for bx in range(width // 4):
            alpha = alphas[(bx * 3 + by) % len(alphas)]
            color = colors[(bx + by) % len(colors)] if alpha else (0, 0, 0)
            for y in range(4):
                for x in range(4):
                    image.putpixel((bx * 4 + x, by * 4 + y), color + (alpha,))
    return image


class XenosDxtTest(unittest.TestCase):
    def test_dxt5_round_trip_keeps_flat_blocks(self) -> None:
        image = flat_image(128, 192, 8)
        data = xenos_dxt.encode(image, 5, 256)
        self.assertEqual(len(data), 128 * 256)
        self.assertEqual(xenos_dxt.decode(data, 128, 192, 5).tobytes(), image.tobytes())

    def test_dxt3_round_trip_keeps_flat_blocks(self) -> None:
        image = flat_image(128, 32, 4)
        data = xenos_dxt.encode(image, 3, 128)
        self.assertEqual(len(data), 128 * 128)
        self.assertEqual(xenos_dxt.decode(data, 128, 32, 3).tobytes(), image.tobytes())

    def test_blocks_are_tiled_and_byte_swapped(self) -> None:
        image = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
        for y in range(4):
            for x in range(4):
                image.putpixel((36 + x, y), (255, 0, 0, 255))
        data = xenos_dxt.encode(image, 5, 128)
        offset = xenos_dxt.tiled_offset(9, 0, 32, 4)
        self.assertNotEqual(offset, 9 * 16)
        self.assertEqual(data[offset:offset + 2], bytes([0xFF, 0xFF]))
        self.assertEqual(data[offset + 8:offset + 10], bytes([0xF8, 0x00]))

    def test_region_blocks_match_the_whole_texture(self) -> None:
        image = flat_image(384, 32, 4)
        whole = xenos_dxt.encode(image, 3, 128)
        blocks = xenos_dxt.encode_blocks(image, 3, 0, 0, 265, 26)
        self.assertEqual(len(blocks), 67 * 7)
        for offset, block in blocks:
            self.assertEqual(whole[offset:offset + 16], block)

    def test_cell_blocks_cover_one_cell(self) -> None:
        blocks = xenos_dxt.cell_ranges(2, 5, 32, 128, 16)
        self.assertEqual(len(blocks), 64)
        self.assertTrue(all(size == 16 for _, size in blocks))
        self.assertEqual(len({offset for offset, _ in blocks}), 64)


if __name__ == "__main__":
    unittest.main()
