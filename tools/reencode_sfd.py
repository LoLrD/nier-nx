import argparse
import bisect
import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

PACK = 2048
PAYLOAD = 2018
PREFIX = b"\x00\x00\x01"
SEQUENCE = 0xB3
GOP = 0xB8
USER = 0xB2
PICTURE = 0x00
FRAME_TICKS = 3003
MIN_PSNR = 33.0


@dataclass
class Source:
    head: list[bytes]
    others: list[tuple[int, bytes]]
    tail: bytes
    es: bytes
    stamps: list[tuple[int, int | None]]
    std: bytes
    mux_rate: bytes
    max_picture: int


@dataclass
class Picture:
    pos: int
    kind: int
    display: int


def read_stamp(b: bytes) -> int:
    return ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1


def write_stamp(prefix: int, value: int) -> bytes:
    return bytes([
        prefix << 4 | ((value >> 29) & 0x0E) | 1,
        (value >> 22) & 0xFF,
        ((value >> 14) & 0xFE) | 1,
        (value >> 7) & 0xFF,
        ((value << 1) & 0xFE) | 1,
    ])


def pes_header(body: bytes) -> tuple[int, bytes, int | None]:
    i = 0
    while body[i] == 0xFF:
        i += 1
    std = body[i:i + 2]
    i += 2
    kind = body[i] >> 4
    if kind == 2:
        return i + 5, std, read_stamp(body[i:i + 5])
    if kind == 3:
        return i + 10, std, read_stamp(body[i:i + 5])
    return i + 1, std, None


def start_codes(es: bytes):
    i = es.find(PREFIX)
    while 0 <= i and i + 3 < len(es):
        yield i, es[i + 3]
        i = es.find(PREFIX, i + 3)


def parse(data: bytes) -> Source:
    head, others, stamps = [], [], []
    es = bytearray()
    std = b""
    tail = b""
    for k in range(len(data) // PACK):
        pack = data[k * PACK:(k + 1) * PACK]
        if pack[:4] != PREFIX + b"\xba":
            tail = pack
        elif pack[15] == 0xE0:
            length = struct.unpack(">H", pack[16:18])[0]
            body = pack[18:18 + length]
            size, std, pts = pes_header(body)
            stamps.append((len(es), pts))
            es += body[size:]
        elif not es:
            head.append(pack)
        else:
            others.append((len(es), pack))
    header = head_body(head)
    return Source(head, others, tail, bytes(es), stamps, std, head[0][9:12], struct.unpack(">I", header[194:198])[0])


def head_body(head: list[bytes]) -> bytes:
    for pack in head:
        if pack[15] == 0xBF:
            return pack[18:]
    raise ValueError("no Sofdec header")


def pictures(es: bytes) -> list[Picture]:
    result = []
    base = count = 0
    for pos, code in start_codes(es):
        if code == GOP:
            base += count
            count = 0
        elif code == PICTURE:
            reference = es[pos + 4] << 2 | es[pos + 5] >> 6
            result.append(Picture(pos, (es[pos + 5] >> 3) & 7, base + reference))
            count += 1
    return result


def user_block(es: bytes, after: int) -> bytes:
    previous = None
    for pos, code in start_codes(es):
        if code == USER and previous == after:
            return es[pos:es.find(PREFIX, pos + 4)]
        previous = code
    raise ValueError(f"no user data after {after:#x}")


def display_times(source: Source, original: list[Picture]) -> list[int]:
    positions = [p.pos for p in original]
    known = {}
    for start, pts in source.stamps:
        if pts is None:
            continue
        i = bisect.bisect_left(positions, start)
        if i < len(original) and original[i].pos < start + PAYLOAD:
            known.setdefault(original[i].display, pts)
    keys = sorted(known)
    times = []
    for d in range(len(original)):
        i = bisect.bisect_left(keys, d)
        if i < len(keys) and keys[i] == d:
            times.append(known[d])
        elif i == 0:
            times.append(known[keys[0]] - (keys[0] - d) * FRAME_TICKS)
        elif i == len(keys):
            times.append(known[keys[-1]] + (d - keys[-1]) * FRAME_TICKS)
        else:
            a, b = keys[i - 1], keys[i]
            times.append(known[a] + round((known[b] - known[a]) * (d - a) / (b - a)))
    return times


def encode(path: Path, bitrate: str, out: Path, work: Path) -> None:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(path), "-map", "0:v:0",
               "-vf", "setpts=N/(30000/1001)/TB", "-r", "30000/1001", "-c:v", "mpeg1video", "-b:v", bitrate,
               "-maxrate", "20M", "-bufsize", "7808k", "-g", "28", "-bf", "2", "-b_strategy", "0",
               "-mbd", "rd", "-trellis", "2", "-cmp", "2", "-subcmp", "2", "-passlogfile", str(work / "pass")]
    subprocess.run(command + ["-pass", "1", "-f", "mpeg1video", "/dev/null"], check=True)
    subprocess.run(command + ["-pass", "2", "-f", "mpeg1video", str(out)], check=True)


def rebuild_es(es: bytes, original: bytes) -> bytes:
    sequence = original[original.find(PREFIX + bytes([SEQUENCE])):][:12]
    picture_user = user_block(original, PICTURE)
    out = bytearray()
    last = 0
    pending = False
    for pos, code in start_codes(es):
        if pending and 0x01 <= code <= 0xAF:
            out += es[last:pos] + picture_user
            last = pos
            pending = False
        if code == SEQUENCE:
            out += es[last:pos]
            header = bytearray(es[pos:pos + 12])
            header[7:11] = sequence[7:11]
            header[11] = (sequence[11] & 0xFC) | (header[11] & 0x03)
            out += header
            last = pos + 12
        pending = pending or code == PICTURE
    out += es[last:]
    return bytes(out)


def stamps_for(new: list[Picture], times: list[int]) -> list[tuple[int, int | None]]:
    result = []
    for i, picture in enumerate(new):
        if picture.kind == 3:
            result.append((times[picture.display], None))
            continue
        following = 0
        while i + 1 + following < len(new) and new[i + 1 + following].kind == 3:
            following += 1
        result.append((times[picture.display], times[picture.display - following]))
    return result


def pack_header(index: int, mux_rate: bytes) -> bytes:
    rate = ((mux_rate[0] & 0x7F) << 15 | mux_rate[1] << 7 | mux_rate[2] >> 1) * 50
    return PREFIX + b"\xba" + write_stamp(2, round((index * PACK + 9) * 90000 / rate)) + mux_rate


def video_pack(index: int, source: Source, payload: bytes, stamp: tuple[int, int | None] | None) -> bytes:
    if stamp is None:
        header = b"\xff" * 9 + source.std + b"\x0f"
    elif stamp[1] is None:
        header = b"\xff" * 5 + source.std + write_stamp(2, stamp[0])
    else:
        header = source.std + write_stamp(3, stamp[0]) + write_stamp(1, stamp[1])
    free = PACK - 12 - 6 - len(header) - len(payload)
    padding = b""
    if 0 < free < 6:
        header = b"\xff" * free + header
    elif free >= 6:
        padding = PREFIX + b"\xbe" + struct.pack(">H", free - 6) + b"\xff" * (free - 6)
    pes = PREFIX + b"\xe0" + struct.pack(">H", len(header) + len(payload)) + header + payload
    return pack_header(index, source.mux_rate) + pes + padding


def mux(source: Source, es: bytes, new: list[Picture], stamps: list[tuple[int, int | None]],
        anchors: list[int]) -> bytes:
    out = bytearray()
    index = 0

    def emit(pack: bytes) -> None:
        nonlocal index
        out.extend(pack_header(index, source.mux_rate) + pack[12:])
        index += 1

    for pack in source.head:
        emit(pack)
    positions = [p.pos for p in new]
    pending = 0
    for start in range(0, len(es), PAYLOAD):
        started = bisect.bisect_left(positions, start)
        while pending < len(source.others) and anchors[pending] <= started:
            emit(source.others[pending][1])
            pending += 1
        first = bisect.bisect_left(positions, start)
        stamp = stamps[first] if first < len(new) and new[first].pos < start + PAYLOAD else None
        out.extend(video_pack(index, source, es[start:start + PAYLOAD], stamp))
        index += 1
    for _, pack in source.others[pending:]:
        emit(pack)
    out.extend(source.tail)
    return bytes(out)


def verify(source: Source, result: bytes, es: bytes) -> None:
    check = parse(result)
    assert check.es == es, "video stream differs after muxing"
    assert [p[12:] for _, p in check.others] == [p[12:] for _, p in source.others], "audio packs differ"
    assert [p[12:] for p in check.head] == [p[12:] for p in source.head], "header packs differ"
    assert check.tail == source.tail, "end block differs"


def psnr(path: Path, original: Path) -> float:
    result = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(path), "-i", str(original),
                             "-lavfi", "[0:v][1:v]psnr", "-f", "null", "-"], capture_output=True, text=True, check=True)
    return float(re.search(r"PSNR y:([0-9.]+)", result.stderr).group(1))


def reencode(path: Path, out: Path, bitrate: str) -> None:
    data = path.read_bytes()
    source = parse(data)
    original = pictures(source.es)
    times = display_times(source, original)
    original_positions = [p.pos for p in original]
    anchors = [bisect.bisect_left(original_positions, pos) for pos, _ in source.others]
    with tempfile.TemporaryDirectory() as work:
        stream = Path(work) / "video.m1v"
        encode(path, bitrate, stream, Path(work))
        es = rebuild_es(stream.read_bytes(), source.es)
    new = pictures(es)
    assert len(new) == len(original), f"{len(new)} pictures, original has {len(original)}"
    largest = max(b.pos - a.pos for a, b in zip(new, new[1:]))
    assert largest <= source.max_picture, f"picture of {largest} bytes, header allows {source.max_picture}"
    result = mux(source, es, new, stamps_for(new, times), anchors)
    verify(source, result, es)
    out.write_bytes(result)
    quality = psnr(out, path)
    assert quality >= MIN_PSNR, f"luma PSNR {quality:.1f} dB against the original"
    print(f"{path.name}: {len(data) >> 20} MB -> {len(result) >> 20} MB, {len(new)} pictures, "
          f"largest {largest >> 10} KB of {source.max_picture >> 10} KB, luma PSNR {quality:.1f} dB")


def main() -> None:
    parser = argparse.ArgumentParser(description="Re-encode the video of a Sofdec .sfd at a lower bitrate")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--bitrate", default="8M")
    args = parser.parse_args()
    reencode(args.source, args.output, args.bitrate)


if __name__ == "__main__":
    main()
