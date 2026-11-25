#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mac"))

import mac_smoke

CLOCKS = re.compile(r"clocks: CPU ([\d.]+) MHz, GPU ([\d.]+) MHz, memory ([\d.]+) MHz")


def parse_clocks(text: str) -> list[dict]:
    seen: list[dict] = []
    for match in CLOCKS.finditer(text):
        entry = {"cpu_mhz": float(match[1]), "gpu_mhz": float(match[2]), "memory_mhz": float(match[3])}
        if entry not in seen:
            seen.append(entry)
    return seen


def read_text(path: Path) -> str:
    return path.read_text(errors="replace") if path.exists() else ""


def summarize(run: Path) -> dict:
    lines = read_text(run / "nier.log").splitlines()
    summary = mac_smoke.summarize_log(lines)
    crashes = read_text(run / "crash_new.txt").split()
    fatal = any("[FATAL]" in line or "[critical]" in line for line in lines)
    summary["result"] = "crash" if crashes or fatal else ("no_log" if not lines else "ok")
    summary["crash_reports"] = crashes
    summary["clocks"] = parse_clocks(read_text(run / "rex_profile.log"))
    return summary


def main(argv: list[str]) -> int:
    run = Path(argv[0])
    summary = summarize(run)
    (run / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != "last_log_line"}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
