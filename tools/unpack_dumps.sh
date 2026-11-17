#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${NIER_DATA:-$HOME/work/nier-nx-data}"
MIN_FREE_GB="${MIN_FREE_GB:-25}"

[[ $# -eq 2 ]] || { echo "usage: tools/unpack_dumps.sh <game.7z> <dlc.zip>" >&2; exit 2; }
GAME_7Z="$1"
DLC_ZIP="$2"

mkdir -p "$DATA"
free_gb=$(df -g "$DATA" | awk 'NR==2 {print $4}')
if (( free_gb < MIN_FREE_GB )); then
  echo "not enough free space in $DATA: ${free_gb} GB, need ${MIN_FREE_GB} GB" >&2
  exit 1
fi

mkdir -p "$DATA/iso" "$DATA/content"
7z x -y -o"$DATA/iso" "$GAME_7Z"
ISO="$(find "$DATA/iso" -maxdepth 1 -name '*.iso' | head -1)"
[[ -n "$ISO" ]] || { echo "no .iso inside $GAME_7Z" >&2; exit 1; }
python3 -I "$ROOT/tools/extract_iso.py" "$ISO" -o "$DATA/game_root"
unzip -o -q "$DLC_ZIP" -d "$DATA/content"
ln -sfn "$DATA" "$ROOT/assets"
echo "ok: $ROOT/assets -> $DATA"
