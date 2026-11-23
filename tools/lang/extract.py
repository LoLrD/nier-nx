import argparse
import sys
from pathlib import Path

import cpk
import textfiles

ROOT = Path(__file__).resolve().parents[2]


def japanese(path: str) -> str:
    return path.replace(".EN.", ".JA.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Write the English game texts, with Japanese beside them, as tables")
    parser.add_argument("--archive", type=Path, default=ROOT / "assets" / "game_root" / "media" / "layer2.cpk")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "lang" / "source")
    args = parser.parse_args()
    listing = cpk.files(args.archive)
    for path in sorted(listing):
        if not path.startswith("TEXT/") or (".EN." not in path and not path.endswith(".TXD")):
            continue
        english_data = cpk.read(args.archive, path)
        english = textfiles.texts(english_data, path)
        if path.endswith(".TXD"):
            reference = {f"{i}:2": fields[1] for i, fields in enumerate(textfiles.records(english_data))}
        elif japanese(path) in listing:
            ja_data = cpk.read(args.archive, japanese(path))
            ja_rows = textfiles.records(ja_data)
            same = len(ja_rows) == len(textfiles.records(english_data))
            reference = {key: ja_rows[int(key.split(":")[0])][int(key.split(":")[1])] for key in english} if same else {}
        else:
            reference = {}
        out = args.out / (path + ".tsv")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("".join(f"{key}\t{textfiles.escape(text)}\t{textfiles.escape(reference.get(key, ''))}\n"
                               for key, text in english.items()))
        print(f"{path}: {len(english)} texts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
