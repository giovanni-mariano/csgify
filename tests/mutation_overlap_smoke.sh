#!/usr/bin/env bash
#
# mutation_overlap_smoke: cad2csg must flag mutations that introduce
# new overlaps between solids. Pre-existing overlaps are reported by
# the analyze pass; mutation-introduced overlaps must be reported on
# the operation that created them, so the user can't silently emit
# a model where --replace-solid-bbox has expanded one cell into a
# neighbor.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "mutation_overlap_smoke: $*" >&2; exit 1; }

step="$fixtures/l_with_inner_box.step"
[ -f "$step" ] \
    || fail "fixture missing: $step (run cad2csg_generate_fixtures)"

# ---- as-loaded: NO overlap between L and inner box -------------------
# The L-shape is non-convex (so inspect exits nonzero with
# nonconvex_solid_requires_decomposition) but the report IS still
# written and is what we need to read.
"$exe" inspect "$step" --report "$outdir/baseline.json" >/dev/null 2>&1 || true
[ -s "$outdir/baseline.json" ] \
    || fail "inspect did not write baseline.json"

python3 - "$outdir/baseline.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["summary"]["solids_seen"] == 2, d["summary"]
assert d.get("overlaps", []) == [], f"baseline should be overlap-free: {d.get('overlaps')}"
PY
[ "$?" -eq 0 ] || fail "baseline must be overlap-free"

# ---- --replace-solid-bbox on solid:1 puts a 10x10x10 box where the
#      L-shape used to sit; the inner 3x3x3 box at (5,5,1) is now
#      fully inside that bbox. Expect a mutation_introduced_overlap.
"$exe" simplify "$step" --replace-solid-bbox solid:1 \
    --out "$outdir/mut.i" --max-rel-delta 100.0 \
    --report "$outdir/mut.json" >"$outdir/mut.stderr" 2>&1 || true
[ -s "$outdir/mut.json" ] || fail "simplify --replace-solid-bbox wrote no report"

python3 - "$outdir/mut.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
diags = d["diagnostics"]
intro = [diag for diag in diags
         if diag["code"] == "mutation_introduced_overlap"]
assert intro, f"expected mutation_introduced_overlap; got codes={[diag['code'] for diag in diags]}"
# The diagnostic must reference the new synthetic body and the inner
# solid:2 by id.
label = intro[0]["shape_label"]
assert "synthetic:" in label, label
assert "solid:2" in label, label
# Intersection volume should be ~27 (3^3 cube fully contained).
msg = intro[0]["message"]
assert "intersection_volume=" in msg, msg
PY
[ "$?" -eq 0 ] || fail "mutation_introduced_overlap not detected"

# ---- Negative control: --replace-solid-bbox on a STANDALONE solid
#      must NOT trigger a false positive (no neighbors to overlap).
"$exe" simplify "$fixtures/box.step" --replace-solid-bbox solid:1 \
    --out "$outdir/box_repl.i" \
    --report "$outdir/box_repl.json" >/dev/null 2>&1 || true
python3 - "$outdir/box_repl.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
intro = [diag for diag in d["diagnostics"]
         if diag["code"] == "mutation_introduced_overlap"]
assert intro == [], f"standalone box.step should not flag overlap; got {intro}"
PY
[ "$?" -eq 0 ] || fail "false positive on standalone replacement"

echo "mutation_overlap_smoke: OK"
exit 0
