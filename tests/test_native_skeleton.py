import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "tests"
REGISTER_FILE = str(ROOT / "sdk" / "src" / "graphics" / "register_file.cpp")
INCLUDES = [
    ROOT / "sdk" / "include",
    ROOT / "out" / "build" / "mac" / "rexglue-sdk" / "include",
    ROOT / "sdk" / "thirdparty" / "fmt" / "include",
    ROOT / "sdk" / "thirdparty" / "spdlog" / "include",
    ROOT / "app" / "src" / "native",
]
SDK_LIBS = ROOT / "out" / "build" / "mac"
SDK_LINK = [
    "-I", str(ROOT / "sdk" / "thirdparty" / "xxHash"),
    "-I", str(ROOT / "sdk" / "thirdparty" / "glslang"),
    "-I", str(ROOT / "sdk" / "thirdparty" / "vulkan-headers" / "include"),
    "-L", str(SDK_LIBS), "-lrexgpu-xenosrd", "-lrexruntimerd", f"-Wl,-rpath,{SDK_LIBS}",
]


def build_and_run(name: str, sources: list[str], link_sdk: bool = False) -> subprocess.CompletedProcess:
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / f"native_{name}_test"
    command = ["clang++", "-std=c++23", "-Wall", "-Werror"]
    for include in INCLUDES:
        command += ["-I", str(include)]
    command += [str(ROOT / "tests" / "cpp" / f"{name}_test.cpp")]
    command += [str(source if source.startswith("/") else ROOT / "app" / "src" / "native" / source) for source in sources]
    if link_sdk:
        command += SDK_LINK
    command += ["-o", str(binary)]
    build = subprocess.run(command, capture_output=True, text=True)
    if build.returncode != 0:
        return build
    return subprocess.run([str(binary)], capture_output=True, text=True)


class NativeSkeletonTest(unittest.TestCase):
    def check(self, name: str, sources: list[str], link_sdk: bool = False) -> None:
        result = build_and_run(name, sources, link_sdk)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pm4_reader(self) -> None:
        self.check("pm4_reader", [])

    def test_gpu_registers(self) -> None:
        self.check("gpu_registers", ["gpu_registers.cpp", REGISTER_FILE])

    def test_pm4_executor(self) -> None:
        self.check("pm4_executor", ["gpu_registers.cpp", "pm4_executor.cpp", REGISTER_FILE])

    def test_ring_state(self) -> None:
        self.check("ring_state", [])

    def test_shader_dump(self) -> None:
        self.check("shader_dump", ["shader_dump.cpp"])

    def test_mirror_sync(self) -> None:
        self.check("mirror_sync", ["mirror_sync.cpp"], link_sdk=True)

    def test_target_keys(self) -> None:
        self.check("target_keys", ["target_keys.cpp"], link_sdk=True)

    def test_primitive_setup(self) -> None:
        self.check("primitive_setup", ["primitive_setup.cpp"], link_sdk=True)

    def test_draw_constants(self) -> None:
        self.check("draw_constants", ["draw_constants.cpp"], link_sdk=True)

    def test_pipeline_state(self) -> None:
        self.check("pipeline_state", ["pipeline_state.cpp"], link_sdk=True)

    def test_texture_loader(self) -> None:
        self.check("texture_loader", ["texture_loader.cpp"], link_sdk=True)

    def test_resolve_plan(self) -> None:
        self.check("resolve_plan", ["resolve_plan.cpp", "target_keys.cpp", "texture_loader.cpp"], link_sdk=True)

    def test_gpu_times(self) -> None:
        self.check("gpu_times", ["gpu_times.cpp"])

    def test_sdk_link(self) -> None:
        self.check("sdk_link", [], link_sdk=True)


if __name__ == "__main__":
    unittest.main()
