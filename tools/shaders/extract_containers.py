import hashlib
import json
import mmap
import re
import struct
import sys
from multiprocessing import Pool
from pathlib import Path

import crilayla

MAGIC = re.compile(rb"\x10\x2A\x11[\x00\x01]")


def be32(data, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def container_size(data, offset: int) -> int:
    available = len(data) - offset
    if available < 28:
        return 0
    vertex = be32(data, offset) & 1
    virtual_size = be32(data, offset + 4)
    physical_size = be32(data, offset + 8)
    header = be32(data, offset + 24)
    total = virtual_size + physical_size
    if total > available or header + 8 > total:
        return 0
    microcode_start = virtual_size + be32(data, offset + header)
    microcode_size = be32(data, offset + header + 4)
    if microcode_size == 0 or microcode_size % 4 or microcode_start + microcode_size > total:
        return 0
    if header >= virtual_size or header + (40 if vertex else 32) > virtual_size:
        return 0
    return total


def find_containers(data) -> tuple[list[bytes], int]:
    found = []
    rejected = 0
    for match in MAGIC.finditer(data):
        size = container_size(data, match.start())
        if size:
            found.append(bytes(data[match.start():match.start() + size]))
        else:
            rejected += 1
    return found, rejected


def scan_block(block: bytes) -> tuple[list[bytes], int, int]:
    try:
        containers, rejected = find_containers(crilayla.decompress(block))
    except (ValueError, IndexError, struct.error):
        return [], 0, 1
    return containers, rejected, 0


def store(out: Path, container: bytes, seen: set[str]) -> None:
    stage = "vs" if be32(container, 0) & 1 else "ps"
    name = f"{stage}_{hashlib.sha256(container).hexdigest()[:16]}.bin"
    if name in seen:
        return
    seen.add(name)
    (out / name).write_bytes(container)


def run(out: Path, files: list[Path]) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    seen = {path.name for path in out.glob("*.bin")}
    summary = {"files": 0, "blocks": 0, "bad_blocks": 0, "containers": 0, "unique": 0, "rejected": 0}
    for path in files:
        with open(path, "rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
            summary["files"] += 1
            results = [(*find_containers(data), 0)]
            blocks = []
            for offset in crilayla.find_blocks(data):
                size = crilayla.block_size(data, offset)
                if size and offset + size <= len(data):
                    blocks.append(bytes(data[offset:offset + size]))
                else:
                    summary["bad_blocks"] += 1
            summary["blocks"] += len(blocks)
            if blocks:
                with Pool() as pool:
                    results += pool.map(scan_block, blocks)
        for containers, rejected, bad in results:
            summary["containers"] += len(containers)
            summary["rejected"] += rejected
            summary["bad_blocks"] += bad
            for container in containers:
                store(out, container, seen)
    summary["unique"] = len(seen)
    return summary


def main(argv: list[str]) -> int:
    summary = run(Path(argv[0]), [Path(name) for name in argv[1:]])
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
