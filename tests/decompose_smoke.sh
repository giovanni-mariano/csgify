#!/usr/bin/env bash
#
# Smoke test for the decomposition proposal pipeline against the L-shape
# fixture (the canonical non-convex single solid). Pins the current
# scoring behavior: the proposer must
#
#   - identify face:1.7 (the convexity-failure face) as the best cutter
#   - return high confidence
#   - report exactly 2 pieces with volume_balance ~= 0.5
#
# These guarantees should survive any future tuning of the scoring
# weights. If they need to change, update this test deliberately.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "decompose_smoke: $*" >&2
    exit 1
}

report="$outdir/proposal.json"
rm -f "$report"

"$exe" decompose-suggest "$fixtures/l_shape.step" solid:1 --report "$report" >/dev/null 2>&1 \
    || fail "decompose-suggest returned non-zero"

[[ -s "$report" ]] || fail "proposal report was not produced"

python3 - "$report" <<'PY' || exit 1
import json, math, sys

with open(sys.argv[1]) as f:
    proposal = json.load(f)

if not proposal.get("ok"):
    print(f"proposal.ok must be True: {proposal!r}", file=sys.stderr)
    sys.exit(1)

if proposal.get("target_id") != "solid:1":
    print(f"expected target_id solid:1, got {proposal.get('target_id')!r}",
          file=sys.stderr)
    sys.exit(1)

best_id = proposal.get("best_candidate_id")
candidates = proposal.get("candidates") or []
best = next((c for c in candidates if c.get("id") == best_id), None)
if not best:
    print(f"best candidate {best_id!r} not present in candidates list",
          file=sys.stderr)
    sys.exit(1)

if best.get("source_face_id") != "face:1.7":
    print(f"expected best.source_face_id face:1.7, got "
          f"{best.get('source_face_id')!r}", file=sys.stderr)
    sys.exit(1)

if best.get("reason") != "convexity_failure_face":
    print(f"expected best.reason convexity_failure_face, got "
          f"{best.get('reason')!r}", file=sys.stderr)
    sys.exit(1)

if not best.get("usable"):
    print(f"best candidate must be usable: {best!r}", file=sys.stderr)
    sys.exit(1)

if best.get("confidence") != "high":
    print(f"expected best.confidence high, got "
          f"{best.get('confidence')!r}", file=sys.stderr)
    sys.exit(1)

if best.get("piece_count") != 2:
    print(f"expected piece_count 2, got {best.get('piece_count')!r}",
          file=sys.stderr)
    sys.exit(1)

balance = best.get("volume_balance", 0.0)
if not math.isclose(balance, 0.5, abs_tol=0.05):
    print(f"expected volume_balance ~ 0.5, got {balance}", file=sys.stderr)
    sys.exit(1)

if best.get("exact_piece_count") != 2:
    print(f"both pieces should be exact CSG, got "
          f"exact_piece_count={best.get('exact_piece_count')}", file=sys.stderr)
    sys.exit(1)

usable_count = sum(1 for c in candidates if c.get("usable"))
if usable_count < 2:
    print(f"expected at least two usable candidates, got {usable_count}",
          file=sys.stderr)
    sys.exit(1)

PY

# Apply the chosen cut via decompose-by-face and verify the pieces emit
# cleanly to MCNP.
out_i="$outdir/decomposed.i"
rm -f "$out_i"
"$exe" simplify "$fixtures/l_shape.step" --decompose-by-face solid:1 \
    --out "$out_i" --by-face face:1.7 >/dev/null 2>&1 \
    || fail "decompose-by-face apply returned non-zero"
[[ -s "$out_i" ]] || fail "MCNP deck not produced after directed decomposition"

echo "decompose_smoke: ok"
