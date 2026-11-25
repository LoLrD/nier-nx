import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROUTES = ROOT / "app" / "input"
out = Path(sys.argv[1]).resolve()
seconds = int(sys.argv[2])
events = []
for item in sys.argv[3].split(";"):
    at, text = item.split("@", 1)
    events.append((float(at), text.replace("|", "\n") + "\n"))
extra = sys.argv[4:]
out.mkdir(parents=True, exist_ok=True)
for old in out.glob("*.ppm"):
    old.unlink()
control = out / "control"
control.write_text("")
env = dict(os.environ, NIER_CONTROL_FILE=str(control))
command = [str(ROOT / "out/mac/nier"), f"--nier_input_script={ROUTES / os.environ.get('NIER_ROUTE', 'village_route.tsv')}", "--keyboard_ui_auto_text=Nier",
           "--audio_mute=true", "--nier_renderer=native", f"--log_file={out / 'nier.log'}", *extra]
log = (out / "stdout.log").open("w")
process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
start = time.monotonic()
try:
    for at, text in sorted(events):
        time.sleep(max(0.0, start + at - time.monotonic()))
        control.write_text(text.replace("{out}", str(out)))
    time.sleep(max(0.0, start + seconds - time.monotonic()))
finally:
    process.terminate()
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
