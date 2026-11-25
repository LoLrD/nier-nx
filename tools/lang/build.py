import argparse
import sys
from pathlib import Path

import badges
import cpk
import flash
import font
import textfiles
import title
import typeface

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build translated game text files from translation tables")
    parser.add_argument("language")
    parser.add_argument("--archive", type=Path, default=ROOT / "assets" / "game_root" / "media" / "layer2.cpk")
    parser.add_argument("--title-archive", type=Path, default=ROOT / "assets" / "game_root" / "media" / "layer1.cpk")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "lang")
    args = parser.parse_args()
    tables = ROOT / "lang" / args.language
    typeface.use(tables / "art")
    failed = False
    for table in sorted(table for table in tables.rglob("*.tsv") if table.parent != tables):
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
    glyphs = font.build(args.archive, args.out / args.language)
    print(f"{font.MAIN}, {font.VRAM}: {glyphs} glyphs drawn")
    target = args.out / args.language
    labels = textfiles.parse_table((tables / "titles.tsv").read_text())
    print(f"title menu: {title.build(args.title_archive, target, labels)} items drawn")
    print(f"{badges.PACK}: {badges.build(args.title_archive, target, labels)} badges drawn")
    texts = textfiles.parse_table((tables / "TEXT" / "NIER_TEXT.TXD.tsv").read_text())
    print(f"{flash.PACK}: {flash.build(args.title_archive, target, texts)} reports drawn")
    listed = sorted(path.relative_to(target).as_posix() for path in target.rglob("*")
                    if path.is_file() and path.name != "files.txt")
    (target / "files.txt").write_text("".join(f"{path}\n" for path in listed))
    typeface.finish()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
