import argparse
import sys
from pathlib import Path

import cpk
import textfiles

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build translated game text files from translation tables")
    parser.add_argument("language")
    parser.add_argument("--archive", type=Path, default=ROOT / "assets" / "game_root" / "media" / "layer2.cpk")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "lang")
    args = parser.parse_args()
    tables = ROOT / "lang" / args.language
    failed = False
    for table in sorted(tables.rglob("*.tsv")):
        path = table.relative_to(tables).as_posix()[:-len(".tsv")]
        original = cpk.read(args.archive, path)
        english = textfiles.texts(original, path)
        translations = textfiles.parse_table(table.read_text())
        for key, text in translations.items():
            for problem in textfiles.problems(textfiles.switch_english(english.get(key, "")), text):
                print(f"{path} {key}: {problem}")
                failed = True
        out = args.out / args.language / path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(textfiles.apply(original, path, translations))
        print(f"{path}: {len(translations)} of {len(english)} texts translated")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
