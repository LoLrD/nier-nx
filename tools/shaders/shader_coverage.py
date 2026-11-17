import argparse
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

FETCH_MASKS = (0x0007FFFF, 0x80000000, 0x80000000)


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def words(data: bytes) -> list[int]:
    return list(struct.unpack(f">{len(data) // 4}I", data[:len(data) // 4 * 4]))


def microcode(container: bytes) -> bytes:
    header = be32(container, 24)
    start = be32(container, 4) + be32(container, header)
    return container[start:start + be32(container, header + 4)]


def fetch_addresses(container: bytes) -> list[int]:
    header = be32(container, 24)
    first = be32(container, header + 24)
    count = be32(container, header + 28)
    return [be32(container, header + 36 + 4 * (first + i)) & 0xFFF for i in range(count)]


def normalized(code: list[int], addresses: list[int]) -> list[int]:
    result = list(code)
    for address in addresses:
        for lane, mask in enumerate(FETCH_MASKS):
            index = address * 3 + lane
            if index < len(result):
                result[index] &= mask
    return result


def coverage(containers_dir: Path, discovered_dir: Path) -> dict:
    pixel = {}
    vertex = defaultdict(list)
    for path in sorted(containers_dir.glob("*.bin")):
        data = path.read_bytes()
        code = microcode(data)
        if be32(data, 0) & 1:
            vertex[len(code)].append((path.name, words(code), fetch_addresses(data)))
        else:
            pixel.setdefault(code, path.name)
    result = {"ps": {"discovered": 0, "matched": 0}, "vs": {"discovered": 0, "matched": 0}, "unmatched": [],
              "matches": {}}
    for path in sorted(discovered_dir.glob("*.ucode")):
        data = path.read_bytes()
        stage = "vs" if path.name.startswith("vs_") else "ps"
        result[stage]["discovered"] += 1
        match = pixel.get(data) if stage == "ps" else None
        if stage == "vs":
            loaded = words(data)
            for name, code, addresses in vertex.get(len(data), []):
                if normalized(loaded, addresses) == normalized(code, addresses):
                    match = name
                    break
        if match:
            result[stage]["matched"] += 1
            result["matches"][path.name] = match
        else:
            result["unmatched"].append(path.name)
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--containers", type=Path, required=True)
    parser.add_argument("--discovered", type=Path, required=True)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    result = coverage(args.containers, args.discovered)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != "matches"}))
    return 1 if result["unmatched"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
