import argparse
import os
import subprocess
import time
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "out" / "mac" / "nier"
PATHS = {"xenos": [], "native": ["--nier_renderer=native"]}


def launch(name: str, out: Path, route: str) -> tuple[subprocess.Popen, Path]:
    control = out / f"{name}.control"
    control.write_text("")
    env = dict(os.environ, NIER_CONTROL_FILE=str(control))
    command = [str(BINARY), f"--nier_input_script={route}", "--keyboard_ui_auto_text=Nier", "--audio_mute=true",
               *PATHS[name]]
    log = (out / f"{name}.log").open("w")
    return subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT), control


def sheet(out: Path, times: list[int]) -> Path:
    width, height = 480, 270
    image = Image.new("RGB", (width * 2, (height + 20) * len(times)))
    draw = ImageDraw.Draw(image)
    for row, at in enumerate(times):
        top = row * (height + 20)
        for column, name in enumerate(PATHS):
            frame = out / f"{name}_{at:04d}.ppm"
            if frame.exists():
                image.paste(Image.open(frame).convert("RGB").resize((width, height)), (column * width, top + 20))
            draw.text((column * width + 4, top + 4), f"{name} {at} s", fill=(255, 255, 0))
    path = out / "compare.png"
    image.save(path)
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description="Runs the Xenos and native paths side by side and captures frames.")
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--start", type=int, default=10)
    parser.add_argument("--every", type=int, default=10)
    parser.add_argument("--route", default="app/input/prologue.tsv")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "compare")
    parser.add_argument("--paths", default="xenos,native")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for old in args.out.glob("*.ppm"):
        old.unlink()
    runs = {name: launch(name, args.out, args.route) for name in args.paths.split(",")}
    start = time.monotonic()
    times = list(range(args.start, args.seconds, args.every))
    try:
        for at in times:
            time.sleep(max(0.0, start + at - time.monotonic()))
            for name, (process, control) in runs.items():
                control.write_text(f"capture = {args.out / f'{name}_{at:04d}.ppm'}\n")
        time.sleep(max(0.0, start + args.seconds - time.monotonic()))
    finally:
        for process, _ in runs.values():
            process.terminate()
        for process, _ in runs.values():
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
    print(sheet(args.out, times))


if __name__ == "__main__":
    main()
