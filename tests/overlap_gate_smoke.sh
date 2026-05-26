#!/usr/bin/env bash
#
# overlap_gate_smoke: cad2csg refuses to emit a broken MCNP deck.
#
# Overlap diagnostics are error-severity by default, so:
#   - default convert (`cad2csg input.step output.i`) refuses to write
#     the .i file when overlaps are present
#   - preprocess --safe-all refuses to proceed
#   - --allow-overlaps downgrades the diagnostic to warning and lets
#     emit proceed (escape hatch for advanced workflows)
#
# Each branch is checked here so the gate cannot silently regress.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "overlap_gate_smoke: $*" >&2; exit 1; }

bad="$fixtures/overlapping_boxes.step"
[ -f "$bad" ] || fail "fixture missing: $bad"

# ---- default convert refuses --------------------------------------------
rm -f "$outdir/refused.i"
"$exe" "$bad" "$outdir/refused.i" >/dev/null 2>"$outdir/refused.err"
status=$?
[ "$status" -ne 0 ] \
    || fail "default convert should refuse overlapping input (got exit 0)"
[ ! -e "$outdir/refused.i" ] \
    || fail "default convert wrote a .i file despite overlap"

# ---- preprocess --safe-all aborts ---------------------------------------
rm -f "$outdir/safe_all.step"
"$exe" preprocess "$bad" --safe-all --out-step "$outdir/safe_all.step" \
    >/dev/null 2>"$outdir/safe_all.err"
status=$?
[ "$status" -ne 0 ] \
    || fail "preprocess --safe-all should abort on overlapping input"
grep -q "preprocess_overlaps_present" "$outdir/safe_all.err" \
    || fail "expected preprocess_overlaps_present in safe-all stderr"
[ ! -e "$outdir/safe_all.step" ] \
    || fail "preprocess --safe-all wrote --out-step despite abort"

# ---- --allow-overlaps lets it through -----------------------------------
"$exe" inspect "$bad" --allow-overlaps --report "$outdir/allowed.json" \
    >/dev/null 2>&1
status=$?
[ "$status" -eq 0 ] \
    || fail "inspect --allow-overlaps should exit 0 (got $status)"

python3 - "$outdir/allowed.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# Overlap is still reported, just at warning severity.
diags = d["diagnostics"]
sev = [(diag["severity"], diag["code"]) for diag in diags]
assert ("warning", "solid_overlap") in sev, sev
assert ("error", "solid_overlap") not in sev, sev
PY
[ "$?" -eq 0 ] || fail "--allow-overlaps validation failed"

# ---- preprocess --safe-all --allow-overlaps proceeds --------------------
# overlapping_boxes has two convex boxes, so the BSP/auto-fit/removal
# passes have nothing to do. We just check that --allow-overlaps lets
# the precheck pass and the command exits zero.
rm -f "$outdir/allow_safe_all.step"
"$exe" preprocess "$bad" --safe-all --allow-overlaps \
    --out-step "$outdir/allow_safe_all.step" >/dev/null 2>&1 || true
[ -s "$outdir/allow_safe_all.step" ] \
    || fail "preprocess --safe-all --allow-overlaps did not write --out-step"

echo "overlap_gate_smoke: OK"
exit 0
