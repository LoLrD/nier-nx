#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/console-test/common.sh"
source "$ROOT/tools/switch/docker.sh"

PGO_DIR="${PGO_DIR:-$ROOT/out/pgo}"
STREAMS="$ROOT/out/pgo-streams"

check_console
rm -rf "$STREAMS"
mkdir -p "$STREAMS"
while IFS= read -r name; do
  [[ "$name" == *.stream ]] || continue
  ftp_get "$SD_DIR/pgo/$name" "$STREAMS/$name"
done < <(ftp_list "$SD_DIR/pgo")
shopt -s nullglob
streams=("$STREAMS"/*.stream)
[[ ${#streams[@]} -gt 0 ]] || { echo "error: no .stream files in $SD_DIR/pgo on the console" >&2; exit 1; }

rm -rf "$PGO_DIR"
mkdir -p "$PGO_DIR"
ensure_image
docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" -e STREAMS="$STREAMS" "$IMAGE" bash -euo pipefail -c '
  for stream in "$STREAMS"/*.stream; do
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcov-tool merge-stream "$stream"
  done
'
python3 "$ROOT/tools/pgo_summary.py" "$PGO_DIR" "${#streams[@]}"
git -C "$ROOT" rev-parse HEAD > "$PGO_DIR/trained_at.txt"
rm -rf "$STREAMS"
for stream in "${streams[@]}"; do
  ftp_del "$SD_DIR/pgo/$(basename "$stream")"
done
echo "ok: profile from ${#streams[@]} session(s) in $PGO_DIR"
