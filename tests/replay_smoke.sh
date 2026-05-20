#!/usr/bin/env bash
#
# End-to-end smoke test for `cad2csg replay`. Builds a recipe.json that
# exercises all three target-resolution outcomes against box.step's single
# solid:
#   - applied:      correct component_entry
#   - soft_applied: wrong component_entry, but matching fingerprint
#   - unmatched:    wrong component_entry, very wrong fingerprint
#
# Asserts the replay report categorizes each correctly.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "replay_smoke: $*" >&2
    exit 1
}

step="$fixtures/box.step"
recipe="$outdir/recipe.json"
out_applied="$outdir/applied.i"
out_soft="$outdir/soft.i"
out_unmatched="$outdir/unmatched.i"
report_applied="$outdir/applied.json"
report_soft="$outdir/soft.json"
report_unmatched="$outdir/unmatched.json"

# --- applied: hand-craft a recipe.json with the correct component_entry.
# box.step has solid:1 with component_entry "0:1:1:1" (verified in
# decision_log_smoke). We don't need a real fingerprint for the primary
# path, but we include one so the JSON shape matches the compactor's.
cat > "$recipe" <<EOF
{
  "manifest": {"step_path":"$step","step_digest":"","cad2csg_version":"unversioned","occt_version":"8.0.0"},
  "ops": [
    {"op":"load_step","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"step_path":"$step"}},
    {"op":"replace_solid_with_bbox","target":{"kind":"solid","shape_id":"solid:1","component_entry":"0:1:1:1","fingerprint":{"volume":100.0,"surface_area":160.0,"bbox_diagonal":11.358,"face_count":6,"centroid":[5.0,2.5,1.0]}},"params":{}},
    {"op":"emit_mcnp","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"output_path":"$out_applied"}}
  ]
}
EOF

step_applied="$outdir/applied.step"
rm -f "$step_applied"
"$exe" replay "$recipe" --out "$out_applied" --report "$report_applied" \
    --out-step "$step_applied" >/dev/null 2>&1 \
    || fail "replay (applied case) returned non-zero"
[[ -s "$step_applied" ]] \
    || fail "replay --out-step did not produce a STEP file"

python3 - "$report_applied" applied <<'PY' || exit 1
import json, sys
report = json.load(open(sys.argv[1]))
expected = sys.argv[2]
ops = [o for o in report["ops"] if o["target_kind"] == "solid"]
if len(ops) != 1:
    print(f"expected 1 solid op, got {len(ops)}", file=sys.stderr)
    sys.exit(1)
if ops[0]["outcome"] != expected:
    print(f"expected {expected!r}, got {ops[0]['outcome']!r}: {ops[0]}",
          file=sys.stderr)
    sys.exit(1)
PY

# --- soft_applied: wrong component_entry, matching fingerprint.
cat > "$recipe" <<EOF
{
  "manifest": {"step_path":"$step","step_digest":"","cad2csg_version":"unversioned","occt_version":"8.0.0"},
  "ops": [
    {"op":"load_step","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"step_path":"$step"}},
    {"op":"replace_solid_with_bbox","target":{"kind":"solid","shape_id":"solid:99","component_entry":"0:9:9:9","fingerprint":{"volume":100.0,"surface_area":160.0,"bbox_diagonal":11.358,"face_count":6,"centroid":[5.0,2.5,1.0]}},"params":{}},
    {"op":"emit_mcnp","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"output_path":"$out_soft"}}
  ]
}
EOF

# soft_applied is still a non-error outcome at the op level but the
# overall replay command returns non-zero because the report includes a
# non-applied entry. The smoke test cares about the categorization, not
# the exit code, so allow either.
"$exe" replay "$recipe" --out "$out_soft" --report "$report_soft" >/dev/null 2>&1 || true

python3 - "$report_soft" soft_applied <<'PY' || exit 1
import json, sys
report = json.load(open(sys.argv[1]))
expected = sys.argv[2]
ops = [o for o in report["ops"] if o["target_kind"] == "solid"]
if len(ops) != 1:
    print(f"expected 1 solid op, got {len(ops)}", file=sys.stderr)
    sys.exit(1)
if ops[0]["outcome"] != expected:
    print(f"expected {expected!r}, got {ops[0]['outcome']!r}: {ops[0]}",
          file=sys.stderr)
    sys.exit(1)
PY

# --- unmatched: wrong component_entry AND a very wrong fingerprint.
cat > "$recipe" <<EOF
{
  "manifest": {"step_path":"$step","step_digest":"","cad2csg_version":"unversioned","occt_version":"8.0.0"},
  "ops": [
    {"op":"load_step","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"step_path":"$step"}},
    {"op":"replace_solid_with_bbox","target":{"kind":"solid","shape_id":"solid:99","component_entry":"0:9:9:9","fingerprint":{"volume":12345.0,"surface_area":67890.0,"bbox_diagonal":999.0,"face_count":42,"centroid":[1000.0,2000.0,3000.0]}},"params":{}},
    {"op":"emit_mcnp","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"output_path":"$out_unmatched"}}
  ]
}
EOF

"$exe" replay "$recipe" --out "$out_unmatched" --report "$report_unmatched" >/dev/null 2>&1 || true

python3 - "$report_unmatched" unmatched <<'PY' || exit 1
import json, sys
report = json.load(open(sys.argv[1]))
expected = sys.argv[2]
ops = [o for o in report["ops"] if o["target_kind"] == "solid"]
if len(ops) != 1:
    print(f"expected 1 solid op, got {len(ops)}", file=sys.stderr)
    sys.exit(1)
if ops[0]["outcome"] != expected:
    print(f"expected {expected!r}, got {ops[0]['outcome']!r}: {ops[0]}",
          file=sys.stderr)
    sys.exit(1)

# new_solids_unaddressed should list the actual solid:1 (its entry was
# never matched by the bogus recipe op).
if not any("0:1:1:1" in s for s in report["new_solids_unaddressed"]):
    print(f"expected solid:1 listed in new_solids_unaddressed; got "
          f"{report['new_solids_unaddressed']}", file=sys.stderr)
    sys.exit(1)
PY

echo "replay_smoke: ok"
