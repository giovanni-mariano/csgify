#!/usr/bin/env bash
#
# Smoke test for the decision-log-backed viewer session (Plan A) and its
# headless decision controller (point-5 refactor).
#
# `csgify view --keys "..."` replays a key string through the SAME
# ViewerController the X11 event loop drives -- with no window, no display, no
# TKOpenGl. Accepting a candidate calls the same session mutators the CLI uses
# on a session with a decision log attached, so a visual decision is logged and
# replayable instead of being a printed string nobody consumes.
#
# This exercises the viewer's actual decision code (remove_target / accept /
# reject / cycle / commit), not an analogue. The only untested remainder is the
# X11 keysym -> controller-call mapping in src/viewer.cpp.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "viewer_session_smoke: $*" >&2; exit 1; }

# --- 1. headless R-removals: drive the controller, then replay --------------
step="$fixtures/four_solids.step"
log="$outdir/decisions.json"
direct="$outdir/headless.i"
rm -f "$log" "$direct" "$outdir/stdout.txt"

# R:<id> removes that solid; Q commits (emits the deck because --out is set).
"$exe" view "$step" \
    --keys "R:doc:1/solid:2,R:doc:1/solid:4,Q" \
    --log "$log" --out "$direct" >"$outdir/stdout.txt" 2>/dev/null \
    || fail "headless view --keys returned non-zero"
[[ -s "$direct" ]] || fail "headless session emitted no deck"
[[ -s "$log" ]]    || fail "no decision log written"

# Each accepted removal must print a machine 'logged ...' line on stdout.
got=$(grep -c '^logged remove_solid ' "$outdir/stdout.txt")
[[ "$got" == "2" ]] || fail "expected 2 'logged remove_solid' lines, got $got"

removals=$(grep -o '"op":"remove_solid"' "$log" | wc -l | tr -d ' ')
[[ "$removals" == "2" ]] || fail "expected 2 logged removals, got $removals"

# --- 2. the logged session compacts and replays byte-identically ------------
recipe="$outdir/recipe.lua"
"$exe" compact "$log" --out "$recipe" >/dev/null 2>&1 \
    || fail "compact returned non-zero"
[[ "$(grep -c remove_solid "$recipe")" == "2" ]] \
    || fail "recipe missing the two removals"

replay="$outdir/replay.i"
sed "s#$direct#$replay#" "$recipe" > "$outdir/recipe_run.lua"
"$exe" lua "$outdir/recipe_run.lua" >/dev/null 2>&1 \
    || fail "recipe replay returned non-zero"
diff "$direct" "$replay" >/dev/null 2>&1 \
    || fail "replayed deck differs from the logged-session deck"

# --- 3. removal-set state machine: Tab / reject / accept --------------------
# Only run if the private rails fixture (which has removal candidates) exists.
rails="$fixtures/rails.stp"
if [[ -f "$rails" ]]; then
    rlog="$outdir/rails.json"
    rm -f "$rlog"
    # Reject the first candidate, skip to the third, accept it: exactly one
    # removal must be logged, and it must NOT be the rejected one.
    out=$("$exe" view "$rails" --review-removals \
            --keys "X,Tab,A,Q" --log "$rlog" 2>&1)
    n=$(grep -o '"op":"remove_solid"' "$rlog" | wc -l | tr -d ' ')
    [[ "$n" == "1" ]] || fail "review-removals X,Tab,A expected 1 removal, got $n"
    echo "$out" | grep -q '^logged remove_solid ' \
        || fail "review-removals accept printed no 'logged' line"
    # The rejected first candidate must NOT have been removed.
    echo "$out" | grep -q 'rejected removal:1' \
        || fail "review-removals did not report rejecting the first candidate"
fi

echo "viewer_session_smoke: ok"
