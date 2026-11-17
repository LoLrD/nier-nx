#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
exec out/mac/nier --mnk_mode=true "$@"
