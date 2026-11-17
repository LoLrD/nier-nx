#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IN="$1"
OUT="$2"
VULKAN_BIN="${VULKAN_BIN:-$HOME/VulkanSDK/1.4.363.1/macOS/bin}"
export DXC="$VULKAN_BIN/dxc" VAL="$VULKAN_BIN/spirv-val" SPIRV_OUT="$OUT/spirv"
rm -rf "$OUT/hlsl" "$OUT/spirv"
mkdir -p "$OUT/spirv"
"$ROOT/out/shader-tools/xenos_hlsl" "$IN" "$OUT/hlsl" "$ROOT/tools/shaders/XenosRecomp/shader_common.h" \
  > "$OUT/translate.log" 2>&1 || true
find "$OUT/hlsl" -name "*.hlsl" -print0 | xargs -0 -P "$(sysctl -n hw.ncpu)" -n 1 "$ROOT/tools/shaders/compile_spirv_one.sh"
python3 - "$IN" "$OUT" <<'PY'
import json, sys
from pathlib import Path
containers, out = Path(sys.argv[1]), Path(sys.argv[2])
names = sorted(p.stem for p in containers.glob("*.bin"))
hlsl = {p.stem for p in (out / "hlsl").glob("*.hlsl")}
spirv = {p.stem for p in (out / "spirv").glob("*.spv")}
report = {"containers": len(names), "hlsl": len(hlsl), "spirv": len(spirv),
          "failed": [n for n in names if n not in spirv]}
(out / "report.json").write_text(json.dumps(report, indent=2))
print(json.dumps({k: (len(v) if isinstance(v, list) else v) for k, v in report.items()}))
PY
