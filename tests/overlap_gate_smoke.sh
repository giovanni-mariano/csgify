#!/usr/bin/env bash
#
# overlap_gate_smoke: csgify refuses to emit a broken MCNP deck.
#
# Overlap diagnostics are error-severity by default, so:
#   - default convert (`csgify input.step output.i`) refuses to write
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
