#!/usr/bin/env python3
import re
import sys
from pathlib import Path

HEADER = re.compile(r"^==== [\d.]+ s \| mode: (?P<mode>[^|]+?) \| game (?P<fps>[\d.]+) fps")


def parse_blocks(text: str) -> list[tuple[str, float]]:
    blocks = []
    for line in text.splitlines():
        match = HEADER.match(line)
        if match and match["mode"] != "startup":
            blocks.append((match["mode"], float(match["fps"])))
    return blocks


def lap_rows(blocks: list[tuple[str, float]]) -> list[dict]:
    rows = []
    normal_ms = None
    for mode, fps in blocks:
        frame_ms = 1000.0 / fps if fps > 0 else None
        saved_ms = None
        if mode == "normal":
            normal_ms = frame_ms
        elif normal_ms is not None and frame_ms is not None:
            saved_ms = normal_ms - frame_ms
        rows.append({"mode": mode, "fps": fps, "frame_ms": frame_ms, "saved_ms": saved_ms})
    return rows


def main(argv: list[str]) -> int:
    for row in lap_rows(parse_blocks(Path(argv[0]).read_text(errors="replace"))):
        frame = f"{row['frame_ms']:7.1f} ms" if row["frame_ms"] is not None else "      - ms"
        saved = f"  saves {row['saved_ms']:6.1f} ms" if row["saved_ms"] is not None else ""
        print(f"{row['mode']:<24} {row['fps']:6.2f} fps {frame}{saved}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
