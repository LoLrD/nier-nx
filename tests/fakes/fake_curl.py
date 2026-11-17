import os
import shutil
import sys
from pathlib import Path
from urllib.parse import urlparse

VALUE_FLAGS = {"--retry", "--retry-delay", "--max-time", "--user", "-o", "-T", "-Q", "--speed-time", "--speed-limit"}


def fail(code: int, message: str) -> None:
    print(f"curl: ({code}) {message}", file=sys.stderr)
    sys.exit(code)


def main(argv: list[str]) -> None:
    sd = Path(os.environ["FAKE_SD"])
    options: dict[str, str] = {}
    listing = False
    url = ""
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in VALUE_FLAGS:
            options[arg] = argv[i + 1]
            i += 2
            continue
        if arg == "-l":
            listing = True
        elif arg.startswith("ftp://"):
            url = arg
        i += 1
    path = sd / urlparse(url).path.lstrip("/")
    if "-Q" in options:
        target = sd / options["-Q"].split(" ", 1)[1].lstrip("/")
        if not target.exists():
            fail(21, "Quote command returned error")
        target.unlink()
        return
    if "-T" in options:
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(options["-T"], path)
        return
    if listing:
        if not path.is_dir():
            fail(9, "Server denied you to change to the given directory")
        for child in sorted(path.iterdir()):
            print(child.name)
        return
    if "-o" in options:
        if not path.is_file():
            fail(78, "The file does not exist")
        shutil.copyfile(path, options["-o"])


if __name__ == "__main__":
    main(sys.argv[1:])
