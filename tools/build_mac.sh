#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/build/mac}"
OUT="$ROOT/out/mac"
if command -v nproc >/dev/null 2>&1; then DEFAULT_JOBS=$(nproc); else DEFAULT_JOBS=$(sysctl -n hw.ncpu); fi
JOBS="${JOBS:-$DEFAULT_JOBS}"

[[ -f "$ROOT/app/generated/rexglue.cmake" ]] || { echo "error: app/generated missing; run tools/codegen.sh" >&2; exit 1; }

cmake -S "$ROOT/app" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXSDK_DIR="$ROOT/sdk"
cmake --build "$BUILD_DIR" --target nier -j "$JOBS"

[[ -x "$BUILD_DIR/nier" ]] || { echo "error: no nier executable produced" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD_DIR/nier" "$BUILD_DIR"/*.dylib "$OUT/"
cp -R "$BUILD_DIR/vulkan" "$OUT/vulkan"
cp "$ROOT/app/nier.toml" "$OUT/nier.toml"
echo "ok: $OUT/nier"
