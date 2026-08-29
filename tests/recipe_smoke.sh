#!/usr/bin/env bash
#
# End-to-end smoke test for the recipe compactor. Runs simplify with
# --log, compacts the log into a Lua recipe, replays the recipe, and
# asserts that the replayed MCNP deck matches the original byte-for-byte.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "recipe_smoke: $*" >&2
    exit 1
}

log="$outdir/decisions.json"
recipe="$outdir/recipe.lua"
orig="$outdir/orig.i"
replay="$outdir/replay.i"

rm -f "$log" "$recipe" "$orig" "$replay"

"$exe" simplify "$fixtures/box.step" --replace-solid-bbox solid:1 \
    --out "$orig" --log "$log" >/dev/null 2>&1 \
    || fail "simplify with --log returned non-zero"

[[ -s "$log" ]] || fail "decision log was not produced"

# A versioned log must never be silently reinterpreted when an operation's
# contract has moved ahead of this binary.
future_log="$outdir/future-operation.json"
python3 - "$log" "$future_log" <<'PY' || exit 1
import sys
source, destination = sys.argv[1:]
text = open(source, encoding="utf-8").read()
text = text.replace('"operation_version":1', '"operation_version":999', 1)
open(destination, "w", encoding="utf-8").write(text)
PY
if "$exe" compact "$future_log" --out "$outdir/future.lua" \
    >/dev/null 2>"$outdir/future.err"; then
    fail "compact accepted an unsupported operation version"
fi
grep -q "unsupported operation version" "$outdir/future.err" \
    || fail "unsupported operation version did not fail clearly"

"$exe" compact "$log" --out "$recipe" >/dev/null 2>&1 \
    || fail "compact returned non-zero"

[[ -s "$recipe" ]] || fail "recipe file was not produced"

python3 - "${recipe%.lua}.json" <<'PY' || exit 1
import json, sys
recipe = json.load(open(sys.argv[1], encoding="utf-8"))
assert recipe.get("schema") == "csgify.recipe", recipe
assert recipe.get("schema_version") == 1, recipe
PY

# Rewrite the emit() path so the replay does not overwrite the original.
# Portable in-place sed (GNU/BSD differ on -i semantics).
python3 - "$recipe" "$orig" "$replay" <<'PY'
import sys
path, orig, replay = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, "r", encoding="utf-8") as f:
    content = f.read()
content = content.replace(orig, replay)
with open(path, "w", encoding="utf-8") as f:
    f.write(content)
PY

"$exe" lua "$recipe" >/dev/null 2>&1 \
    || fail "recipe replay (lua) returned non-zero"

[[ -s "$replay" ]] || fail "replay did not produce output deck"

diff -q "$orig" "$replay" >/dev/null \
    || fail "replayed deck differs from original"

# Spot-check the recipe content: it must mention the specific
# replace_solid_with_bbox method (not the generic apply_replacement_operation
# enum) and carry the component_entry as a comment.
grep -q "replace_solid_with_bbox" "$recipe" \
    || fail "recipe missing translated replace_solid_with_bbox call"
grep -q "component_entry" "$recipe" \
    || fail "recipe missing component_entry provenance comment"
grep -q "step_digest" "$recipe" \
    || fail "recipe missing step_digest in header"
grep -q "fingerprint:" "$recipe" \
    || fail "recipe missing fingerprint comment on shape op"

echo "recipe_smoke: ok"
