#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

GCDA_MAGIC = 0x67636461
TAG_OBJECT_SUMMARY = 0xA1000000
TAG_ARC_COUNTERS = 0x01A10000


def records(data):
    if len(data) < 16 or struct.unpack_from("<I", data, 0)[0] != GCDA_MAGIC:
        raise ValueError("not a little-endian gcda file")
    position = 16
    while position + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, position)
        position += 8
        if length < 0:
            yield tag, position, 0
            continue
        yield tag, position, length
        position += length


def largest_arc_count(data):
    largest = 0
    for tag, start, length in records(data):
        if tag == TAG_ARC_COUNTERS and length:
            largest = max(largest, max(struct.unpack_from(f"<{length // 8}Q", data, start)))
    return largest


def write_summary(data, runs, sum_max):
    for tag, start, length in records(data):
        if tag == TAG_OBJECT_SUMMARY and length >= 8:
            struct.pack_into("<II", data, start, runs, sum_max)
            return True
    return False


def main():
    folder, runs = Path(sys.argv[1]), int(sys.argv[2])
    files = sorted(folder.glob("*.gcda"))
    contents = {path: bytearray(path.read_bytes()) for path in files}
    largest = max((largest_arc_count(data) for data in contents.values()), default=0)
    executed = sum(1 for data in contents.values() if largest_arc_count(data))
    patched = 0
    for path, data in contents.items():
        if write_summary(data, runs, min(largest, 0xFFFFFFFF)):
            path.write_bytes(data)
            patched += 1
    print(f"{len(files)} profile files, {executed} with executed code, summary written to {patched}: "
          f"runs {runs}, largest count {largest}")
    return 0 if files and patched == len(files) and largest else 1


if __name__ == "__main__":
    sys.exit(main())
