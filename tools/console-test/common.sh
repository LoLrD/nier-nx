CT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$CT_DIR/../.." && pwd)"
CONSOLE_ENV="${CONSOLE_ENV:-$CT_DIR/credentials.env}"
if [[ -f "$CONSOLE_ENV" ]]; then
  while IFS='=' read -r key value; do
    case "$key" in ''|\#*) continue ;; esac
    [[ -z "${!key:-}" ]] && export "$key=$value"
  done < "$CONSOLE_ENV"
fi
: "${SWITCH_IP:?SWITCH_IP is not set: copy tools/console-test/credentials.env.example to credentials.env}"
FTP_PORT="${FTP_PORT:-5000}"
FTP_USER="${FTP_USER:-anonymous}"
FTP_PASS="${FTP_PASS:-}"
SD_DIR="${SD_DIR:-/switch/nier-nx}"
OUT_ROOT="${OUT_ROOT:-$ROOT/out/console-test}"
NX_OUT="${NX_OUT:-$ROOT/out/nx}"
DEPLOYED_DIR="$OUT_ROOT/deployed"
CRASH_DIRS=(/atmosphere/crash_reports /atmosphere/fatal_reports)
FTP_BASE="ftp://$SWITCH_IP:$FTP_PORT/"

check_console() {
  curl -sS --max-time 5 --user "$FTP_USER:$FTP_PASS" "$FTP_BASE" -l > /dev/null 2>&1 \
    || { echo "error: no connection to $SWITCH_IP:$FTP_PORT (console off, other IP, or sys-ftpd not running)" >&2; exit 1; }
}
ftp_curl() { curl -sS --retry 3 --retry-delay 3 --retry-all-errors --user "$FTP_USER:$FTP_PASS" "$@"; }
ftp_put()  { ftp_curl --ftp-create-dirs -T "$1" "${FTP_BASE}${SD_DIR}/$2"; }
ftp_get()  { ftp_curl --max-time 300 "${FTP_BASE}$1" -o "$2"; }
ftp_list() {
  local out rc=0
  out=$(ftp_curl "${FTP_BASE}$1/" -l 2>/dev/null) || rc=$?
  case $rc in
    0) [[ -z "$out" ]] || printf '%s\n' "$out" | tr -d '\r' | sed 's#.*/##' ;;
    9) ;;
    *) echo "error: cannot list $1 on $SWITCH_IP:$FTP_PORT (curl exit $rc)" >&2; return "$rc" ;;
  esac
}
ftp_del()  { ftp_curl "${FTP_BASE}" -Q "DELE $1" -o /dev/null 2>/dev/null || true; }
