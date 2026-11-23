import struct
from pathlib import Path

TYPE_SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 9: 8, 0xA: 4, 0xB: 8, 0xC: 16}


def utf_rows(data: bytes) -> list:
    if data[:4] != b"@UTF":
        plain = bytearray(data)
        key = 0x5F
        for i in range(len(plain)):
            plain[i] ^= key
            key = (key * 0x15) & 0xFF
        data = bytes(plain)
    big = lambda at, size: int.from_bytes(data[at:at + size], "big")
    rows_offset, strings, columns, width, count = big(10, 2), big(12, 4), big(24, 2), big(26, 2), big(28, 4)

    def string(offset: int) -> str:
        start = 8 + strings + offset
        return data[start:data.index(b"\0", start)].decode("utf-8")

    schema, at = [], 32
    for _ in range(columns):
        flags, name = data[at], string(big(at + 1, 4))
        at += 5
        default = None
        if flags & 0x20:
            default = (flags & 0xF, at)
            at += TYPE_SIZES[flags & 0xF]
        schema.append((name, flags, default))

    def value(kind: int, where: int):
        return string(big(where, 4)) if kind == 0xA else big(where, TYPE_SIZES[kind])

    rows = []
    for row in range(count):
        at = 8 + rows_offset + row * width
        values = {}
        for name, flags, default in schema:
            if flags & 0x40:
                values[name] = value(flags & 0xF, at)
                at += TYPE_SIZES[flags & 0xF]
            elif default:
                values[name] = value(*default)
        rows.append(values)
    return rows


def chunk(file, offset: int) -> bytes:
    file.seek(offset)
    header = file.read(16)
    return file.read(struct.unpack_from("<I", header, 8)[0])


def files(archive: Path) -> dict:
    with archive.open("rb") as file:
        header = utf_rows(chunk(file, 0))[0]
        toc = utf_rows(chunk(file, header["TocOffset"]))
    base = min(header["TocOffset"], header.get("ContentOffset", header["TocOffset"]))
    listing = {}
    for row in toc:
        directory = row.get("DirName") or ""
        path = f"{directory}/{row['FileName']}" if directory and directory != "<NULL>" else row["FileName"]
        listing[path] = (base + row["FileOffset"], row["FileSize"])
    return listing


def read(archive: Path, path: str) -> bytes:
    offset, size = files(archive)[path]
    with archive.open("rb") as file:
        file.seek(offset)
        return file.read(size)
