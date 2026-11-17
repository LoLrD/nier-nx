import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CT = ROOT / "tools" / "console-test"
FAKE_CURL = ROOT / "tests" / "fakes" / "fake_curl.py"
CLOCKS = "     clocks: CPU 1020.0 MHz, GPU 460.8 MHz, memory 1600.0 MHz | process cores 3 (mask 0x7) | SoC 43.7 C\n"


class ConsoleCycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.sd = self.tmp / "sd"
        (self.sd / "switch" / "nier-nx" / "logs" / "rex").mkdir(parents=True)
        (self.sd / "atmosphere" / "fatal_reports").mkdir(parents=True)
        (self.sd / "atmosphere" / "fatal_reports" / "01000_0100000000000013.log").write_text("old")
        self.nx = self.tmp / "nx"
        self.nx.mkdir()
        for name in ("nier", "nier.toml", "nier-nx.nro"):
            (self.nx / name).write_text(name)
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        (bin_dir / "curl").write_text(f"#!/bin/sh\nexec python3 {FAKE_CURL} \"$@\"\n")
        (bin_dir / "curl").chmod(0o755)
        self.out = self.tmp / "out"
        self.env = {"PATH": f"{bin_dir}:/opt/homebrew/bin:/usr/bin:/bin", "HOME": str(Path.home()), "FAKE_SD": str(self.sd),
                    "SWITCH_IP": "192.0.2.7", "CONSOLE_ENV": "/dev/null", "OUT_ROOT": str(self.out),
                    "NX_OUT": str(self.nx)}

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        result = subprocess.run(["bash", str(CT / args[0]), *args[1:]], env=self.env, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def play(self, log: str, crash: str = "", profile: str = "") -> None:
        (self.sd / "switch" / "nier-nx" / "nier.log").write_text(log)
        if crash:
            (self.sd / "atmosphere" / "fatal_reports" / crash).write_text("fatal")
        if profile:
            (self.sd / "switch" / "nier-nx" / "logs" / "rex" / "rex_profile.log").write_text(profile)

    def summary(self, name: str) -> dict:
        return json.loads((self.out / name / "summary.json").read_text())

    def test_missing_report_folder_counts_as_empty(self) -> None:
        self.run_script("deploy.sh")
        self.assertTrue((self.sd / "switch" / "nier-nx" / "nier-nx.nro").exists())

    def test_each_collect_reports_only_its_own_session(self) -> None:
        self.run_script("deploy.sh")
        self.play("session A\n", crash="02000_0100000000000013.log")
        self.run_script("collect.sh", "a")
        self.play("session B\n")
        self.run_script("collect.sh", "b")
        self.assertEqual(self.summary("a")["crash_reports"], ["/atmosphere/fatal_reports/02000_0100000000000013.log"])
        self.assertEqual(self.summary("a")["result"], "crash")
        self.assertEqual(self.summary("b")["crash_reports"], [])
        self.assertEqual((self.out / "b" / "nier.log").read_text(), "session B\n")

    def test_summary_records_the_clocks_of_the_run(self) -> None:
        self.run_script("deploy.sh")
        self.play("session\n", profile="==== 10.0 s | mode: normal | game 6.3 fps\n" + CLOCKS)
        self.run_script("collect.sh", "c")
        self.assertEqual(self.summary("c")["clocks"], [{"cpu_mhz": 1020.0, "gpu_mhz": 460.8, "memory_mhz": 1600.0}])

    def test_collect_writes_a_summary_when_the_log_is_missing(self) -> None:
        self.run_script("deploy.sh")
        result = self.run_script("collect.sh", "d")
        self.assertEqual(self.summary("d")["result"], "no_log")
        self.assertIn("nier.log", result.stderr)


if __name__ == "__main__":
    unittest.main()
