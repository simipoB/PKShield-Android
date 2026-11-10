#!/bin/bash
# spv_nan_minmax (and spv_nan_select, the same rule written out with isnan) on hand-written modules: GLSL.std.450 FMin / FMax / FClamp become NMin / NMax / NClamp (scalars and vectors), the integer ones and the ops of another
# instruction set stay, a module without them is left alone, and every module that comes out is valid SPIR-V (spirv-val). Needs the NDK's shader tools:
# NDK=<ndk> bash t_nan_minmax.sh
set -u
T=${NDK:?set NDK to the Android NDK}/shader-tools/linux-x86_64; D=$(mktemp -d); cd "$(dirname "$0")"
cc -O1 -Wall -Wextra -Wno-unused-parameter -I../nvn -o "$D/t" t_nan_minmax.c ../nvn/nvn_spv.c || exit 1
checks=0; fails=0
check() { checks=$((checks + 1)); if eval "$1"; then echo "✓ $2"; else echo "✗ $2"; fails=$((fails + 1)); fi; }

head_() {   # the module up to its function; $1: extra imports, $2: extensions
cat <<EOF
; SPIR-V
; Version: 1.0
OpCapability Shader
${2:-}
%glsl = OpExtInstImport "GLSL.std.450"
$1
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %in %out
OpExecutionMode %main OriginUpperLeft
OpDecorate %in Location 0
OpDecorate %out Location 0
%void = OpTypeVoid
%fnty = OpTypeFunction %void
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%ptr_in = OpTypePointer Input %v4float
%ptr_out = OpTypePointer Output %v4float
%in = OpVariable %ptr_in Input
%out = OpVariable %ptr_out Output
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%i3 = OpConstant %int 3
%u5 = OpConstant %uint 5
%v2_0 = OpConstantComposite %v2float %f0 %f0
%v2_1 = OpConstantComposite %v2float %f1 %f1
EOF
}

# 1: every float min / max / clamp, scalar and vector, next to the integer ones
{ head_ ""; cat <<'EOF'
%main = OpFunction %void None %fnty
%l = OpLabel
%v = OpLoad %v4float %in
%x = OpCompositeExtract %float %v 0
%y = OpCompositeExtract %float %v 1
%xy = OpVectorShuffle %v2float %v %v 0 1
%a = OpExtInst %float %glsl FMin %x %y
%b = OpExtInst %float %glsl FMax %a %f0
%c = OpExtInst %float %glsl FClamp %b %f0 %f1
%d = OpExtInst %v2float %glsl FClamp %xy %v2_0 %v2_1
%e = OpExtInst %v2float %glsl FMin %d %v2_1
%si = OpExtInst %int %glsl SMin %i3 %i3
%ui = OpExtInst %uint %glsl UMax %u5 %u5
%sc = OpExtInst %int %glsl SClamp %i3 %i3 %i3
%f = OpExtInst %float %glsl Sqrt %c
%e0 = OpCompositeExtract %float %e 0
%r = OpCompositeConstruct %v4float %f %e0 %c %a
OpStore %out %r
OpReturn
OpFunctionEnd
EOF
} > "$D/all.spvasm"
# 2: none of them: left alone
{ head_ ""; cat <<'EOF'
%main = OpFunction %void None %fnty
%l = OpLabel
%v = OpLoad %v4float %in
%x = OpCompositeExtract %float %v 0
%f = OpExtInst %float %glsl Sqrt %x
%si = OpExtInst %int %glsl SMin %i3 %i3
%r = OpCompositeConstruct %v4float %f %f %f %f
OpStore %out %r
OpReturn
OpFunctionEnd
EOF
} > "$D/none.spvasm"
# 3: a second instruction set whose op numbers 37 / 40 / 43 are not GLSL's: only the GLSL.std.450 ones change
{ head_ '%dbg = OpExtInstImport "NonSemantic.Test"' 'OpExtension "SPV_KHR_non_semantic_info"'; cat <<'EOF'
%main = OpFunction %void None %fnty
%l = OpLabel
%v = OpLoad %v4float %in
%x = OpCompositeExtract %float %v 0
%n1 = OpExtInst %void %dbg 37 %x
%n2 = OpExtInst %void %dbg 40 %x
%n3 = OpExtInst %void %dbg 43 %x
%a = OpExtInst %float %glsl FMax %x %f0
%r = OpCompositeConstruct %v4float %a %a %a %a
OpStore %out %r
OpReturn
OpFunctionEnd
EOF
} > "$D/other.spvasm"

for m in all none other; do
    "$T/spirv-as" --target-env spv1.0 "$D/$m.spvasm" -o "$D/$m.spv"
done

out=$("$D/t" "$D/all.spv" "$D/all.out.spv"); "$T/spirv-dis" "$D/all.out.spv" > "$D/all.dis"
check '[ "${out#patched}" != "$out" ]' "every float min / max / clamp: patched ($out)"
check '"$T/spirv-val" "$D/all.out.spv" && "$T/spirv-val" "$D/all.spv"' "the input and the patched module validate"
check '[ "$(grep -c " NMin " "$D/all.dis")" = 2 ] && [ "$(grep -c " NMax " "$D/all.dis")" = 1 ] && [ "$(grep -c " NClamp " "$D/all.dis")" = 2 ]' "two NMin, one NMax, two NClamp (scalar and vector)"
check '! grep -qE " FMin | FMax | FClamp " "$D/all.dis"' "no FMin / FMax / FClamp left"
check 'grep -q " SMin " "$D/all.dis" && grep -q " UMax " "$D/all.dis" && grep -q " SClamp " "$D/all.dis" && grep -q " Sqrt " "$D/all.dis"' "the integer ones and Sqrt stay"
check '[ "$(stat -c %s "$D/all.spv")" = "$(stat -c %s "$D/all.out.spv")" ]' "same length"
check '[ "$("$D/t" "$D/none.spv" "$D/none.out.spv")" = none ]' "a module without them: none"
if [ -f "$D/other.spv" ]; then
    out=$("$D/t" "$D/other.spv" "$D/other.out.spv"); "$T/spirv-dis" --raw-id "$D/other.out.spv" > "$D/other.dis"
    check '"$T/spirv-val" "$D/other.spv" && "$T/spirv-val" "$D/other.out.spv"' "with another instruction set: the input and the patched module validate"
    check 'grep -q " NMax " "$D/other.dis" && [ "$(grep -c "%dbg 37\|%dbg 40\|%dbg 43\|%[0-9]* 37 \|%[0-9]* 40 \|%[0-9]* 43 " "$D/other.dis")" -ge 3 ]' "another instruction set's ops 37 / 40 / 43 stay, GLSL's FMax changes"
fi
# spv_nan_select: the rule written out. On the plain module, and on the one spv_nan_minmax made (the engine's order)
for src in all all.out; do
    out=$("$D/t" "$D/$src.spv" "$D/$src.sel.spv" select); "$T/spirv-dis" "$D/$src.sel.spv" > "$D/sel.dis"
    check '[ "${out#patched}" != "$out" ] && "$T/spirv-val" "$D/$src.sel.spv"' "written out ($src): patched and valid ($out)"
    check '[ "$(grep -c "OpIsNan" "$D/sel.dis")" = 8 ] && [ "$(grep -c "OpSelect" "$D/sel.dis")" = 8 ]' "written out ($src): 2 isnan + 2 selects per min / max, 1 + 1 per clamp"
    check '[ "$(grep -c "OpTypeBool" "$D/sel.dis")" = 1 ] && [ "$(grep -c "OpTypeVector %bool 2" "$D/sel.dis")" = 1 ]' "written out ($src): one bool and one bvec2 type added"
    check 'grep -q "OpSelect %float %[0-9a-z_]* %float_0 %" "$D/sel.dis"' "written out ($src): a NaN into the scalar clamp gives its lower limit"
    check 'grep -q " SMin " "$D/sel.dis" && grep -q " UMax " "$D/sel.dis" && grep -q " SClamp " "$D/sel.dis"' "written out ($src): the integer ones stay"
done
check '[ "$("$D/t" "$D/none.spv" "$D/none.sel.spv" select)" = none ]' "written out: a module without them: none"
out=$("$D/t" "$D/all.sel.spv" "$D/twice.spv" select)
check '"$T/spirv-val" "$D/twice.spv"' "written out twice: still valid ($out)"
# real translated shaders, when given: SHADERS=<folder of .spv>: each one through both passes validates as before
if [ -n "${SHADERS:-}" ]; then
    n=0; bad=0
    for f in "$SHADERS"/*.spv; do
        "$T/spirv-val" "$f" 2>/dev/null || continue
        cp "$f" "$D/g.spv"; r=$("$D/t" "$D/g.spv" "$D/g1.spv"); [ "$r" = none ] || cp "$D/g1.spv" "$D/g.spv"
        r=$("$D/t" "$D/g.spv" "$D/g2.spv" select); [ "$r" = none ] && continue
        n=$((n + 1)); "$T/spirv-val" "$D/g2.spv" || { bad=$((bad + 1)); echo "  invalid after the passes: $f"; }
    done
    check '[ "$bad" = 0 ] && [ "$n" -gt 0 ]' "game shaders in $SHADERS: $n patched, $bad invalid"
fi
rm -rf "$D"
echo "$checks checks, $fails failed"; [ "$fails" = 0 ]
