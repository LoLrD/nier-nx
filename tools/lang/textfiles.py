import re
from collections import Counter

TEXT_COLUMNS = {".TXD": (2,), ".TND": (2,), ".TKD": (15,), ".CAP": (9,), ".INW": (12, 13), ".RAW": (11,)}
TAG = re.compile(r"<[^<>]*>|%%")
PLACEHOLDER = re.compile(r"<(NIER|ITEM|NUM|TEXT|DEC_\d+|ICON=\d+)>")
ESCAPE = re.compile(r"\\(.)")
BUTTON_WORDS = re.compile(r"button|press", re.IGNORECASE)
SWITCH_BUTTONS = [(re.compile(r"\bSTART\b"), "<ICON=13>"), (re.compile(r"\bBACK\b"), "<ICON=12>")]


def text_columns(path: str) -> tuple:
    return TEXT_COLUMNS[path[path.rindex("."):]]


def records(data: bytes) -> list:
    if not data.endswith(b"\r\n"):
        raise ValueError("a text file ends with CR LF")
    return [record.decode("utf-8").split("\t") for record in data[:-2].split(b"\r\n")]


def texts(data: bytes, path: str) -> dict:
    found = {}
    for index, fields in enumerate(records(data)):
        for column in text_columns(path):
            if fields[column]:
                found[f"{index}:{column}"] = fields[column]
    return found


def apply(data: bytes, path: str, translations: dict) -> bytes:
    rows = records(data)
    columns = text_columns(path)
    for key, text in translations.items():
        index, column = (int(part) for part in key.split(":"))
        if index >= len(rows) or column not in columns:
            raise KeyError(key)
        rows[index][column] = text
    return b"".join("\t".join(fields).encode("utf-8") + b"\r\n" for fields in rows)


def switch_english(text: str) -> str:
    if not BUTTON_WORDS.search(text):
        return text
    for pattern, icon in SWITCH_BUTTONS:
        text = pattern.sub(icon, text)
    return text


def structure(text: str) -> list:
    return [tag for tag in TAG.findall(text) if not PLACEHOLDER.fullmatch(tag)]


def problems(original: str, translation: str) -> list:
    found = []
    if Counter(TAG.findall(original)) != Counter(TAG.findall(translation)):
        found.append("tags differ")
    elif structure(original) != structure(translation):
        found.append("tag order differs")
    if "\t" in translation or "\r" in translation:
        found.append("tab or CR in text")
    return found


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def unescape(text: str) -> str:
    return ESCAPE.sub(lambda m: "\n" if m.group(1) == "n" else m.group(1), text)


def format_table(rows: dict) -> str:
    return "".join(f"{key}\t{escape(text)}\n" for key, text in rows.items())


def parse_table(text: str) -> dict:
    rows = {}
    for line in text.splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("\t", 1)
            rows[key] = unescape(value)
    return rows
