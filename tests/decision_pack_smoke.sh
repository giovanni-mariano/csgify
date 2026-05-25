#!/usr/bin/env bash
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "decision_pack_smoke: $*" >&2
    exit 1
}

"$exe" decision-pack "$fixtures/l_shape.step" --out "$outdir/l_shape" >/dev/null 2>&1 \
    || fail "decision-pack failed on l_shape"

packet="$outdir/l_shape/decision-decomposition-solid-1"
test -f "$packet/manifest.json" || fail "missing manifest.json"
test -f "$packet/decision.md" || fail "missing decision.md"
test -f "$packet/preview.html" || fail "missing preview.html"
test -f "$packet/target.svg" || fail "missing target.svg"

python3 - "$packet/manifest.json" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
assert manifest["decision_id"] == "decision:decomposition:solid:1", manifest
assert manifest["kind"] == "decomposition", manifest
assert manifest["target_id"] == "solid:1", manifest
assert manifest["recommended_action"] == "decompose-suggest", manifest
assert manifest["images"]["target"] == "target.svg", manifest
assert manifest["candidates"], manifest
PY
if [ "$?" -ne 0 ]; then
    fail "manifest contract validation failed"
fi

grep -q "<svg" "$packet/target.svg" || fail "target.svg is not SVG"
grep -q "decomposition solid:1" "$packet/preview.html" \
    || fail "preview.html missing decision title"

"$exe" decision-pack "$fixtures/spline_face.step" --out "$outdir/spline" \
    --target face:1.1 >/dev/null 2>&1 \
    || fail "decision-pack failed on targeted spline face"

spline_packet="$outdir/spline/decision-spline-replacement-face-1-1"
test -f "$spline_packet/manifest.json" || fail "missing targeted spline manifest"
python3 - "$spline_packet/manifest.json" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
assert manifest["kind"] == "spline_replacement", manifest
assert manifest["target_id"] == "face:1.1", manifest
assert manifest["recommended_action"] == "replace_face_with_cylinder", manifest
PY
if [ "$?" -ne 0 ]; then
    fail "targeted spline manifest contract validation failed"
fi

"$exe" decision-pack "$fixtures/box.step" --out "$outdir/box" >/dev/null 2>&1 \
    || fail "decision-pack should succeed with zero packets"

"$exe" decision-pack "$fixtures/bolt_hole.step" --out "$outdir/hole" \
    --hole-fill --host solid:1 --near-removed solid:2 \
    --max-radius 2 --max-volume-fraction 1 >/dev/null 2>&1 \
    || fail "decision-pack failed on hole-fill proposal"

hole_packet="$outdir/hole/decision-hole-fill-1"
test -f "$hole_packet/manifest.json" || fail "missing hole-fill manifest"
test -f "$hole_packet/decision.md" || fail "missing hole-fill decision.md"
test -f "$hole_packet/preview.html" || fail "missing hole-fill preview.html"
test -f "$hole_packet/target.svg" || fail "missing hole-fill target.svg"
python3 - "$hole_packet/manifest.json" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
assert manifest["decision_id"] == "decision:hole-fill:1", manifest
assert manifest["kind"] == "hole_fill", manifest
assert manifest["target_id"] == "hole-fill:1", manifest
assert manifest["recommended_action"] == "fill_hole", manifest
assert manifest["candidates"][0]["action"] == "fill_hole", manifest
assert manifest["candidates"][1]["action"] == "leave_void", manifest
metrics = {m["name"]: m["value"] for m in manifest["metrics"]}
assert abs(metrics["radius"] - 1.0) < 1e-8, manifest
assert abs(metrics["length"] - 4.0) < 1e-5, manifest
PY
if [ "$?" -ne 0 ]; then
    fail "hole-fill manifest contract validation failed"
fi
grep -q "fill this cavity" "$hole_packet/target.svg" \
    || fail "hole-fill SVG missing decision question"

exit 0
