#!/usr/bin/env bash
set -euo pipefail

bin=$1
fixtures=$2
out=$3
rm -rf -- "$out"
mkdir -p "$out"

project="$out/project"
blocked="$out/coverage-blocked.json"
corrected="$out/coverage-corrected.json"
queue="$out/coverage-queue.json"
resolved="$out/coverage-resolved-queue.json"

# Index a real overlapping model. The project remains useful even though MCNP
# ownership is intentionally unsafe at this point.
"$bin" inspect "$fixtures/overlapping_boxes.step" --allow-overlaps \
  --project "$project" --report "$out/overlap-report.json" \
  >"$out/inspect.stdout" 2>"$out/inspect.stderr"

common=(--max-depth 4 --min-size 0.5 --workers 1 --scratch-bytes 65536
        --samples 5 --domain-id domain:phase9-workspace
        --bounds '-1,-1,-1,20,20,20')

# A valid certificate is still emitted on the expected safety failure.
set +e
"$bin" voids "$fixtures/overlapping_boxes.step" --out "$blocked" \
  --out-mcnp "$out/blocked.i" "${common[@]}" \
  >"$out/blocked.stdout" 2>"$out/blocked.stderr"
blocked_status=$?
set -e
test "$blocked_status" -eq 1
grep -q '"status": "blocked_overlap"' "$blocked"
grep -q '"overlap_count": 1' "$blocked"

set +e
"$bin" coverage queue "$blocked" --source "$fixtures/overlapping_boxes.step" \
  --out "$queue" 2>"$out/queue.stderr"
queue_status=$?
set -e
test "$queue_status" -eq 2
grep -q '"human_review": 1' "$queue"
grep -q 'coverage_resolve_overlap_ownership' "$queue"

# Coverage findings cannot be dismissed as harmless geometry.
item_id=$(sed -n 's/.*"id":"\([^"]*\)".*/\1/p' "$queue" | head -1)
test -n "$item_id"
set +e
"$bin" queue reject "$queue" "$item_id" --rationale unsafe \
  --out "$out/rejected.json" >"$out/reject.stdout" 2>"$out/reject.stderr"
reject_status=$?
set -e
test "$reject_status" -ne 0
grep -q 'queue_item_not_rejectable' "$out/reject.stderr"

# Construct the complete Qt workspace with real project/certificate/queue
# attachments. Smoke mode closes on the event loop without loading STEP.
QT_QPA_PLATFORM=offscreen "$bin" project review "$project" \
  --source "$fixtures/overlapping_boxes.step" --coverage "$blocked" \
  --queue "$queue" --smoke-test

# Model the corrective single-owner result with one component and reconcile
# against evidence, not an acknowledgement flag.
"$bin" voids "$fixtures/box.step" --out "$corrected" \
  --out-mcnp "$out/corrected.i" "${common[@]}" \
  >"$out/corrected.stdout" 2>"$out/corrected.stderr"
grep -q '"status": "certified"' "$corrected"
"$bin" coverage queue "$corrected" --source "$fixtures/box.step" \
  --resume "$queue" --out "$resolved" 2>"$out/reconcile.stderr"
grep -q '"human_review": 0' "$resolved"
grep -q '"executed":true' "$resolved"
grep -q '"execution_ok":true' "$resolved"

# Qualify the LLM automatic path and its final reproducible recipe in the same
# workspace scenario.
"$bin" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --queue-report "$out/autonomy.json" --log "$out/decisions.json" \
  --out "$out/final.i" --out-step "$out/final.step"
test -s "$out/final.i"
test -s "$out/final.step"
grep -q '"approval_mode":"automatic"' "$out/decisions.json"

"$bin" compact "$out/decisions.json" --out "$out/recipe.lua" \
  --preserve-step-exports
test -s "$out/recipe.lua"
grep -Eq 'apply_decomposition_candidate|decompose_by_face' "$out/recipe.lua"

QT_QPA_PLATFORM=offscreen "$bin" project review "$project" \
  --source "$fixtures/l_shape.step" --coverage "$corrected" \
  --queue "$out/autonomy.json" --smoke-test

"$bin" project status "$project" --limit 10 >"$out/project-status.txt"
grep -q '^Assembly: sources=1' "$out/project-status.txt"
grep -Eq '^Cache: artifacts=[1-9]' "$out/project-status.txt"
grep -Eq 'spatial=[1-9]' "$out/project-status.txt"
