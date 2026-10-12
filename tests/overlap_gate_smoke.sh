#!/usr/bin/env bash
#
# overlap_gate_smoke: csgify refuses to emit a broken MCNP deck.
#
# Overlap diagnostics are error-severity by default, so:
#   - default convert (`csgify input.step output.i`) refuses to write
#     the .i file when overlaps are present
#   - preprocess --safe-all refuses to proceed
#   - --allow-overlaps downgrades inspection/intermediate diagnostics only;
#     production MCNP emission remains blocked
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

# A failed attempt must not clobber an earlier valid deck or leave a temporary
# deck that could be mistaken for usable output.
printf '%s\n' 'previous-valid-deck' >"$outdir/preserved.i"
"$exe" "$bad" "$outdir/preserved.i" >/dev/null 2>&1 || true
grep -qx 'previous-valid-deck' "$outdir/preserved.i" \
    || fail "failed emission clobbered an existing deck"
if compgen -G "$outdir/preserved.i.csgify-tmp-*" >/dev/null; then
    fail "failed emission left a temporary MCNP artifact"
fi

# ---- direct session emit refuses without caller-side analyze -------------
cat >"$outdir/direct_emit.lua" <<'LUA'
local s = csgify.session()
local doc = s:load_step(arg[1])
local ok, err = s:emit(doc, arg[2])
if ok then
    error("direct emit unexpectedly succeeded", 0)
end
local report = s:report_table()
local found = false
for _, d in ipairs(report.diagnostics) do
    if d.code == "solid_overlap" and d.severity == "error" then
        found = true
    end
end
if not found then
    error("direct emit did not report solid_overlap", 0)
end
LUA
rm -f "$outdir/direct_emit.i"
"$exe" lua "$outdir/direct_emit.lua" "$bad" "$outdir/direct_emit.i" \
    >/dev/null 2>"$outdir/direct_emit.err"
status=$?
[ "$status" -eq 0 ] \
    || fail "direct Lua emit overlap check failed"
[ ! -e "$outdir/direct_emit.i" ] \
    || fail "direct Lua emit wrote a .i file despite overlap"

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

# ---- --allow-overlaps remains available for inspection ------------------
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

# ---- bounded single-owner resolution -----------------------------------
# The overlap is exactly 1 mm^3. Each source box is 8 mm^3 and the document
# baseline is 16 mm^3, so 5% is too small while 20% permits the operation.
rm -f "$outdir/resolved.i" "$outdir/resolved.step" "$outdir/resolved.json" \
    "$outdir/resolved.log"
if "$exe" decompose-apply "$bad" --out "$outdir/too_small.i" \
    --approx-volume 5% --allow-shatter-fallback \
    >/dev/null 2>"$outdir/too_small.err"; then
    fail "overlap ownership exceeded its 5% body budget but was accepted"
fi
grep -q "approximation_action_budget_exceeded\|approximation_body_budget_exceeded" \
    "$outdir/too_small.err" \
    || fail "small-budget overlap rejection omitted its measured reason"

"$exe" decompose-apply "$bad" --out "$outdir/resolved.i" \
    --out-step "$outdir/resolved.step" --approx-volume 20% \
    --allow-shatter-fallback --max-actions 16 \
    --log "$outdir/resolved.log" \
    --report "$outdir/resolved.json" >/dev/null 2>"$outdir/resolved.err" \
    || fail "bounded overlap ownership/decomposition failed"
python3 - "$outdir/resolved.json" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
s = r["mcnp_geometry_safety"]
a = r["approximation"]
assert s["validated"] is True and s["safe_to_emit"] is True
assert s["overlap_count"] == 0 and s["overlap_unknown_count"] == 0
assert a["policy"]["allow_overlap_resolution"] is True
assert abs(a["ledger"]["cumulative_removed_volume"] - 1.0) < 1e-8
assert a["ledger"]["cumulative_added_volume"] == 0
assert sum(b["accepted_actions"] for b in a["ledger"]["bodies"]) == 1
PY
[ -s "$outdir/resolved.i" ] || fail "resolved overlap wrote no MCNP deck"
[ -s "$outdir/resolved.step" ] || fail "resolved overlap wrote no STEP"

# Ownership is causal: compaction must retain it before the decomposition,
# and replay must reproduce both the deck and terminal approximation ledger.
"$exe" compact "$outdir/resolved.log" --out "$outdir/resolved-recipe.lua" \
    >/dev/null || fail "could not compact overlap-ownership recipe"
grep -q 's:resolve_overlap(' "$outdir/resolved-recipe.lua" \
    || fail "compactor dropped the overlap-ownership decision"
"$exe" replay "$outdir/resolved-recipe.json" --step "$bad" \
    --out "$outdir/resolved-replay.i" \
    --report "$outdir/resolved-replay.json" >/dev/null \
    || fail "overlap-ownership recipe did not replay"
cmp "$outdir/resolved.i" "$outdir/resolved-replay.i" \
    || fail "overlap-ownership replay deck differs"
grep -q '"postconditions": {"checked":true,"passed":true}' \
    "$outdir/resolved-replay.json" \
    || fail "overlap-ownership ledger postcondition failed"

echo "overlap_gate_smoke: OK"
exit 0
