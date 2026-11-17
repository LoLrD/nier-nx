#!/usr/bin/env python3
import fnmatch
import subprocess
import sys

FORBIDDEN = [
    "*.xex",
    "*.xexp",
    "*.iso",
    "*.7z",
    "*.mesp",
    "prod.keys",
    "*/prod.keys",
    "assets/*",
    "app/generated/*",
]


def tracked_files() -> list[str]:
    output = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    return [path for path in output.decode().split("\0") if path]


def forbidden(paths: list[str]) -> list[str]:
    return [path for path in paths if any(fnmatch.fnmatch(path, pattern) for pattern in FORBIDDEN)]


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
