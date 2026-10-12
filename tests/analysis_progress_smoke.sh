#!/usr/bin/env bash
set -euo pipefail

CSGIFY=$1
FIXTURE=$2
OUT=$3
mkdir -p "$OUT"
EVENTS="$OUT/analysis-events.jsonl"
REPORT="$OUT/analysis-report.json"

"$CSGIFY" inspect "$FIXTURE" --progress-json "$EVENTS" --report "$REPORT"
test -s "$EVENTS"
test -s "$REPORT"

python3 - "$EVENTS" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert events
assert [event["sequence"] for event in events] == list(range(1, len(events) + 1))
assert sum(event["phase"] == "finished" for event in events) == 1
assert events[-1]["phase"] == "finished"
assert events[-1]["outcome"] == "committed"
assert any(event["phase"] == "analyzing_solids" and event["completed"] > 0
           and event["total_known"] for event in events)
assert any(event["phase"] == "checking_overlaps" for event in events)
PY
