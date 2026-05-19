#!/usr/bin/env bash
#
# Pins the diagnostic-code contract that agents and external tooling depend
# on. Each fixture is a minimal STEP file under tests/fixtures/, generated
# by the cad2csg_generate_fixtures developer tool.
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
"$exe" "$fixtures/box.step" "$outdir/box.i" >/dev/null 2>&1 \
    || fail "box.step failed to convert"

run_inspect sphere.step "$outdir/sphere.json"
assert_codes "$outdir/sphere.json" ""
"$exe" "$fixtures/sphere.step" "$outdir/sphere.i" >/dev/null 2>&1 \
    || fail "sphere.step failed to convert"

run_inspect cylinder.step "$outdir/cylinder.json"
assert_codes "$outdir/cylinder.json" ""
"$exe" "$fixtures/cylinder.step" "$outdir/cylinder.i" >/dev/null 2>&1 \
    || fail "cylinder.step failed to convert"

# ---- non-convex solid: must produce the documented decomposition code ----

run_inspect l_shape.step "$outdir/l_shape.json"
assert_codes "$outdir/l_shape.json" "nonconvex_solid_requires_decomposition"

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
applied = [d for d in report["diagnostics"]
           if d["code"] == "face_replacement_applied"]
assert applied, "expected face_replacement_applied info diagnostic"
PY
if [ "$?" -ne 0 ]; then
    fail "face replacement contract validation failed"
fi

exit 0
