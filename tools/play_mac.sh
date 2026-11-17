#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
export SDL_AUDIO_DEVICE_SAMPLE_FRAMES="${SDL_AUDIO_DEVICE_SAMPLE_FRAMES:-2048}"
exec out/mac/nier --mnk_mode=true "$@"
