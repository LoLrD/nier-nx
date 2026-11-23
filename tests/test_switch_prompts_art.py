import sys
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "switch_prompts"))

import draw  # noqa: E402
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


def visible(image: Image.Image, kind: int, height: int) -> list:
    pitch = image.width
    decoded = xenos_dxt.decode(xenos_dxt.encode(image, kind, 128), pitch, 32, kind).crop((0, 0, pitch, height))
    data = decoded.tobytes()
    return [tuple(data[i:i + 4]) for i in range(0, len(data), 4) if data[i + 3] > 16]


def alpha_profile(image: Image.Image, rows: bool) -> list:
    alpha = image.getchannel("A")
    data = alpha.tobytes()
    width, height = alpha.size
    if rows:
        return [sum(data[y * width:(y + 1) * width]) for y in range(height)]
    return [sum(data[y * width + x] for y in range(height)) for x in range(width)]


def best_shift(thin: list, bold: list, offset: int, start: int, end: int) -> int:
    def score(shift: int) -> int:
        return sum(thin[x] * bold[x + offset + shift] for x in range(start, end) if 0 <= x + offset + shift < len(bold))
    return max(range(-4, 5), key=lambda shift: (score(shift), -abs(shift)))


class SwitchArtTest(unittest.TestCase):
    def test_title_layers_line_up_letter_by_letter(self) -> None:
        for _, label, width, height, pitch in draw.TITLES:
            thin_spec = next(spec for spec in draw.SELECTIONS if spec[1] == label)
            _, _, thin_width, thin_height, thin_pitch, top = thin_spec
            placement = draw.layout(label, thin_width, thin_height)
            bold = draw.title(placement, width, height, pitch).crop((0, 0, width, height))
            thin = draw.selection(placement, thin_width, thin_height, thin_pitch, top)
            thin = thin.crop((0, top, thin_width, top + thin_height))
            columns = alpha_profile(thin, False)
            ink = [x for x, value in enumerate(columns) if value]
            third = (ink[-1] - ink[0]) // 3
            for window in range(3):
                start = ink[0] + window * third
                shift = best_shift(columns, alpha_profile(bold, False), (width - thin_width) // 2, start, start + third)
                self.assertLessEqual(abs(shift), 1, f"{label}: part {window} is off by {shift}")
            shift = best_shift(alpha_profile(thin, True), alpha_profile(bold, True), (height - thin_height) // 2, 0,
                               thin_height)
            self.assertLessEqual(abs(shift), 1, f"{label}: rows are off by {shift}")

    def test_title_glow_is_light_like_the_original(self) -> None:
        image = draw.title(draw.layout("Press the + Button", 255, 14), 265, 26, 384)
        pixels = visible(image, 3, 32)
        self.assertGreater(len(pixels), 1000)
        self.assertGreaterEqual(min(min(p[:3]) for p in pixels), 200)

    def test_selection_layer_is_dark_grey_with_full_coverage(self) -> None:
        image = draw.selection(draw.layout("Press the + Button", 255, 14), 255, 14, 256, 0)
        pixels = visible(image, 3, 14)
        self.assertGreater(len(pixels), 300)
        self.assertLessEqual(max(max(p[:3]) for p in pixels), 90)
        self.assertGreaterEqual(min(min(p[:3]) for p in pixels), 60)
        self.assertEqual(max(p[3] for p in pixels), 255)

    def test_skill_label_is_as_faint_as_the_original_chalk(self) -> None:
        pixels = visible(draw.skill_label("ZL", 32, 30), 3, 30)
        self.assertGreater(len(pixels), 50)
        self.assertEqual(max(p[3] for p in pixels), 170)
        self.assertGreaterEqual(min(min(p[:3]) for p in pixels), 200)


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

    def test_dxt1_round_trip_keeps_flat_opaque_blocks(self) -> None:
        image = flat_image(256, 64, 8)
        image.putalpha(255)
        blocks = xenos_dxt.encode_blocks(image, 1, 0, 0, 256, 64)
        data = bytearray(256 * 64 // 2 * 4)
        for offset, block in blocks:
            self.assertEqual(len(block), 8)
            data[offset:offset + 8] = block
        self.assertEqual(xenos_dxt.decode(bytes(data), 256, 64, 1).tobytes(), image.tobytes())

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
