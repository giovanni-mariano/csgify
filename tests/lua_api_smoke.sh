#!/usr/bin/env bash
# lua_api_smoke: runs tests/lua_api.lua and asserts a clean exit.
#
# The Lua script (tests/lua_api.lua) covers the Lua-binding fixes:
#   #1 CommandResult.ok semantics (analyze + mutating ops)
#   #2 attach_decision_log exposed
#   #3 ConversionOptions accepted by session() and load_step()
#   #4 push_report_table exposes M2 + replacement_candidates fields
#   #5 unified table return shape
#   #8 ReplacementCheck visible via .check
#   #9 export_step writes a STEP file

set -u

exe="$1"
script="$2"
fixtures="$3"
outdir="$4"

mkdir -p "$outdir"

"$exe" lua "$script" "$fixtures" "$outdir" 2>&1
status=$?
if [ "$status" -ne 0 ]; then
    echo "lua_api_smoke: lua script exited $status" >&2
    exit 1
fi
