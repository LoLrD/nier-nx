#!/usr/bin/env python3
import sys


def pick_new(before: set[str], after: list[str]) -> list[str]:
    return sorted(name for name in after if name.endswith(".log") and name not in before)


def main(argv: list[str]) -> int:
    before = set(open(argv[0]).read().split())
    after = open(argv[1]).read().split()
    print("\n".join(pick_new(before, after)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
