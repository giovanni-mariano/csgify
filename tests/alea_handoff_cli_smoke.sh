#!/usr/bin/env bash
set -euo pipefail

bin=$1
fixtures=$2
out=$3
mkdir -p "$out"

"$bin" handoff "$fixtures/box.step" \
  --out "$out/handoff.json" \
  --report "$out/report.json" \
  --project "$out/project"

grep -q '"schema": "csgify.alea-handoff"' "$out/handoff.json"
grep -q '"contract_digest": "fnv1a128:' "$out/handoff.json"
grep -q '"kind": "alea_geometry_mcnp"' "$out/handoff.json"
grep -q '"compatible": true' "$out/report.json"
grep -q '"chunk_count": 1' "$out/report.json"
test -s "$out/project/project.sqlite"
find "$out/project/objects" -type f -name '*.json' -size +0c | grep -q .
find "$out/project/objects" -type f -name '*.i' -size +0c | grep -q .

"$bin" handoff --help | grep -q 'versioned csgify-to-libalea handoff'
