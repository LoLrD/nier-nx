import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SMOKE = ROOT / "tools" / "mac_smoke.py"
sys.path.insert(0, str(ROOT / "tools"))

import mac_smoke


def run_smoke(out_root: Path, command: list[str], seconds: int, silence: int) -> tuple[int, dict, Path]:
    result = subprocess.run(
        [sys.executable, str(SMOKE), "--seconds", str(seconds), "--name", "t", "--silence-seconds", str(silence),
         "--out-root", str(out_root), "--command", *command],
        capture_output=True,
        text=True,
        timeout=60,
    )
    run_dir = out_root / "t"
    return result.returncode, json.loads((run_dir / "summary.json").read_text()), run_dir


class SummarizeLogTest(unittest.TestCase):
    def test_summarize_counts_unregistered_calls(self) -> None:
        lines = [
            "[error] Call to invalid or unregistered function at guest address 82123450",
            "[info] something else",
            "[error] Call to invalid or unregistered function at guest address 8234ABC0",
        ]
        summary = mac_smoke.summarize_log(lines)
        self.assertEqual(summary["unregistered_calls"], ["82123450", "8234ABC0"])

    def test_summarize_counts_gpu_presents_and_file_opens(self) -> None:
        lines = [
            "[2026-11-17 14:31:16.178] [debug] [gpu] [t1] XELOG_GPU PRESENT: swap_texture_view=0x7b0eb94b40 packet_size=1280x720",
            "[2026-11-17 14:31:16.195] [debug] [gpu] [t1] XELOG_GPU PRESENT: swap_texture_view=0x7b0eb94900 packet_size=1280x720",
            "[2026-11-17 14:31:16.199] [debug] [apu] [t2] SDLCallback: no frames queued (silence)",
            "[2026-11-17 14:31:16.200] [debug] [krnl] [t3] NtCreateFile(GAME:\\MEDIA\\LAYER1.CPK) -> 00000000",
        ]
        summary = mac_smoke.summarize_log(lines)
        self.assertEqual(summary["frames"], 2)
        self.assertEqual(summary["files_opened"], 1)


class SmokeRunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_hang_detected(self) -> None:
        code, summary, run_dir = run_smoke(
            self.out,
            [sys.executable, "-c", "import time; print('x', flush=True); time.sleep(60)"],
            seconds=30,
            silence=1,
        )
        self.assertEqual(summary["result"], "hang")
        self.assertTrue((run_dir / "stacks.txt").exists())
        self.assertNotEqual(code, 0)

    def test_crash_detected(self) -> None:
        code, summary, _ = run_smoke(
            self.out,
            [sys.executable, "-c", "import sys; print('boot', flush=True); sys.exit(134)"],
            seconds=10,
            silence=5,
        )
        self.assertEqual(summary["result"], "crash")
        self.assertEqual(summary["last_log_line"], "boot")
        self.assertNotEqual(code, 0)

    def test_log_file_growth_is_not_silence(self) -> None:
        log_file = self.out / "game.log"
        script = (
            "import time\n"
            f"f = open({str(log_file)!r}, 'w')\n"
            "while True:\n"
            " f.write('[error] Call to invalid or unregistered function at guest address 82000010\\n')\n"
            " f.flush()\n"
            " time.sleep(0.2)\n"
        )
        result = subprocess.run(
            [sys.executable, str(SMOKE), "--seconds", "2", "--name", "t", "--silence-seconds", "1",
             "--out-root", str(self.out), "--log-file", str(log_file), "--command", sys.executable, "-c", script],
            capture_output=True,
            text=True,
            timeout=60,
        )
        summary = json.loads((self.out / "t" / "summary.json").read_text())
        self.assertEqual(summary["result"], "timeout_ok", result.stdout + result.stderr)
        self.assertEqual(summary["unregistered_calls"], ["82000010"])
        self.assertTrue((self.out / "t" / "game.log").exists())

    def test_stacks_at_dumps_threads_and_keeps_running(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SMOKE), "--seconds", "3", "--name", "t", "--silence-seconds", "10",
             "--out-root", str(self.out), "--stacks-at", "1",
             "--command", sys.executable, "-c", "import time\nwhile True:\n print('tick', flush=True)\n time.sleep(0.2)"],
            capture_output=True,
            text=True,
            timeout=180,
        )
        summary = json.loads((self.out / "t" / "summary.json").read_text())
        self.assertEqual(summary["result"], "timeout_ok", result.stdout + result.stderr)
        self.assertTrue((self.out / "t" / "stacks-0001.0s.txt").exists())

    def test_shots_without_window_are_skipped(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SMOKE), "--seconds", "2", "--name", "t", "--silence-seconds", "5",
             "--out-root", str(self.out), "--shots", "1",
             "--command", sys.executable, "-c", "import time\nwhile True:\n print('tick', flush=True)\n time.sleep(0.2)"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        summary = json.loads((self.out / "t" / "summary.json").read_text())
        self.assertEqual(summary["result"], "timeout_ok", result.stdout + result.stderr)
        self.assertEqual(summary["shots"], [])

    def test_survives_until_timeout(self) -> None:
        code, summary, _ = run_smoke(
            self.out,
            [sys.executable, "-c", "import time\nwhile True:\n print('tick', flush=True)\n time.sleep(0.2)"],
            seconds=2,
            silence=5,
        )
        self.assertEqual(summary["result"], "timeout_ok")
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
