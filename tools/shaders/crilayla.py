import re
import struct

SIGNATURE = b"CRILAYLA"
PREFIX_SIZE = 0x100
VLE_LEVELS = (2, 3, 5, 8)


def find_blocks(data) -> list[int]:
    return [match.start() for match in re.finditer(SIGNATURE, data)]


def block_size(data, offset: int) -> int:
    if offset + 0x10 > len(data):
        return 0
    _, compressed = struct.unpack_from("<II", data, offset + 8)
    return 0x10 + compressed + PREFIX_SIZE


def decompress(block: bytes) -> bytes:
    uncompressed, compressed = struct.unpack_from("<II", block, 8)
    prefix_start = 0x10 + compressed
    out = bytearray(PREFIX_SIZE + uncompressed)
    out[:PREFIX_SIZE] = block[prefix_start:prefix_start + PREFIX_SIZE]
    position = prefix_start - 1
    pool = 0
    left = 0

    def bits(count: int) -> int:
        nonlocal position, pool, left
        value = 0
        while count:
            if not left:
                if position < 0x10:
                    raise ValueError("CRILAYLA stream ends early")
                pool = block[position]
                position -= 1
                left = 8
            take = min(left, count)
            value = (value << take) | ((pool >> (left - take)) & ((1 << take) - 1))
            left -= take
            count -= take
        return value

    end = PREFIX_SIZE + uncompressed - 1
    written = 0
    while written < uncompressed:
        if bits(1):
            source = end - written + bits(13) + 3
            if source > end:
                raise ValueError("CRILAYLA reference outside the output")
            length = 3
            for width in VLE_LEVELS:
                level = bits(width)
                length += level
                if level != (1 << width) - 1:
                    break
            else:
                while True:
                    level = bits(8)
                    length += level
                    if level != 255:
                        break
            for _ in range(length):
                out[end - written] = out[source]
                source -= 1
                written += 1
        else:
            out[end - written] = bits(8)
            written += 1
    return bytes(out)
