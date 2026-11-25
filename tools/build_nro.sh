#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESA_SDK="${MESA_SDK:-$HOME/work/nier-nx-data/mesa-sdk}"
NRO_OUT="${NRO_OUT:-out/nx}"
JOBS="${JOBS:-8}"
LIBVULKAN="$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a"

[[ -f "$LIBVULKAN" ]] || { echo "error: the NVK driver was not found at $LIBVULKAN; build it with mesa/build_mesa_docker.sh" >&2; exit 1; }
[[ -f "$ROOT/app/generated/rexglue.cmake" ]] || { echo "error: app/generated missing; run tools/codegen.sh" >&2; exit 1; }
MESA_SDK="$(cd "$MESA_SDK" && pwd -P)"
source "$ROOT/tools/switch/docker.sh"
ensure_image

docker run --rm -v "$ROOT:$ROOT" -v "$MESA_SDK:$MESA_SDK:ro" -w "$ROOT" \
  -e MESA_SDK="$MESA_SDK" -e NRO_OUT="$NRO_OUT" -e JOBS="$JOBS" -e ROOT="$ROOT" -e CMAKE_EXTRA="${CMAKE_EXTRA:-}" \
  "$IMAGE" bash -euo pipefail -c '
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm --defined-only \
      "$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a" > /tmp/nvk-symbols.txt
    grep -Eq " [BD] nvk_switch_draw$" /tmp/nvk-symbols.txt || { echo "error: the Mesa SDK lacks the NVK patches; rebuild it with mesa/build_mesa_docker.sh" >&2; exit 1; }
    cmake -S app -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$ROOT/sdk" \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch" $CMAKE_EXTRA
    cmake --build "$NRO_OUT" -j "$JOBS" 2>&1 | tee "$NRO_OUT/build.log"
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/nier" | grep "^0000000000000000 T _start$" > /dev/null
    echo "OK: _start at 0"
  '
if grep -q "NIER_PGO:STRING=use" "$ROOT/$NRO_OUT/CMakeCache.txt"; then
  echo "profile: $(grep -c "Wcoverage-mismatch" "$ROOT/$NRO_OUT/build.log" || true) functions changed since training," \
    "$(grep -c "Wmissing-profile" "$ROOT/$NRO_OUT/build.log" || true) files without a profile"
fi
cp -f "$ROOT/$NRO_OUT/nier.nro" "$ROOT/$NRO_OUT/nier-nx.nro"
cp -f "$ROOT/app/nier_switch.toml" "$ROOT/$NRO_OUT/nier.toml"
echo "ok: $ROOT/$NRO_OUT/nier-nx.nro"
