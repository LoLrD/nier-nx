import base64
import json
import math
import os
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

STORE_FILE = "typeface.json"
_store = None


class Store:
    def __init__(self, directory: Path):
        self.directory = directory
        self.path = directory / STORE_FILE
        self.saved = json.loads(self.path.read_text()) if self.path.exists() else {}
        self.used = {}

    def take(self, key: str, make):
        if key in self.used:
            return self.used[key]
        if make is None:
            if key not in self.saved:
                raise SystemExit(f"{self.path}: nothing stored for {key}; this text has to be drawn on macOS, "
                                 "where the fonts are")
            value = self.saved[key]
        else:
            value = make()
        self.used[key] = value
        return value

    def finish(self, recorded: bool) -> None:
        if not recorded:
            return
        self.directory.mkdir(parents=True, exist_ok=True)
        text = json.dumps(self.used, ensure_ascii=False, indent=0, sort_keys=True) + "\n"
        if not self.path.exists() or self.path.read_text() != text:
            self.path.write_text(text)


def use(directory: Path | None) -> None:
    global _store
    _store = Store(directory) if directory else None


def finish() -> None:
    if _store:
        _store.finish(_recorded)


_recorded = False


def stored_only() -> bool:
    return os.environ.get("NIER_LANG_FONTS") == "stored"


class Face:
    def __init__(self, path: str, size: int, index: int = 0):
        self.name = f"{Path(path).name}#{index}@{size}"
        self.font = None
        if Path(path).exists() and not stored_only():
            self.font = ImageFont.truetype(path, size, index=index)
        elif _store is None:
            raise SystemExit(f"{path} is missing and no stored drawings are in use")

    def _take(self, key: str, make):
        global _recorded
        if _store is None:
            return make()
        if self.font is None:
            return _store.take(key, None)
        _recorded = True
        return _store.take(key, make)

    def getlength(self, text: str) -> float:
        return self._take(f"length {self.name} {text!r}", lambda: self.font.getlength(text))

    def getbbox(self, text: str, anchor: str = "la") -> tuple:
        return tuple(self._take(f"bbox {self.name} {anchor} {text!r}",
                                lambda: list(self.font.getbbox(text, anchor=anchor))))

    def _mask(self, text: str, anchor: str, fx: float, fy: float) -> tuple:
        left, top, right, bottom = self.font.getbbox(text, anchor=anchor)
        ox, oy = math.ceil(-left) + 2, math.ceil(-top) + 2
        scratch = Image.new("L", (ox + math.ceil(right) + 4, oy + math.ceil(bottom) + 4), 0)
        ImageDraw.Draw(scratch).text((ox + fx, oy + fy), text, font=self.font, fill=255, anchor=anchor)
        box = scratch.getbbox()
        if box is None:
            return None, 0, 0
        return scratch.crop(box), box[0] - ox, box[1] - oy

    def text(self, image: Image.Image, xy: tuple, text: str, anchor: str = "la") -> None:
        x, y = xy
        fx, fy = round(x - math.floor(x), 4), round(y - math.floor(y), 4)

        def make() -> list:
            drawn, dx, dy = self._mask(text, anchor, fx, fy)
            if drawn is None:
                return []
            packed = base64.b64encode(zlib.compress(drawn.tobytes(), 9)).decode()
            return [dx, dy, drawn.width, drawn.height, packed]

        entry = self._take(f"draw {self.name} {anchor} {fx} {fy} {text!r}", make)
        if entry:
            dx, dy, width, height, packed = entry
            mask = Image.frombytes("L", (width, height), zlib.decompress(base64.b64decode(packed)))
            image.paste(255, (math.floor(x) + dx, math.floor(y) + dy), mask)
