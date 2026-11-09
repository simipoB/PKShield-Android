#!/bin/bash
# spv_clamp_zero (Arm erratum 3779215) on hand-written modules: the ops of the erratum's shape get a zero the compiler cannot fold, nothing else changes, and every module
# that comes out is valid SPIR-V (spirv-val). Needs the NDK's shader tools: NDK=<ndk> bash t_clamp_zero.sh
set -u
T=${NDK:?set NDK to the Android NDK}/shader-tools/linux-x86_64; D=$(mktemp -d); cd "$(dirname "$0")"
cc -O1 -Wall -Wextra -Wno-unused-parameter -I../nvn -o "$D/t" t_clamp_zero.c ../nvn/nvn_spv.c || exit 1
checks=0; fails=0
check() { checks=$((checks + 1)); if eval "$1"; then echo "✓ $2"; else echo "✗ $2"; fails=$((fails + 1)); fi; }

body() {   # $1: the SPIR-V version, $2: the entry point's interface
cat <<EOF
; SPIR-V
; Version: $1
OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" $2
OpExecutionMode %main OriginUpperLeft
OpDecorate %in Location 0
OpDecorate %out Location 0
OpDecorate %outi Location 1
%void = OpTypeVoid
%fnty = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2uint = OpTypeVector %uint 2
%bool = OpTypeBool
%v2bool = OpTypeVector %bool 2
%ufnty = OpTypeFunction %v2uint %float
%ptr_in_f = OpTypePointer Input %float
%ptr_out_f = OpTypePointer Output %float
%ptr_out_u2 = OpTypePointer Output %v2uint
%in = OpVariable %ptr_in_f Input
%out = OpVariable %ptr_out_f Output
%outi = OpVariable %ptr_out_u2 Output
%float_0 = OpConstant %float 0
%float_1 = OpConstant %float 1
%float_m2 = OpConstant %float -2
%float_3 = OpConstant %float 3
%uint_5 = OpConstant %uint 5
%uint_7 = OpConstant %uint 7
%u2_57 = OpConstantComposite %v2uint %uint_5 %uint_7
%u2_75 = OpConstantComposite %v2uint %uint_7 %uint_5
%u2_0 = OpConstantNull %v2uint
%helper = OpFunction %v2uint None %ufnty
%hp = OpFunctionParameter %float
%hl = OpLabel
%hc = OpFOrdLessThan %bool %hp %float_1
%hc2 = OpCompositeConstruct %v2bool %hc %hc
%hs = OpSelect %v2uint %hc2 %u2_57 %u2_75
%hr = OpExtInst %v2uint %glsl UMin %hs %u2_0
OpReturnValue %hr
OpFunctionEnd
%main = OpFunction %void None %fnty
%ml = OpLabel
%v = OpLoad %float %in
%c = OpFOrdGreaterThan %bool %v %float_0
%s = OpSelect %float %c %float_m2 %float_3
%r1 = OpExtInst %float %glsl FClamp %s %float_0 %float_1
%r2 = OpExtInst %float %glsl FMax %float_0 %s
%r3 = OpExtInst %float %glsl FMax %v %float_0
%a1 = OpFAdd %float %r1 %r2
%a2 = OpFAdd %float %a1 %r3
OpStore %out %a2
%u = OpFunctionCall %v2uint %helper %v
OpStore %outi %u
OpReturn
OpFunctionEnd
EOF
}

for ver in 1.0 1.5; do
  env=vulkan1.0; [ $ver = 1.5 ] && env=vulkan1.2
  body $ver "%in %out %outi" > "$D/m$ver.spvasm"
  "$T/spirv-as" --target-env $env "$D/m$ver.spvasm" -o "$D/m$ver.spv" || { echo "✗ assembling the $ver module"; exit 1; }
  check '"$T/spirv-val" --target-env $env "$D/m$ver.spv"' "SPIR-V $ver: the test module itself is valid"
  out=$("$D/t" "$D/m$ver.spv" "$D/o$ver.spv"); check '[ "${out%% *}" = patched ]' "SPIR-V $ver: the pass changes it"
  check '"$T/spirv-val" --target-env $env "$D/o$ver.spv"' "SPIR-V $ver: what comes out is valid SPIR-V"
  "$T/spirv-dis" --raw-id "$D/o$ver.spv" > "$D/o$ver.txt"; "$T/spirv-dis" --raw-id "$D/m$ver.spv" > "$D/m$ver.txt"
  Z=$(grep -E '= OpConstant %[0-9]+ 0$' "$D/m$ver.txt" | head -1 | awk '{print $1}')   # %float_0's raw id (the disassembly is indented)
  ZU=$(grep -E 'OpConstantNull' "$D/m$ver.txt" | awk '{print $1}')
  CL=$(grep 'FClamp' "$D/o$ver.txt"); MX=$(grep 'FMax' "$D/o$ver.txt"); UM=$(grep 'UMin' "$D/o$ver.txt")
  check '! echo "$CL" | grep -q -- " $Z "' "SPIR-V $ver: clamp(select of constants, 0, 1): its 0 is no longer the constant"
  check '[ "$(echo "$MX" | grep -cw -- "$Z")" = 1 ] && [ "$(echo "$MX" | grep -c -- " $Z$")" = 1 ]' "SPIR-V $ver: max(0, select of constants) changed; max(input, 0) left as it was"
  check '! echo "$UM" | grep -q -- " $ZU$"' "SPIR-V $ver: umin(select of vector constants, null) in a function the entry point calls: changed there"
  check '[ "$(grep -c "OpVariable %[0-9]* Private" "$D/o$ver.txt")" = 2 ]' "SPIR-V $ver: one Private zero per type (float, uvec2)"
  check 'grep -q "OpStore %[0-9]* $Z$" "$D/o$ver.txt" && grep -q "OpStore %[0-9]* $ZU$" "$D/o$ver.txt"' "SPIR-V $ver: the entry point stores both zeros"
  check '[ "$(grep -c "= OpLoad" "$D/o$ver.txt")" = 3 ]' "SPIR-V $ver: loads: the input, the float zero in main, the uvec2 zero in the helper"
  if [ $ver = 1.5 ]; then
    check '[ "$(grep OpEntryPoint "$D/o$ver.txt" | wc -w)" = 9 ]' "SPIR-V 1.5: the entry point lists the two Private variables too"
  fi
done

# nothing of the erratum's shape: nothing changed
body 1.0 "%in %out %outi" | sed -e 's/^%r1 = .*/%r1 = OpExtInst %float %glsl FClamp %v %float_0 %float_1/' -e 's/^%r2 = .*/%r2 = OpExtInst %float %glsl FMax %float_0 %v/' \
    -e 's/^%hr = .*/%hr = OpExtInst %v2uint %glsl UMin %u2_57 %u2_75/' > "$D/n.spvasm"
"$T/spirv-as" --target-env vulkan1.0 "$D/n.spvasm" -o "$D/n.spv" || exit 1
check '[ "$("$D/t" "$D/n.spv" "$D/no.spv")" = none ]' "no op of the erratum's shape (clamps of inputs, a min of two constants): nothing to change"

rm -rf "$D"
echo "$checks checks, $fails failed"; [ $fails = 0 ]
