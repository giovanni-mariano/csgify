#!/usr/bin/env bash
set -u

exe="$1"
script="$2"
fixture="$3"
out="$4"

"$exe" lua "$script" "$fixture" > "$out" 2>/dev/null || {
    echo "lua_smoke: script failed" >&2
    exit 1
}

python3 - "$out" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    report = json.load(handle)

assert report["document"]["id"] == "doc:1", report.get("document")
assert report["solids"][0]["id"] == "doc:1/solid:1", report["solids"][0]
PY
