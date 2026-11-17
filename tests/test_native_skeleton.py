import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "tests"
INCLUDES = [
    ROOT / "sdk" / "include",
    ROOT / "out" / "build" / "mac" / "rexglue-sdk" / "include",
    ROOT / "sdk" / "thirdparty" / "fmt" / "include",
    ROOT / "sdk" / "thirdparty" / "spdlog" / "include",
    ROOT / "app" / "src" / "native",
]


def build_and_run(name: str, sources: list[str]) -> subprocess.CompletedProcess:
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / f"native_{name}_test"
    command = ["clang++", "-std=c++23", "-Wall", "-Werror"]
    for include in INCLUDES:
        command += ["-I", str(include)]
    command += [str(ROOT / "tests" / "cpp" / f"{name}_test.cpp")]
    command += [str(ROOT / "app" / "src" / "native" / source) for source in sources]
    command += ["-o", str(binary)]
    build = subprocess.run(command, capture_output=True, text=True)
    if build.returncode != 0:
        return build
    return subprocess.run([str(binary)], capture_output=True, text=True)


class NativeSkeletonTest(unittest.TestCase):
    def check(self, name: str, sources: list[str]) -> None:
        result = build_and_run(name, sources)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pm4_reader(self) -> None:
        self.check("pm4_reader", [])

    def test_gpu_registers(self) -> None:
        self.check("gpu_registers", ["gpu_registers.cpp"])

    def test_pm4_executor(self) -> None:
        self.check("pm4_executor", ["gpu_registers.cpp", "pm4_executor.cpp"])

    def test_ring_state(self) -> None:
        self.check("ring_state", [])

    def test_shader_dump(self) -> None:
        self.check("shader_dump", ["shader_dump.cpp"])


if __name__ == "__main__":
    unittest.main()
