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

echo "step_export_smoke: ok"
