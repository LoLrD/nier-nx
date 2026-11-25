import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_prompts"))

import cpk  # noqa: E402
import title  # noqa: E402
import typeface  # noqa: E402
import xenos_dxt  # noqa: E402

PACK = "2D/SCRIPT/FLASH01_US"
REPORTS = {
    "flash01_$flash01_D1": ("875:2", [(488, 205), (950, 85), (1015, 300), (600, 500)]),
    "flash01_$flash02_D1": ("876:2", [(455, 170), (955, 170), (955, 440), (455, 440)]),
    "flash01_$flash03_D1": ("877:2", [(500, 140), (930, 160), (905, 460), (470, 440)]),
    "flash01_$flash04_D1": ("878:2", [(480, 195), (935, 160), (975, 425), (515, 480)]),
}
FACE = "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf"
REDACTED = "«Нуар»"
PLAYER = "Ниер"
SCALE = 4
PAPER_FILTER = 21
TEXT_CONTRAST = 45
ROWS = 13
LAST_ROW = 0.75
INK_CONTRAST = 100
INK_OPACITY = 0.92
LINE_TO_SIZE = 1.22


def wrap(text: str, width: float, measure) -> list:
    lines = []
    for paragraph in text.split("\n"):
        line = ""
        for word in paragraph.split():
            candidate = f"{line} {word}" if line else word
            if line and measure(candidate) > width:
                lines.append(line)
                line = word
            else:
                line = candidate
        lines.append(line)
    return lines


def oriented_box(points: np.ndarray) -> tuple:
    best = None
    for tenth in range(-250, 251, 2):
        angle = math.radians(tenth / 10)
        u = points[:, 0] * math.cos(angle) + points[:, 1] * math.sin(angle)
        v = -points[:, 0] * math.sin(angle) + points[:, 1] * math.cos(angle)
        low_u, high_u = np.percentile(u, [0.3, 99.7])
        low_v, high_v = np.percentile(v, [0.3, 99.7])
        area = (high_u - low_u) * (high_v - low_v)
        if best is None or area < best[0]:
            best = (area, tenth / 10, low_u, high_u, low_v, high_v)
    return best[1:]


def word_span(line: str, word: str, measure) -> tuple | None:
    at = line.find(word)
    if at < 0:
        return None
    return measure(line[:at]), measure(line[:at + len(word)])


def redraw(image: Image.Image, polygon: list, text: str) -> Image.Image:
    rgb = np.array(image.convert("RGB")).astype(float)
    luminance = np.array(image.convert("L")).astype(float)
    paper_image = (image.convert("RGB").filter(ImageFilter.MaxFilter(PAPER_FILTER))
                   .filter(ImageFilter.MinFilter(PAPER_FILTER)).filter(ImageFilter.GaussianBlur(2)))
    paper = np.array(paper_image).astype(float)
    paper_luminance = np.array(paper_image.convert("L")).astype(float)
    region = Image.new("L", image.size, 0)
    ImageDraw.Draw(region).polygon(polygon, fill=255)
    letters = (np.array(region) > 0) & (luminance < paper_luminance - TEXT_CONTRAST)
    ink = np.median(rgb[letters & (luminance < paper_luminance - INK_CONTRAST)], axis=0)
    ys, xs = np.nonzero(letters)
    angle, low_u, high_u, low_v, high_v = oriented_box(np.stack([xs, ys], 1).astype(float))
    radians = math.radians(angle)
    pitch = (high_v - low_v) / (ROWS - 1 + LAST_ROW)
    spread = np.array(Image.fromarray((letters * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(7))) > 0
    cleaned = np.where(spread[..., None], paper, rgb)
    font = typeface.Face(FACE, round(pitch / LINE_TO_SIZE * SCALE))
    width = (high_u - low_u) * SCALE
    lines = wrap(text.replace("<NIER>", PLAYER), width, font.getlength)[:ROWS]
    ascent = font.getbbox("Н", anchor="ls")[1]
    flat = Image.new("L", (math.ceil(width), math.ceil((ROWS + 1) * pitch * SCALE)), 0)
    draw = ImageDraw.Draw(flat)
    for number, line in enumerate(lines):
        baseline = number * pitch * SCALE - ascent
        font.text(flat, (0, baseline), line, "ls")
        span = word_span(line, REDACTED, font.getlength)
        if span:
            draw.rounded_rectangle((span[0] - SCALE, baseline + ascent - SCALE, span[1] + SCALE, baseline + 2 * SCALE),
                                   radius=3 * SCALE, fill=255)
    cos, sin = math.cos(radians), math.sin(radians)
    warped = flat.transform((image.width * SCALE, image.height * SCALE), Image.AFFINE,
                            (cos, sin, -low_u * SCALE, -sin, cos, -low_v * SCALE), resample=Image.BICUBIC)
    coverage = np.array(warped.resize(image.size, Image.LANCZOS)).astype(float)[..., None] / 255 * INK_OPACITY
    result = cleaned * (1 - coverage) + ink * coverage
    return Image.fromarray(np.clip(result, 0, 255).astype(np.uint8)).convert("RGBA")


def decode(pixels: bytes, texture: dict) -> Image.Image:
    raw = pixels[texture["data"]:texture["data"] + texture["bytes"]]
    rows = max(32, math.ceil(texture["height"] / 32) * 32)
    return xenos_dxt.decode(raw, texture["pitch"], rows, 1).crop((0, 0, texture["width"], texture["height"]))


def build(archive: Path, out: Path, texts: dict) -> int:
    index = cpk.read(archive, PACK + ".2DV")
    pixels = bytearray(cpk.read(archive, PACK + ".2DP"))
    found = title.textures(index)
    for name, (key, polygon) in REPORTS.items():
        texture = found[name]
        image = redraw(decode(pixels, texture), polygon, texts[key])
        for at, block in xenos_dxt.encode_blocks(image, 1, 0, 0, texture["width"], texture["height"]):
            pixels[texture["data"] + at:texture["data"] + at + len(block)] = block
    target = out / (PACK + ".2DP")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(bytes(pixels))
    return len(REPORTS)
