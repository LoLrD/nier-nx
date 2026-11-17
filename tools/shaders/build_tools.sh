#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SHADERS="$ROOT/tools/shaders"
OUT="$ROOT/out/shader-tools"
mkdir -p "$OUT"
EXTRA=()
clang++ -x c++ -std=c++20 -fexperimental-library -fsyntax-only /dev/null 2>/dev/null && EXTRA+=(-fexperimental-library)
clang++ -std=c++20 -O2 "${EXTRA[@]}" -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions \
  -include "$SHADERS/XenosRecomp/pch_min.h" -I"$SHADERS" -I"$SHADERS/XenosRecomp" \
  -I"$ROOT/sdk/thirdparty/fmt/include" -I"$ROOT/sdk/thirdparty/xxHash" \
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
  "$SHADERS/translate/masseffect_hlsl.cpp" "$SHADERS/XenosRecomp/shader_recompiler.cpp" -o "$OUT/xenos_hlsl"
echo "$OUT/xenos_hlsl"
