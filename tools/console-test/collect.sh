#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:?usage: collect.sh NAME}"
check_console
RUN_DIR="$OUT_ROOT/$NAME"
mkdir -p "$RUN_DIR/crash"
cp "$DEPLOYED_DIR/nier.elf" "$DEPLOYED_DIR/nier.toml" "$RUN_DIR/"
ftp_get "$SD_DIR/nier.log" "$RUN_DIR/nier.log" || true
ftp_get "$SD_DIR/logs/rex/rex_profile.log" "$RUN_DIR/rex_profile.log" || true
for dir in "${CRASH_DIRS[@]}"; do ftp_list "$dir" | sed "s#^#$dir/#"; done > "$RUN_DIR/crash_after.txt"
sed 's#.*/##' "$RUN_DIR/crash_after.txt" > "$RUN_DIR/crash_after_names.txt"
python3 "$CT_DIR/crash_reports.py" "$DEPLOYED_DIR/crash_before.txt" "$RUN_DIR/crash_after_names.txt" > "$RUN_DIR/crash_new.txt"
while read -r report; do
  [[ -n "$report" ]] || continue
  ftp_get "$(grep "/$report\$" "$RUN_DIR/crash_after.txt")" "$RUN_DIR/crash/$report" || true
done < "$RUN_DIR/crash_new.txt"

python3 - "$RUN_DIR" <<'PY'
import json, sys
from pathlib import Path
run = Path(sys.argv[1])
sys.path.insert(0, str(run.parents[2] / "tools"))
import mac_smoke
log = run / "nier.log"
lines = log.read_text(errors="replace").splitlines() if log.exists() else []
summary = mac_smoke.summarize_log(lines)
crashes = [l for l in (run / "crash_new.txt").read_text().split() if l]
fatal = any("[FATAL]" in l or "[critical]" in l for l in lines)
summary["result"] = "crash" if crashes or fatal else ("no_log" if not lines else "ok")
summary["crash_reports"] = crashes
(run / "summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps({k: v for k, v in summary.items() if k != "last_log_line"}))
PY
