import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "switch_prompts"))

import cpk  # noqa: E402
import xenos_dxt  # noqa: E402

FACE = "/System/Library/Fonts/Times.ttc"
FACE_INDEX = 1
SIZE = 31.0
SQUEEZE = 0.95
TRACKING = 0.8
BASELINE_LIFT = 2
FAINT = 8
SCALE = 4
ORIGIN_STEP = 80
CODES = [0x401, *range(0x410, 0x450), 0x451, 0x2026, 0x2116]
ATLAS = 1024
PAGE = 1
PAGE_BYTES = {1: 2, 2: 1, 4: 0, 8: 3}
VRAM_HEADER = 0x1000
MAIN = "FONT/FONT_MAIN.X360.BIN"
VRAM = "FONT/FONT_VRAM.X360.BIN"


def load_font() -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FACE, round(SIZE * SCALE), index=FACE_INDEX)


def round_up(value: int, step: int) -> int:
    return -(-value // step) * step


def render_glyph(face: ImageFont.FreeTypeFont, code: int) -> tuple:
    char = chr(code)
    left, top, right, bottom = face.getbbox(char, anchor="ls")
    origin_x = round_up(max(0, -left) + 2 * SCALE, ORIGIN_STEP)
    origin_y = round_up(max(0, -top) + 2 * SCALE, SCALE)
    width = round_up(origin_x + right + 2 * SCALE, ORIGIN_STEP)
    height = round_up(origin_y + max(0, bottom) + 2 * SCALE, SCALE)
    big = Image.new("L", (width, height), 0)
    ImageDraw.Draw(big).text((origin_x, origin_y), char, font=face, fill=255, anchor="ls")
    squeezed = big.resize((round(width * SQUEEZE), height), Image.LANCZOS)
    small = np.array(squeezed.resize((squeezed.width // SCALE, height // SCALE), Image.LANCZOS))
    small[small < FAINT] = 0
    rows = np.where(small.any(axis=1))[0]
    columns = np.where(small.any(axis=0))[0]
    image = small[rows.min():rows.max() + 1, columns.min():columns.max() + 1]
    bearing_x = int(columns.min()) - round(origin_x * SQUEEZE) // SCALE
    bearing_y = int(rows.min()) - origin_y // SCALE - BASELINE_LIFT
    advance = round(face.getlength(char) * SQUEEZE / SCALE + TRACKING)
    return image, bearing_x, bearing_y, advance


def pack(sizes: list, top: int, width: int, height: int, gap: int = 2) -> list:
    x, y, row_height = 0, top, 0
    spots = []
    for glyph_width, glyph_height in sizes:
        if x + glyph_width > width:
            x, y, row_height = 0, y + row_height + gap, 0
        if y + glyph_height > height:
            raise ValueError("the glyphs do not fit in the atlas")
        spots.append((x, y))
        x += glyph_width + gap
        row_height = max(row_height, glyph_height)
    return spots


def record(x: int, y: int, width: int, height: int, bearing_x: int, bearing_y: int, advance: int, page: int) -> bytes:
    right = bearing_x + width - 1
    bottom = bearing_y + height - 1
    return struct.pack(">HHHHbbbbBBbB", x, y, x + width - 1, y + height - 1, bearing_x, bearing_y, right, bottom,
                       page, advance, advance - right + 1, 1)


def put_glyph(texture: bytearray, page: int, x: int, y: int, image: np.ndarray) -> None:
    channel = PAGE_BYTES[page]
    for row in range(image.shape[0]):
        for column in range(image.shape[1]):
            texture[xenos_dxt.tiled_offset(x + column, y + row, ATLAS, 2) + channel] = int(image[row, column])


def glyph_table(main: bytes) -> tuple:
    entries = struct.unpack_from(">I", main, 4)[0]
    offsets = struct.unpack_from(f">{entries}I", main, 8)
    sizes = struct.unpack_from(f">{entries}I", main, 8 + 4 * entries)
    start, size = next((o, s) for o, s in zip(offsets, sizes) if main[o:o + 4] == b"TNOF")
    count = struct.unpack_from(">I", main, start + 4)[0]
    codes_at = start + size - 4 * count
    records_at = codes_at - 16 * count
    codes = [struct.unpack_from(">H", main, codes_at + 4 * i)[0] for i in range(count)]
    return records_at, codes


def build(archive: Path, out: Path) -> int:
    main = bytearray(cpk.read(archive, MAIN))
    vram = bytearray(cpk.read(archive, VRAM))
    texture = bytearray(vram[VRAM_HEADER:VRAM_HEADER + ATLAS * ATLAS * 4])
    records_at, codes = glyph_table(main)
    index = {code: i for i, code in enumerate(codes)}
    used = [struct.unpack_from(">HHHHbbbbB", main, records_at + 16 * i) for i in range(len(codes))]
    top = max(entry[3] for entry in used if entry[8] == PAGE) + 4
    face = load_font()
    glyphs = [render_glyph(face, code) for code in CODES]
    spots = pack([(image.shape[1], image.shape[0]) for image, *_ in glyphs], top, ATLAS, ATLAS)
    for code, (image, bearing_x, bearing_y, advance), (x, y) in zip(CODES, glyphs, spots):
        put_glyph(texture, PAGE, x, y, image)
        at = records_at + 16 * index[code]
        main[at:at + 16] = record(x, y, image.shape[1], image.shape[0], bearing_x, bearing_y, advance, PAGE)
    vram[VRAM_HEADER:VRAM_HEADER + len(texture)] = texture
    for path, data in ((MAIN, main), (VRAM, vram)):
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(bytes(data))
    return len(CODES)
