#!/usr/bin/env bash
#
# Scaffolding smoke test for the surface-splitting work track. The
# tilted_torus fixture has a single solid whose only face is a torus on
# axis (1,1,1)/sqrt(3) — explicitly not axis-aligned. cad2csg currently
# cannot convert this to MCNP; the test pins the agent-facing contract
# that must remain stable while the surface-splitter is being built:
#
#   - inspect produces `unsupported_torus_orientation` diagnostic on the
#     face, with the new action vocabulary including `split_face_piecewise`
#     and `decompose_along_torus_axis_plane`.
#   - decompose-suggest on the bounding solid surfaces a candidate whose
#     tool kind is `torus_axis_plane`, regardless of whether the apply
#     path succeeds today.
#
# Once the piecewise splitter lands, this test grows: it should then
# assert that the apply path actually converts the model. For now it
# pins the diagnostic surface only.
set -u

exe="$1"
fixtures="$2"
outdir="$3"

mkdir -p "$outdir"

fail() {
    echo "tilted_torus_smoke: $*" >&2
    exit 1
}

report="$outdir/inspect.json"
proposal="$outdir/proposal.json"
rm -f "$report" "$proposal"

# inspect must surface the diagnostic and the new actions. inspect exits
# non-zero when the report is not ok (which is the case here — the model
# does not convert). That non-zero exit is expected behavior.
"$exe" inspect "$fixtures/tilted_torus.step" --report "$report" >/dev/null 2>&1 || true
[[ -s "$report" ]] || fail "inspect report was not produced"

python3 - "$report" <<'PY' || exit 1
import json, sys
with open(sys.argv[1]) as f:
    report = json.load(f)

diagnostics = report.get("diagnostics") or []
target = [d for d in diagnostics
          if d.get("code") == "unsupported_torus_orientation"]
if not target:
    print(f"expected unsupported_torus_orientation diagnostic; got codes: "
          f"{[d.get('code') for d in diagnostics]}", file=sys.stderr)
    sys.exit(1)

actions = target[0].get("actions") or []
required = {
    "decompose_along_torus_axis_plane",
    "replace_with_cylinder",
    "split_face_piecewise",
}
missing = required - set(actions)
if missing:
    print(f"diagnostic actions missing {sorted(missing)}; got {actions}",
          file=sys.stderr)
    sys.exit(1)
PY

# decompose-suggest must offer a torus_axis_plane candidate. The model
# does not currently convert post-split, so the command's exit code is
# allowed to be non-zero — the test only inspects the proposal.
"$exe" decompose-suggest "$fixtures/tilted_torus.step" solid:1 \
    --report "$proposal" >/dev/null 2>&1 || true
[[ -s "$proposal" ]] || fail "decompose-suggest proposal was not produced"

python3 - "$proposal" <<'PY' || exit 1
import json, sys
with open(sys.argv[1]) as f:
    proposal = json.load(f)

candidates = proposal.get("candidates") or []
torus_candidates = [c for c in candidates
                    if c.get("tool_kind") == "torus_axis_plane"]
if not torus_candidates:
    kinds = sorted({c.get("tool_kind") for c in candidates})
    print(f"no torus_axis_plane candidate proposed; tool_kinds seen: "
          f"{kinds}", file=sys.stderr)
    sys.exit(1)

# Surfaces the candidate offers must reference face:1.1 (the only face on
# the tilted torus solid).
if not any(c.get("source_face_id") == "face:1.1" for c in torus_candidates):
    print(f"no torus_axis_plane candidate references face:1.1: "
          f"{torus_candidates}", file=sys.stderr)
    sys.exit(1)
PY

echo "tilted_torus_smoke: ok"
