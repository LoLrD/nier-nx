#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="$(cd "$ROOT/assets" && pwd -P)"
IMAGE="${DEVKITA64_IMAGE:-nier-nx-build}"

ensure_image() {
  command -v docker >/dev/null 2>&1 || { echo "error: docker not found in PATH" >&2; exit 1; }
  docker info >/dev/null 2>&1 || { echo "error: the Docker daemon is not running (start OrbStack)" >&2; exit 1; }
  docker image inspect "$IMAGE" >/dev/null 2>&1 && return
  echo "== building the Docker image $IMAGE (devkitpro/devkita64 + glslang-tools), once" >&2
  docker build -t "$IMAGE" - <<'DOCKERFILE'
FROM devkitpro/devkita64:20260219
RUN apt-get update && apt-get install -y --no-install-recommends glslang-tools && rm -rf /var/lib/apt/lists/*
DOCKERFILE
}

in_container() {
  ensure_image
  docker run --rm -v "$ROOT:$ROOT" -v "$DATA:$DATA" -w "$ROOT" "$IMAGE" "$@"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  "$@"
fi
