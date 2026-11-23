import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

import xenos_dxt

ROOT = Path(__file__).resolve().parents[2]
PREVIEW = ROOT / "out" / "switch_prompts"
HEADER = ROOT / "app" / "src" / "nier_switch_prompts_data.h"
ROUNDED = "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf"
CHALK = "/System/Library/Fonts/Supplemental/Chalkduster.ttf"
TITLE_BOLD = "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf"
TITLE_THIN = "/System/Library/Fonts/Supplemental/Times New Roman.ttf"

SCALE = 4
CELL = 32
BIG = CELL * SCALE
WHITE = (242, 242, 242, 255)
DARK = (38, 38, 38, 255)
RIM = (230, 230, 230, 255)
FACE = (43, 43, 43, 255)
GREY = (150, 150, 150, 255)
LINE = 6
TITLE_INK = (232, 232, 232)
SELECTION_INK = (76, 77, 76)
CHALK_INK = (255, 255, 255)
CHALK_ALPHA = 170
TITLE_SIZE = 20.5
TRACKING = 0.08
WORD_GAP = 0.68
CORE_BLUR = 0.4
GLOW_RADIUS = 2.5
GLOW_GAIN = 3
GLOW_CAP = 190
EMBLEM_CELL = (2, 5)
SKILL_LABELS = [("L", 31, 30), ("ZL", 32, 30), ("R", 37, 27), ("ZR", 34, 27)]
TITLES = [
    ("kTitleEn", "Press the + Button", 265, 26, 384),
    ("kTitleDe", "Drücke die +-Taste", 263, 26, 384),
    ("kTitleFr", "Appuyez sur +", 218, 29, 256),
    ("kTitlePush", "Push +", 131, 26, 256),
    ("kTitlePushButton", "Push + button", 199, 26, 256),
]
SELECTIONS = [
    ("kSelectEn", "Press the + Button", 255, 14, 256, 16),
    ("kSelectDe", "Drücke die +-Taste", 252, 14, 256, 16),
    ("kSelectFr", "Appuyez sur +", 206, 17, 256, 0),
    ("kSelectPush", "Push +", 119, 14, 128, 16),
    ("kSelectPushButton", "Push + button", 189, 14, 256, 16),
]


def canvas() -> tuple:
    image = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
    return image, ImageDraw.Draw(image)


def text(draw: ImageDraw.ImageDraw, label: str, size: int, fill: tuple, center: tuple, font: str = ROUNDED) -> None:
    draw.text(center, label, font=ImageFont.truetype(font, size), fill=fill, anchor="mm")


def face(letter: str) -> Image.Image:
    image, draw = canvas()
    draw.ellipse((6, 6, BIG - 6, BIG - 6), fill=RIM)
    draw.ellipse((13, 13, BIG - 13, BIG - 13), fill=FACE)
    text(draw, letter, 68, WHITE, (BIG // 2, BIG // 2 + 3))
    return image


def shoulder(label: str, trigger: bool) -> Image.Image:
    image, draw = canvas()
    box = (6, 18, BIG - 6, BIG - 18) if trigger else (6, 30, BIG - 6, BIG - 30)
    draw.rounded_rectangle(box, radius=30 if trigger else 34, fill=WHITE, outline=DARK, width=LINE)
    text(draw, label, 54 if len(label) > 1 else 58, DARK, (BIG // 2, BIG // 2 + 2))
    return image


def round_button(draw: ImageDraw.ImageDraw, box: tuple, fill: tuple = WHITE) -> None:
    draw.ellipse(box, fill=fill, outline=DARK, width=LINE)


def triangle(draw: ImageDraw.ImageDraw, cx: int, cy: int, size: int, direction: str, fill: tuple = DARK) -> None:
    points = {
        "up": [(cx, cy - size), (cx - size, cy + size), (cx + size, cy + size)],
        "down": [(cx, cy + size), (cx - size, cy - size), (cx + size, cy - size)],
        "left": [(cx - size, cy), (cx + size, cy - size), (cx + size, cy + size)],
        "right": [(cx + size, cy), (cx - size, cy - size), (cx - size, cy + size)],
    }[direction]
    draw.polygon(points, fill=fill)


def stick(letter: str, pressed: bool = False, arrows: str = "") -> Image.Image:
    image, draw = canvas()
    if pressed:
        triangle(draw, BIG // 2, 14, 12, "down")
        round_button(draw, (16, 28, BIG - 16, BIG - 4))
        text(draw, letter, 56, DARK, (BIG // 2, 66 + 2))
        return image
    round_button(draw, (8, 8, BIG - 8, BIG - 8))
    text(draw, letter, 60 if not arrows else 52, DARK, (BIG // 2, BIG // 2 + 2))
    if arrows == "h":
        triangle(draw, 24, BIG // 2, 10, "left")
        triangle(draw, BIG - 24, BIG // 2, 10, "right")
    if arrows == "v":
        triangle(draw, BIG // 2, 24, 10, "up")
        triangle(draw, BIG // 2, BIG - 24, 10, "down")
    return image


def sign(plus: bool) -> Image.Image:
    image, draw = canvas()
    round_button(draw, (12, 12, BIG - 12, BIG - 12))
    c, half, thick = BIG // 2, 30, 8
    draw.rectangle((c - half, c - thick, c + half, c + thick), fill=DARK)
    if plus:
        draw.rectangle((c - thick, c - half, c + thick, c + half), fill=DARK)
    return image


def cross(draw: ImageDraw.ImageDraw, fill: tuple, highlight: str = "") -> None:
    c, arm, half = BIG // 2, 40, 14
    draw.rectangle((c - half, c - arm, c + half, c + arm), fill=fill, outline=DARK, width=4)
    draw.rectangle((c - arm, c - half, c + arm, c + half), fill=fill, outline=DARK, width=4)
    draw.rectangle((c - half + 4, c - half + 4, c + half - 4, c + half - 4), fill=fill)
    boxes = {
        "right": (c + half, c - half + 4, c + arm - 4, c + half - 4),
        "left": (c - arm + 4, c - half + 4, c - half, c + half - 4),
        "up": (c - half + 4, c - arm + 4, c + half - 4, c - half),
        "down": (c - half + 4, c + half, c + half - 4, c + arm - 4),
    }
    if highlight:
        draw.rectangle(boxes[highlight], fill=WHITE)


def dpad(highlight: str = "") -> Image.Image:
    image, draw = canvas()
    round_button(draw, (6, 6, BIG - 6, BIG - 6), WHITE if not highlight else GREY)
    cross(draw, WHITE if not highlight else (110, 110, 110, 255), highlight)
    return image


def home() -> Image.Image:
    image, draw = canvas()
    round_button(draw, (8, 8, BIG - 8, BIG - 8))
    c = BIG // 2
    draw.polygon([(c, 30), (c - 36, 62), (c + 36, 62)], fill=DARK)
    draw.rectangle((c - 24, 60, c + 24, 96), fill=DARK)
    draw.rectangle((c - 8, 74, c + 8, 96), fill=WHITE)
    return image


def cells() -> dict:
    return {
        (0, 0): face("A"), (1, 0): face("B"), (2, 0): face("X"), (3, 0): face("Y"),
        (0, 1): shoulder("ZL", True), (1, 1): shoulder("ZR", True), (2, 1): shoulder("L", False),
        (3, 1): shoulder("R", False),
        (0, 2): stick("L"), (1, 2): stick("R"), (2, 2): stick("L", pressed=True), (3, 2): stick("R", pressed=True),
        (0, 3): sign(False), (1, 3): sign(True), (2, 3): dpad(), (3, 3): home(),
        (0, 4): dpad("right"), (1, 4): dpad("left"), (2, 4): dpad("up"), (3, 4): dpad("down"),
        (0, 5): stick("L", arrows="h"), (1, 5): stick("L", arrows="v"),
    }


def atlas() -> Image.Image:
    image = Image.new("RGBA", (CELL * 4, CELL * 6), (0, 0, 0, 0))
    for (column, row), big in cells().items():
        image.alpha_composite(big.resize((CELL, CELL), Image.LANCZOS), (column * CELL, row * CELL))
    return image


def inked(color: tuple, pitch: int, coverage: Image.Image, at: tuple) -> Image.Image:
    alpha = Image.new("L", (pitch, 32), 0)
    alpha.paste(coverage, at)
    image = Image.new("RGBA", (pitch, 32), color + (0,))
    image.putalpha(alpha)
    return image


def skill_label(label: str, width: int, height: int) -> Image.Image:
    big = Image.new("L", (width * SCALE, height * SCALE), 0)
    draw = ImageDraw.Draw(big)
    size = 100
    while size > 20:
        font = ImageFont.truetype(CHALK, size)
        left, top, right, bottom = draw.textbbox((0, 0), label, font=font)
        if right - left <= (width - 2) * SCALE and bottom - top <= (height - 2) * SCALE:
            break
        size -= 2
    draw.text((width * SCALE // 2, height * SCALE // 2), label, font=font, fill=255, anchor="mm")
    small = big.resize((width, height), Image.LANCZOS).point(lambda a: round(a * CHALK_ALPHA / 255))
    return inked(CHALK_INK, 128, small, (0, 0))


def layout(label: str, width: int, height: int) -> tuple:
    size = TITLE_SIZE * SCALE
    while True:
        font = ImageFont.truetype(TITLE_THIN, round(size))
        advances = [size * WORD_GAP if ch == " " else font.getlength(ch) + size * TRACKING for ch in label]
        total = sum(advances) - size * TRACKING
        _, top, _, bottom = font.getbbox(label, anchor="ls")
        if total <= (width - 2) * SCALE and bottom - top <= height * SCALE:
            break
        size -= 1
    x = -total / 2
    glyphs = []
    for ch, advance in zip(label, advances):
        if ch != " ":
            glyphs.append((ch, x + font.getlength(ch) / 2))
        x += advance
    return round(size), glyphs, -(top + bottom) / 2


def lettering(font_path: str, placement: tuple, width: int, height: int) -> Image.Image:
    size, glyphs, baseline = placement
    font = ImageFont.truetype(font_path, size)
    coverage = Image.new("L", (width * SCALE, height * SCALE), 0)
    draw = ImageDraw.Draw(coverage)
    for ch, x in glyphs:
        draw.text((width * SCALE / 2 + x, height * SCALE / 2 + baseline), ch, font=font, fill=255, anchor="ms")
    return coverage


def title(placement: tuple, width: int, height: int, pitch: int) -> Image.Image:
    coverage = lettering(TITLE_BOLD, placement, width, height).filter(ImageFilter.GaussianBlur(CORE_BLUR * SCALE))
    glow = coverage.filter(ImageFilter.GaussianBlur(GLOW_RADIUS * SCALE)).point(lambda a: min(GLOW_CAP, int(a * GLOW_GAIN)))
    small = ImageChops.screen(coverage, glow).resize((width, height), Image.LANCZOS)
    return inked(TITLE_INK, pitch, small, (0, 0))


def selection(placement: tuple, width: int, height: int, pitch: int, top: int) -> Image.Image:
    coverage = lettering(TITLE_THIN, placement, width, height)
    coverage = Image.blend(coverage, coverage.filter(ImageFilter.MaxFilter(3)), 0.5).resize((width, height), Image.LANCZOS)
    return inked(SELECTION_INK, pitch, coverage, (0, top))


def blocks(name: str, items: list) -> str:
    rows = [f"{{0x{offset:05X}, {{{', '.join(f'0x{b:02X}' for b in data)}}}}}" for offset, data in items]
    return f"inline constexpr Block {name}[] = {{\n    " + ",\n    ".join(rows) + "};\n"


def preview(images: list, path: Path) -> None:
    width = max(i.width * 3 for i in images)
    height = sum(i.height * 3 + 8 for i in images)
    sheet = Image.new("RGBA", (width, height), (70, 70, 100, 255))
    y = 0
    for image in images:
        sheet.alpha_composite(image.resize((image.width * 3, image.height * 3), Image.NEAREST), (0, y))
        y += image.height * 3 + 8
    sheet.save(path)


def main() -> int:
    PREVIEW.mkdir(parents=True, exist_ok=True)
    icons = atlas()
    icons.save(PREVIEW / "atlas.png")
    parts = ["#pragma once\n\n#include <cstdint>\n\nnamespace nier::switch_prompts {\n\n",
             "struct Block {\n  uint32_t offset;\n  uint8_t bytes[16];\n};\n\n"]
    atlas_blocks = []
    for column, row in sorted(cells(), key=lambda cell: (cell[1], cell[0])):
        atlas_blocks += xenos_dxt.encode_blocks(icons, 5, column * CELL, row * CELL, CELL, CELL)
    parts.append(blocks("kIconAtlas", atlas_blocks))
    labels = []
    for label, width, height in SKILL_LABELS:
        image = skill_label(label, width, height)
        labels.append(image)
        parts.append("\n" + blocks(f"kSkill{label}", xenos_dxt.encode_blocks(image, 3, 0, 0, width, height)))
    titles = []
    placements = {label: layout(label, width, height) for _, label, width, height, _, _ in SELECTIONS}
    for name, label, width, height, pitch in TITLES:
        image = title(placements[label], width, height, pitch)
        titles.append(image.crop((0, 0, width, height)))
        parts.append("\n" + blocks(name, xenos_dxt.encode_blocks(image, 3, 0, 0, width, height)))
    for name, label, width, height, pitch, top in SELECTIONS:
        image = selection(placements[label], width, height, pitch, top)
        titles.append(image.crop((0, top, width, top + height)))
        parts.append("\n" + blocks(name, xenos_dxt.encode_blocks(image, 3, 0, top, width, height)))
    parts.append("\n}  // namespace nier::switch_prompts\n")
    preview([icons] + labels + titles, PREVIEW / "preview.png")
    HEADER.write_text("".join(parts))
    print(PREVIEW / "preview.png", HEADER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
