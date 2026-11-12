#!/bin/bash
# spv_fmz on hand-written modules: every 32-bit float OpFMul (scalar, vector) and GLSL.std.450 Fma gets "an operand is 0 -> 0 (Fma: c)" written out, a module with bool
# and zero constants of its own and one without both come out valid SPIR-V (spirv-val), and a module with no float multiply is left alone. Needs the NDK's shader tools:
# NDK=<ndk> bash t_fmz.sh   (on Windows through WSL: TOOLS=<ndk>/shader-tools/windows-x86_64 EXE=.exe TMPBASE=<a folder under /mnt/c>)
set -u
T=${TOOLS:-${NDK:?set NDK to the Android NDK}/shader-tools/linux-x86_64}; X=${EXE:-}; D=$(mktemp -d -p "${TMPBASE:-/tmp}"); cd "$(dirname "$0")"
w() { if [ -n "$X" ]; then wslpath -w "$1"; else echo "$1"; fi; }   # (the Windows tools from WSL: TMPBASE under /mnt/c, paths as Windows paths)
cc -O1 -Wall -Wextra -Wno-unused-parameter -I../nvn -o "$D/t" t_fmz.c ../nvn/nvn_spv.c || exit 1
checks=0; fails=0
check() { checks=$((checks + 1)); if eval "$1"; then echo "✓ $2"; else echo "✗ $2"; fails=$((fails + 1)); fi; }
mod() {   # $1: extra types, $2: body ops (the inputs are %a %b %c, floats; %va %vb %vc, vec4s)
cat <<EOF
; SPIR-V
; Version: 1.0
OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %in %vin %out %vout
OpExecutionMode %main OriginUpperLeft
OpDecorate %in Location 0
OpDecorate %vin Location 1
OpDecorate %out Location 0
OpDecorate %vout Location 1
%void = OpTypeVoid
%fnty = OpTypeFunction %void
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%int = OpTypeInt 32 1
$1
%pin = OpTypePointer Input %float
%pvin = OpTypePointer Input %v4float
%pout = OpTypePointer Output %float
%pvout = OpTypePointer Output %v4float
%in = OpVariable %pin Input
%vin = OpVariable %pvin Input
%out = OpVariable %pout Output
%vout = OpVariable %pvout Output
%main = OpFunction %void None %fnty
%l = OpLabel
%a = OpLoad %float %in
%b = OpFAdd %float %a %a
%c = OpFSub %float %a %b
%va = OpLoad %v4float %vin
%vb = OpFAdd %v4float %va %va
%vc = OpFSub %v4float %va %vb
$2
OpReturn
OpFunctionEnd
EOF
}
run() {   # $1 name, $2 types, $3 body; leaves $D/$1.out.spv
  mod "$2" "$3" > "$D/$1.spvasm"; "$T/spirv-as$X" --target-env vulkan1.0 "$(w "$D/$1.spvasm")" -o "$(w "$D/$1.spv")" || { echo "✗ $1: the test module does not assemble"; fails=$((fails + 1)); return 1; }
  "$D/t" "$D/$1.spv" "$D/$1.out.spv"; echo $? > "$D/$1.rc"
}
dis() { "$T/spirv-dis$X" --raw-id "$(w "$D/$1.out.spv")" 2>/dev/null | tr -d '\r'; }   # (the Windows tools end lines with CR)

run scalar "" '%m = OpFMul %float %a %b
OpStore %out %m'
check '[ "$(cat $D/scalar.rc)" = 0 ]' "a scalar FMul is patched"
check '"$T/spirv-val$X" --target-env vulkan1.0 "$(w "$D/scalar.out.spv")"' "the patched scalar module is valid"
T_GUARD=1 "$D/t" "$D/scalar.spv" "$D/scalar.g.spv"; rc=$?
check '[ $rc = 0 ] && cmp -s "$D/scalar.out.spv" "$D/scalar.g.spv"' "a module ending right before an unmapped page: nothing read past its end (Galar #172 #181)"
check '[ "$(dis scalar | grep -c OpFOrdEqual)" = 2 ] && [ "$(dis scalar | grep -c OpLogicalOr)" = 1 ] && [ "$(dis scalar | grep -c OpSelect)" = 1 ]' "two compares, an or and a select for one multiply"
check 'dis scalar | grep -q "OpTypeBool" && dis scalar | grep -q "OpConstant %[0-9]* 0$"' "a bool type and a float zero added (the module had neither)"

run vector "" '%m = OpFMul %v4float %va %vb
OpStore %vout %m'
check '[ "$(cat $D/vector.rc)" = 0 ] && "$T/spirv-val$X" --target-env vulkan1.0 "$(w "$D/vector.out.spv")"' "a vec4 FMul is patched and valid"
check 'dis vector | grep -q "OpConstantComposite" && dis vector | grep -q "OpTypeVector %[0-9]* 4"' "a vec4 zero and a bvec4 added"

run fma "" '%f = OpExtInst %float %glsl Fma %a %b %c
%vf = OpExtInst %v4float %glsl Fma %va %vb %vc
OpStore %out %f
OpStore %vout %vf'
check '[ "$(cat $D/fma.rc)" = 0 ] && "$T/spirv-val$X" --target-env vulkan1.0 "$(w "$D/fma.out.spv")"' "scalar and vec4 Fma are patched and valid"
check '[ "$(dis fma | grep -c OpSelect)" = 2 ]' "one select each"

run has '%bool = OpTypeBool
%v4bool = OpTypeVector %bool 4
%zero = OpConstant %float 0' '%m = OpFMul %float %a %b
%vm = OpFMul %v4float %va %vb
%t = OpFMul %float %m %zero
OpStore %out %t
OpStore %vout %vm'
check '[ "$(cat $D/has.rc)" = 0 ] && "$T/spirv-val$X" --target-env vulkan1.0 "$(w "$D/has.out.spv")"' "a module with bool types and a zero of its own: patched and valid"
check '[ "$(dis has | grep -c "OpTypeBool")" = 1 ]' "its bool type is reused, not added twice"

mod "%one = OpConstant %int 1" '%s = OpFAdd %float %a %b
%i = OpIMul %int %one %one
OpStore %out %s' > "$D/none.spvasm"; "$T/spirv-as$X" --target-env vulkan1.0 "$(w "$D/none.spvasm")" -o "$(w "$D/none.spv")"; "$D/t" "$D/none.spv" "$D/none.out.spv"; rc=$?
check '[ $rc = 2 ]' "a module with no float multiply is left alone (an integer multiply is not one)"

echo "$checks checks, $fails failed"; rm -rf "$D"; [ $fails = 0 ]
