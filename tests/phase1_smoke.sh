#!/usr/bin/env bash
set -u

exe="$1"
step="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "phase1_smoke: $*" >&2
    exit 1
}

run_expect_success() {
    "$@" || fail "command failed: $*"
}

run_expect_failure() {
    "$@"
    status=$?
    if [ "$status" -eq 0 ]; then
        fail "command succeeded but failure was expected: $*"
    fi
}

report="$outdir/screw_report.json"
cylinder_report="$outdir/screw_fit_cyl_report.json"
cylinder_mcnp="$outdir/screw_fit_cyl.i"
cone_mcnp="$outdir/screw_fit_cone.i"
cone_rejected_mcnp="$outdir/screw_fit_cone_rejected.i"
decomp_report="$outdir/screw_decomp_report.json"
decomp_mcnp="$outdir/screw_decomp.i"
sphere_mcnp="$outdir/screw_fit_sphere.i"

run_expect_success "$exe" --version

# The screw sample is intentionally not directly convertible because it still
# has torus faces and is non-convex. Inspect must still write a usable report.
"$exe" inspect "$step" --report "$report"
inspect_status=$?
if [ "$inspect_status" -eq 0 ]; then
    fail "inspect unexpectedly succeeded for non-convex screw sample"
fi

python3 - "$report" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

solid = report["solids"][0]
assert solid["id"] == "solid:1"
assert solid["replacement_candidates"]["fitted_cylinder"]["available"] is True
assert solid["replacement_candidates"]["fitted_cone"]["available"] is True
assert solid["replacement_candidates"]["fitted_sphere"]["available"] is False

cone_faces = [face for face in report["faces"] if face["surface_type"] == "cone"]
assert cone_faces, "expected cone faces in screw sample"
assert all(face["exact_csg_supported"] for face in cone_faces)

torus_faces = [face for face in report["faces"] if face["surface_type"] == "torus"]
assert torus_faces, "expected torus faces in screw sample"
assert all(face["exact_csg_supported"] for face in torus_faces)

unsupported = [diag["shape_id"] for diag in report["diagnostics"]
               if diag["code"] == "unsupported_analytic_face"]
assert unsupported == [], unsupported
PY
if [ "$?" -ne 0 ]; then
    fail "report validation failed"
fi

run_expect_success "$exe" simplify "$step" --replace-solid-fitted-cylinder solid:1 --out "$cylinder_mcnp" --max-rel-delta 0.13 --report "$cylinder_report"
grep -q "RCC" "$cylinder_mcnp" || fail "fitted cylinder did not emit RCC"
python3 -m json.tool "$cylinder_report" >/dev/null || fail "simplify report is not valid JSON"

run_expect_success "$exe" simplify "$step" --replace-solid-fitted-cone solid:1 --out "$cone_mcnp"
grep -Eq " K(/?[XYZ])? | GQ " "$cone_mcnp" || fail "fitted cone did not emit cone side surface"
grep -Eq " K(/?[XYZ])? " "$cone_mcnp" || fail "axis-aligned fitted cone did not emit K cone surface"
plane_count=$(grep -c "^.* P " "$cone_mcnp")
if [ "$plane_count" -lt 2 ]; then
    fail "fitted cone did not emit cap planes"
fi

run_expect_failure "$exe" simplify "$step" --replace-solid-fitted-cone solid:1 --out "$cone_rejected_mcnp" --max-rel-delta 0.5
run_expect_failure "$exe" simplify "$step" --decompose-planar solid:1 --out "$decomp_mcnp" --report "$decomp_report"
python3 - "$decomp_report" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

assert report["summary"]["decomposition_pieces"] == 5
assert report["summary"]["solids_seen"] == 5
assert report["summary"]["solids_converted"] == 2
pieces = report["decomposed_pieces"]
assert len(pieces) == 5
assert sum(1 for piece in pieces if piece["exact_csg_supported"]) == 2
assert sum(1 for piece in pieces if piece["needs_decomposition"]) == 3
assert all(piece["source_solid_id"] == "solid:1" for piece in pieces)
diagnostics = report["diagnostics"]
planar = [diag for diag in diagnostics if diag["code"] == "planar_decomposition"]
assert planar, "expected planar_decomposition diagnostic"
assert "relative_delta=" in planar[0]["message"]
PY
if [ "$?" -ne 0 ]; then
    fail "decomposition report validation failed"
fi
run_expect_failure "$exe" simplify "$step" --replace-solid-fitted-sphere solid:1 --out "$sphere_mcnp"

exit 0
