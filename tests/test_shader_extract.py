import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "shaders"))

import crilayla
import extract_containers


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def put(self, value: int, count: int) -> None:
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def to_backward_bytes(self) -> bytes:
        bits = self.bits + [0] * (-len(self.bits) % 8)
        chunks = [bits[i:i + 8] for i in range(0, len(bits), 8)]
        values = [sum(b << (7 - j) for j, b in enumerate(chunk)) for chunk in chunks]
        return bytes(reversed(values))


def encode(prefix: bytes, data: bytes, tokens: list[tuple]) -> bytes:
    writer = BitWriter()
    for token in tokens:
        if token[0] == "lit":
            writer.put(0, 1)
            writer.put(token[1], 8)
        else:
            writer.put(1, 1)
            writer.put(token[1], 13)
            writer.put(0, 2)
    compressed = writer.to_backward_bytes()
    return b"CRILAYLA" + struct.pack("<II", len(data), len(compressed)) + compressed + prefix


def literal_tokens(data: bytes) -> list[tuple]:
    return [("lit", b) for b in reversed(data)]


def make_container(vertex: bool = False, microcode: bytes = b"\x11\x22\x33\x44" * 6) -> bytes:
    virtual = bytearray(64)
    header = 16
    struct.pack_into(">I", virtual, 0, 0x102A1100 | int(vertex))
    struct.pack_into(">I", virtual, 4, len(virtual))
    struct.pack_into(">I", virtual, 8, len(microcode))
    struct.pack_into(">I", virtual, 24, header)
    struct.pack_into(">II", virtual, header, 0, len(microcode))
    return bytes(virtual) + microcode


class CrilaylaTest(unittest.TestCase):
    def test_literal_roundtrip(self) -> None:
        prefix = bytes(range(256))
        data = b"hello shaders"
        self.assertEqual(crilayla.decompress(encode(prefix, data, literal_tokens(data))), prefix + data)

    def test_backreference_copies(self) -> None:
        prefix = bytes(256)
        data = b"abcabc"
        tokens = [("lit", ord("c")), ("lit", ord("b")), ("lit", ord("a")), ("ref", 0)]
        self.assertEqual(crilayla.decompress(encode(prefix, data, tokens)), prefix + data)

    def test_find_blocks(self) -> None:
        blob = b"xx" + encode(bytes(256), b"a", literal_tokens(b"a")) + b"CRILAYLA"
        self.assertEqual(crilayla.find_blocks(blob), [2, len(blob) - 8])


class ContainerTest(unittest.TestCase):
    def test_finds_valid_container(self) -> None:
        container = make_container()
        found, rejected = extract_containers.find_containers(b"\x00" * 13 + container + b"\xff" * 9)
        self.assertEqual(found, [container])
        self.assertEqual(rejected, 0)

    def test_false_magic_rejected(self) -> None:
        found, rejected = extract_containers.find_containers(b"\x10\x2a\x11\x00" + b"\xff" * 60)
        self.assertEqual(found, [])
        self.assertEqual(rejected, 1)

    def test_truncated_container_skipped(self) -> None:
        container = make_container()
        found, rejected = extract_containers.find_containers(container[:-8])
        self.assertEqual(found, [])
        self.assertEqual(rejected, 1)

    def test_container_inside_crilayla_block_found(self) -> None:
        container = make_container(vertex=True)
        block = encode(bytes(256), container, literal_tokens(container))
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "archive.cpk"
            source.write_bytes(b"\x00" * 32 + block)
            out = Path(tmp) / "out"
            summary = extract_containers.run(out, [source])
            self.assertEqual(summary["unique"], 1)
            self.assertEqual(summary["blocks"], 1)
            self.assertTrue(next(out.glob("vs_*.bin")).read_bytes() == container)

    def test_bad_crilayla_blocks_are_counted(self) -> None:
        container = make_container()
        good = encode(bytes(256), container, literal_tokens(container))
        corrupt = b"CRILAYLA" + struct.pack("<II", 4096, 8) + b"\xff" * 8 + bytes(256)
        truncated = good[:-40]
        tail = b"CRILAYLA" + b"\x01\x02"
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "archive.cpk"
            source.write_bytes(good + b"\x00" * 8 + corrupt + b"\x00" * 8 + truncated + tail)
            summary = extract_containers.run(Path(tmp) / "out", [source])
            self.assertEqual(summary["unique"], 1)
            self.assertEqual(summary["bad_blocks"], 3)

    def test_duplicates_stored_once(self) -> None:
        container = make_container()
        with tempfile.TemporaryDirectory() as tmp:
            files = []
            for name in ("a.bin", "b.bin"):
                path = Path(tmp) / name
                path.write_bytes(b"\x01" * 7 + container)
                files.append(path)
            out = Path(tmp) / "out"
            summary = extract_containers.run(out, files)
            self.assertEqual((summary["containers"], summary["unique"]), (2, 1))
            self.assertEqual(len(list(out.glob("ps_*.bin"))), 1)


if __name__ == "__main__":
    unittest.main()
