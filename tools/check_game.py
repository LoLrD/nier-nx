#!/usr/bin/env python3
import argparse
import hashlib
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DLC_DIR = Path("content") / "535107E8" / "00000002"


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def dlc_package(root: Path) -> Path | None:
    folder = root / DLC_DIR
    packages = sorted(p for p in folder.iterdir() if p.is_file()) if folder.is_dir() else []
    return packages[0] if packages else None


def check_iso(iso: Path, lock: dict) -> int:
    got = file_sha256(iso)
    if got != lock["iso_sha256"]:
        print(f"{iso.name} does not match app/game.lock (expected {lock['iso_sha256']}, got {got}): "
              "this port supports only Nier (USA, Europe) disc", file=sys.stderr)
        return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Checks the extracted game against app/game.lock.")
    parser.add_argument("--root", type=Path, default=ROOT / "assets")
    parser.add_argument("--lock", type=Path, default=ROOT / "app" / "game.lock")
    parser.add_argument("--iso", type=Path, help="check only this disc image")
    args = parser.parse_args(argv)
    lock = tomllib.loads(args.lock.read_text())
    if args.iso:
        return check_iso(args.iso, lock)

    xex = args.root / "game_root" / "default.xex"
    if not xex.is_file():
        print(f"{xex}: default.xex not found; run tools/unpack_dumps.sh first", file=sys.stderr)
        return 3
    got = file_sha256(xex)
    if got != lock["xex_sha256"]:
        print(
            f"default.xex does not match app/game.lock (expected {lock['xex_sha256']}, got {got}): "
            "this port supports only Nier (USA, Europe) disc",
            file=sys.stderr,
        )
        return 2

    package = dlc_package(args.root)
    if package is None:
        print(f"{args.root / DLC_DIR}: 15 Nightmares DLC package not found", file=sys.stderr)
        return 3
    got = file_sha256(package)
    if got != lock["dlc_15_nightmares_sha256"]:
        print(
            f"15 Nightmares DLC does not match app/game.lock (expected {lock['dlc_15_nightmares_sha256']}, got {got})",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
