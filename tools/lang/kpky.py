import struct


def entries(data: bytes) -> list:
    count = struct.unpack_from(">I", data, 4)[0]
    offsets = struct.unpack_from(f">{count}I", data, 12)
    sizes = struct.unpack_from(f">{count}I", data, 12 + 4 * count)
    return [(offset, size) for offset, size in zip(offsets, sizes) if size]


def find(data: bytes, magic: bytes) -> tuple:
    return next((offset, size) for offset, size in entries(data) if data[offset:offset + 4] == magic)
