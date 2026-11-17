#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTAINERS="$1"
SPIRV="$2"
OUTPUT="$3"
TOOLS="$ROOT/out/shader-tools"
mkdir -p "$TOOLS"
clang++ -std=c++20 -O2 -I"$ROOT/sdk/thirdparty/xxHash" -I"$ROOT/app/src/native" \
  "$ROOT/tools/shaders/pack/me_pack_shaders.cpp" "$ROOT/app/src/native/shader_library.cpp" -o "$TOOLS/pack_shaders"
rm -f "$OUTPUT" "$OUTPUT.idx"
"$TOOLS/pack_shaders" "$CONTAINERS" "$SPIRV" "$OUTPUT"
