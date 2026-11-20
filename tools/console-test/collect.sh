#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:?usage: collect.sh NAME}"
check_console
RUN_DIR="$OUT_ROOT/$NAME"
mkdir -p "$RUN_DIR/crash"
cp "$DEPLOYED_DIR/nier.elf" "$DEPLOYED_DIR/nier.toml" "$RUN_DIR/"
if ftp_get "$SD_DIR/nier.log" "$RUN_DIR/nier.log" 2>/dev/null; then
  ftp_del "$SD_DIR/nier.log"
else
  echo "warning: no nier.log on the console" >&2
fi
ftp_get "$SD_DIR/logs/rex/rex_profile.log" "$RUN_DIR/rex_profile.log" 2>/dev/null \
  || echo "warning: no rex_profile.log on the console" >&2
if ftp_get "$SD_DIR/nak.txt" "$RUN_DIR/nak.txt" 2>/dev/null; then
  ftp_del "$SD_DIR/nak.txt"
fi
for dir in "${CRASH_DIRS[@]}"; do ftp_list "$dir" | sed "s#^#$dir/#"; done > "$RUN_DIR/crash_after.txt" \
  || echo "warning: crash report folders not listed" >&2
python3 "$CT_DIR/crash_reports.py" "$DEPLOYED_DIR/crash_before.txt" "$RUN_DIR/crash_after.txt" > "$RUN_DIR/crash_new.txt"
while read -r report; do
  [[ -n "$report" ]] || continue
  if ftp_get "$report" "$RUN_DIR/crash/${report##*/}"; then
    echo "$report" >> "$DEPLOYED_DIR/crash_before.txt"
  fi
done < "$RUN_DIR/crash_new.txt"
python3 "$CT_DIR/run_summary.py" "$RUN_DIR"
