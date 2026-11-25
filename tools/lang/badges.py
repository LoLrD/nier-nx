import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_prompts"))

import cpk  # noqa: E402
import title  # noqa: E402
import typeface  # noqa: E402
import xenos_dxt  # noqa: E402

PACK = "2D/COMMON"
FACE = ("/System/Library/Fonts/HelveticaNeue.ttc", 1)
CONDENSED = ("/System/Library/Fonts/HelveticaNeue.ttc", 4)
SCALE = 8
GRAIN = 0.3
GRAIN_DEPTH = 0.7
GLOW_RADIUS = 1.5
HAZE_LIMIT = 128
HAZE_PERCENTILE = 10
SEED = 7
BADGES = {
    "icn_item_all": {"erase": (5, 20, 56, 40), "letters": (5, 21, 55, 39), "face": FACE, "ink": (46, 52, 76),
                     "glow": 0.0},
    "new_new": {"erase": (7, 6, 53, 24), "letters": (8, 8, 52, 22), "face": CONDENSED, "ink": (255, 255, 255),
                "glow": 0.5},
}


def lettering(shape: tuple, letters: tuple, label: str, face: tuple) -> np.ndarray:
    height, width = shape
    x0, y0, x1, y1 = letters
    probe = typeface.Face(face[0], 100, face[1])
    cap = -probe.getbbox("Н", anchor="ls")[1]
    font = typeface.Face(face[0], round(100 * (y1 - y0) * SCALE / cap), face[1])
    left, _, right, _ = font.getbbox(label, anchor="ls")
    strip = Image.new("L", (right - left + 2 * SCALE, height * SCALE), 0)
    font.text(strip, (SCALE - left, y1 * SCALE), label, "ls")
    squeeze = min(1.0, (x1 - x0) * SCALE / (right - left))
    strip = strip.resize((max(1, round(strip.width * squeeze)), strip.height), Image.LANCZOS)
    ink_left, _, ink_right, _ = strip.getbbox()
    canvas = Image.new("L", (width * SCALE, height * SCALE), 0)
    canvas.paste(strip, (round((x0 + x1) / 2 * SCALE - (ink_left + ink_right) / 2), 0))
    holes = np.random.default_rng(SEED).random((height * SCALE // 2, width * SCALE // 2)) < GRAIN
    grain = 1.0 - GRAIN_DEPTH * np.kron(holes, np.ones((2, 2)))
    coverage = Image.fromarray((np.array(canvas) * grain).astype(np.uint8)).resize((width, height), Image.BOX)
    return np.array(coverage)


def relabel(alpha: np.ndarray, erase: tuple, letters: tuple, label: str, face: tuple, glow: float = 0.0) -> np.ndarray:
    result = alpha.copy()
    x0, y0, x1, y1 = erase
    region = alpha[y0:y1, x0:x1]
    result[y0:y1, x0:x1] = np.percentile(region[region < HAZE_LIMIT], HAZE_PERCENTILE)
    ink = lettering(alpha.shape, letters, label, face)
    halo = np.array(Image.fromarray(ink).filter(ImageFilter.GaussianBlur(GLOW_RADIUS))) * glow
    return np.maximum(result, np.maximum(ink, halo.astype(np.uint8)))


def build(archive: Path, out: Path, labels: dict) -> int:
    index = cpk.read(archive, PACK + ".2DV")
    pixels = bytearray(cpk.read(archive, PACK + ".2DP"))
    found = title.textures(index)
    for name, badge in BADGES.items():
        texture = found[name]
        width, height = texture["width"], texture["height"]
        raw = pixels[texture["data"]:texture["data"] + texture["bytes"]]
        image = xenos_dxt.decode(raw, texture["pitch"], max(32, math.ceil(height / 32) * 32), texture["kind"])
        alpha = np.array(image.getchannel("A"))
        drawn = relabel(alpha[:height, :width], badge["erase"], badge["letters"], labels[name], badge["face"],
                        badge["glow"])
        rows, columns = np.nonzero(drawn != alpha[:height, :width])
        left, top = columns.min() // 4 * 4, rows.min() // 4 * 4
        right, bottom = columns.max() // 4 * 4 + 4, rows.max() // 4 * 4 + 4
        alpha[:height, :width] = drawn
        colors = np.array(image.convert("RGB"))
        colors[top:bottom, left:right] = badge["ink"]
        result = Image.fromarray(np.dstack([colors, alpha]), "RGBA")
        for at, block in xenos_dxt.encode_blocks(result, texture["kind"], left, top, right - left, bottom - top):
            pixels[texture["data"] + at:texture["data"] + at + len(block)] = block
    target = out / (PACK + ".2DP")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(bytes(pixels))
    return len(BADGES)
