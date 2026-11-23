import argparse
import re
import sys
from collections import Counter
from pathlib import Path

import cpk
import textfiles

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / "assets" / "game_root" / "media" / "layer2.cpk"
WORK = ROOT / "out" / "lang" / "work"
ID_COLUMNS = {".TXD": 0, ".TND": 0, ".TKD": 5, ".RAW": 4, ".INW": 4, ".CAP": 0}
JAPANESE = re.compile(r"[぀-ヿ一-鿿]")
NAME_LENGTH = 40
REPEAT_LENGTH = 30
SPEAKER = re.compile(r"([A-Z]+\d{3})_[A-Z]$")


def extension(path: str) -> str:
    return path[path.rindex("."):]


def text_path(path: str) -> bool:
    if path.startswith("TEXT/"):
        return ".EN." in path or extension(path) in (".TXD", ".TND")
    return path.startswith("EVENT/CAPTION/") and ".EN." in path


def record_ids(rows: list, path: str) -> list:
    column = ID_COLUMNS[extension(path)]
    ids, current = [], ""
    for fields in rows:
        current = fields[column] or current
        ids.append(current)
    return ids


def is_name(path: str, english: str) -> bool:
    return (extension(path) in (".TXD", ".TND") and len(english) <= NAME_LENGTH and "\n" not in english
            and not english.rstrip().endswith((".", "!", "?", "…", ":")) and english[:1].isupper())


def chunk(entries: list, words: int) -> list:
    chunks, current, count = [], [], 0
    for entry in entries:
        if current and count >= words and (entry["id"], entry["path"]) != (current[-1]["id"], current[-1]["path"]):
            chunks.append(current)
            current, count = [], 0
        current.append(entry)
        count += len(entry["en"].split())
    if current:
        chunks.append(current)
    return chunks


def issues(source: list, done: list) -> list:
    expected = {(entry["path"], entry["key"]): entry["en"] for entry in source}
    seen, found = {}, []
    for path, key, text in done:
        if (path, key) in seen:
            found.append(f"{path} {key}: twice")
        seen[(path, key)] = text
    found += [f"{path} {key}: missing" for path, key in expected if (path, key) not in seen]
    found += [f"{path} {key}: not in the chunk" for path, key in seen if (path, key) not in expected]
    for (path, key), text in seen.items():
        if (path, key) in expected:
            found += [f"{path} {key}: {problem}"
                      for problem in textfiles.problems(textfiles.switch_english(expected[(path, key)]), text)]
    return found


def speaker(ident: str) -> str:
    found = SPEAKER.search(ident)
    return found.group(1) if found else ""


def harmonize(rows: list) -> list:
    groups = {}
    for row in rows:
        if len(row["en"]) >= REPEAT_LENGTH:
            groups.setdefault((row["en"], speaker(row["id"])), []).append(row["ru"])
    chosen = {group: Counter(texts).most_common(1)[0][0] for group, texts in groups.items()}
    return [dict(row, ru=chosen.get((row["en"], speaker(row["id"])), row["ru"])) for row in rows]


def fix_issues(source: list, fixes: list) -> list:
    expected = {(entry["path"], entry["key"]): entry["en"] for entry in source}
    found = []
    for path, key, text in fixes:
        if (path, key) not in expected:
            found.append(f"{path} {key}: not in the review")
            continue
        found += [f"{path} {key}: {problem}"
                  for problem in textfiles.problems(textfiles.switch_english(expected[(path, key)]), text)]
    return found


def spread(rows: list, fixes: dict) -> dict:
    groups = {}
    for row in rows:
        if len(row["en"]) >= REPEAT_LENGTH:
            groups.setdefault((row["en"], speaker(row["id"])), []).append((row["path"], row["key"]))
    result = dict(fixes)
    for row in rows:
        place = (row["path"], row["key"])
        if place not in fixes or len(row["en"]) < REPEAT_LENGTH:
            continue
        for other in groups[(row["en"], speaker(row["id"]))]:
            result.setdefault(other, fixes[place])
    return result


def apply_fixes(existing: dict, fixes: dict) -> dict:
    return {**existing, **fixes}


def russian_punctuation(text: str) -> str:
    parts = re.split(r"(<[^<>]*>)", text)
    return "".join(part if part.startswith("<") else part.replace("!?", "?!") for part in parts)


def merge(existing: dict, new: dict) -> dict:
    combined = {**new, **existing}
    return dict(sorted(combined.items(), key=lambda item: tuple(int(part) for part in item[0].split(":"))))


def japanese_reference(archive: Path, path: str, rows: list, listing: dict) -> list:
    if extension(path) in (".TXD", ".TND"):
        return [fields[1] for fields in rows]
    other = path.replace(".EN.", ".JA.")
    if other in listing:
        japanese = textfiles.records(cpk.read(archive, other))
        if len(japanese) == len(rows):
            return japanese
    return []


def source_entries(archive: Path, language: str, translated: bool = False) -> list:
    listing = cpk.files(archive)
    entries = []
    for path in sorted(listing):
        if not text_path(path):
            continue
        rows = textfiles.records(cpk.read(archive, path))
        ids = record_ids(rows, path)
        japanese = japanese_reference(archive, path, rows, listing)
        table = ROOT / "lang" / language / (path + ".tsv")
        done = textfiles.parse_table(table.read_text()) if table.exists() else {}
        for index, fields in enumerate(rows):
            for column in textfiles.text_columns(path):
                english, key = fields[column], f"{index}:{column}"
                if not english.strip() or JAPANESE.search(english) or (key in done) != translated:
                    continue
                reference = ""
                if japanese and isinstance(japanese[0], str):
                    reference = japanese[index]
                elif japanese:
                    reference = japanese[index][column]
                entries.append({"path": path, "key": key, "id": ids[index], "en": english, "ja": reference,
                                "ru": done.get(key, "")})
    return entries


def review_split(language: str, words: int) -> int:
    rows = source_entries(ARCHIVE, language, translated=True)
    folder = WORK / language / "audit" / "source"
    folder.mkdir(parents=True, exist_ok=True)
    for number, part in enumerate(chunk(rows, words)):
        lines = ["\t".join(textfiles.escape(entry[field]) for field in ("path", "key", "id", "en", "ja", "ru"))
                 for entry in part]
        (folder / f"{number:02d}.tsv").write_text("".join(line + "\n" for line in lines))
        print(f"{number:02d}: {len(part)} texts, {sum(len(entry['en'].split()) for entry in part)} words")
    return 0


def check_fixes(language: str, number: str) -> int:
    source = [dict(zip(("path", "key", "id", "en", "ja", "ru"), row))
              for row in read_rows(WORK / language / "audit" / "source" / f"{number}.tsv", 6)]
    fixes = [tuple(row[:3]) for row in read_rows(WORK / language / "audit" / "fixes" / f"{number}.tsv", 5)]
    found = fix_issues(source, fixes)
    for line in found:
        print(line)
    print(f"{number}: {len(fixes)} fixes, {len(found)} problems")
    return 1 if found else 0


def apply_review(language: str) -> int:
    fixes, categories, rows = {}, Counter(), []
    for source in sorted((WORK / language / "audit" / "source").glob("*.tsv")):
        rows += [dict(zip(("path", "key", "id", "en"), row)) for row in read_rows(source, 4)]
    for done in sorted((WORK / language / "audit" / "fixes").glob("*.tsv")):
        for row in read_rows(done, 5):
            fixes[(row[0], row[1])] = row[2]
            categories[row[3] if len(row) > 3 else ""] += 1
    spreaded = spread(rows, fixes)
    grouped = {}
    for (path, key), text in spreaded.items():
        grouped.setdefault(path, {})[key] = text
    print(f"{len(spreaded) - len(fixes)} repeated lines follow the fixes")
    for path, rows in grouped.items():
        table = ROOT / "lang" / language / (path + ".tsv")
        table.write_text(textfiles.format_table(apply_fixes(textfiles.parse_table(table.read_text()), rows)))
    print(f"{sum(categories.values())} fixes applied:", dict(categories.most_common()))
    return 0


def punctuate(language: str) -> int:
    changed = 0
    for table in sorted((ROOT / "lang" / language).rglob("*.tsv")):
        if table.parent == ROOT / "lang" / language:
            continue
        rows = textfiles.parse_table(table.read_text())
        fixed = {key: russian_punctuation(text) for key, text in rows.items()}
        changed += sum(rows[key] != fixed[key] for key in rows)
        table.write_text(textfiles.format_table(fixed))
    print(f"{changed} lines punctuated")
    return 0


def read_rows(path: Path, columns: int) -> list:
    rows = []
    for line in path.read_text().splitlines():
        if line:
            rows.append([textfiles.unescape(part) for part in line.split("\t")][:columns])
    return rows


def split(language: str, words: int) -> int:
    entries = source_entries(ARCHIVE, language)
    names = [entry for entry in entries if is_name(entry["path"], entry["en"])]
    rest = [entry for entry in entries if not is_name(entry["path"], entry["en"])]
    folder = WORK / language / "source"
    folder.mkdir(parents=True, exist_ok=True)
    for number, part in enumerate([names] + chunk(rest, words)):
        lines = ["\t".join(textfiles.escape(entry[field]) for field in ("path", "key", "id", "en", "ja"))
                 for entry in part]
        (folder / f"{number:02d}.tsv").write_text("".join(line + "\n" for line in lines))
        print(f"{number:02d}: {len(part)} texts, {sum(len(entry['en'].split()) for entry in part)} words")
    return 0


def check(language: str, number: str) -> int:
    source = [dict(zip(("path", "key", "id", "en", "ja"), row))
              for row in read_rows(WORK / language / "source" / f"{number}.tsv", 5)]
    done = [tuple(row) for row in read_rows(WORK / language / "done" / f"{number}.tsv", 3)]
    found = issues(source, done)
    for line in found:
        print(line)
    print(f"{number}: {len(done)} of {len(source)} texts, {len(found)} problems")
    return 1 if found else 0


def merge_done(language: str) -> int:
    rows = []
    for done in sorted((WORK / language / "done").glob("*.tsv")):
        source = {(row[0], row[1]): row for row in read_rows(WORK / language / "source" / done.name, 5)}
        for path, key, text in read_rows(done, 3):
            rows.append({"path": path, "key": key, "id": source[(path, key)][2], "en": source[(path, key)][3],
                         "ru": text})
    harmonized = harmonize(rows)
    print(f"{sum(a['ru'] != b['ru'] for a, b in zip(rows, harmonized))} repeated lines aligned")
    grouped = {}
    for row in harmonized:
        grouped.setdefault(row["path"], {})[row["key"]] = row["ru"]
    for path, rows in grouped.items():
        table = ROOT / "lang" / language / (path + ".tsv")
        existing = textfiles.parse_table(table.read_text()) if table.exists() else {}
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_text(textfiles.format_table(merge(existing, rows)))
        print(f"{path}: {len(rows)} texts merged")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Split game texts into translation chunks, check and merge them")
    commands = parser.add_subparsers(dest="command", required=True)
    split_parser = commands.add_parser("split")
    split_parser.add_argument("language")
    split_parser.add_argument("--words", type=int, default=11000)
    check_parser = commands.add_parser("check")
    check_parser.add_argument("language")
    check_parser.add_argument("number")
    merge_parser = commands.add_parser("merge")
    merge_parser.add_argument("language")
    review_parser = commands.add_parser("review-split")
    review_parser.add_argument("language")
    review_parser.add_argument("--words", type=int, default=13000)
    fixes_parser = commands.add_parser("check-fixes")
    fixes_parser.add_argument("language")
    fixes_parser.add_argument("number")
    apply_parser = commands.add_parser("apply-review")
    apply_parser.add_argument("language")
    punctuate_parser = commands.add_parser("punctuate")
    punctuate_parser.add_argument("language")
    args = parser.parse_args()
    if args.command == "review-split":
        return review_split(args.language, args.words)
    if args.command == "check-fixes":
        return check_fixes(args.language, args.number)
    if args.command == "apply-review":
        return apply_review(args.language)
    if args.command == "punctuate":
        return punctuate(args.language)
    if args.command == "split":
        return split(args.language, args.words)
    if args.command == "check":
        return check(args.language, args.number)
    return merge_done(args.language)


if __name__ == "__main__":
    sys.exit(main())
