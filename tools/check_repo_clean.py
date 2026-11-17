#!/usr/bin/env python3
import fnmatch
import subprocess
import sys
from pathlib import Path

FORBIDDEN = [
    "*.xex",
    "*.xexp",
    "*.iso",
    "*.7z",
    "*.mesp",
    "*.bin",
    "*.cpk",
    "*.zip",
    "prod.keys",
    "*/prod.keys",
    "assets",
    "assets/*",
    "app/generated/*",
]
GAME_MAGICS = (b"XEX2", b"CON ", b"LIVE", b"PIRS", b"CPK ")


def tracked_files() -> list[str]:
    output = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    return [path for path in output.decode().split("\0") if path]


def has_game_magic(path: str) -> bool:
    file = Path(path)
    if not file.is_file() or file.is_symlink():
        return False
    with file.open("rb") as fh:
        return fh.read(4) in GAME_MAGICS


def forbidden(paths: list[str]) -> list[str]:
    return [path for path in paths
            if any(fnmatch.fnmatch(path, pattern) for pattern in FORBIDDEN) or has_game_magic(path)]


def main(argv: list[str] | None = None) -> int:
    bad = forbidden(tracked_files())
    if not bad:
        return 0
    print("game data or generated code is tracked by git:")
    for path in bad:
        print("  " + path)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
