import struct

from PIL import Image


def tiled_offset(x: int, y: int, pitch: int, bpp_log2: int) -> int:
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def swap16(block: bytes) -> bytes:
    out = bytearray(len(block))
    out[0::2] = block[1::2]
    out[1::2] = block[0::2]
    return bytes(out)


def to565(color: tuple) -> int:
    return (color[0] >> 3) << 11 | (color[1] >> 2) << 5 | color[2] >> 3


def from565(value: int) -> tuple:
    r, g, b = value >> 11, (value >> 5) & 63, value & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def color_palette(c0: int, c1: int) -> list:
    p0, p1 = from565(c0), from565(c1)
    return [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]


def nearest(palette: list, value) -> int:
    if isinstance(value, int):
        return min(range(len(palette)), key=lambda i: abs(palette[i] - value))
    return min(range(len(palette)), key=lambda i: sum((a - b) ** 2 for a, b in zip(palette[i], value)))


def encode_color(pixels: list) -> bytes:
    visible = [p[:3] for p in pixels if p[3]] or [(0, 0, 0)]
    luma = [r * 3 + g * 6 + b for r, g, b in visible]
    c0 = to565(visible[luma.index(max(luma))])
    c1 = to565(visible[luma.index(min(luma))])
    if c0 < c1:
        c0, c1 = c1, c0
    palette = color_palette(c0, c1)
    bits = 0
    for i, p in enumerate(pixels):
        bits |= nearest(palette, p[:3]) << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def alpha_palette(a0: int, a1: int) -> list:
    if a0 > a1:
        return [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    return [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]


def encode_alpha5(pixels: list) -> bytes:
    alphas = [p[3] for p in pixels]
    a0, a1 = max(alphas), min(alphas)
    palette = alpha_palette(a0, a1)
    bits = 0
    for i, a in enumerate(alphas):
        bits |= nearest(palette, a) << (3 * i)
    return bytes([a0, a1]) + bits.to_bytes(6, "little")


def encode_alpha3(pixels: list) -> bytes:
    bits = 0
    for i, p in enumerate(pixels):
        bits |= ((p[3] * 15 + 127) // 255) << (4 * i)
    return bits.to_bytes(8, "little")


def block_pixels(image: Image.Image, bx: int, by: int) -> list:
    return [image.getpixel((bx * 4 + (i & 3), by * 4 + (i >> 2))) for i in range(16)]


def block_log2(kind: int) -> int:
    return 3 if kind == 1 else 4


def encode_block(image: Image.Image, kind: int, bx: int, by: int) -> bytes:
    pixels = block_pixels(image, bx, by)
    if kind == 1:
        return swap16(encode_color(pixels))
    alpha = encode_alpha5(pixels) if kind == 5 else encode_alpha3(pixels)
    return swap16(alpha + encode_color(pixels))


def encode(image: Image.Image, kind: int, allocated_height: int) -> bytes:
    image = image.convert("RGBA")
    pitch = ((image.width // 4) + 31) & ~31
    rows = allocated_height // 4
    out = bytearray(pitch * rows * 16)
    for by in range(image.height // 4):
        for bx in range(image.width // 4):
            offset = tiled_offset(bx, by, pitch, 4)
            out[offset:offset + 16] = encode_block(image, kind, bx, by)
    return bytes(out)


def encode_blocks(image: Image.Image, kind: int, left: int, top: int, width: int, height: int) -> list:
    image = image.convert("RGBA")
    pitch = ((image.width // 4) + 31) & ~31
    return [(tiled_offset(bx, by, pitch, block_log2(kind)), encode_block(image, kind, bx, by))
            for by in range(top // 4, (top + height + 3) // 4)
            for bx in range(left // 4, (left + width + 3) // 4)]


def decode(data: bytes, width: int, height: int, kind: int) -> Image.Image:
    pitch = ((width // 4) + 31) & ~31
    image = Image.new("RGBA", (width, height))
    for by in range(height // 4):
        for bx in range(width // 4):
            offset = tiled_offset(bx, by, pitch, block_log2(kind))
            block = swap16(data[offset:offset + (8 if kind == 1 else 16)])
            c0, c1, bits = struct.unpack_from("<HHI", block, 0 if kind == 1 else 8)
            colors = [color_palette(c0, c1)[(bits >> (2 * i)) & 3] for i in range(16)]
            if kind == 1:
                alphas = [255] * 16
            elif kind == 5:
                palette = alpha_palette(block[0], block[1])
                alpha_bits = int.from_bytes(block[2:8], "little")
                alphas = [palette[(alpha_bits >> (3 * i)) & 7] for i in range(16)]
            else:
                alpha_bits = int.from_bytes(block[0:8], "little")
                alphas = [((alpha_bits >> (4 * i)) & 15) * 17 for i in range(16)]
            for i in range(16):
                image.putpixel((bx * 4 + (i & 3), by * 4 + (i >> 2)), colors[i] + (alphas[i],))
    return image


def cell_ranges(column: int, row: int, cell: int, width: int, block_bytes: int) -> list:
    pitch = ((width // 4) + 31) & ~31
    blocks = cell // 4
    return [(tiled_offset(column * blocks + x, row * blocks + y, pitch, 4), block_bytes)
            for y in range(blocks) for x in range(blocks)]
