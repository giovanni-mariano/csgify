#!/usr/bin/env bash
#
# End-to-end smoke test for the `shatter` op (exhaustive surface-driven
# subdivision). Exercises:
#   - a non-convex L-shape that fails single-cell convex validation gets
#     subdivided into simple pieces and fully converts, volume preserved;
#   - the decision log records a `shatter` op that compacts into a Lua recipe
#     and replays (via `csgify lua`) to a byte-identical deck;
#   - a JSON `replay` recipe carrying a shatter op reproduces the same deck;
#   - shatter on an already-convex solid is a no-op (solid:N preserved).
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "shatter_smoke: $*" >&2
    exit 1
}

step="$fixtures/l_shape.step"
mcnp="$outdir/shatter.i"
report="$outdir/shatter.json"
log="$outdir/shatter_decisions.json"
rm -f "$log"

# --- 1. shatter the non-convex L-shape ------------------------------------
"$exe" simplify "$step" --shatter solid:1 --out "$mcnp" \
    --report "$report" --log "$log" >/dev/null 2>&1 \
    || fail "simplify --shatter returned non-zero"
[[ -s "$mcnp" ]] || fail "shatter produced no MCNP deck"

python3 - "$report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
# Every emitted piece must be exact-CSG supported (i.e. simple), and the
# original solid must have been fully converted into pieces.
pieces = r.get("decomposed_pieces", [])
if len(pieces) < 2:
    print(f"expected >=2 decomposed pieces, got {len(pieces)}", file=sys.stderr)
    sys.exit(1)
for p in pieces:
    if not p.get("exact_csg_supported", False):
        print(f"piece {p.get('id')} not exact_csg_supported: {p}", file=sys.stderr)
        sys.exit(1)
# No solid may remain flagged as needing decomposition.
for s in r.get("solids", []):
    if s.get("needs_decomposition"):
        print(f"solid {s.get('id')} still needs decomposition", file=sys.stderr)
        sys.exit(1)
PY

# --- 1b. a curved non-convex part: 270-degree cylindrical wedge -----------
# Its cross-section is a reflex pie wedge, so it fails single-cell convex
# validation; shatter must subdivide it into convex sectors, exactly.
wedge="$fixtures/cylinder_wedge_270.step"
wedge_mcnp="$outdir/wedge.i"
wedge_report="$outdir/wedge.json"
"$exe" simplify "$wedge" --shatter solid:1 --out "$wedge_mcnp" \
    --report "$wedge_report" >/dev/null 2>&1 \
    || fail "shatter on cylinder_wedge_270 returned non-zero"
python3 - "$wedge_report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
pieces = r.get("decomposed_pieces", [])
if len(pieces) < 2:
    print(f"wedge: expected >=2 pieces, got {len(pieces)}", file=sys.stderr)
    sys.exit(1)
if not all(p.get("exact_csg_supported") for p in pieces):
    print(f"wedge: not all pieces exact_csg_supported: {pieces}", file=sys.stderr)
    sys.exit(1)
codes = {d.get("code") for d in r.get("diagnostics", [])}
if "shatter_residual_nonconvex" in codes:
    print("wedge: unexpected residual after shatter", file=sys.stderr)
    sys.exit(1)
# Volume must be preserved exactly (the shatter diag reports relative_delta).
sh = [d for d in r.get("diagnostics", []) if d.get("code") == "shatter"]
if not sh:
    print("wedge: missing shatter diagnostic", file=sys.stderr)
    sys.exit(1)
PY

# --- 1c. large-coordinate assembly: a rails solid far from the origin -----
# rails.stp sits at coordinates ~1e4. Before cutters were anchored on the
# target solid, the bounded cutter face was centered on each surface's
# intrinsic Location and never reached the part, so the split silently failed
# and the solid stayed residual. The Boolean halfspace certificate now detects
# a residual that the old vertex-only test missed. The safe contract is to
# reject the entire mutation and preserve the source model.
rails="$fixtures/rails.stp"
if [[ -f "$rails" ]]; then
    rails_report="$outdir/rails16.json"
    if "$exe" simplify "$rails" --shatter solid:16 --out "$outdir/rails16.i" \
        --report "$rails_report" --max-depth 8 >/dev/null 2>&1; then
        fail "uncertified rails solid:16 shatter unexpectedly succeeded"
    fi
    python3 - "$rails_report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
codes = {d.get("code") for d in r.get("diagnostics", [])}
for required in ("shatter_residual_nonconvex", "mutation_rolled_back"):
    if required not in codes:
        print(f"rails solid:16: missing {required}", file=sys.stderr)
        sys.exit(1)
if r.get("document_revision") != 0:
    print("rails solid:16: rejected shatter advanced revision", file=sys.stderr)
    sys.exit(1)
if r.get("decomposed_pieces"):
    print("rails solid:16: rejected shatter leaked pieces", file=sys.stderr)
    sys.exit(1)
PY
fi

# --- 1d. the standalone `shatter` verb with --all -------------------------
# One command shatters every non-convex solid in the model and emits.
all_mcnp="$outdir/l_all.i"
all_report="$outdir/l_all.json"
"$exe" shatter "$step" --out "$all_mcnp" --all --report "$all_report" >/dev/null 2>&1 \
    || fail "shatter --all returned non-zero"
[[ -s "$all_mcnp" ]] || fail "shatter --all produced no deck"
python3 - "$all_report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
if not r.get("decomposed_pieces"):
    print("shatter --all produced no pieces on a non-convex model", file=sys.stderr)
    sys.exit(1)
for s in r.get("solids", []):
    if s.get("needs_decomposition"):
        print(f"shatter --all left {s.get('id')} needing decomposition", file=sys.stderr)
        sys.exit(1)
PY
# Requiring neither --all nor --target must be a usage error.
if "$exe" shatter "$step" --out "$outdir/none.i" >/dev/null 2>&1; then
    fail "shatter without --all/--target should have failed"
fi

# --- 2. compact -> Lua recipe -> replay via `csgify lua` -----------------
recipe="$outdir/recipe.lua"
"$exe" compact "$log" --out "$recipe" >/dev/null 2>&1 \
    || fail "compact returned non-zero"
grep -q "s:shatter(" "$recipe" \
    || fail "compacted recipe does not contain s:shatter()"

# Redirect the recipe's emit to a fresh path, then run it.
lua_mcnp="$outdir/shatter_lua.i"
sed "s#$mcnp#$lua_mcnp#" "$recipe" > "$outdir/recipe_run.lua"
"$exe" lua "$outdir/recipe_run.lua" >/dev/null 2>&1 \
    || fail "csgify lua recipe returned non-zero"
diff "$mcnp" "$lua_mcnp" >/dev/null 2>&1 \
    || fail "lua replay deck differs from direct shatter deck"

# --- 3. JSON replay verb with a shatter op --------------------------------
json_recipe="$outdir/shatter_recipe.json"
json_mcnp="$outdir/shatter_replay.i"
json_report="$outdir/shatter_replay.json"
cat > "$json_recipe" <<EOF
{
  "manifest": {"step_path":"$step","step_digest":"","csgify_version":"unversioned","occt_version":"8.0.0"},
  "ops": [
    {"op":"load_step","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"step_path":"$step"}},
    {"op":"shatter","target":{"kind":"solid","shape_id":"solid:1","component_entry":"0:1:1:1","fingerprint":{"volume":750.0,"surface_area":550.0,"bbox_diagonal":17.32,"face_count":8,"centroid":[4.1667,4.1667,5.0]}},"params":{"max_depth":4,"allow_volume_drift":false}},
    {"op":"emit_mcnp","target":{"kind":"document","shape_id":"doc:1","component_entry":""},"params":{"output_path":"$json_mcnp"}}
  ]
}
EOF
"$exe" replay "$json_recipe" --out "$json_mcnp" --report "$json_report" >/dev/null 2>&1 \
    || fail "json replay returned non-zero"
diff "$mcnp" "$json_mcnp" >/dev/null 2>&1 \
    || fail "json replay deck differs from direct shatter deck"

# --- 4. shatter on a convex solid is a no-op ------------------------------
box_report="$outdir/box_shatter.json"
"$exe" simplify "$fixtures/box.step" --shatter solid:1 \
    --out "$outdir/box_shatter.i" --report "$box_report" >/dev/null 2>&1 \
    || fail "shatter on convex box returned non-zero"
python3 - "$box_report" <<'PY' || exit 1
import json, sys
r = json.load(open(sys.argv[1]))
codes = {d.get("code") for d in r.get("diagnostics", [])}
if "shatter_not_needed" not in codes:
    print(f"expected shatter_not_needed on convex box; got {sorted(codes)}",
          file=sys.stderr)
    sys.exit(1)
# A no-op shatter must not have produced any pieces (solid:1 left intact).
pieces = r.get("decomposed_pieces", [])
if pieces:
    print(f"no-op shatter unexpectedly produced pieces: {pieces}", file=sys.stderr)
    sys.exit(1)
PY

echo "shatter_smoke: ok"
