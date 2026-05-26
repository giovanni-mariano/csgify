#!/usr/bin/env bash
# lua_coverage_smoke: verifies the Lua bindings cover every CLI verb
# (s:explain, cad2csg.compact, cad2csg.replay, s:write_decision_pack,
# s:preprocess_safe_all). The audit lives in docs/AGENT_WORKFLOW.md.

set -u

exe="$1"
script="$2"
fixtures="$3"
outdir="$4"

mkdir -p "$outdir"

"$exe" lua "$script" "$fixtures" "$outdir" 2>&1
status=$?
if [ "$status" -ne 0 ]; then
    echo "lua_coverage_smoke: lua script exited $status" >&2
    exit 1
fi
