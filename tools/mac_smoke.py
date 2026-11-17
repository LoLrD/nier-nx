#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMMAND = [str(ROOT / "out" / "mac" / "nier")]
DEFAULT_LOG_FILE = ROOT / "out" / "mac" / "nier.log"
UNREGISTERED = re.compile(r"Call to invalid or unregistered function at guest address (?:0x)?([0-9A-Fa-f]{8})")
FRAME = "XELOG_GPU PRESENT:"
IO_STATS = re.compile(r"\[io\] \d+ s: (\d+) opens \((\d+) failed\)")


def summarize_log(lines: list[str]) -> dict:
    unregistered = []
    opens = failed = 0
    for line in lines:
        match = UNREGISTERED.search(line)
        if match and match.group(1).upper() not in unregistered:
            unregistered.append(match.group(1).upper())
        io = IO_STATS.search(line)
        if io:
            opens += int(io.group(1))
            failed += int(io.group(2))
    return {
        "frames": sum(1 for line in lines if FRAME in line),
        "files_opened": opens - failed,
        "files_failed": failed,
        "unregistered_calls": unregistered,
        "last_log_line": next((line for line in reversed(lines) if line.strip()), ""),
    }


def rotated_logs(log_file: Path) -> list[Path]:
    pattern = re.compile(re.escape(log_file.stem) + r"\.(\d+)" + re.escape(log_file.suffix) + "$")
    rotated = [(int(m.group(1)), p) for p in log_file.parent.glob(f"{log_file.stem}.*{log_file.suffix}")
               for m in [pattern.match(p.name)] if m]
    return [p for _, p in sorted(rotated, reverse=True)]


def collect_log(log_file: Path) -> list[str]:
    lines: list[str] = []
    for path in rotated_logs(log_file) + ([log_file] if log_file.exists() else []):
        lines.extend(path.read_text(errors="replace").splitlines())
    return lines


def remove_logs(log_file: Path) -> None:
    for path in rotated_logs(log_file) + ([log_file] if log_file.exists() else []):
        path.unlink()


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
    try:
        result = subprocess.run(
            ["lldb", "--batch", "-p", str(pid), "-o", "thread backtrace all", "-o", "detach",
             "-k", "thread backtrace all", "-k", "detach"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        path.write_text(result.stdout + result.stderr)
    except subprocess.TimeoutExpired:
        path.write_text("lldb timed out after 120 s\n")


class Activity:
    def __init__(self, log_file: Path | None) -> None:
        self.log_file = log_file
        self.offset = 0
        self.lock = threading.Lock()
        self.last_output = time.monotonic()
        self.last_frame: float | None = None

    def saw_line(self, line: str) -> None:
        now = time.monotonic()
        with self.lock:
            self.last_output = now
            if FRAME in line:
                self.last_frame = now

    def poll_log(self) -> None:
        if not self.log_file or not self.log_file.exists():
            return
        size = self.log_file.stat().st_size
        if size < self.offset:
            self.offset = 0
        if size == self.offset:
            return
        with self.log_file.open("rb") as fh:
            fh.seek(self.offset)
            chunk = fh.read(size - self.offset)
        self.offset = size
        for line in chunk.decode(errors="replace").splitlines():
            self.saw_line(line)

    def silent_for(self) -> float:
        with self.lock:
            reference = self.last_frame if self.last_frame is not None else self.last_output
        return time.monotonic() - reference

    def touch(self) -> None:
        with self.lock:
            self.last_output = time.monotonic()
            if self.last_frame is not None:
                self.last_frame = self.last_output


def run(command: list[str], seconds: float, silence_seconds: float, run_dir: Path, log_file: Path | None = None,
        shot_times: list[float] | None = None, stack_times: list[float] | None = None) -> dict:
    pending_shots = sorted(shot_times or [])
    pending_stacks = sorted(stack_times or [])
    shots: list[str] = []
    run_dir.mkdir(parents=True, exist_ok=True)
    if log_file:
        remove_logs(log_file)
    activity = Activity(log_file)
    lines: list[str] = []
    stdout_log = (run_dir / "nier.log").open("w")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               errors="replace", cwd=ROOT)

    def reader() -> None:
        for line in process.stdout:
            stdout_log.write(line)
            stdout_log.flush()
            lines.append(line.rstrip("\n"))
            activity.saw_line(line)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    start = time.monotonic()
    result = "timeout_ok"
    while True:
        if process.poll() is not None:
            result = "ok" if process.returncode == 0 else "crash"
            break
        activity.poll_log()
        now = time.monotonic()
        if activity.silent_for() >= silence_seconds:
            dump_stacks(process.pid, run_dir / "stacks.txt")
            process.kill()
            result = "hang"
            break
        while pending_shots and now - start >= pending_shots[0]:
            at = pending_shots.pop(0)
            shot = run_dir / "shots" / f"{at:06.1f}s.png"
            if take_shot(process.pid, shot):
                shots.append(str(shot))
        while pending_stacks and now - start >= pending_stacks[0]:
            at = pending_stacks.pop(0)
            dump_stacks(process.pid, run_dir / f"stacks-{at:06.1f}s.txt")
            activity.touch()
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
    stdout_log.close()
    if log_file:
        game_lines = collect_log(log_file)
        (run_dir / "game.log").write_text("\n".join(game_lines) + "\n")
        lines = lines + game_lines
    summary = summarize_log(lines)
    summary["result"] = result
    summary["shots"] = shots
    summary["seconds"] = round(time.monotonic() - start, 2)
    summary["last_frame_seconds"] = round(activity.last_frame - start, 2) if activity.last_frame else None
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
    parser.add_argument("--stacks-at", type=lambda v: [float(x) for x in v.split(",")], default=[])
    parser.add_argument("--log-file", type=Path, default=DEFAULT_LOG_FILE)
    parser.add_argument("--command", nargs=argparse.REMAINDER, default=DEFAULT_COMMAND)
    args = parser.parse_args(argv)
    summary = run(args.command, args.seconds, args.silence_seconds, args.out_root / args.name, args.log_file,
                  args.shots, args.stacks_at)
    print(json.dumps(summary, indent=2))
    return 0 if summary["result"] in ("ok", "timeout_ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
