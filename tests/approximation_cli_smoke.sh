#!/usr/bin/env bash
set -u

exe="$1"
fixtures="$2"
outdir="$3"
mkdir -p "$outdir"
fail() { echo "approximation_cli_smoke: $*" >&2; exit 1; }
rm -f "$outdir/strict.i" "$outdir/accepted.i" "$outdir/accepted.json" \
  "$outdir/accepted-report.json" "$outdir/shatter.i" "$outdir/shatter.json"

set +e
"$exe" simplify "$fixtures/l_shape.step" --replace-solid-bbox solid:1 \
  --out "$outdir/strict.i" --approx-volume 1% \
  --allow-containment-change >/dev/null 2>"$outdir/strict.err"
strict_rc=$?
set -e
[[ "$strict_rc" == "1" ]] || fail "1% bbox should be rejected, got $strict_rc"
[[ ! -e "$outdir/strict.i" ]] || fail "rejected approximation wrote MCNP"
grep -q "approximation_action_budget_exceeded" "$outdir/strict.err" \
  || fail "rejected approximation omitted its budget reason"

"$exe" simplify "$fixtures/l_shape.step" --replace-solid-bbox solid:1 \
  --out "$outdir/accepted.i" --approx-volume 40% \
  --allow-containment-change --log "$outdir/accepted.json" \
  --report "$outdir/accepted-report.json" \
  >/dev/null 2>"$outdir/accepted.err" \
  || fail "40% explicitly permitted bbox replacement failed"
[[ -s "$outdir/accepted.i" ]] || fail "accepted approximation wrote no MCNP"
grep -q '"op":"apply_budgeted_replacement_operation"' \
  "$outdir/accepted.json" || fail "budgeted operation was not audited"
grep -q '"changed_volume":' "$outdir/accepted.json" \
  || fail "audit omitted changed-volume evidence"
python3 - "$outdir/accepted-report.json" <<'PY' \
  || fail "report omitted approximation policy/ledger"
import json, sys
r = json.load(open(sys.argv[1]))
a = r["approximation"]
assert a["enabled"] is True
assert abs(a["policy"]["max_global_changed_volume_fraction"] - 0.4) < 1e-12
assert abs(a["ledger"]["cumulative_changed_volume"] - 250.0) < 1e-3
assert a["ledger"]["bodies"][0]["accepted_actions"] == 1
PY

set +e
"$exe" simplify "$fixtures/l_shape.step" --replace-solid-bbox solid:1 \
  --out "$outdir/no_suffix.i" --approx-volume 1 \
  >/dev/null 2>"$outdir/no_suffix.err"
suffix_rc=$?
set -e
[[ "$suffix_rc" == "1" ]] || fail "ambiguous percent without suffix was accepted"
grep -q "explicit '%' suffix" "$outdir/no_suffix.err" \
  || fail "percent syntax error is unclear"

# Exact shatter remains preferred and consumes effectively zero of an available
# approximation budget.
"$exe" simplify "$fixtures/l_shape.step" --shatter solid:1 \
  --out "$outdir/shatter.i" --approx-volume 0.1% \
  --report "$outdir/shatter.json" >/dev/null 2>"$outdir/shatter.err" \
  || fail "exact shatter failed when an approximation budget was present"
python3 - "$outdir/shatter.json" <<'PY' \
  || fail "exact shatter incorrectly consumed approximation budget"
import json, sys
r = json.load(open(sys.argv[1]))
assert r["approximation"]["enabled"] is True
assert r["approximation"]["ledger"]["cumulative_changed_volume"] < 1e-6
PY

# Compaction/replay preserves the immutable policy and re-measures the
# symmetric difference. The terminal ledger is a replay postcondition.
rm -f "$outdir/recipe.lua" "$outdir/recipe.json" \
  "$outdir/replayed.i" "$outdir/replayed-report.json"
"$exe" compact "$outdir/accepted.json" --out "$outdir/recipe.lua" \
  || fail "could not compact approximation decision log"
"$exe" replay "$outdir/recipe.json" --step "$fixtures/l_shape.step" \
  --out "$outdir/replayed.i" --report "$outdir/replayed-report.json" \
  || fail "budgeted approximation recipe did not replay"
cmp "$outdir/accepted.i" "$outdir/replayed.i" \
  || fail "replayed MCNP differs from direct budgeted output"
grep -q '"approximation_ledger"' "$outdir/recipe.json" \
  || fail "recipe omitted approximation ledger postcondition"
grep -q '"postconditions": {"checked":true,"passed":true}' \
  "$outdir/replayed-report.json" \
  || fail "approximation ledger replay postcondition did not pass"

set +e
"$exe" simplify "$fixtures/l_shape.step" --replace-solid-bbox solid:1 \
  --out "$outdir/action-axis.i" --approx-volume 40% \
  --approx-action-volume 20% --allow-containment-change \
  >/dev/null 2>"$outdir/action-axis.err"
action_axis_rc=$?
set -e
[[ "$action_axis_rc" == "1" ]] \
  || fail "per-action override did not constrain the global budget"
grep -q "approximation_action_budget_exceeded" "$outdir/action-axis.err" \
  || fail "per-action rejection reason missing"

set +e
"$exe" simplify "$fixtures/l_shape.step" --replace-solid-bbox solid:1 \
  --out "$outdir/protected.i" --approx-volume 40% \
  --allow-containment-change --protect solid:1 \
  >/dev/null 2>"$outdir/protected.err"
protected_rc=$?
set -e
[[ "$protected_rc" == "1" ]] || fail "protected target was approximated"
grep -q "approximation_protected_geometry" "$outdir/protected.err" \
  || fail "protected-target rejection reason missing"

"$exe" simplify "$fixtures/l_shape.step" --shatter solid:1 \
  --out "$outdir/preset.i" --approx-preset moderate \
  --report "$outdir/preset.json" >/dev/null 2>"$outdir/preset.err" \
  || fail "moderate preset did not expand to an exact shatter budget"
grep -q '"max_global_changed_volume_fraction": 0.01' "$outdir/preset.json" \
  || fail "preset was not expanded in the report"

# Exact surface support substitutions can use the common gate immediately;
# non-exact partial-face changes fail closed until the complete owner can be
# materialized and measured as a closed solid.
"$exe" propose surface face:1.1 "$fixtures/spline_face.step" \
  --report "$outdir/surface-proposal.json" >/dev/null \
  || fail "spline surface proposal failed"
python3 - "$outdir/surface-proposal.json" <<'PY' \
  || fail "sampled spline fit was promoted to certified exact evidence"
import json, sys
p = json.load(open(sys.argv[1]))
best = next(c for c in p["candidates"] if c["id"] == p["best_candidate_id"])
assert best["reason"] == "sampled_analytic_fit"
assert best["deviation_certified"] is False
assert best["exact"] is False
PY

set +e
"$exe" simplify "$fixtures/spline_face.step" \
  --apply-surface-candidate face:1.1 --out "$outdir/surface-exact.i" \
  --candidate candidate:3 --approx-volume 0.1% \
  --max-deviation 0.000001 --log "$outdir/surface-exact.log" \
  --report "$outdir/surface-exact.json" >/dev/null 2>"$outdir/surface-exact.err"
surface_exact_rc=$?
set -e
[[ "$surface_exact_rc" == "1" ]] \
  || fail "single exact face unexpectedly made the whole fixture emit-ready"
grep -q '"op":"apply_budgeted_surface_replacement_candidate"' \
  "$outdir/surface-exact.log" \
  || fail "exact surface substitution did not use the common budget gate"
grep -q '"accepted_actions":1' "$outdir/surface-exact.json" \
  || fail "bounded surface substitution was not ledgered"

set +e
"$exe" simplify "$fixtures/spline_face.step" \
  --apply-surface-candidate face:1.1 --out "$outdir/surface-partial.i" \
  --candidate candidate:1 --approx-volume 100% \
  --max-deviation 3 --allow-containment-change --allow-topology-change \
  >/dev/null 2>"$outdir/surface-partial.err"
surface_partial_rc=$?
set -e
[[ "$surface_partial_rc" == "1" ]] \
  || fail "uncertified partial surface approximation was accepted"
grep -q "approximation_measurement_unknown" "$outdir/surface-partial.err" \
  || fail "partial surface uncertainty did not fail closed"
[[ ! -e "$outdir/surface-partial.i" ]] \
  || fail "uncertified surface approximation wrote MCNP"

echo "approximation_cli_smoke: OK"
