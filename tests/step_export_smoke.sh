#!/usr/bin/env bash
#
# Smoke test for `cad2csg simplify ... --out-step ...` (pass 1). Runs a
# bbox simplification on box.step, asserts the resulting .step file
# loads back through cad2csg and contains the expected solid.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "step_export_smoke: $*" >&2
    exit 1
}

step_in="$fixtures/box.step"
mcnp_out="$outdir/simplified.i"
step_out="$outdir/simplified.step"

rm -f "$mcnp_out" "$step_out"

"$exe" simplify "$step_in" --replace-solid-bbox solid:1 \
    --out "$mcnp_out" --out-step "$step_out" >/dev/null 2>&1 \
    || fail "simplify with --out-step returned non-zero"

[[ -s "$step_out" ]] || fail "simplified STEP file was not produced"

# Re-import the simplified STEP via dump-assembly and assert exactly one
# solid (the bbox replacement) survives.
solids=$("$exe" dump-assembly "$step_out" 2>/dev/null \
    | grep -c "^solid:")
[[ "$solids" -eq 1 ]] \
    || fail "expected 1 solid in simplified STEP, got $solids"

assembly_dump="$outdir/simplified_assembly.txt"
"$exe" dump-assembly "$step_out" >"$assembly_dump" 2>/dev/null \
    || fail "could not re-import simplified STEP assembly"
grep -q "replacement:bbox" "$assembly_dump" \
    || fail "simplified STEP did not preserve replacement provenance name"

# Volume must agree with the original within a generous tolerance (bbox
# of a box is the box).
"$exe" volume "$step_in" --compare "$step_out" --rel-tol 0.01 >/dev/null 2>&1 \
    || fail "simplified STEP volume does not match original within rel-tol 0.01"

# Verify removed-solid path: when the only solid is removed (no
# replacement), export must fail loudly rather than silently producing
# an empty STEP.
nothing_out="$outdir/nothing.step"
rm -f "$nothing_out"
if "$exe" simplify "$step_in" --remove-solid solid:1 \
       --out "$outdir/nothing.i" --out-step "$nothing_out" >/dev/null 2>&1; then
    fail "export should fail when every solid is removed (got success)"
fi
if [[ -s "$nothing_out" ]]; then
    fail "no STEP file should be written when nothing survives"
fi

# Decomposition pieces should carry source-name provenance into the
# review STEP so users can identify them after opening the file in CAD.
decomp_i="$outdir/l_shape_decomposed.i"
decomp_step="$outdir/l_shape_decomposed.step"
"$exe" decompose-apply "$fixtures/l_shape.step" \
    --out "$decomp_i" --target solid:1 --max-targets 1 \
    --out-step "$decomp_step" >/dev/null 2>&1 \
    || fail "decompose-apply with --out-step returned non-zero"
decomp_dump="$outdir/l_shape_decomposed_assembly.txt"
"$exe" dump-assembly "$decomp_step" >"$decomp_dump" 2>/dev/null \
    || fail "could not re-import decomposed STEP assembly"
grep -q "piece:1" "$decomp_dump" \
    || fail "decomposed STEP did not preserve piece:1 name"
grep -q "piece:2" "$decomp_dump" \
    || fail "decomposed STEP did not preserve piece:2 name"

echo "step_export_smoke: ok"
