import subprocess
import sys
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CT = ROOT / "tools" / "console-test"
sys.path.insert(0, str(CT))

import crash_reports


class CrashReportsTest(unittest.TestCase):
    def test_only_reports_that_appeared_during_the_run(self) -> None:
        before = {"01762345_0100000000001000.log"}
        after = ["01762345_0100000000001000.log", "01769999_0100000000001000.log", "notes.txt"]
        self.assertEqual(crash_reports.pick_new(before, after), ["01769999_0100000000001000.log"])


class DeployScriptTest(unittest.TestCase):
    def test_unreachable_console_fails_fast(self) -> None:
        start = time.monotonic()
        result = subprocess.run(
            ["bash", str(CT / "deploy.sh")],
            env={"PATH": "/usr/bin:/bin:/opt/homebrew/bin", "SWITCH_IP": "192.0.2.1", "CONSOLE_ENV": "/dev/null",
                 "HOME": str(Path.home())},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertLess(time.monotonic() - start, 30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no connection to 192.0.2.1:5000", result.stderr)


if __name__ == "__main__":
    unittest.main()
