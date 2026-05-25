#!/usr/bin/env bash
#
# explain_smoke: verify the `explain` command surfaces blocker reasons
# and exit codes that reflect whether the target is emit-ready.

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "explain_smoke: $*" >&2; exit 1; }

# ---- explain on a ready solid: exit 0, mentions ready --------------------
"$exe" explain solid:1 "$fixtures/box.step" >"$outdir/box.out" 2>"$outdir/box.err"
status=$?
[ "$status" -eq 0 ] || fail "explain ready solid exit=$status (expected 0)"
grep -q "ready:.*yes" "$outdir/box.out" || fail "missing 'ready: yes' for box.step"

# ---- explain on a spline solid: exit 1, lists spline blockers ------------
"$exe" explain solid:1 "$fixtures/spline_face.step" >"$outdir/spline.out" 2>"$outdir/spline.err"
status=$?
[ "$status" -eq 1 ] || fail "explain spline solid exit=$status (expected 1)"
grep -q "ready:.*no" "$outdir/spline.out" || fail "missing 'ready: no' for spline_face.step"
grep -q "spline" "$outdir/spline.out" || fail "missing 'spline' blocker mention"
grep -q "Blocking faces" "$outdir/spline.out" \
    || fail "missing 'Blocking faces' section"

# ---- explain on a face id: surface candidates surfaced -------------------
"$exe" explain face:1.1 "$fixtures/spline_face.step" >"$outdir/face.out" 2>"$outdir/face.err"
status=$?
# exit code may be 0 (if face is somehow analytic) or 1 (more likely); we
# don't assert here -- only that the output looks like a face explanation.
grep -q "kind:.*face" "$outdir/face.out" || fail "face explain missing kind:face"
grep -q "type:" "$outdir/face.out" || fail "face explain missing 'type:' line"

# ---- explain on unknown id: exit 1, error message ------------------------
"$exe" explain solid:99 "$fixtures/box.step" >"$outdir/missing.out" 2>"$outdir/missing.err"
status=$?
[ "$status" -ne 0 ] || fail "explain unknown solid should exit nonzero"
grep -q "explain_target_not_found" "$outdir/missing.err" \
    || fail "missing explain_target_not_found diagnostic"

# ---- explain --help shows the verb help ----------------------------------
"$exe" explain --help >"$outdir/help.out" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "explain --help exit=$status"
grep -q "Explain why" "$outdir/help.out" || fail "explain --help missing description"

echo "explain_smoke: OK"
exit 0
