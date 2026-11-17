#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNREGISTERED = re.compile(r"Call to invalid or unregistered function at guest address (?:0x)?([0-9A-Fa-f]{8})")
FRAME = re.compile(r"XELOG_GPU PRESENT:")
FILE_OPEN = re.compile(r"\bNtCreateFile\b|\bNtOpenFile\b|\bopened file\b", re.IGNORECASE)


def summarize_log(lines: list[str]) -> dict:
    unregistered = []
    for line in lines:
        match = UNREGISTERED.search(line)
        if match and match.group(1).upper() not in unregistered:
            unregistered.append(match.group(1).upper())
    return {
        "frames": sum(1 for line in lines if FRAME.search(line)),
        "files_opened": sum(1 for line in lines if FILE_OPEN.search(line)),
        "unregistered_calls": unregistered,
        "last_log_line": next((line for line in reversed(lines) if line.strip()), ""),
    }


def window_id(pid: int) -> str:
    binary = ROOT / "out" / "bin" / "window_id"
    if not binary.exists():
        binary.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["swiftc", "-O", str(ROOT / "tools" / "window_id.swift"), "-o", str(binary)], check=True)
    return subprocess.run([str(binary), str(pid)], capture_output=True, text=True).stdout.strip()


def take_shot(pid: int, path: Path) -> bool:
    wid = window_id(pid)
    if not wid:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["screencapture", "-x", "-o", "-l", wid, str(path)], capture_output=True)
    return path.exists()


def dump_stacks(pid: int, path: Path) -> None:
    result = subprocess.run(
        ["lldb", "--batch", "-p", str(pid), "-o", "thread backtrace all", "-o", "detach"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    path.write_text(result.stdout + result.stderr)


def log_size(log_file: Path | None) -> int:
    return log_file.stat().st_size if log_file and log_file.exists() else 0


def run(command: list[str], seconds: float, silence_seconds: float, run_dir: Path, log_file: Path | None = None,
        shot_times: list[float] | None = None) -> dict:
    pending_shots = sorted(shot_times or [])
    shots: list[str] = []
    run_dir.mkdir(parents=True, exist_ok=True)
    if log_file and log_file.exists():
        log_file.unlink()
    seen_size = 0
    lines: list[str] = []
    last_output = time.monotonic()
    lock = threading.Lock()
    log = (run_dir / "nier.log").open("w")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)

    def reader() -> None:
        nonlocal last_output
        for line in process.stdout:
            log.write(line)
            log.flush()
            with lock:
                lines.append(line.rstrip("\n"))
                last_output = time.monotonic()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    start = time.monotonic()
    result = "timeout_ok"
    while True:
        if process.poll() is not None:
            result = "ok" if process.returncode == 0 else "crash"
            break
        now = time.monotonic()
        size = log_size(log_file)
        with lock:
            if size != seen_size:
                seen_size = size
                last_output = now
            silent_for = now - last_output
        if silent_for >= silence_seconds:
            dump_stacks(process.pid, run_dir / "stacks.txt")
            process.kill()
            result = "hang"
            break
        while pending_shots and now - start >= pending_shots[0]:
            at = pending_shots.pop(0)
            shot = run_dir / "shots" / f"{at:06.1f}s.png"
            if take_shot(process.pid, shot):
                shots.append(str(shot))
        if now - start >= seconds:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
            break
        time.sleep(0.1)
    process.wait()
    thread.join(timeout=5)
    log.close()
    if log_file and log_file.exists():
        shutil.copyfile(log_file, run_dir / "game.log")
        lines = lines + (run_dir / "game.log").read_text(errors="replace").splitlines()
    with lock:
        summary = summarize_log(lines)
    summary["result"] = result
    summary["shots"] = shots
    summary["seconds"] = round(time.monotonic() - start, 2)
    summary["exit_code"] = process.returncode
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Runs the macOS build for a while and classifies the outcome.")
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--name", default=time.strftime("%Y%m%d-%H%M%S"))
    parser.add_argument("--silence-seconds", type=float, default=20)
    parser.add_argument("--out-root", type=Path, default=ROOT / "out" / "smoke")
    parser.add_argument("--shots", type=lambda v: [float(x) for x in v.split(",")], default=[])
    parser.add_argument("--log-file", type=Path, default=ROOT / "out" / "mac" / "nier.log")
    parser.add_argument("--command", nargs=argparse.REMAINDER,
                        default=[str(ROOT / "out" / "mac" / "nier")])
    args = parser.parse_args(argv)
    summary = run(args.command, args.seconds, args.silence_seconds, args.out_root / args.name, args.log_file, args.shots)
    print(json.dumps(summary, indent=2))
    return 0 if summary["result"] in ("ok", "timeout_ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
