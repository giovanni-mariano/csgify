#!/usr/bin/env bash
set -euo pipefail

bin=$1
fixtures=$2
out=$3
rm -rf "$out"
mkdir -p "$out"

common=(
  --project "$out/project"
  --max-depth 4
  --min-size 0.5
  --workers 1
  --scratch-bytes 65536
  --samples 5
  --domain-id domain:box-test
  --bounds "-1,-1,-1,11,6,3"
)

"$bin" voids "$fixtures/box.step" \
  --out "$out/coverage-first.json" \
  --out-mcnp "$out/geometry-first.i" \
  "${common[@]}" \
  2>"$out/first.log"

grep -q '"schema": "csgify.alea-coverage-certificate"' \
  "$out/coverage-first.json"
grep -q '"status": "certified"' "$out/coverage-first.json"
grep -q '"id": "domain:box-test"' "$out/coverage-first.json"
grep -q '"structural_exact_complement": true' "$out/coverage-first.json"
grep -q '"sample_count": 125' "$out/coverage-first.json"
grep -q 'stage alea_void_checkpoint: completed' "$out/first.log"
test -s "$out/geometry-first.i"
test -s "$out/project/project.sqlite"

"$bin" project status "$out/project" >"$out/project-status.txt"
grep -q '^Jobs: queued=0, running=0, succeeded=1, failed=0, cancelled=0$' \
  "$out/project-status.txt"
job_id=$(awk '$1 == "job" { print $2; exit }' "$out/project-status.txt")
test -n "$job_id"
"$bin" project events "$out/project" --job "$job_id" \
  >"$out/project-events.jsonl"
python3 - "$out/project-events.jsonl" "$job_id" <<'PY'
import json
import sys

path, job_id = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    events = [json.loads(line) for line in stream if line.strip()]
assert events
assert all(event["job_id"] == job_id for event in events)
assert [event["sequence"] for event in events] == list(range(1, len(events) + 1))
assert sum(event["phase"] == "finished" for event in events) == 1
assert events[-1]["outcome"] == "committed"
assert events[-1]["operation"] == "alea_void_generation"
assert events[-1]["artifact_id"]
assert {event["phase"] for event in events} >= {
    "certifying_solids", "terminal_verification", "committing", "finished"
}
PY

"$bin" voids "$fixtures/box.step" \
  --out "$out/coverage-resumed.json" \
  --out-mcnp "$out/geometry-resumed.i" \
  "${common[@]}" \
  2>"$out/resumed.log"

cmp "$out/coverage-first.json" "$out/coverage-resumed.json"
cmp "$out/geometry-first.i" "$out/geometry-resumed.i"
grep -q 'stage alea_void_resume: reused' "$out/resumed.log"
find "$out/project/objects" -type f -name '*.json' -size +0c | grep -q .
find "$out/project/objects" -type f -name '*.i' -size +0c | grep -q .

"$bin" coverage status "$out/coverage-resumed.json" \
  >"$out/coverage-status.txt"
grep -q '^Status: certified$' "$out/coverage-status.txt"
grep -q '^Ownership: samples=125, unique=125, gaps=0, overlaps=0' \
  "$out/coverage-status.txt"
grep -q '^INFO coverage:certified' "$out/coverage-status.txt"

"$bin" coverage queue "$out/coverage-resumed.json" \
  --out "$out/coverage-queue.json" \
  --source "$fixtures/box.step" \
  2>"$out/coverage-queue.log"
grep -q '"schema": "csgify.autonomy-queue"' "$out/coverage-queue.json"
grep -q '"coverage_certificate_digest": "fnv1a128:' \
  "$out/coverage-queue.json"
grep -q '"human_review": 0' "$out/coverage-queue.json"
"$bin" queue status "$out/coverage-queue.json" \
  --out "$out/coverage-queue-roundtrip.json"
cmp "$out/coverage-queue.json" "$out/coverage-queue-roundtrip.json"

"$bin" voids --help | grep -q 'Generate modeled-domain voids with libalea'
"$bin" coverage --help | grep -q 'searchable Qt 6 coverage decision panel'
