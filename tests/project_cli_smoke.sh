#!/usr/bin/env bash
set -euo pipefail

CSGIFY=$1
FIXTURES=$2
OUT=$3

mkdir -p "$OUT"
PROJECT="$OUT/project"
REPORT="$OUT/report.json"
rm -rf -- "$PROJECT"
rm -f -- "$REPORT"

# Read-only project commands must never create a project as a side effect.
MISSING_PROJECT="$OUT/missing-project"
MISSING_STATUS="$OUT/missing-project-status.txt"
rm -rf -- "$MISSING_PROJECT"
if "$CSGIFY" project status "$MISSING_PROJECT" > "$MISSING_STATUS" 2>&1; then
    echo "project status unexpectedly initialized a missing project" >&2
    exit 1
fi
grep -q 'project is not initialized' "$MISSING_STATUS"
test ! -e "$MISSING_PROJECT"

# Project creation is explicit and idempotent.
INIT_PROJECT="$OUT/initialized-project"
rm -rf -- "$INIT_PROJECT"
"$CSGIFY" project init "$INIT_PROJECT" > "$OUT/project-init.txt"
grep -q '^project_initialized ' "$OUT/project-init.txt"
test -s "$INIT_PROJECT/project.sqlite"
test -s "$INIT_PROJECT/project-policy.json"
python3 - "$INIT_PROJECT/project-policy.json" <<'PY'
import json, sys
policy = json.load(open(sys.argv[1], encoding="utf-8"))
assert policy["schema"] == "csgify.project-policy"
assert policy["schema_version"] == 1
assert policy["project_id"] == "initialized-project"
assert policy["authority"]["mode"] == "propose"
assert policy["authority"]["approximation"]["enabled"] is False
PY
cp "$INIT_PROJECT/project-policy.json" "$OUT/project-policy-before.json"
"$CSGIFY" project init "$INIT_PROJECT" > "$OUT/project-reinit.txt"
cmp "$OUT/project-policy-before.json" "$INIT_PROJECT/project-policy.json"
"$CSGIFY" context "$FIXTURES/box.step" --project "$INIT_PROJECT" \
    --out "$OUT/project-context.json"
python3 - "$OUT/project-context.json" <<'PY'
import json, sys
context = json.load(open(sys.argv[1], encoding="utf-8"))
assert context["project"]["id"] == "initialized-project"
assert context["policy"]["mode"] == "propose"
PY
"$CSGIFY" project status "$INIT_PROJECT" --limit 1 \
    > "$OUT/initialized-project-status.txt"
grep -q '^Assembly: sources=0' "$OUT/initialized-project-status.txt"

"$CSGIFY" inspect "$FIXTURES/box.step" \
    --report "$REPORT" --project "$PROJECT"

test -s "$PROJECT/project.sqlite"

# Phase 9 project status must use the resumable index without reopening STEP.
STATUS="$OUT/project-status.txt"
"$CSGIFY" project status "$PROJECT" --limit 5 > "$STATUS"
grep -q '^Assembly: sources=1' "$STATUS"
grep -q '^Cache: artifacts=' "$STATUS"
grep -q '^Jobs: queued=' "$STATUS"
grep -q '^source ' "$STATUS"
test -d "$PROJECT/objects"
test -d "$PROJECT/tmp"
find "$PROJECT/objects" -type f -name '*.json' | grep -q .

# Re-import must update the same source transactionally and remain reopenable.
# Reports include timing metrics, so two runs are not assumed byte-identical.
"$CSGIFY" inspect "$FIXTURES/box.step" \
    --report "$REPORT" --project "$PROJECT"
test -s "$PROJECT/project.sqlite"
