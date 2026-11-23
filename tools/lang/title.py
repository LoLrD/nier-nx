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

WHITE = (255, 255, 255)
GAME_OVER_INK = (66, 67, 66)
LEVEL_GLOW_INK = (206, 206, 206)
LEVEL_INK = (231, 231, 231)
LEVEL_SIZE = 32
SHARP = {"tracking": 0.04, "blur": 0.15, "radius": 2.0, "face": draw.TITLE_BOLD}
TITLE_GROUPS = [
    {"thin": "title_gameselection", "ink": WHITE, "items": ["title_new", "title_load", "title_setting"]},
    {"thin": "title_gameselection_dl", "ink": WHITE, "items": ["title_download content"]},
    {"thin": "title_diffselection", "ink": draw.SELECTION_INK, "items": ["title_easy", "title_normal", "title_hard"]},
    {"thin": "title_startselection", "ink": draw.SELECTION_INK, "items": ["title_start"]},
]
PACKS = {
    "2D/TITLE": TITLE_GROUPS,
    "2D/TITLE_GESTALT_US": TITLE_GROUPS,
    "2D/GAME_EVENT": [
        {"thin": "game_over_un", "ink": GAME_OVER_INK, "items": ["game_over_continue", "game_over_exit"], "axis": "x",
         **SHARP},
        {"glow": "levelup_levelup_a_D5", "crisp": ["levelup_$levelup_b_D5", "levelup_=ins_0_levelup_b_D5"], **SHARP},
    ],
    "2D/BATTLE": [{"thin": "chain_chain", "ink": WHITE, "items": []}],
}
THIN_MARGIN = 2
GLOW_MARGIN = 4
BOLD_ROWS = 32
WORD_GAP = 8


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


def line_centers(alpha: np.ndarray, gap: int = 1) -> list:
    groups = []
    for row in np.where((alpha > 100).any(axis=1))[0]:
        if groups and row - groups[-1][1] <= gap:
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
    found = {}
    for nested, _ in kpky.entries(index):
        if not nested or index[nested:nested + 4] != b"KPKy":
            continue
        entries = kpky.entries(index[nested:])
        count = struct.unpack_from(">I", index, nested + 4)[0]
        names_at = nested + 12 + 8 * count + 4
        for number, (offset, _) in enumerate(entries):
            spd = nested + offset
            if index[spd:spd + 4] != b"0DPS":
                continue
            base = index[names_at + 32 * number:].split(b"\0")[0].decode("latin-1").removesuffix(".spd")
            table, total = struct.unpack_from(">II", index, spd + 0xC)
            for i in range(total):
                at = spd + table + 32 * i
                name_at = struct.unpack_from(">I", index, at + 4)[0]
                name = index[spd + name_at:].split(b"\0")[0].decode("latin-1")
                found[f"{base}_{name}"] = {"rect_at": at + 16, "rect": struct.unpack_from(">4f", index, at + 16)}
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


def rows_for(height: int, top: int = 0) -> int:
    return max(32, math.ceil((top + height) / 4) * 4)


def render_group(index: bytearray, pixels: bytearray, group: dict, labels: dict) -> int:
    found, rects = textures(index), sprites(index)
    thin = found[group["thin"]]
    items = group["items"]
    bold = [found[name] for name in items]
    words = [labels[name] for name in items] or [labels[group["thin"]]]
    layout = (group.get("tracking", draw.TRACKING), group.get("face", draw.TITLE_THIN))

    def measure(size: int) -> list:
        values = []
        for word in words:
            values.append(draw.measure(word, size, *layout)[2] / draw.SCALE)
            if bold:
                values.append(bold_extent(draw.placement(word, size, *layout))[0])
        return values

    limits = []
    for texture in bold or [None] * len(words):
        limits.append(thin["pitch"] - 2 * THIN_MARGIN)
        if texture:
            limits.append(texture["pitch"] - 2 * GLOW_MARGIN)
    size = fit_size(measure, limits, round(draw.TITLE_SIZE * draw.SCALE))
    placements = [draw.placement(word, size, *layout) for word in words]
    for name, texture, placement in zip(items, bold, placements):
        extent_width, extent_height = bold_extent(placement)
        width = min(texture["pitch"], max(texture["width"], math.ceil(extent_width) + 2 * GLOW_MARGIN))
        height = min(BOLD_ROWS, max(texture["height"], math.ceil(extent_height) + 2 * GLOW_MARGIN + 2))
        image = draw.title(placement, width, height, texture["pitch"], draw.TITLE_INK, group.get("blur", draw.CORE_BLUR),
                           group.get("radius", draw.GLOW_RADIUS))
        write_texture(index, pixels, texture, image, width, height, 0)
        move_sprite(index, rects[name], width, height)
    alpha = decode_alpha(pixels, thin)
    centers = line_centers(alpha)
    if group.get("axis") == "x":
        columns = line_centers(alpha.T, WORD_GAP)
        centers = centers[:1] * len(words)
    else:
        columns = [thin["width"] / 2] * len(words)
    widths = [draw.measure(word, size, *layout)[2] / draw.SCALE for word in words]
    spans = [(bottom - top) / draw.SCALE for _, _, _, top, bottom in (draw.measure(word, size, *layout) for word in words)]
    pad = max(0, math.ceil(max(max(span / 2 - center, center + span / 2 - thin["height"]) + 1
                               for center, span in zip(centers, spans))))
    grow = max(max(word / 2 + THIN_MARGIN - column, column + word / 2 + THIN_MARGIN - thin["width"])
               for word, column in zip(widths, columns))
    width = min(thin["pitch"], thin["width"] + max(0, math.ceil(2 * grow)))
    height = thin["height"] + 2 * pad
    coverage = Image.new("L", (width * draw.SCALE, height * draw.SCALE), 0)
    for (scaled, glyphs, baseline), center, column in zip(placements, centers, columns):
        dx = (column + (width - thin["width"]) / 2 - width / 2) * draw.SCALE
        shifted = (scaled, [(ch, x + dx) for ch, x in glyphs],
                   baseline + (center + pad - height / 2) * draw.SCALE)
        coverage = ImageChops.lighter(coverage, draw.lettering(draw.TITLE_THIN, shifted, width, height))
    coverage = Image.blend(coverage, coverage.filter(ImageFilter.MaxFilter(3)), 0.5).resize((width, height), Image.LANCZOS)
    top = 16 if height < 16 else 0
    image = draw.inked(group["ink"], thin["pitch"], coverage, (0, top), rows_for(height, top))
    write_texture(index, pixels, thin, image, width, height, top)
    move_sprite(index, rects[group["thin"]], width, height)
    return len(bold) + 1


def render_pair(index: bytearray, pixels: bytearray, group: dict, labels: dict) -> int:
    found, rects = textures(index), sprites(index)
    glow = found[group["glow"]]
    crisp = [found[name] for name in group["crisp"]]
    word = labels[group["glow"]]
    layout = (group["tracking"], group["face"])
    limits = [glow["pitch"] - 2 * GLOW_MARGIN, min(texture["pitch"] for texture in crisp) - 2 * THIN_MARGIN]
    size = fit_size(lambda size: [bold_extent(draw.placement(word, size, *layout))[0]] * 2, limits,
                    round(LEVEL_SIZE * draw.SCALE))
    placement = draw.placement(word, size, *layout)
    extent_width, _ = bold_extent(placement)
    width = min(glow["pitch"], max(glow["width"], math.ceil(extent_width) + 2 * GLOW_MARGIN))
    image = draw.title(placement, width, glow["height"], glow["pitch"], LEVEL_GLOW_INK, group["blur"], group["radius"])
    write_texture(index, pixels, glow, image, width, glow["height"], 0)
    move_sprite(index, rects[group["glow"]], width, glow["height"])
    for name, texture in zip(group["crisp"], crisp):
        width = min(texture["pitch"], max(texture["width"], math.ceil(extent_width) + 2 * THIN_MARGIN))
        coverage = draw.lettering(draw.TITLE_BOLD, placement, width, texture["height"])
        coverage = coverage.resize((width, texture["height"]), Image.LANCZOS)
        image = draw.inked(LEVEL_INK, texture["pitch"], coverage, (0, 0), rows_for(texture["height"]))
        write_texture(index, pixels, texture, image, width, texture["height"], 0)
        if name in rects:
            move_sprite(index, rects[name], width, texture["height"])
    return 1 + len(crisp)


def labelled(group: dict, labels: dict) -> bool:
    if "glow" in group:
        return group["glow"] in labels
    return all(name in labels for name in group["items"] or [group["thin"]])


def build(archive: Path, out: Path, labels: dict) -> int:
    drawn = 0
    for pack, groups in PACKS.items():
        index = bytearray(cpk.read(archive, pack + ".2DV"))
        pixels = bytearray(cpk.read(archive, pack + ".2DP"))
        for group in groups:
            if labelled(group, labels):
                drawn += (render_pair if "glow" in group else render_group)(index, pixels, group, labels)
        for extension, data in ((".2DV", index), (".2DP", pixels)):
            target = out / (pack + extension)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(bytes(data))
    return drawn
