#!/usr/bin/env bash
#
# units_smoke: verifies STEP length-unit reporting and the override
# flags introduced by Workstream 2 of UX_AND_FEATURES_IMPLEMENTATION_PLAN.md.
#
# Coverage:
#   - inspect reports a `units` block in JSON
#   - the human summary mentions the unit
#   - --units overrides the reported unit name (no geometry change)
#   - --scale overrides the to-mm factor (no geometry change)
#   - --normalize-units actually rescales geometry and updates the report
#   - a decision-log entry records the rescale

set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() { echo "units_smoke: $*" >&2; exit 1; }

step="$fixtures/box.step"
[ -f "$step" ] || fail "fixture missing: $step"

# ---- JSON report has a units block --------------------------------------
report="$outdir/box.json"
"$exe" inspect "$step" --report "$report" >/dev/null 2>"$outdir/box.err" \
    || fail "inspect failed: $(cat "$outdir/box.err")"

python3 - "$report" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
u = d.get("units")
assert u, "units block missing from JSON report"
assert "length_unit" in u, "units.length_unit missing"
assert "length_unit_to_mm" in u, "units.length_unit_to_mm missing"
# box.step is exported as mm; OCCT normalizes to mm regardless.
assert u["length_unit"] == "mm", f"expected mm, got {u['length_unit']!r}"
assert abs(u["length_unit_to_mm"] - 1.0) < 1e-9, u
PY
[ "$?" -eq 0 ] || fail "JSON units validation failed"

# ---- human summary names the unit ----------------------------------------
human="$outdir/box.human"
"$exe" inspect "$step" >"$human" 2>"$outdir/box.human.err"
grep -q "units:" "$human" || fail "human summary missing 'units:' line"
grep -q "volume:.*mm\^3" "$human" || fail "human summary missing 'mm^3' on volume"

# ---- --units override changes the reported name --------------------------
override="$outdir/box_in.json"
"$exe" inspect "$step" --units in --report "$override" >/dev/null 2>&1 \
    || fail "inspect --units in failed"

python3 - "$override" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
assert d["units"]["length_unit"] == "in", d["units"]
assert abs(d["units"]["length_unit_to_mm"] - 25.4) < 1e-9, d["units"]
# Override must surface as a diagnostic, not as a silent rewrite.
codes = [diag["code"] for diag in d["diagnostics"]]
assert "unit_override" in codes, f"missing unit_override diagnostic; got {codes}"
PY
[ "$?" -eq 0 ] || fail "--units override validation failed"

# ---- --scale override updates the factor without renaming ----------------
scaled="$outdir/box_scale.json"
"$exe" inspect "$step" --scale 2.5 --report "$scaled" >/dev/null 2>&1 \
    || fail "inspect --scale 2.5 failed"

python3 - "$scaled" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
assert abs(d["units"]["length_unit_to_mm"] - 2.5) < 1e-9, d["units"]
codes = [diag["code"] for diag in d["diagnostics"]]
assert "unit_scale_override" in codes, f"missing unit_scale_override; got {codes}"
PY
[ "$?" -eq 0 ] || fail "--scale override validation failed"

# ---- --normalize-units rescales geometry; volume changes by factor^3 ------
# box.step's volume is 100 mm^3. Rescaling mm -> cm divides each coord by
# 10, so the new volume should be 100 cm^3 = 0.1 (in the new unit, which
# is what report.total_volume now reports since we rescaled the
# geometry).
norm="$outdir/box_norm.json"
"$exe" inspect "$step" --normalize-units cm --report "$norm" \
    >/dev/null 2>"$outdir/box_norm.err" \
    || fail "inspect --normalize-units cm failed"

python3 - "$norm" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
u = d["units"]
assert u["length_unit"] == "cm", u
assert abs(u["length_unit_to_mm"] - 10.0) < 1e-9, u
# Volume must have shrunk by 1000x (1 cm^3 = 1000 mm^3).
vol = d["summary"]["total_volume"]
assert abs(vol - 0.1) < 1e-6, f"expected 0.1 cm^3, got {vol}"
codes = [diag["code"] for diag in d["diagnostics"]]
assert "unit_normalized" in codes, f"missing unit_normalized; got {codes}"
PY
[ "$?" -eq 0 ] || fail "--normalize-units validation failed"

# ---- normalize logged into decision log ----------------------------------
logged_log="$outdir/log.json"
rm -f "$logged_log"
"$exe" inspect "$step" --normalize-units m \
    --log "$logged_log" --report "$outdir/box_norm_m.json" \
    >/dev/null 2>&1 || fail "inspect --normalize-units m --log failed"
[ -s "$logged_log" ] || fail "decision log not written"
# The log should mention the normalization in some form. We don't
# enforce a strict schema here -- just that the log is non-empty and
# the report records the change.
python3 - "$outdir/box_norm_m.json" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as h:
    d = json.load(h)
codes = [diag["code"] for diag in d["diagnostics"]]
assert "unit_normalized" in codes, codes
PY
[ "$?" -eq 0 ] || fail "decision log + normalize report validation failed"

echo "units_smoke: OK"
exit 0
