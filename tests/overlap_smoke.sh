#!/usr/bin/env bash
#
# overlap_smoke: pairwise solid-overlap detection.
#
# MCNP cells must be non-overlapping; cad2csg's analyze stage detects
# interpenetrating solid pairs and emits a `solid_overlap` warning.
# Asserts:
#   - overlapping_boxes.step (built-in fixture: two 2x2x2 boxes with
#     a (1,1,1)..(2,2,2) overlap = 1 mm^3) produces exactly one pair
#     in report.overlaps[] with intersection_volume ~ 1.
#   - four_solids.step (disjoint) produces zero pairs.
#   - --no-overlap-check on the bad fixture suppresses both the
#     diagnostic and the report.overlaps[] entries.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "overlap_smoke: $*" >&2; exit 1; }

bad="$fixtures/overlapping_boxes.step"
good="$fixtures/four_solids.step"
[ -f "$bad" ]  || fail "fixture missing: $bad (run cad2csg_generate_fixtures)"
[ -f "$good" ] || fail "fixture missing: $good"

# ---- overlapping fixture: one detected pair --------------------------
# Overlap diagnostics are error-severity by default, so inspect exits
# non-zero. The report file is still written, which is what we read.
bad_report="$outdir/overlapping.json"
"$exe" inspect "$bad" --report "$bad_report" >/dev/null 2>"$outdir/overlapping.err" || true
[ -s "$bad_report" ] \
    || fail "inspect did not write report on $bad"

python3 - "$bad_report" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
overlaps = d.get("overlaps", [])
assert isinstance(overlaps, list), overlaps
assert len(overlaps) == 1, f"expected 1 overlap pair, got {len(overlaps)}"
ov = overlaps[0]
assert ov["solid_a_id"] == "solid:1", ov
assert ov["solid_b_id"] == "solid:2", ov
# Intersection cube is 1x1x1 = 1.0; allow numerical noise.
assert abs(ov["intersection_volume"] - 1.0) < 1e-6, ov
# Each solid is 2x2x2 = 8 mm^3, so min_fraction = 1/8 = 0.125.
assert abs(ov["min_fraction"] - 0.125) < 1e-6, ov

# Diagnostics must include a solid_overlap *error* referring to both ids
# (overlaps are error-severity by default; --allow-overlaps downgrades).
codes = [(diag["severity"], diag["code"]) for diag in d["diagnostics"]]
assert ("error", "solid_overlap") in codes, codes
PY
[ "$?" -eq 0 ] || fail "JSON validation failed on $bad_report"

# Human stdout summary mentions overlaps.
"$exe" inspect "$bad" >"$outdir/overlapping.human" 2>/dev/null
grep -q "overlaps:" "$outdir/overlapping.human" \
    || fail "human inspect did not surface overlap count"

# ---- disjoint fixture: zero pairs ------------------------------------
good_report="$outdir/four_solids.json"
"$exe" inspect "$good" --report "$good_report" >/dev/null 2>"$outdir/four_solids.err" \
    || fail "inspect failed on $good"

python3 - "$good_report" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
assert d.get("overlaps", []) == [], d.get("overlaps")
codes = [diag["code"] for diag in d["diagnostics"]]
assert "solid_overlap" not in codes, codes
PY
[ "$?" -eq 0 ] || fail "disjoint fixture should not flag overlaps"

# ---- --no-overlap-check suppresses detection -------------------------
skip_report="$outdir/skip.json"
"$exe" inspect "$bad" --no-overlap-check --report "$skip_report" \
    >/dev/null 2>"$outdir/skip.err" \
    || fail "inspect --no-overlap-check failed"

python3 - "$skip_report" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
assert d.get("overlaps", []) == [], d.get("overlaps")
codes = [diag["code"] for diag in d["diagnostics"]]
assert "solid_overlap" not in codes, codes
PY
[ "$?" -eq 0 ] || fail "--no-overlap-check did not suppress detection"

echo "overlap_smoke: OK"
exit 0
