#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
GLSLANG=${GLSLANG:-$HOME/VulkanSDK/1.4.363.1/macOS/bin/glslangValidator}
for source in "$ROOT"/app/src/native/shaders/*.comp; do
  name=$(basename "$source" .comp)
  "$GLSLANG" -V --target-env vulkan1.1 --vn "${name}_cs" -o "$ROOT/app/src/native/shaders/${name}_cs.h" "$source"
done
