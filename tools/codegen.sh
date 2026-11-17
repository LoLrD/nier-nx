#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app"
REXGLUE="${REXGLUE:-$ROOT/sdk/out/host/rexglue}"
MANIFEST="${MANIFEST:-nier_manifest.toml}"

[[ -x "$REXGLUE" ]] || { echo "error: $REXGLUE not found; run tools/build_host.sh (or set REXGLUE)" >&2; exit 1; }
python3 "$ROOT/tools/check_game.py"

mkdir -p "$ROOT/out"
echo "== codegen ($APP/$MANIFEST)"
if ! ( cd "$APP" && REX_MAX_JUMP_TABLE_ENTRIES=1024 "$REXGLUE" codegen "$MANIFEST" ) 2>&1 | tee "$ROOT/out/codegen.log"; then
  echo "codegen failed, see out/codegen.log" >&2
  exit 1
fi
[[ -f "$APP/generated/default/sources.cmake" ]] || { echo "error: the generator produced no $APP/generated/default/sources.cmake" >&2; exit 1; }
echo "ok: $APP/generated/default"
