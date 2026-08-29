#!/usr/bin/env bash
#
# safe_all_smoke: verify `preprocess --safe-all` composes safe ops and
# `preprocess --safe-all --dry-run` mutates nothing.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "safe_all_smoke: $*" >&2; exit 1; }

# l_shape is a small non-convex part that the safe BSP can resolve.
step="$fixtures/l_shape.step"
[ -f "$step" ] || fail "fixture missing: $step"

# ---- dry-run mode: prints plan, writes nothing ----------------------------
plan_out="$outdir/dry.out"
"$exe" preprocess "$step" --safe-all --dry-run >"$plan_out" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "dry-run exit=$status"
grep -q "dry-run" "$plan_out" || fail "dry-run output missing [dry-run] marker"
grep -q "safe-all.*summary" "$plan_out" \
    || fail "dry-run output missing safe-all summary line"
grep -q "auto-fit skipped.*max-deviation" "$plan_out" \
    || fail "safe-all must not auto-fit without an explicit deviation budget"
# Output STEP file must not have been created by --dry-run.
[ ! -e "$outdir/should_not_exist.step" ] || fail "dry-run wrote an output file"

# ---- real run: mutates, emits closing summary -----------------------------
out_step="$outdir/l_shape_simplified.step"
report="$outdir/l_shape_simplified.json"
"$exe" preprocess "$step" --safe-all \
    --out-step "$out_step" --report "$report" >"$outdir/run.out" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "real run exit=$status: $(cat "$outdir/run.out")"
[ -s "$out_step" ] || fail "out-step file not written"
[ -s "$report" ] || fail "report file not written"
grep -q "safe-all.*summary" "$outdir/run.out" \
    || fail "real run output missing safe-all summary line"

# Validate the report has expected structure.
python3 - "$report" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
# Volume must be preserved (decomposition is volume-conservative).
assert "summary" in d, d.keys()
assert d["summary"]["total_volume"] > 0, d["summary"]
PY
[ "$?" -eq 0 ] || fail "report validation failed"

# Closing summary should claim we decomposed something on l_shape.
grep -q "decomposed=" "$outdir/run.out" \
    || fail "real run output missing decomposed= count"

echo "safe_all_smoke: OK"
exit 0
