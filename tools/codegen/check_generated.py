#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def fatal_lines(gen: Path) -> list[str]:
    found: list[str] = []
    for source in sorted(gen.glob("*.cpp")):
        for line in source.read_text(errors="replace").splitlines():
            if "REX_FATAL(" in line and line.strip() not in found:
                found.append(line.strip())
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fails when the generated code contains REX_FATAL paths.")
    parser.add_argument("--gen", type=Path, default=ROOT / "app" / "generated" / "default")
    args = parser.parse_args(argv)
    found = fatal_lines(args.gen)
    if not found:
        return 0
    print(f"{len(found)} REX_FATAL path(s) in {args.gen}:", file=sys.stderr)
    for line in found:
        print("  " + line, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
