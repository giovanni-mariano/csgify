#!/usr/bin/env bash
#
# Smoke test for the CLI usability fixes:
#   - `help` / `--help` / `-h` exit 0 with usage on stdout
#   - per-verb `<verb> --help` shows that verb's options + mutation contract
#   - unknown verbs print did-you-mean and exit 2
#   - `inspect input.step` (no --report) works and prints a human summary
#   - `--report -` writes JSON to stdout

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "help_smoke: $*" >&2
    exit 1
}

# ---- top-level help on stdout exits 0 ----------------------------------------
out="$outdir/help_top.txt"
err="$outdir/help_top.err"
"$exe" help >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`help\` exited $status (expected 0)"
grep -q "Verbs:" "$out" || fail "\`help\` stdout missing 'Verbs:' section"
grep -q "inspect" "$out" || fail "\`help\` stdout missing 'inspect' verb"

"$exe" --help >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`--help\` exited $status (expected 0)"
grep -q "Verbs:" "$out" || fail "\`--help\` stdout missing 'Verbs:' section"

"$exe" -h >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`-h\` exited $status (expected 0)"

# ---- per-verb help on stdout exits 0 -----------------------------------------
"$exe" simplify --help >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`simplify --help\` exited $status"
grep -q -- "--replace-solid-bbox" "$out" \
    || fail "\`simplify --help\` missing --replace-solid-bbox option"
grep -q "Mutation contract" "$out" \
    || fail "\`simplify --help\` missing Mutation contract section"

"$exe" help inspect >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`help inspect\` exited $status"
grep -q -- "--report PATH" "$out" || fail "\`help inspect\` missing --report PATH"

"$exe" preprocess -h >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`preprocess -h\` exited $status"
grep -q "Mutation contract" "$out" \
    || fail "\`preprocess -h\` missing Mutation contract section"

# ---- unknown verb: did-you-mean + exit 2 -------------------------------------
"$exe" inspct foo.step >"$out" 2>"$err"
status=$?
[ "$status" -eq 2 ] || fail "unknown verb 'inspct' exited $status (expected 2)"
grep -q "unknown_command" "$err" \
    || fail "unknown verb did not report 'unknown_command'"
grep -q "did you mean 'inspect'" "$err" \
    || fail "unknown verb did not suggest 'inspect'"

"$exe" not-a-verb >"$out" 2>"$err"
status=$?
[ "$status" -eq 2 ] || fail "unknown verb 'not-a-verb' exited $status (expected 2)"

# ---- inspect without --report: stdout summary, exit 0 for clean STEP ---------
"$exe" inspect "$fixtures/box.step" >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`inspect box.step\` exited $status (expected 0)"
grep -q "solid:1" "$out" || fail "\`inspect box.step\` stdout missing solid:1"
grep -q "volume:" "$out" || fail "\`inspect box.step\` stdout missing volume line"
# JSON should NOT appear on stdout when --report is omitted
grep -q '^{' "$out" && fail "\`inspect box.step\` stdout contained JSON (should be human)"

# ---- --report - routes JSON to stdout ----------------------------------------
"$exe" inspect "$fixtures/box.step" --report - >"$out" 2>"$err"
status=$?
[ "$status" -eq 0 ] || fail "\`inspect --report -\` exited $status (expected 0)"
head -1 "$out" | grep -q '^{' \
    || fail "\`inspect --report -\` first stdout line is not JSON"
python3 -m json.tool "$out" >/dev/null \
    || fail "\`inspect --report -\` stdout is not valid JSON"

echo "help_smoke: OK"
exit 0
