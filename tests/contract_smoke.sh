#!/usr/bin/env bash
#
# Pins the diagnostic-code contract that agents and external tooling depend
# on. Each fixture is a minimal STEP file under tests/fixtures/, generated
# by the csgify_generate_fixtures developer tool.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "contract_smoke: $*" >&2
    exit 1
}

run_inspect() {
    local fixture="$1"
    local report="$2"
    "$exe" inspect "$fixtures/$fixture" --report "$report" >/dev/null 2>&1
    return 0
}

assert_codes() {
    local report="$1"
    local expected="$2"
    python3 - "$report" "$expected" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

expected = set(s for s in sys.argv[2].split(",") if s)
codes = {d["code"] for d in report["diagnostics"]}
if codes != expected:
    print(f"diagnostic mismatch: expected={sorted(expected)} actual={sorted(codes)}",
          file=sys.stderr)
    sys.exit(1)
PY
    if [ "$?" -ne 0 ]; then
        fail "code assertion failed for $report"
    fi
}

# ---- clean fixtures: no error-level diagnostics, single solid converts ----

run_inspect box.step "$outdir/box.json"
assert_codes "$outdir/box.json" ""
python3 - "$outdir/box.json" <<'PY'
import json
import sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
assert "decision_requests" in report, "report must expose decision_requests"
assert report["decision_requests"] == [], report["decision_requests"]
PY
if [ "$?" -ne 0 ]; then
    fail "box decision_requests contract validation failed"
fi
"$exe" "$fixtures/box.step" "$outdir/box.i" >/dev/null 2>&1 \
    || fail "box.step failed to convert"

run_inspect sphere.step "$outdir/sphere.json"
assert_codes "$outdir/sphere.json" ""
"$exe" "$fixtures/sphere.step" "$outdir/sphere.i" >/dev/null 2>&1 \
    || fail "sphere.step failed to convert"

"$exe" decompose-suggest "$fixtures/sphere.step" solid:1 \
    --report "$outdir/sphere_proposals.json" >/dev/null 2>&1 \
    || fail "decompose-suggest failed to produce fallback candidates on sphere fixture"

python3 - "$outdir/sphere_proposals.json" <<'PY'
import json
import sys

proposal = json.load(open(sys.argv[1], encoding="utf-8"))
fallbacks = [c for c in proposal["candidates"]
             if c["reason"] == "bbox_axis_fallback"]
# 3 single-axis + 1 multi-axis multi-tool variant
assert len(fallbacks) == 4, proposal["candidates"]
assert all(c["usable"] for c in fallbacks), fallbacks
assert all(c["confidence"] != "high" for c in fallbacks), fallbacks
assert all("plane_valid" in c for c in fallbacks), fallbacks
assert all("plane_origin" in c for c in fallbacks), fallbacks
assert all("plane_normal" in c for c in fallbacks), fallbacks
assert all("planes" in c for c in fallbacks), fallbacks
assert {c["tool_kind"] for c in fallbacks} == {
    "bbox_plane_x", "bbox_plane_y", "bbox_plane_z", "bbox_planes_xyz",
}, fallbacks
# The multi-tool variant should produce more pieces than any single-axis
# cut on a sphere (8 vs 2).
multi = [c for c in fallbacks if c["tool_kind"] == "bbox_planes_xyz"]
assert len(multi) == 1, multi
assert multi[0]["piece_count"] > max(
    c["piece_count"] for c in fallbacks
    if c["tool_kind"] != "bbox_planes_xyz"
), fallbacks
assert len(multi[0]["planes"]) == 3, multi[0]
PY
if [ "$?" -ne 0 ]; then
    fail "bbox fallback proposal contract validation failed"
fi

"$exe" simplify "$fixtures/sphere.step" \
    --apply-decomposition-candidate solid:1 \
    --out "$outdir/sphere_apply_bbox_candidate.i" \
    --candidate candidate:3 \
    --report "$outdir/sphere_apply_bbox_candidate.json" >/dev/null 2>&1 \
    || fail "apply-decomposition-candidate failed for bbox plane candidate"

python3 - "$outdir/sphere_apply_bbox_candidate.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], encoding="utf-8"))
summary = report["summary"]
assert summary["solids_converted"] == 2, summary
codes = {d["code"] for d in report["diagnostics"]}
assert "plane_decomposition" in codes, codes
PY
if [ "$?" -ne 0 ]; then
    fail "bbox candidate application contract validation failed"
fi

run_inspect cylinder.step "$outdir/cylinder.json"
assert_codes "$outdir/cylinder.json" ""
"$exe" "$fixtures/cylinder.step" "$outdir/cylinder.i" >/dev/null 2>&1 \
    || fail "cylinder.step failed to convert"

# ---- non-convex solid: must produce the documented decomposition code ----

run_inspect l_shape.step "$outdir/l_shape.json"
assert_codes "$outdir/l_shape.json" "nonconvex_solid_requires_decomposition"
python3 - "$outdir/l_shape.json" <<'PY'
import json
import sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
requests = report["decision_requests"]
decomp = [r for r in requests
          if r["kind"] == "decomposition" and r["target_id"] == "solid:1"]
assert len(decomp) == 1, requests
assert decomp[0]["recommended_action"] == "decompose-suggest", decomp[0]
assert decomp[0]["candidates"], decomp[0]
PY
if [ "$?" -ne 0 ]; then
    fail "l_shape decision_requests contract validation failed"
fi

"$exe" decompose-suggest "$fixtures/l_shape.step" solid:1 \
    --report "$outdir/l_shape_proposals.json" >/dev/null 2>&1 \
    || fail "decompose-suggest failed on l_shape fixture"

python3 - "$outdir/l_shape_proposals.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    proposal = json.load(handle)

assert proposal["ok"] is True, proposal
assert proposal["best_candidate_id"], proposal
usable = [c for c in proposal["candidates"] if c["usable"]]
assert usable, "expected at least one usable split candidate"
best = usable[0]
assert best["confidence"] == "high", best
assert best["piece_count"] == 2, best
assert best["exact_piece_count"] == 2, best
assert best["relative_delta"] <= 1e-8, best
assert best["reason"] == "convexity_failure_face", best
assert best["source_face_id"] == "face:1.7", best
PY
if [ "$?" -ne 0 ]; then
    fail "decomposition proposal contract validation failed"
fi

"$exe" simplify "$fixtures/l_shape.step" \
    --decompose-by-face solid:1 \
    --out "$outdir/l_shape_split_by_face.i" \
    --by-face face:1.7 \
    --report "$outdir/l_shape_split_by_face.json" >/dev/null 2>&1 \
    || fail "directed face decomposition failed on l_shape fixture"

python3 - "$outdir/l_shape_split_by_face.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

summary = report["summary"]
assert summary["decomposition_pieces"] == 2, summary
assert summary["solids_converted"] == 2, summary
codes = {d["code"] for d in report["diagnostics"]}
assert "face_decomposition" in codes, codes
assert "nonconvex_solid_requires_decomposition" not in codes, codes
PY
if [ "$?" -ne 0 ]; then
    fail "directed face decomposition contract validation failed"
fi

"$exe" simplify "$fixtures/l_shape.step" \
    --auto-decompose-bsp solid:1 \
    --out "$outdir/l_shape_auto_bsp.i" \
    --report "$outdir/l_shape_auto_bsp.json" >/dev/null 2>&1 \
    || fail "safe BSP auto-decomposition failed on l_shape fixture"

python3 - "$outdir/l_shape_auto_bsp.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

summary = report["summary"]
assert summary["decomposition_pieces"] == 2, summary
assert summary["solids_converted"] == 2, summary
codes = {d["code"] for d in report["diagnostics"]}
assert "auto_decompose_bsp_complete" in codes, codes
assert "face_decomposition" in codes, codes
assert "nonconvex_solid_requires_decomposition" not in codes, codes
PY
if [ "$?" -ne 0 ]; then
    fail "safe BSP auto-decomposition contract validation failed"
fi

"$exe" simplify "$fixtures/l_shape.step" \
    --apply-decomposition-candidate solid:1 \
    --out "$outdir/l_shape_apply_candidate.i" \
    --candidate candidate:7 \
    --report "$outdir/l_shape_apply_candidate.json" \
    --proposal-report "$outdir/l_shape_apply_candidate_proposals.json" >/dev/null 2>&1 \
    || fail "apply-decomposition-candidate failed on l_shape fixture"

python3 - "$outdir/l_shape_apply_candidate.json" "$outdir/l_shape_apply_candidate_proposals.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], encoding="utf-8"))
proposal = json.load(open(sys.argv[2], encoding="utf-8"))
assert report["summary"]["solids_converted"] == 2, report["summary"]
assert report["summary"]["decomposition_pieces"] == 2, report["summary"]
assert proposal["best_candidate_id"] == "candidate:7", proposal
assert proposal["candidates"][0]["source_face_id"] == "face:1.7", proposal["candidates"][0]
PY
if [ "$?" -ne 0 ]; then
    fail "apply-decomposition-candidate contract validation failed"
fi

"$exe" simplify "$fixtures/l_shape.step" \
    --auto-decompose-bsp solid:1 \
    --out "$outdir/l_shape_auto_bsp_depth0.i" \
    --max-depth 0 \
    --proposal-report "$outdir/l_shape_auto_bsp_depth0_proposals.json" >/dev/null 2>&1 || true

python3 - "$outdir/l_shape_auto_bsp_depth0_proposals.json" <<'PY'
import json
import sys

proposal = json.load(open(sys.argv[1], encoding="utf-8"))
assert proposal["ok"] is True, proposal
assert proposal["best_candidate_id"] == "candidate:7", proposal
assert proposal["candidates"][0]["confidence"] == "high", proposal["candidates"][0]
PY
if [ "$?" -ne 0 ]; then
    fail "stopped BSP proposal-report contract validation failed"
fi

# ---- spline solid: must produce unsupported_spline_face and emit useful
#      fit candidates that an agent can act on.

run_inspect spline_face.step "$outdir/spline_face.json"
assert_codes "$outdir/spline_face.json" "unsupported_spline_face"
python3 - "$outdir/spline_face.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

splines = [face for face in report["faces"] if face["surface_type"] == "bspline"]
assert splines, "expected at least one bspline face"

requests = report["decision_requests"]
spline_requests = [r for r in requests if r["kind"] == "spline_replacement"]
assert len(spline_requests) == len(splines), requests
assert any(r["recommended_action"] == "replace_face_with_cylinder"
           for r in spline_requests), spline_requests
assert all(r["candidates"] for r in spline_requests), spline_requests

# At least one face must fit cylinder near-exactly (curved side of NURBS
# cylinder) and at least one must fit plane near-exactly (a planar cap).
def best_fit_kind(face):
    fits = face["spline_fits"]
    best = None
    for kind in ("plane", "sphere", "cylinder"):
        fit = fits[kind]
        if not fit["available"]:
            continue
        if best is None or fit["max_deviation"] < best[1]["max_deviation"]:
            best = (kind, fit)
    return best

kinds = {}
for f in splines:
    chosen = best_fit_kind(f)
    if chosen is None:
        continue
    kinds.setdefault(chosen[0], []).append(chosen[1]["max_deviation"])

assert "cylinder" in kinds, f"expected at least one cylinder fit; got {kinds}"
assert "plane" in kinds, f"expected at least one plane fit; got {kinds}"
assert min(kinds["cylinder"]) < 1e-9, f"cylinder fit not exact: {kinds['cylinder']}"
assert min(kinds["plane"]) < 1e-9, f"plane fit not exact: {kinds['plane']}"

# Diagnostic message must include the best fit so the CLI is useful without
# parsing JSON.
splined = [d for d in report["diagnostics"] if d["code"] == "unsupported_spline_face"]
assert splined, "expected unsupported_spline_face diagnostic"
assert any("Best analytic fit" in d["message"] for d in splined), \
    "diagnostic message must surface the best fit"

# Actions list must offer face-level replacement options.
actions = splined[0]["actions"]
assert any("replace_with_cylinder" in a for a in actions), \
    f"actions must include replace_with_cylinder; got {actions}"
PY
if [ "$?" -ne 0 ]; then
    fail "spline contract validation failed"
fi

# ---- face replacement command: applying a recorded cylinder fit must
#      eliminate the spline diagnostic for that face.

"$exe" simplify "$fixtures/spline_face.step" \
    --replace-face-with-cylinder face:1.1 \
    --out "$outdir/spline_face_replaced.i" \
    --max-deviation 1e-6 \
    --report "$outdir/spline_face_replaced.json" >/dev/null 2>&1 || true
# the solid may still fail to convert (other spline faces remain) but the
# replaced face must no longer be flagged.
python3 - "$outdir/spline_face_replaced.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

for d in report["diagnostics"]:
    if d["code"] == "unsupported_spline_face" and d.get("shape_id") == "face:1.1":
        print(f"face:1.1 should have been replaced, still flagged: {d}", file=sys.stderr)
        sys.exit(1)

# The replacement record must show up as an info diagnostic.
recorded = [d for d in report["diagnostics"]
            if d["code"] == "face_replacement_recorded"]
assert recorded, "expected face_replacement_recorded info diagnostic"
assert recorded[0]["shape_id"] == "face:1.1", recorded[0]
assert recorded[0]["shape_label"] == "solid[1]/face[1]", recorded[0]

applied = [d for d in report["diagnostics"]
           if d["code"] == "face_replacement_applied"]
assert applied, "expected face_replacement_applied info diagnostic"
PY
if [ "$?" -ne 0 ]; then
    fail "face replacement contract validation failed"
fi

# ---- auto-fit: the NURBS cylinder fixture is a clean cylinder whose every
#      face is a B-spline. Auto-fit with a tight budget must convert it
#      end-to-end (3/3 spline faces accepted, 1/1 solids converted).

if "$exe" auto-fit "$fixtures/spline_face.step" \
    --out "$outdir/spline_face_unbounded.i" \
    >"$outdir/spline_face_unbounded.out" 2>&1; then
    fail "auto-fit accepted an unbounded approximation"
fi
grep -q "max_deviation_required" "$outdir/spline_face_unbounded.out" \
    || fail "unbounded auto-fit did not report max_deviation_required"

"$exe" auto-fit "$fixtures/spline_face.step" \
    --out "$outdir/spline_face_auto.i" \
    --max-deviation 1e-6 \
    --report "$outdir/spline_face_auto.json" >/dev/null 2>&1 \
    || fail "auto-fit failed end-to-end on spline_face fixture"

python3 - "$outdir/spline_face_auto.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

assert report["summary"]["solids_converted"] == 1, report["summary"]
assert report["summary"]["spline_faces"] == 0, \
    f"expected no spline diagnostics after auto-fit; got {report['summary']}"
applied = [d for d in report["diagnostics"]
           if d["code"] == "face_replacement_recorded"]
assert len(applied) == 3, f"expected 3 face_replacement_recorded; got {len(applied)}"
materialized = [d for d in report["diagnostics"]
                if d["code"] == "spline_solid_materialized"]
assert materialized, "expected fully replaced spline solid to be materialized"
PY
if [ "$?" -ne 0 ]; then
    fail "auto-fit contract validation failed"
fi

# ---- auto-fit with a too-tight budget should leave most faces untouched.
"$exe" auto-fit "$fixtures/spline_face.step" \
    --out "$outdir/spline_face_strict.i" \
    --max-deviation 1e-20 \
    --report "$outdir/spline_face_strict.json" >/dev/null 2>&1 || true
python3 - "$outdir/spline_face_strict.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)
over = [d for d in report["diagnostics"] if d["code"] == "auto_fit_over_budget"]
assert over, "tight budget must produce auto_fit_over_budget warnings"
PY
if [ "$?" -ne 0 ]; then
    fail "auto-fit budget contract validation failed"
fi

# ---- cone fit: the spline_cone fixture has a NURBS cone side that must
#      yield a cone candidate which beats plane/sphere/cylinder. The fit
#      is currently first-pass and not pixel-perfect, but it must be the
#      best of the four and stay within ~5% relative deviation.

run_inspect spline_cone.step "$outdir/spline_cone.json"
python3 - "$outdir/spline_cone.json" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)
splines = [face for face in report["faces"] if face["surface_type"] == "bspline"]
# Find the curved face: the one whose cone fit beats its plane fit.
curved = None
for face in splines:
    fits = face["spline_fits"]
    if fits["cone"]["max_deviation"] < fits["plane"]["max_deviation"]:
        curved = face
        break
assert curved is not None, "expected at least one face where cone beats plane"
cone = curved["spline_fits"]["cone"]
assert cone["available"], cone
assert cone["relative_max_deviation"] < 0.03, \
    f"cone fit deviates too much: {cone['relative_max_deviation']}"
# Half-angle must be roughly atan(1/4) ≈ 0.245 (cone fixture is r1=2, r2=1, h=4).
import math
assert abs(cone["half_angle"] - math.atan(0.25)) < 0.02, \
    f"cone half-angle off: {cone['half_angle']}"
PY
if [ "$?" -ne 0 ]; then
    fail "cone fit contract validation failed"
fi

exit 0
