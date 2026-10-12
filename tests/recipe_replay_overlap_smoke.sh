#!/usr/bin/env bash
#
# recipe_replay_overlap_smoke: allow_overlaps round-trips through a recipe.
#
# A session that loads with { allow_overlaps = true } may downgrade overlap
# diagnostics for inspection/intermediate work, but the production MCNP gate
# must still reject the deck. The option survives compaction for compatibility
# without weakening final emission. This guards the whole chain:
#   1. load_step journals allow_overlaps; compaction keeps it on recipe.json.
#   2. `replay` of that recipe on overlapping geometry refuses (no deck).
#   3. a recipe WITHOUT the option likewise refuses.
#
# Also checks that export_step (a side-output) does not leak into the recipe.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "recipe_replay_overlap_smoke: $*" >&2; exit 1; }

bad="$fixtures/overlapping_boxes.step"
[ -f "$bad" ] || fail "fixture missing: $bad"
rm -f "$outdir/allow_log.json" "$outdir/deny_log.json"

# ---- build an allow_overlaps decision log, then compact ------------------
# attach_decision_log BEFORE load_step so load_step is journaled as the first
# op (it carries the option and declares the doc handle).
cat > "$outdir/allow_sess.lua" <<EOF
local s = csgify.session()
s:attach_decision_log("$outdir/allow_log.json", "$bad")
local doc = s:load_step("$bad", { allow_overlaps = true })
s:analyze(doc)
local ok = s:emit(doc, "$outdir/allow_sess.i")
if ok then error("production emit bypassed the hard overlap gate") end
s:export_step(doc, "$outdir/allow_sess.step")
EOF
"$exe" lua "$outdir/allow_sess.lua" >/dev/null 2>"$outdir/allow_sess.err" \
    || fail "allow_overlaps session failed: $(cat "$outdir/allow_sess.err")"

"$exe" compact "$outdir/allow_log.json" --out "$outdir/allow_recipe.lua" \
    >/dev/null 2>"$outdir/allow_compact.err" \
    || fail "compact failed: $(cat "$outdir/allow_compact.err")"
[ -f "$outdir/allow_recipe.json" ] \
    || fail "compact did not produce the recipe.json sibling"

# recipe.json load_step must carry allow_overlaps; export_step must be absent.
python3 - "$outdir/allow_recipe.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
ops = d["ops"]
loads = [o for o in ops if o["op"] == "load_step"]
assert loads, "recipe.json has no load_step op"
assert loads[0].get("params", {}).get("allow_overlaps") is True, loads[0]
assert not [o for o in ops if o["op"] == "export_step"], "export_step leaked into recipe"
PY
[ "$?" -eq 0 ] || fail "recipe.json did not round-trip allow_overlaps"

# recipe.lua should re-emit the option on load_step.
grep -q "allow_overlaps = true" "$outdir/allow_recipe.lua" \
    || fail "recipe.lua load_step is missing { allow_overlaps = true }"

# ---- replay the allow_overlaps recipe: still refuses a deck ---------------
rm -f "$outdir/allow_replay.i"
"$exe" replay "$outdir/allow_recipe.json" --step "$bad" \
    --out "$outdir/allow_replay.i" --report "$outdir/allow_replay.json" \
    >/dev/null 2>&1
status=$?
[ "$status" -ne 0 ] \
    || fail "allow_overlaps recipe bypassed production MCNP safety"
[ ! -e "$outdir/allow_replay.i" ] \
    || fail "allow_overlaps replay wrote an overlapping deck"

# ---- a recipe WITHOUT allow_overlaps refuses on replay -------------------
# The in-session emit fails (gate), so compaction keeps only load_step; the
# replay fallback emit then hits the live gate and must refuse.
cat > "$outdir/noallow_sess.lua" <<EOF
local s = csgify.session()
s:attach_decision_log("$outdir/noallow_log.json", "$bad")
local doc = s:load_step("$bad")
s:analyze(doc)
s:emit(doc, "$outdir/noallow_sess.i")
EOF
"$exe" lua "$outdir/noallow_sess.lua" >/dev/null 2>&1 || true
"$exe" compact "$outdir/noallow_log.json" --out "$outdir/noallow_recipe.lua" \
    >/dev/null 2>&1 || fail "compact of no-flag log failed"

rm -f "$outdir/noallow_replay.i"
"$exe" replay "$outdir/noallow_recipe.json" --step "$bad" \
    --out "$outdir/noallow_replay.i" --report "$outdir/noallow_replay.json" \
    >/dev/null 2>&1
status=$?
[ "$status" -ne 0 ] \
    || fail "replay without allow_overlaps should refuse overlapping geometry"
[ ! -e "$outdir/noallow_replay.i" ] \
    || fail "replay without allow_overlaps wrote a deck despite the overlap"

echo "recipe_replay_overlap_smoke: OK"
exit 0
