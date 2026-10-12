#!/usr/bin/env bash
#
# Smoke test for the append-only decision log. See
# docs/DECISION_LOG_AND_RECIPE.md. Verifies that a simplify command with
# --log produces a valid JSON file containing the expected records.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "decision_log_smoke: $*" >&2
    exit 1
}

log="$outdir/decisions.json"
out="$outdir/simplified.i"
rm -f "$log"

"$exe" simplify "$fixtures/box.step" --replace-solid-bbox solid:1 \
    --out "$out" --log "$log" >/dev/null 2>&1 \
    || fail "simplify with --log returned non-zero"

[[ -s "$log" ]] || fail "decision log file is missing or empty"

python3 - "$log" <<'PY' || exit 1
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    records = [json.loads(line) for line in handle if line.strip()]

if not records or records[0].get("kind") != "manifest":
    print("first record must be manifest", file=sys.stderr)
    sys.exit(1)

manifest = records[0]
if manifest.get("schema") != "csgify.decision-log" or manifest.get("schema_version") != 1:
    print(f"unexpected decision-log schema: {manifest}", file=sys.stderr)
    sys.exit(1)
for required in ("csgify_version", "occt_version", "step_path",
                 "step_digest", "created_at"):
    if required not in manifest:
        print(f"manifest missing field: {required}", file=sys.stderr)
        sys.exit(1)

if not manifest["csgify_version"] or manifest["csgify_version"] == "unversioned":
    print(f"decision log has no concrete csgify version: {manifest}",
          file=sys.stderr)
    sys.exit(1)

if not manifest["step_digest"].startswith("fnv1a128:"):
    print(f"step_digest missing algorithm prefix: {manifest['step_digest']}",
          file=sys.stderr)
    sys.exit(1)

ops = [r.get("op") for r in records[1:] if r.get("kind") == "decision"]
expected_ops = {"load_step", "apply_replacement_operation", "emit_mcnp"}
missing = expected_ops - set(ops)
if missing:
    print(f"missing expected ops: {sorted(missing)} (saw {ops})", file=sys.stderr)
    sys.exit(1)

# Every decision record must carry a target with kind + shape_id; solid
# decisions must additionally have a non-empty component_entry so replay
# can match across CAD revisions.
for r in records[1:]:
    if r.get("kind") != "decision":
        continue
    if r.get("operation_version") != 1:
        print(f"decision has unknown operation version: {r}", file=sys.stderr)
        sys.exit(1)
    target = r.get("target") or {}
    if not target.get("kind") or "shape_id" not in target:
        print(f"record missing target fields: {r}", file=sys.stderr)
        sys.exit(1)
    if target.get("kind") == "solid" and not target.get("component_entry"):
        print(f"solid decision missing component_entry: {r}", file=sys.stderr)
        sys.exit(1)
    # Soft-match identity for replay: every solid decision must carry a
    # geometric fingerprint with the canonical fields.
    if target.get("kind") == "solid":
        fp = target.get("fingerprint")
        if not fp:
            print(f"solid decision missing fingerprint: {r}", file=sys.stderr)
            sys.exit(1)
        for field in ("volume", "surface_area", "bbox_diagonal",
                      "face_count", "centroid"):
            if field not in fp:
                print(f"fingerprint missing {field}: {fp}", file=sys.stderr)
                sys.exit(1)
        if not isinstance(fp["centroid"], list) or len(fp["centroid"]) != 3:
            print(f"fingerprint.centroid must be [x,y,z]: {fp}", file=sys.stderr)
            sys.exit(1)
PY

# Digest stability: re-running against the same STEP must produce the
# same step_digest. (The created_at timestamp will differ; the digest
# must not.)
log2="$outdir/decisions2.json"
rm -f "$log2"
"$exe" simplify "$fixtures/box.step" --replace-solid-bbox solid:1 \
    --out "$outdir/simplified2.i" --log "$log2" >/dev/null 2>&1 \
    || fail "second simplify run failed"

python3 - "$log" "$log2" <<'PY' || exit 1
import json, sys
with open(sys.argv[1]) as a, open(sys.argv[2]) as b:
    d1 = json.loads(next(line for line in a if line.strip()))["step_digest"]
    d2 = json.loads(next(line for line in b if line.strip()))["step_digest"]
if d1 != d2:
    print(f"step_digest unstable: {d1} != {d2}", file=sys.stderr)
    sys.exit(1)
PY

echo "decision_log_smoke: ok"
