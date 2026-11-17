#!/bin/sh
f=$1
b=$(basename "$f" .hlsl)
case $b in ps_*) t=ps_6_6; x="";; *) t=vs_6_6; x=-fvk-invert-y;; esac
if "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
     -Werror=parameter-usage $x -Fo "$SPIRV_OUT/$b.spv" "$f" >/dev/null 2>"$SPIRV_OUT/$b.err" &&
   "$VAL" --target-env vulkan1.2 --scalar-block-layout "$SPIRV_OUT/$b.spv" 2>>"$SPIRV_OUT/$b.err"; then
  rm -f "$SPIRV_OUT/$b.err"
else
  rm -f "$SPIRV_OUT/$b.spv"
fi
