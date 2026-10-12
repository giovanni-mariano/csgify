#!/usr/bin/env bash
set -u

exe="$1"
fixtures="$2"
outdir="$3"
mkdir -p "$outdir"

fail() {
    echo "fixed_point_decomposition_smoke: $*" >&2
    exit 1
}

step="$fixtures/l_shape.step"
deck="$outdir/fixed_point.i"
review="$outdir/fixed_point.step"
report="$outdir/fixed_point.json"
log="$outdir/fixed_point.log"
progress="$outdir/fixed_point_progress.jsonl"
recipe="$outdir/fixed_point.lua"
replay_deck="$outdir/fixed_point_replay.i"
rm -f "$deck" "$review" "$report" "$log" "$progress" "$recipe" "$replay_deck"

"$exe" decompose-apply "$step" --out "$deck" --out-step "$review" \
    --report "$report" --log "$log" --progress-json "$progress" \
    --fixed-point >/dev/null 2>&1 \
    || fail "fixed-point L-shape run failed"

[[ -s "$deck" ]] || fail "MCNP deck missing"
[[ -s "$review" ]] || fail "review STEP missing"
[[ -s "$report" ]] || fail "report missing"
[[ -s "$log" ]] || fail "decision log missing"
[[ -s "$progress" ]] || fail "progress stream missing"

python3 - "$progress" <<'PY' || fail "fixed-point progress stream is invalid"
import json, sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert events[0]["phase"] == "started"
assert events[-1]["phase"] == "finished"
assert events[-1]["stop_reason"] == "exact"
assert any(event["phase"] == "action_accepted" for event in events)
PY

python3 - "$report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
s = r["summary"]
assert r["ok"], r
assert s["solids_requiring_decomposition"] == 0, s
assert s["spline_faces"] == 0, s
PY

"$exe" compact "$log" --out "$recipe" >/dev/null 2>&1 \
    || fail "compaction failed"
recipe_json="${recipe%.lua}.json"
python3 - "$recipe_json" <<'PY' || fail "fixed-point recipe postconditions missing"
import json, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
assert r["postconditions"]["residual_count"] == 0
assert r["postconditions"]["active_piece_count"] == 2
assert r["postconditions"]["max_overlap_errors"] == 0
PY
if grep -q 's:export_step' "$recipe"; then
    fail "default compaction unexpectedly preserved STEP export"
fi
grep -q 's:decompose_by_face\|s:decompose_by_plane\|s:decompose_prismatic' "$recipe" \
    || fail "recipe contains no accepted decomposition operation"
sed "s#$deck#$replay_deck#" "$recipe" > "$outdir/fixed_point_replay.lua"
"$exe" lua "$outdir/fixed_point_replay.lua" >/dev/null 2>&1 \
    || fail "compacted recipe replay failed"
diff "$deck" "$replay_deck" >/dev/null 2>&1 \
    || fail "compacted recipe did not reproduce the deck"

preserved_recipe="$outdir/fixed_point_with_step.lua"
"$exe" compact "$log" --out "$preserved_recipe" \
    --preserve-step-exports >/dev/null 2>&1 \
    || fail "compaction with preserved STEP exports failed"
grep -q 's:export_step' "$preserved_recipe" \
    || fail "preserved recipe is missing STEP export"
preserved_json="${preserved_recipe%.lua}.json"
preserved_deck="$outdir/fixed_point_preserved_replay.i"
preserved_step="$outdir/fixed_point_preserved_replay.step"
"$exe" replay "$preserved_json" --out "$preserved_deck" \
    --out-step "$preserved_step" --report "$outdir/preserved_replay.json" \
    >/dev/null 2>&1 || fail "JSON replay with preserved STEP export failed"
[[ -s "$preserved_step" ]] || fail "preserved JSON replay STEP missing"
diff "$deck" "$preserved_deck" >/dev/null 2>&1 \
    || fail "preserved JSON replay did not reproduce the deck"
python3 - "$outdir/preserved_replay.json" <<'PY' \
    || fail "replay did not certify recipe postconditions"
import json, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
assert r["postconditions"] == {"checked": True, "passed": True}
PY

bad_recipe="$outdir/fixed_point_bad_postcondition.json"
rm -f "$outdir/should_not_emit.i" "$outdir/bad_postcondition_report.json"
python3 - "$preserved_json" "$bad_recipe" <<'PY'
import json, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
r["postconditions"]["active_piece_count"] += 1
json.dump(r, open(sys.argv[2], "w", encoding="utf-8"))
PY
if "$exe" replay "$bad_recipe" --out "$outdir/should_not_emit.i" \
    --report "$outdir/bad_postcondition_report.json" >/dev/null 2>&1; then
    fail "replay ignored a mismatched terminal geometry postcondition"
fi
[[ ! -e "$outdir/should_not_emit.i" ]] \
    || fail "replay emitted output before checking terminal postconditions"

box_deck="$outdir/fixed_point_box.i"
box_step="$outdir/fixed_point_box_decomposed.step"
rm -f "$box_deck" "$box_step"
"$exe" decompose-apply "$fixtures/box.step" --out "$box_deck" \
    --fixed-point >/dev/null 2>&1 \
    || fail "already-exact fixed-point run failed"
[[ -s "$box_deck" ]] || fail "already-exact deck missing"
[[ -s "$box_step" ]] || fail "default final STEP missing"

cat > "$outdir/fixed_point_api.lua" <<EOF
local s = csgify.session()
local d = s:load_step("$step")
local r = s:decompose_to_fixed_point(d, { max_actions = 4 })
assert(r.ok and r.exact and r.residual_count == 0)
EOF
"$exe" lua "$outdir/fixed_point_api.lua" >/dev/null 2>&1 \
    || fail "Lua fixed-point API failed"

echo "fixed_point_decomposition_smoke: ok"
