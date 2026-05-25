#!/usr/bin/env bash
#
# End-to-end smoke test for `cad2csg decompose-apply`. Runs the batched
# command against the L-shape (a single non-convex solid) and verifies:
#   - the MCNP deck is produced
#   - the decision log records the decomposition op
#   - the STEP review artifact is produced and re-imports as >1 solid
#   - every output is non-empty
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "decompose_apply_smoke: $*" >&2
    exit 1
}

step_in="$fixtures/l_shape.step"
mcnp_out="$outdir/out.i"
step_out="$outdir/review.step"
log_out="$outdir/decisions.json"
report_out="$outdir/report.json"
pre_step_out="$outdir/preprocess.step"
pre_report_out="$outdir/preprocess.json"
removal_report_out="$outdir/removals.json"

rm -f "$mcnp_out" "$step_out" "$log_out" "$report_out" \
      "$pre_step_out" "$pre_report_out" "$removal_report_out"

"$exe" decompose-apply "$step_in" --out "$mcnp_out" \
    --log "$log_out" --out-step "$step_out" --report "$report_out" \
    >/dev/null 2>&1 \
    || fail "decompose-apply returned non-zero"

[[ -s "$mcnp_out" ]]   || fail "MCNP deck not produced"
[[ -s "$step_out" ]]   || fail "review STEP not produced"
[[ -s "$log_out" ]]    || fail "decision log not produced"
[[ -s "$report_out" ]] || fail "analysis report not produced"

# The decomposition split solid:1 into pieces; the re-imported review
# STEP must contain more than one solid.
solids=$("$exe" dump-assembly "$step_out" 2>/dev/null \
    | grep -c "^solid:")
[[ "$solids" -gt 1 ]] \
    || fail "expected >1 solids in review STEP, got $solids"

# Decision log must contain at least one decomposition-flavored op.
python3 - "$log_out" <<'PY' || exit 1
import json, sys

with open(sys.argv[1]) as f:
    records = json.load(f)

ops = [r.get("op") for r in records if r.get("kind") == "decision"]
decomposition_ops = [o for o in ops
                     if o and ("decompose" in o or "decomposition" in o)]
if not decomposition_ops:
    print(f"no decomposition op in log: {ops}", file=sys.stderr)
    sys.exit(1)
PY

# Volume must be preserved across the decomposition.
"$exe" volume "$step_in" --compare "$step_out" --rel-tol 1e-6 >/dev/null 2>&1 \
    || fail "decomposition did not preserve volume within rel-tol 1e-6"

"$exe" preprocess "$fixtures/rails.stp" \
    --out-step "$pre_step_out" \
    --apply-removal-candidates high \
    --max-targets 1 --max-splits 1 --max-depth 1 \
    --report "$pre_report_out" >/dev/null 2>&1 \
    || fail "bounded preprocess returned non-zero"
[[ -s "$pre_step_out" ]]   || fail "bounded preprocess STEP not produced"
[[ -s "$pre_report_out" ]] || fail "bounded preprocess report not produced"
python3 - "$pre_report_out" <<'PY' || exit 1
import json, sys

report = json.load(open(sys.argv[1], encoding="utf-8"))
perf = report.get("performance")
if not perf:
    print("missing performance block", file=sys.stderr)
    sys.exit(1)
for key in ("load_step_ms", "analyze_ms", "propose_ms", "export_step_ms"):
    if perf.get(key, 0) <= 0:
        print(f"expected positive {key}, got {perf.get(key)}", file=sys.stderr)
        sys.exit(1)
if perf.get("candidates_evaluated", 0) <= 0:
    print("expected candidates_evaluated > 0", file=sys.stderr)
    sys.exit(1)
PY

"$exe" remove-suggest "$fixtures/rails.stp" \
    --report "$removal_report_out" >/dev/null 2>&1 \
    || fail "remove-suggest returned non-zero on rails fixture"
python3 - "$removal_report_out" <<'PY' || exit 1
import json, sys

proposal = json.load(open(sys.argv[1], encoding="utf-8"))
if not proposal.get("ok"):
    print("expected removal proposal ok", file=sys.stderr)
    sys.exit(1)
if not proposal.get("candidates"):
    print("expected at least one removal candidate", file=sys.stderr)
    sys.exit(1)
first = proposal["candidates"][0]
for key in ("solid_id", "reason_codes", "blockers", "confidence",
            "suggested_action", "volume_fraction"):
    if key not in first:
        print(f"missing {key} in removal candidate", file=sys.stderr)
        sys.exit(1)
PY

# When given a model with no non-convex solids, decompose-apply should
# succeed without doing anything destructive and still emit the deck.
box_mcnp="$outdir/box.i"
"$exe" decompose-apply "$fixtures/box.step" --out "$box_mcnp" >/dev/null 2>&1 \
    || fail "decompose-apply on a convex-only model should succeed"
[[ -s "$box_mcnp" ]] || fail "decompose-apply did not emit deck on convex-only input"

echo "decompose_apply_smoke: ok"
