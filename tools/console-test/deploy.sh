#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ROUTE=""
STACKS=0
LANGUAGE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --route) ROUTE="$2"; shift ;;
    --stacks) STACKS=1 ;;
    --language) LANGUAGE="$2"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

check_console
rm -rf "$DEPLOYED_DIR"
mkdir -p "$DEPLOYED_DIR"
cp "$NX_OUT/nier" "$DEPLOYED_DIR/nier.elf"
for dir in "${CRASH_DIRS[@]}"; do ftp_list "$dir" | sed "s#^#$dir/#"; done > "$DEPLOYED_DIR/crash_before.txt"

TOML="$DEPLOYED_DIR/nier.toml"
LANG_OUT="$ROOT/out/lang/$LANGUAGE"
if [[ -n "$LANGUAGE" && ! -d "$LANG_OUT" ]]; then
  echo "error: $LANG_OUT is missing, run tools/lang/build.py $LANGUAGE first" >&2
  exit 2
fi
{
  if [[ -n "$ROUTE" ]]; then
    echo "nier_input_script = \"sdmc:$SD_DIR/route.tsv\""
    echo "keyboard_ui_auto_text = \"Nier\""
  fi
  if [[ -n "$LANGUAGE" ]]; then
    echo "nier_language_dir = \"sdmc:$SD_DIR/lang/$LANGUAGE\""
  fi
  cat "$NX_OUT/nier.toml"
} > "$TOML"
if [[ -n "$ROUTE" ]]; then
  ftp_put "$ROOT/$ROUTE" route.tsv
fi
if [[ -n "$LANGUAGE" ]]; then
  while IFS= read -r file; do
    ftp_put "$LANG_OUT/$file" "lang/$LANGUAGE/$file"
  done < <(cd "$LANG_OUT" && find . -type f | sed 's#^\./##')
fi
ftp_put "$TOML" nier.toml
ftp_put "$NX_OUT/nier-nx.nro" nier-nx.nro
ftp_del "$SD_DIR/nier.log"
ftp_del "$SD_DIR/logs/rex/rex_profile.log"
if [[ $STACKS -eq 1 ]]; then
  : > "$DEPLOYED_DIR/stacks_profile.flag"
  ftp_put "$DEPLOYED_DIR/stacks_profile.flag" logs/rex/stacks_profile.flag
else
  ftp_del "$SD_DIR/logs/rex/stacks_profile.flag"
fi
echo "deployed: start NieR from hbmenu (hold R while launching a game), close it when done, then run collect.sh NAME" >&2
