import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_prompts"))

import cpk  # noqa: E402
import draw  # noqa: E402
import kpky  # noqa: E402
import xenos_dxt  # noqa: E402

PACKS = ["2D/TITLE", "2D/TITLE_GESTALT_US"]
WHITE = (255, 255, 255)
GROUPS = [
    ("title_gameselection", WHITE, ["title_new", "title_load", "title_setting"]),
    ("title_gameselection_dl", WHITE, ["title_download content"]),
    ("title_diffselection", draw.SELECTION_INK, ["title_easy", "title_normal", "title_hard"]),
    ("title_startselection", draw.SELECTION_INK, ["title_start"]),
]
THIN_MARGIN = 2
GLOW_MARGIN = 4
BOLD_ROWS = 32
SPRITE_PREFIX = "title_"


def centered(rect: tuple, width: int, height: int) -> tuple:
    x, y, w, h = rect
    return x + (w - width) / 2, y + (h - height) / 2, float(width), float(height)


def size_field(f2: int, width: int, height: int) -> int:
    return (f2 & ~0x3FFFFFF) | (width - 1) | ((height - 1) << 13)


def fit_size(measure, limits: list, start: int) -> int:
    size = start
    while any(value > limit for value, limit in zip(measure(size), limits)):
        size -= 1
    return size


def line_centers(alpha: np.ndarray) -> list:
    groups = []
    for row in np.where((alpha > 100).any(axis=1))[0]:
        if groups and row == groups[-1][1] + 1:
            groups[-1][1] = row
        else:
            groups.append([row, row])
    return [(first + last + 1) / 2 for first, last in groups]


def textures(index: bytes) -> dict:
    heap, _ = kpky.find(index, b"HEAP")
    entries_base, names_base, descriptors = struct.unpack_from(">III", index, heap + 8)
    count = struct.unpack_from(">I", index, heap + 0x1C)[0]
    found = {}
    for i in range(count):
        tag, name_at, _, descriptor, _, data, size = struct.unpack_from(">4sIIIIII", index, heap + entries_base + 0x20 * i)
        if tag != b"TX2D":
            continue
        name = index[heap + names_base + name_at:].split(b"\0")[0].decode("latin-1")
        at = heap + descriptors + descriptor + 28
        f0, f1, f2 = struct.unpack_from(">III", index, at)
        found[name] = {"size_at": at + 8, "f2": f2, "data": data, "bytes": size, "kind": 3 if f1 & 0x3F == 0x13 else 5,
                       "pitch": ((f0 >> 22) & 0x1FF) * 32, "width": (f2 & 0x1FFF) + 1,
                       "height": ((f2 >> 13) & 0x1FFF) + 1}
    return found


def sprites(index: bytes) -> dict:
    nested, _ = next((offset, size) for offset, size in kpky.entries(index)
                     if offset and index[offset:offset + 4] == b"KPKy")
    spd = nested + kpky.entries(index[nested:])[0][0]
    table, count = struct.unpack_from(">II", index, spd + 0xC)
    found = {}
    for i in range(count):
        at = spd + table + 32 * i
        name_at = struct.unpack_from(">I", index, at + 4)[0]
        name = index[spd + name_at:].split(b"\0")[0].decode("latin-1")
        found[SPRITE_PREFIX + name] = {"rect_at": at + 16, "rect": struct.unpack_from(">4f", index, at + 16)}
    return found


def decode_alpha(pixels: bytes, texture: dict) -> np.ndarray:
    raw = pixels[texture["data"]:texture["data"] + texture["bytes"]]
    rows = max(32, math.ceil(texture["height"] / 32) * 32)
    top = 16 if texture["height"] < 16 else 0
    image = xenos_dxt.decode(raw, texture["pitch"], rows, texture["kind"])
    return np.array(image.getchannel("A"))[top:top + texture["height"], :texture["width"]]


def bold_extent(placement: tuple) -> tuple:
    coverage = draw.lettering(draw.TITLE_BOLD, placement, 512, 64)
    left, top, right, bottom = coverage.getbbox()
    return (right - left) / draw.SCALE, (bottom - top) / draw.SCALE


def write_texture(index: bytearray, pixels: bytearray, texture: dict, image: Image.Image, width: int, height: int,
                  top: int) -> None:
    for at, block in xenos_dxt.encode_blocks(image, texture["kind"], 0, top, width, height):
        pixels[texture["data"] + at:texture["data"] + at + 16] = block
    struct.pack_into(">I", index, texture["size_at"], size_field(texture["f2"], width, height))


def move_sprite(index: bytearray, sprite: dict, width: int, height: int) -> None:
    struct.pack_into(">4f", index, sprite["rect_at"], *centered(sprite["rect"], width, height))


def render_group(index: bytearray, pixels: bytearray, thin_name: str, ink: tuple, items: list, labels: dict) -> None:
    found, rects = textures(index), sprites(index)
    thin = found[thin_name]
    bold = [found[name] for name in items]
    words = [labels[name] for name in items]

    def measure(size: int) -> list:
        values = []
        for word in words:
            placement = draw.placement(word, size)
            values += [draw.measure(word, size)[2] / draw.SCALE, bold_extent(placement)[0]]
        return values

    limits = []
    for texture in bold:
        limits += [thin["pitch"] - 2 * THIN_MARGIN, texture["pitch"] - 2 * GLOW_MARGIN]
    size = fit_size(measure, limits, round(draw.TITLE_SIZE * draw.SCALE))
    placements = [draw.placement(word, size) for word in words]
    for name, texture, placement in zip(items, bold, placements):
        extent_width, extent_height = bold_extent(placement)
        width = min(texture["pitch"], max(texture["width"], math.ceil(extent_width) + 2 * GLOW_MARGIN))
        height = min(BOLD_ROWS, max(texture["height"], math.ceil(extent_height) + 2 * GLOW_MARGIN + 2))
        write_texture(index, pixels, texture, draw.title(placement, width, height, texture["pitch"]), width, height, 0)
        move_sprite(index, rects[name], width, height)
    centers = line_centers(decode_alpha(pixels, thin))
    spans = [(bottom - top) / draw.SCALE for _, _, _, top, bottom in (draw.measure(word, size) for word in words)]
    pad = max(0, math.ceil(max(max(span / 2 - center, center + span / 2 - thin["height"]) + 1
                               for center, span in zip(centers, spans))))
    width = min(thin["pitch"], max(thin["width"], math.ceil(max(measure(size)[0::2])) + 2 * THIN_MARGIN))
    height = thin["height"] + 2 * pad
    coverage = Image.new("L", (width * draw.SCALE, height * draw.SCALE), 0)
    for (scaled, glyphs, baseline), center in zip(placements, centers):
        shifted = (scaled, glyphs, baseline + (center + pad - height / 2) * draw.SCALE)
        coverage = ImageChops.lighter(coverage, draw.lettering(draw.TITLE_THIN, shifted, width, height))
    coverage = Image.blend(coverage, coverage.filter(ImageFilter.MaxFilter(3)), 0.5).resize((width, height), Image.LANCZOS)
    top = 16 if height < 16 else 0
    rows = max(32, math.ceil((top + height) / 4) * 4)
    image = draw.inked(ink, thin["pitch"], coverage, (0, top), rows)
    write_texture(index, pixels, thin, image, width, height, top)
    move_sprite(index, rects[thin_name], width, height)


def build(archive: Path, out: Path, labels: dict) -> int:
    drawn = 0
    for pack in PACKS:
        index = bytearray(cpk.read(archive, pack + ".2DV"))
        pixels = bytearray(cpk.read(archive, pack + ".2DP"))
        for thin_name, ink, items in GROUPS:
            if all(name in labels for name in items):
                render_group(index, pixels, thin_name, ink, items, labels)
                drawn += len(items)
        for extension, data in ((".2DV", index), (".2DP", pixels)):
            target = out / (pack + extension)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(bytes(data))
    return drawn
