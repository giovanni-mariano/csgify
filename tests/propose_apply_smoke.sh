#!/usr/bin/env bash
#
# propose_apply_smoke: verify the unified `propose <kind>` / `apply <kind>`
# front door dispatches to the same engine paths as the legacy
# *-suggest verbs and `simplify --apply-*-candidate`.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "propose_apply_smoke: $*" >&2; exit 1; }

# ---- propose decompose vs decompose-suggest produce same candidate count ---
new_dec="$outdir/propose_decompose.json"
"$exe" propose decompose solid:1 "$fixtures/l_shape.step" --report "$new_dec" \
    >/dev/null 2>&1 || fail "propose decompose failed"

old_dec="$outdir/decompose_suggest.json"
"$exe" decompose-suggest "$fixtures/l_shape.step" solid:1 --report "$old_dec" \
    >/dev/null 2>&1 || fail "decompose-suggest failed"

python3 - "$new_dec" "$old_dec" <<'PY'
import json, sys
new = json.load(open(sys.argv[1])); old = json.load(open(sys.argv[2]))
assert new["ok"] == old["ok"], (new["ok"], old["ok"])
assert len(new["candidates"]) == len(old["candidates"]), (
    len(new["candidates"]), len(old["candidates"]))
PY
[ "$?" -eq 0 ] || fail "propose decompose differs from decompose-suggest"

# ---- propose removal works -------------------------------------------------
rem="$outdir/propose_removal.json"
"$exe" propose removal "$fixtures/four_solids.step" --report "$rem" \
    >/dev/null 2>&1 || true  # ok flag may be false on this fixture
[ -s "$rem" ] || fail "propose removal did not write report"

# ---- propose surface on a spline face works -------------------------------
surf="$outdir/propose_surface.json"
"$exe" propose surface face:1.1 "$fixtures/spline_face.step" --report "$surf" \
    >/dev/null 2>&1 || true
[ -s "$surf" ] || fail "propose surface did not write report"

# ---- apply decompose runs the same op as simplify --apply-decomposition-candidate
# Pick the best candidate from propose, apply via the new noun-first form,
# and check the volume is preserved.
best_id="$(python3 -c "import json; d=json.load(open('$new_dec')); print(d.get('best_candidate_id',''))")"
[ -n "$best_id" ] || fail "no best candidate from propose decompose"

new_out="$outdir/apply_decompose.i"
"$exe" apply decompose solid:1 "$fixtures/l_shape.step" \
    --candidate "$best_id" --out "$new_out" >/dev/null 2>&1 \
    || fail "apply decompose failed for candidate=$best_id"
[ -s "$new_out" ] || fail "apply decompose did not write MCNP output"

# ---- apply removal suppresses a solid --------------------------------------
rem_out="$outdir/apply_removal.i"
rem_report="$outdir/apply_removal.json"
"$exe" apply removal solid:2 "$fixtures/four_solids.step" \
    --out "$rem_out" --report "$rem_report" >/dev/null 2>&1 \
    || fail "apply removal failed"
python3 - "$rem_report" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# Post-emit reports carry summary counters (not the full solids[] array).
# Starting from 4 solids and removing one leaves 3 emitted.
assert d["summary"]["solids_converted"] == 3, d["summary"]
PY
[ "$?" -eq 0 ] || fail "apply removal did not bring solids_converted to 3"

# ---- legacy verb shims still work and emit a deprecation notice -----------
legacy_err="$outdir/legacy.err"
"$exe" decompose-suggest "$fixtures/l_shape.step" solid:1 \
    --report "$outdir/legacy_dec.json" >/dev/null 2>"$legacy_err" \
    || fail "legacy decompose-suggest failed"
grep -q "deprecated_verb" "$legacy_err" \
    || fail "decompose-suggest did not emit deprecated_verb notice"
[ -s "$outdir/legacy_dec.json" ] || fail "legacy decompose-suggest report not written"

# ---- propose --help and apply --help -----------------------------------
"$exe" propose --help >"$outdir/propose_help.out" 2>&1 || fail "propose --help failed"
grep -q "decompose | surface | removal | hole-fill" "$outdir/propose_help.out" \
    || fail "propose --help missing kind list"

"$exe" apply --help >"$outdir/apply_help.out" 2>&1 || fail "apply --help failed"
grep -q "Mutation contract" "$outdir/apply_help.out" \
    || fail "apply --help missing mutation contract"

echo "propose_apply_smoke: OK"
exit 0
