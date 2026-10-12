#!/usr/bin/env bash
set -u

exe="$1"
fixtures="$2"
outdir="$3"
mkdir -p "$outdir"
fail() { echo "autonomy_smoke: $*" >&2; exit 1; }
rm -f "$outdir/propose.i" "$outdir/auto.i" "$outdir/removal.i" \
  "$outdir/resumed.i" "$outdir/decisions.json" "$outdir/reviews.json" \
  "$outdir/reconstruction.json" "$outdir/intermediate_decisions.json" \
  "$outdir/project-retry-pending.log" \
  "$outdir/project-retry-completed.log"
rm -f "$outdir/no_progress.step" "$outdir/no_progress.i"
rm -f "$outdir/project-replayed.i" "$outdir/project-replayed.step" \
  "$outdir/project-output-replay.log"
rm -rf "$outdir/idempotency-project"
rm -rf "$outdir/audit-failure-project" "$outdir/audit-log-target"
"$exe" project init "$outdir/idempotency-project" \
  >"$outdir/idempotency-project-init.txt" \
  || fail "could not initialize idempotency project"
"$exe" project init "$outdir/audit-failure-project" \
  >"$outdir/audit-failure-project-init.txt" \
  || fail "could not initialize audit-failure project"

rm -rf "$outdir/uninitialized-project"
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --project "$outdir/uninitialized-project" \
  --queue-report "$outdir/uninitialized-project.json" \
  >"$outdir/uninitialized-project.stdout" \
  2>"$outdir/uninitialized-project.stderr"
uninitialized_project_rc=$?
set -e
[[ "$uninitialized_project_rc" == "1" ]] \
  || fail "autonomy accepted an uninitialized project"
grep -q 'autonomy_project_not_initialized' \
  "$outdir/uninitialized-project.stderr" \
  || fail "uninitialized project did not report the lifecycle error"
[[ ! -e "$outdir/uninitialized-project" ]] \
  || fail "autonomy initialized a project as a side effect"

"$exe" context "$fixtures/l_shape.step" \
  --policy-file "$fixtures/project_policy.json" \
  --out "$outdir/policy-context.json" \
  || fail "shared project policy could not generate context"
python3 - "$outdir/policy-context.json" <<'PY' \
  || fail "context did not preserve shared project policy"
import json, sys
context = json.load(open(sys.argv[1]))
assert context["project"] == {
    "id": "fixture-l-shape",
    "intended_analysis": "geometry-prototype",
}
assert context["document"]["units"] == "mm"
assert context["preparation"]["heal_shape"] is False
assert context["preparation"]["unit_override"] == "mm"
assert context["policy"]["mode"] == "propose"
assert context["policy"]["max_relative_volume_delta"] == 1e-8
PY

set +e
"$exe" context "$fixtures/l_shape.step" \
  --policy-file "$fixtures/project_policy.json" --max-relative-delta 0.1 \
  --out "$outdir/policy-conflict-context.json" \
  >"$outdir/policy-conflict-context.stdout" \
  2>"$outdir/policy-conflict-context.stderr"
policy_conflict_rc=$?
set -e
[[ "$policy_conflict_rc" == "1" ]] \
  || fail "context accepted ambiguous project policy overrides"
grep -q 'context_policy_conflict' "$outdir/policy-conflict-context.stderr" \
  || fail "context policy conflict was not reported"

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --units mm --no-heal \
  --queue-report "$outdir/propose.json" --out "$outdir/propose.i"
rc=$?
set -e
[[ "$rc" == "2" ]] || fail "propose mode should return review-required (2), got $rc"
[[ ! -e "$outdir/propose.i" ]] || fail "propose mode mutated/emitted output"
grep -q '"disposition":"human_review"' "$outdir/propose.json" \
  || fail "propose mode did not queue human review"

# Limit one child process to a 1 KiB file. The manifest fits, but a later
# decision append does not; the autonomous mutation must fail and emit no
# geometry output when its required audit trail cannot be completed.
rm -f "$outdir/audit-append-failure.log" \
  "$outdir/audit-append-failure.i" "$outdir/audit-append-failure.json"
set +e
(
  trap '' XFSZ
  ulimit -f 1
  exec "$exe" autonomy "$fixtures/l_shape.step" \
    --actions "$fixtures/autonomy_decompose.json" --mode auto \
    --queue-report "$outdir/audit-append-failure.json" \
    --log "$outdir/audit-append-failure.log" \
    --out "$outdir/audit-append-failure.i"
) >"$outdir/audit-append-failure.stdout" \
  2>"$outdir/audit-append-failure.stderr"
audit_append_failure_rc=$?
set -e
[[ "$audit_append_failure_rc" == "1" ]] \
  || fail "decision-log append failure did not fail autonomy execution"
grep -q 'autonomy_execution_audit_failed' \
  "$outdir/audit-append-failure.stderr" \
  || fail "decision-log append failure was not reported"
[[ ! -e "$outdir/audit-append-failure.i" ]] \
  || fail "autonomy emitted output after its decision-log append failed"
python3 - "$outdir/audit-append-failure.log" <<'PY' \
  || fail "failed decision append left a partial or discontinuous journal"
import json, sys
records = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
assert records and records[0]["kind"] == "manifest"
assert [record["seq"] for record in records] == list(range(len(records)))
PY

mkdir -p "$outdir/audit-log-target"
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --project "$outdir/audit-failure-project" \
  --queue-report "$outdir/audit-failure.json" \
  --log "$outdir/audit-log-target" \
  >"$outdir/audit-failure.stdout" 2>"$outdir/audit-failure.stderr"
audit_failure_rc=$?
set -e
[[ "$audit_failure_rc" == "1" ]] \
  || fail "invalid audit sink did not fail before autonomy execution"
grep -q 'autonomy_decision_log_open_failed' "$outdir/audit-failure.stderr" \
  || fail "invalid audit sink did not report a bounded error"
"$exe" project status "$outdir/audit-failure-project" \
  >"$outdir/audit-failure-project-status.txt" \
  || fail "audit-failure project could not be inspected"
grep -q 'autonomy_requests=0' "$outdir/audit-failure-project-status.txt" \
  || fail "audit preflight failure left an in-progress request reservation"
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --project "$outdir/audit-failure-project" \
  --queue-report "$outdir/audit-retry.json"
audit_retry_rc=$?
set -e
[[ "$audit_retry_rc" == "2" ]] \
  || fail "request was not immediately retryable after audit preflight failure"

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-first.json"
project_first_rc=$?
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode propose \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-retry-pending.json" \
  --log "$outdir/project-retry-pending.log" \
  2>"$outdir/project-retry-pending.stderr"
project_pending_retry_rc=$?
set -e
[[ "$project_first_rc" == "2" && "$project_pending_retry_rc" == "2" ]] \
  || fail "project-backed pending request did not retain its outcome"
grep -q 'autonomy_request_reused request:exact-l-split' \
  "$outdir/project-retry-pending.stderr" \
  || fail "exact pending retry did not use its stored result"
[[ ! -e "$outdir/project-retry-pending.log" ]] \
  || fail "exact pending retry opened a duplicate decision log"

"$exe" autonomy "$fixtures/l_shape.step" \
  --resume "$outdir/project-first.json" --mode auto \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-completed.json" \
  --out "$outdir/project-completed.i" \
  || fail "project-backed request did not complete from its stored queue"
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-retry-completed.json" \
  --log "$outdir/project-retry-completed.log" \
  2>"$outdir/project-retry-completed.stderr" \
  || fail "completed project request did not return its stored outcome"
grep -q 'autonomy_request_reused request:exact-l-split' \
  "$outdir/project-retry-completed.stderr" \
  || fail "completed exact retry did not use its stored result"
[[ ! -e "$outdir/project-retry-completed.log" ]] \
  || fail "completed exact retry opened a duplicate decision log"
python3 - "$outdir/project-completed.json" \
  "$outdir/project-retry-completed.json" <<'PY' \
  || fail "completed project retry changed the durable outcome"
import json, sys
first, retry = (json.load(open(path)) for path in sys.argv[1:])
assert first["request_id"] == retry["request_id"]
assert first["request_digest"] == retry["request_digest"]
assert first["summary"] == retry["summary"]
PY

# Queue completion and output delivery are separate. If a caller retries the
# same completed request with output paths, reconstruct the durable mutations
# and deliver those files without executing a second logical request.
rm -rf "$outdir/missing-output-parent"
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-output-failed.json" \
  --out "$outdir/missing-output-parent/failed.i" \
  >"$outdir/project-output-failed.stdout" \
  2>"$outdir/project-output-failed.stderr"
project_output_failed_rc=$?
set -e
[[ "$project_output_failed_rc" == "1" ]] \
  || fail "failed output delivery changed the completed queue outcome"
[[ ! -e "$outdir/missing-output-parent/failed.i" ]] \
  || fail "failed output delivery left a result file"

"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-output-replay.json" \
  --out "$outdir/project-replayed.i" \
  --out-step "$outdir/project-replayed.step" \
  --log "$outdir/project-output-replay.log" \
  2>"$outdir/project-output-replay.stderr" \
  || fail "completed project request did not regenerate requested outputs"
grep -q 'autonomy_request_output_replayed request:exact-l-split' \
  "$outdir/project-output-replay.stderr" \
  || fail "completed output retry did not report reconstruction"
[[ -s "$outdir/project-replayed.i" ]] \
  || fail "completed output retry did not regenerate the MCNP deck"
[[ -s "$outdir/project-replayed.step" ]] \
  || fail "completed output retry did not regenerate STEP"
[[ ! -e "$outdir/project-output-replay.log" ]] \
  || fail "completed output retry opened a duplicate decision log"

"$exe" project status "$outdir/idempotency-project" \
  >"$outdir/idempotency-project-status.txt" \
  || fail "project status could not read autonomy request count"
grep -q 'autonomy_requests=1' "$outdir/idempotency-project-status.txt" \
  || fail "project did not retain exactly one logical autonomy request"

python3 - "$fixtures/autonomy_decompose.json" \
  "$outdir/project-conflict-request.json" <<'PY'
import json, sys
request = json.load(open(sys.argv[1]))
request["actions"][0]["rationale"] = "different payload under the same request id"
json.dump(request, open(sys.argv[2], "w"))
PY
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$outdir/project-conflict-request.json" --mode auto \
  --project "$outdir/idempotency-project" \
  --queue-report "$outdir/project-conflict.json" \
  >"$outdir/project-conflict.stdout" 2>"$outdir/project-conflict.stderr"
project_conflict_rc=$?
set -e
[[ "$project_conflict_rc" == "1" ]] \
  || fail "project accepted different input under an existing request id"
grep -q 'autonomy_project_request_conflict' \
  "$outdir/project-conflict.stderr" \
  || fail "project request-id conflict was not reported"

inline_request='{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:inline-proposal","source_digest":"dff5aa386cae4b29b05d6da4ac0c2f8f","expected_revision":0,"actor":"llm:inline-test","actions":[{"id":"inline-proposal","op":"apply_decomposition_candidate","target":"doc:1/solid:1","candidate":"best","rationale":"exercise direct JSON transport"}]}'
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json "$inline_request" --mode propose --queue-report "$outdir/inline.json"
inline_rc=$?
set -e
[[ "$inline_rc" == "2" ]] || fail "inline proposal should return review-required (2), got $inline_rc"
grep -q '"actor":"llm:inline-test"' "$outdir/inline.json" \
  || fail "inline autonomy request lost its actor"
grep -q '"disposition":"human_review"' "$outdir/inline.json" \
  || fail "inline autonomy request did not produce a review queue"

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json "$inline_request" \
  --policy-file "$fixtures/project_policy.json" \
  --queue-report "$outdir/policy-inline.json"
policy_inline_rc=$?
set -e
[[ "$policy_inline_rc" == "2" ]] \
  || fail "shared project policy did not drive autonomy evaluation"
grep -q '"disposition":"human_review"' "$outdir/policy-inline.json" \
  || fail "project-policy autonomy run lost its proposed disposition"

rm -rf "$outdir/policy-discovery-project"
"$exe" project init "$outdir/policy-discovery-project" \
  >"$outdir/policy-discovery-init.txt" \
  || fail "could not initialize project-owned policy"
set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json "$inline_request" \
  --project "$outdir/policy-discovery-project" \
  --queue-report "$outdir/policy-discovered.json"
policy_discovered_rc=$?
set -e
[[ "$policy_discovered_rc" == "2" ]] \
  || fail "autonomy did not discover the project-owned policy"
grep -q '"disposition":"human_review"' "$outdir/policy-discovered.json" \
  || fail "discovered project policy did not govern autonomy"

wrong_source_request=${inline_request/dff5aa386cae4b29b05d6da4ac0c2f8f/00000000000000000000000000000000}
stale_request=${inline_request/\"expected_revision\":0/\"expected_revision\":9}
set +e
"$exe" autonomy "$fixtures/l_shape.step" --actions-json "$wrong_source_request" \
  --mode propose --queue-report "$outdir/wrong-source-request.json" \
  >"$outdir/wrong-source-request.stdout" 2>"$outdir/wrong-source-request.stderr"
wrong_source_request_rc=$?
"$exe" autonomy "$fixtures/l_shape.step" --actions-json "$stale_request" \
  --mode propose --queue-report "$outdir/stale-request.json" \
  >"$outdir/stale-request.stdout" 2>"$outdir/stale-request.stderr"
stale_request_rc=$?
set -e
[[ "$wrong_source_request_rc" == "1" ]] \
  || fail "request bound to another source was accepted"
grep -q 'request source_digest does not match' \
  "$outdir/wrong-source-request.stderr" \
  || fail "wrong-source request did not explain its identity failure"
[[ "$stale_request_rc" == "1" ]] || fail "stale request revision was accepted"
grep -q 'autonomy_request_stale' "$outdir/stale-request.stderr" \
  || fail "stale request did not report its revision failure"

"$exe" queue status "$outdir/propose.json" --out "$outdir/status.json" \
  || fail "durable queue could not be read back"
python3 - "$outdir/status.json" <<'PY' || fail "queue status schema/counts are invalid"
import json, sys
q = json.load(open(sys.argv[1]))
assert q["schema"] == "csgify.autonomy-queue"
assert q["schema_version"] == 4
assert q["request_id"] == "request:exact-l-split"
assert q["request_digest"]
assert q["expected_revision"] == 0
assert q["summary"]["human_review"] == 1
assert q["source_path"]
assert q["source_digest"]
assert q["policy_digest"]
assert q["conversion_policy_digest"]
assert q["preparation"]["unit_override"] == "mm"
assert q["preparation"]["heal_shape"] is False
PY

"$exe" autonomy "$fixtures/l_shape.step" \
  --resume "$outdir/propose.json" --mode auto \
  --queue-report "$outdir/resumed.json" --out "$outdir/resumed.i" \
  || fail "saved queue did not resume under auto policy"
[[ -s "$outdir/resumed.i" ]] || fail "resumed queue emitted no MCNP deck"
grep -q '"applied": 1' "$outdir/resumed.json" \
  || fail "resumed queue did not persist its applied state"

cp "$outdir/propose.json" "$outdir/rejected.json"
"$exe" queue reject "$outdir/rejected.json" exact-l-split \
  --rationale "human rejected the proposed partition" \
  --log "$outdir/reviews.json" \
  || fail "queue rejection failed"
python3 - "$outdir/rejected.json" "$outdir/reviews.json" <<'PY' \
  || fail "queue rejection state/audit are invalid"
import json, sys
q = json.load(open(sys.argv[1]))
item = q["items"][0]
assert item["rejected"] is True
assert item["review_rationale"] == "human rejected the proposed partition"
assert q["summary"]["human_review"] == 0
records = [json.loads(line) for line in open(sys.argv[2])]
review = next(record for record in records if record.get("kind") == "review")
assert review["proposal_id"] == "exact-l-split"
assert review["decision"] == "rejected"
assert review["audit"]["actor"] == "human"
assert review["audit"]["proposal_actor"] == "llm:test"
PY

"$exe" context "$fixtures/l_shape.step" --target solid:1 \
  --history "$outdir/reviews.json" --out "$outdir/context.json" \
  --allow-contained-medium material:7:liquid:0.997 \
  || fail "LLM context generation failed"
python3 - "$outdir/context.json" <<'PY' || fail "LLM context contract is invalid"
import json, sys
c = json.load(open(sys.argv[1]))
assert c["schema"] == "csgify.llm-context"
assert c["schema_version"] == 1
assert c["document"]["source_digest"]
assert c["action_contract"]["schema"] == "csgify.autonomy-actions"
assert c["action_contract"]["schema_version"] == 2
assert set(c["action_contract"]["requires"]) == {
    "request_id", "source_digest", "expected_revision", "actions"}
assert c["selection"] == "solid:1"
assert c["selection_context"]["kind"] == "solid"
assert c["selection_context"]["resolved"] is True
assert c["solids_page"] == {
    "total": 1, "offset": 0, "emitted": 1,
    "next_offset": 1, "truncated": False}
assert c["solids"] and c["solids"][0]["needs_decomposition"] is True
assert c["solids"][0]["bbox"]["valid"] is True
assert c["solids"][0]["face_ids"]
assert c["solids"][0]["component_instance_id"].startswith("inst:")
assert c["solids"][0]["component_definition_id"].startswith("def:")
assert c["candidates"]["decomposition"]
assert all(x["usable"] for x in c["candidates"]["decomposition"])
assert any(x["name"] == "apply_decomposition_candidate" for x in c["operations"])
assert any(x["name"] == "decompose_to_fixed_point" for x in c["operations"])
assert len(c["operations"]) == 8
assert all(x["restart_supported"] for x in c["operations"])
assert all(x["argument_schema"] is not None for x in c["operations"])
assert all(x["recovery_action"] for x in c["operations"])
assert not any(x["name"] == "define_material" for x in c["operations"])
fixed = next(x for x in c["operations"]
             if x["name"] == "decompose_to_fixed_point")
assert fixed["qt_review_supported"] is True
assert fixed["recovery_action"] == "review_residual_or_run_bounded_shatter"
assert c["global_progress"]["residual_count"] == 1
assert c["global_progress"]["active_pieces"] == 1
assert c["policy"]["allow_contained_medium_generation"] is True
assert c["policy"]["contained_medium_materials"][0]["id"] == "material:7"
semantic = c["semantic_context"]
assert semantic["owner_occurrence_id"].startswith("inst:")
assert semantic["definition_id"].startswith("def:")
assert semantic["occurrences_page"] == {
    "total": 1, "offset": 0, "emitted": 1,
    "next_offset": 1, "truncated": False}
assert semantic["occurrences"][0]["selected"] is True
assert semantic["occurrences"][0]["source_shape_id"].endswith("solid:1")
assert semantic["material"]["effective_id"] == "material:1"
assert semantic["material"]["source"] == "document_default"
assert semantic["material"]["available"][0]["id"] == "material:1"
rejected = next(x for x in c["history"]
                if x["proposal_id"] == "exact-l-split"
                and x["decision"] == "rejected")
assert rejected["target_id"].endswith("solid:1")
assert rejected["candidate_id"]
assert rejected["actor"] == "human"
assert rejected["rationale"] == "human rejected the proposed partition"
assert rejected["policy_rule"]
PY

"$exe" context "$fixtures/l_shape.step" --target face:1.1 \
  --out "$outdir/face-context.json" \
  || fail "focused face context generation failed"
python3 - "$outdir/face-context.json" <<'PY' \
  || fail "focused face context lacks owner geometry"
import json, sys
c = json.load(open(sys.argv[1]))
assert c["selection"] == "face:1.1"
assert c["selection_context"]["kind"] == "face"
assert c["selection_context"]["resolved"] is True
owner = c["selection_context"]["owner_solid_id"]
assert owner.endswith("solid:1"), owner
assert len(c["faces"]) == 1
assert c["faces"][0]["id"].endswith("face:1.1")
assert c["faces"][0]["owner_solid_id"] == owner
assert len(c["solids"]) == 1
assert c["solids"][0]["id"] == owner
assert c["solids_page"] == {
    "total": 1, "offset": 0, "emitted": 1,
    "next_offset": 1, "truncated": False}
for candidate in c["candidates"]["surface"]:
    assert candidate["owner_solid_id"] == owner
PY

"$exe" context "$fixtures/straight_annular_pipe.step" --target solid:1 \
  --max-interfaces 1 --out "$outdir/pipe-context.json" \
  || fail "focused pipe interface context generation failed"
python3 - "$outdir/pipe-context.json" <<'PY' \
  || fail "focused pipe context lacks bounded interface semantics"
import json, sys
c = json.load(open(sys.argv[1]))
semantic = c["semantic_context"]
assert semantic["owner_occurrence_id"].startswith("inst:")
assert semantic["interfaces_total"] == 2
assert semantic["interfaces_truncated"] is True
assert len(semantic["interfaces"]) == 1
port = semantic["interfaces"][0]
assert port["kind"] == "pipe_port"
assert port["disposition"] == "unresolved"
assert len(port["center"]) == 3 and len(port["normal"]) == 3
assert c["context_limits"]["max_interfaces"] == 1
PY

"$exe" context "$fixtures/four_solids.step" --solid-offset 1 \
  --max-solids 2 --out "$outdir/solid-page.json" \
  || fail "paged solid context generation failed"
python3 - "$outdir/solid-page.json" <<'PY' \
  || fail "solid context pagination metadata is invalid"
import json, sys
c = json.load(open(sys.argv[1]))
page = c["solids_page"]
assert page == {
    "total": 4, "offset": 1, "emitted": 2,
    "next_offset": 3, "truncated": True}, page
assert len(c["solids"]) == 2
assert c["context_limits"]["solid_offset"] == 1
assert c["context_limits"]["max_solids"] == 2
assert c["context_limits"]["max_serialized_bytes"] == 1048576
PY

"$exe" context "$fixtures/l_shape.step" --target solid:1 \
  --candidate-offset 1 --max-candidates 1 \
  --out "$outdir/candidate-page.json" \
  || fail "paged candidate context generation failed"
python3 - "$outdir/candidate-page.json" <<'PY' \
  || fail "candidate context pagination metadata is invalid"
import json, sys
c = json.load(open(sys.argv[1]))
page = c["candidate_pages"]["decomposition"]
assert page == {
    "total": 2, "offset": 1, "emitted": 1,
    "next_offset": 2, "truncated": False}, page
assert len(c["candidates"]["decomposition"]) == 1
assert c["context_limits"]["candidate_offset"] == 1
assert c["context_limits"]["max_candidates"] == 1
PY

"$exe" context "$fixtures/overlapping_boxes.step" --target solid:1 \
  --overlap-offset 1 --max-local-overlaps 1 \
  --out "$outdir/overlap-page.json" \
  || fail "paged local-overlap context generation failed"
python3 - "$outdir/overlap-page.json" <<'PY' \
  || fail "local-overlap context pagination metadata is invalid"
import json, sys
c = json.load(open(sys.argv[1]))
page = c["local_overlaps_page"]
assert page == {
    "total": 1, "offset": 1, "emitted": 0,
    "next_offset": 1, "truncated": False}, page
assert c["local_overlaps"] == []
assert c["context_limits"]["overlap_offset"] == 1
assert c["context_limits"]["max_local_overlaps"] == 1
PY

medium_request='{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:typed-medium","source_digest":"5990a67a9bfdb9444fa199d46e9d2e02","expected_revision":0,"actor":"llm:medium-test","actions":[{"id":"typed-medium","op":"apply_contained_medium","rationale":"exercise typed payload and immutable policy","medium_request":{"id":"medium:test","material_id":"material:7","fill_mode":"full","require_exact_geometry":true,"host_component_ids":["inst:missing"],"ports":[{"port_id":"port:1","disposition":"cap_here"},{"port_id":"port:2","disposition":"cap_here"}]}}]}'
set +e
"$exe" autonomy "$fixtures/box.step" \
  --actions-json "$medium_request" --mode auto \
  --allow-contained-medium material:7:liquid:0.997 \
  --queue-report "$outdir/typed_medium.json"
medium_rc=$?
set -e
[[ "$medium_rc" == "2" ]] \
  || fail "invalid typed medium should require review (2), got $medium_rc"
python3 - "$outdir/typed_medium.json" <<'PY' \
  || fail "typed medium policy/payload was not persisted"
import json, sys
q = json.load(open(sys.argv[1]))
assert q["policy"]["allow_contained_medium_generation"] is True
assert q["policy"]["contained_medium_materials"][0]["id"] == "material:7"
item = q["items"][0]
assert item["medium_request"]["material_id"] == "material:7"
assert item["medium_request"]["ports"][1]["disposition"] == "cap_here"
assert item["disposition"] == "human_review"
assert item["evidence"]["certificate_passed"] is False
PY

"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_fixed_point.json" --mode auto \
  --queue-report "$outdir/fixed_point.json" \
  --out "$outdir/fixed_point.i" \
  || fail "LLM fixed-point orchestration failed"
python3 - "$outdir/fixed_point.json" "$outdir/fixed_point.i" <<'PY' \
  || fail "fixed-point autonomy result is invalid"
import json, pathlib, sys
q = json.load(open(sys.argv[1]))
item = q["items"][0]
assert item["op"] == "decompose_to_fixed_point"
assert item["executed"] and item["execution_ok"]
assert item["policy_rule"] == "decomposition.global_certified_progress"
assert item["evidence"]["residual_count"] == 0
assert pathlib.Path(sys.argv[2]).stat().st_size > 0
PY

"$exe" autonomy "$fixtures/l_shape.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --queue-report "$outdir/auto.json" --log "$outdir/decisions.json" \
  --out "$outdir/auto.i" || fail "certified auto decomposition failed"
[[ -s "$outdir/auto.i" ]] || fail "auto mode emitted no MCNP deck"
grep -q '"applied": 1' "$outdir/auto.json" || fail "queue did not report apply"
grep -q '"actor":"llm:test"' "$outdir/decisions.json" \
  || fail "decision log missing LLM actor"
grep -q '"proposal_actor":"llm:test"' "$outdir/decisions.json" \
  || fail "decision log missing proposal origin"
grep -q '"policy_rule":"decomposition.high_exact_certified"' \
  "$outdir/decisions.json" || fail "decision log missing policy rule"
grep -q '"approval_mode":"automatic"' "$outdir/decisions.json" \
  || fail "decision log missing automatic approval"

"$exe" autonomy "$fixtures/l_shape.step" --resume "$outdir/auto.json" \
  --queue-report "$outdir/reconstructed.json" \
  --log "$outdir/reconstruction.json" --out "$outdir/reconstructed.i" \
  || fail "completed queue reconstruction failed"
python3 - "$outdir/reconstruction.json" <<'PY' \
  || fail "resume duplicated historical operations in the decision log"
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
assert records and all(record.get("kind") != "operation" for record in records)
PY
python3 - "$outdir/auto.json" "$outdir/reconstructed.json" <<'PY' \
  || fail "resume did not preserve one logical request outcome"
import json, sys
before, after = (json.load(open(path)) for path in sys.argv[1:])
assert before["request_id"] == after["request_id"]
assert before["request_digest"] == after["request_digest"]
assert len(after["items"]) == 1
assert after["summary"]["applied"] == 1
PY

cp "$fixtures/l_shape.step" "$outdir/source-bound.step"
"$exe" autonomy "$outdir/source-bound.step" \
  --actions "$fixtures/autonomy_decompose.json" --mode auto \
  --queue-report "$outdir/source-bound.json" >/dev/null \
  || fail "source-bound queue setup failed"
cp "$fixtures/l_with_inner_box.step" "$outdir/source-bound.step"
set +e
"$exe" autonomy "$outdir/source-bound.step" \
  --resume "$outdir/source-bound.json" \
  --queue-report "$outdir/source-changed.json" \
  >"$outdir/source-changed.stdout" 2>"$outdir/source-changed.stderr"
source_changed_rc=$?
set -e
[[ "$source_changed_rc" == "1" ]] \
  || fail "queue resumed after its source CAD contents changed"
grep -q 'source_digest does not match' "$outdir/source-changed.stderr" \
  || fail "changed-source resume did not report its identity failure"

python3 - "$outdir/propose.json" "$outdir/policy-tampered.json" \
  "$outdir/preparation-tampered.json" "$outdir/legacy-unbound.json" \
  "$outdir/request-tampered.json" "$outdir/legacy-schema.json" <<'PY'
import json, sys
source = json.load(open(sys.argv[1]))
policy = json.loads(json.dumps(source))
policy["policy"]["max_relative_volume_delta"] = 0.25
json.dump(policy, open(sys.argv[2], "w"))
preparation = json.loads(json.dumps(source))
preparation["preparation"]["heal_shape"] = True
json.dump(preparation, open(sys.argv[3], "w"))
legacy = json.loads(json.dumps(source))
legacy.pop("policy_digest")
legacy.pop("conversion_policy_digest")
json.dump(legacy, open(sys.argv[4], "w"))
request = json.loads(json.dumps(source))
request["items"][0]["rationale"] = "edited after evaluation"
json.dump(request, open(sys.argv[5], "w"))
schema = json.loads(json.dumps(source))
schema["schema_version"] = 3
json.dump(schema, open(sys.argv[6], "w"))
PY
for changed_queue in policy-tampered preparation-tampered legacy-unbound; do
  set +e
  "$exe" autonomy "$fixtures/l_shape.step" \
    --resume "$outdir/$changed_queue.json" \
    --queue-report "$outdir/$changed_queue-result.json" \
    >"$outdir/$changed_queue.stdout" 2>"$outdir/$changed_queue.stderr"
  changed_queue_rc=$?
  set -e
  [[ "$changed_queue_rc" == "1" ]] \
    || fail "$changed_queue queue resumed without its bound policy identity"
  grep -Eq 'policy_digest|conversion_policy_digest' \
    "$outdir/$changed_queue.stderr" \
    || fail "$changed_queue did not report the policy identity failure"
done
for invalid_queue in request-tampered legacy-schema; do
  set +e
  "$exe" queue status "$outdir/$invalid_queue.json" \
    --out "$outdir/$invalid_queue-status.json" \
    >"$outdir/$invalid_queue.stdout" 2>"$outdir/$invalid_queue.stderr"
  invalid_queue_rc=$?
  set -e
  [[ "$invalid_queue_rc" == "1" ]] \
    || fail "$invalid_queue queue passed durable identity validation"
done
grep -q 'request_digest does not match' "$outdir/request-tampered.stderr" \
  || fail "edited request payload did not report its digest failure"
grep -q 'unsupported autonomy queue schema_version' \
  "$outdir/legacy-schema.stderr" \
  || fail "legacy queue schema did not require fresh evaluation"

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --resume "$outdir/propose.json" --units cm \
  --queue-report "$outdir/preparation-override.json" \
  >"$outdir/preparation-override.stdout" \
  2>"$outdir/preparation-override.stderr"
preparation_override_rc=$?
set -e
[[ "$preparation_override_rc" == "1" ]] \
  || fail "resume accepted preparation options different from evaluation"
grep -q 'resume preparation options differ' \
  "$outdir/preparation-override.stderr" \
  || fail "preparation override did not request fresh evaluation"

"$exe" compact "$outdir/decisions.json" --out "$outdir/recipe.lua" \
  >/dev/null || fail "actor-aware log no longer compacts"
grep -Eq 'apply_decomposition_candidate|decompose_by_face' "$outdir/recipe.lua" \
  || fail "compacted recipe lost autonomous mutation"

set +e
"$exe" autonomy "$fixtures/four_solids.step" \
  --actions "$fixtures/autonomy_remove.json" --mode auto \
  --queue-report "$outdir/removal.json" --out "$outdir/removal.i"
remove_rc=$?
"$exe" autonomy "$fixtures/box.step" \
  --actions "$fixtures/autonomy_invalid.json" --mode auto \
  --queue-report "$outdir/invalid.json"
invalid_rc=$?
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json '{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:registered-unavailable","source_digest":"dff5aa386cae4b29b05d6da4ac0c2f8f","expected_revision":0,"actions":[{"id":"registered-but-unavailable","op":"decompose_planar","target":"solid:1","rationale":"must use the declared autonomy contract"}]}' \
  --mode auto --queue-report "$outdir/registered-unavailable.json"
registered_unavailable_rc=$?
set -e
[[ "$remove_rc" == "2" ]] || fail "default removal should require review"
[[ ! -e "$outdir/removal.i" ]] || fail "review-required removal emitted output"
grep -q '"actor":"llm"' "$outdir/removal.json" \
  || fail "untrusted request impersonated a human actor"
[[ "$invalid_rc" == "1" ]] || fail "unknown operation should be denied"
grep -q '"disposition":"denied"' "$outdir/invalid.json" \
  || fail "unknown operation was not marked denied"
[[ "$registered_unavailable_rc" == "1" ]] \
  || fail "registered operation without an autonomy contract should be denied"
grep -q 'operation.autonomy_contract_available' \
  "$outdir/registered-unavailable.json" \
  || fail "unavailable registered operation did not explain its denial"

set +e
"$exe" autonomy "$fixtures/tilted_torus.step" \
  --actions-json '{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:unsupported-fixed-point","source_digest":"2f46804609473a8d7fb7868574ee5e3f","expected_revision":0,"actor":"llm:test","actions":[{"id":"unsupported-fixed-point","op":"decompose_to_fixed_point","rationale":"verify no-progress reporting"}]}' \
  --mode auto --queue-report "$outdir/no_progress.json" \
  --out-step "$outdir/no_progress.step" --out "$outdir/no_progress.i"
no_progress_rc=$?
set -e
[[ "$no_progress_rc" == "2" ]] \
  || fail "unsupported fixed point should request follow-up (2), got $no_progress_rc"
[[ ! -e "$outdir/no_progress.step" && ! -e "$outdir/no_progress.i" ]] \
  || fail "zero-progress fixed point wrote misleading final artifacts"
python3 - "$outdir/no_progress.json" <<'PY' \
  || fail "zero-progress fixed-point outcome is misleading"
import json, sys
q = json.load(open(sys.argv[1]))
item = q["items"][0]
assert q["summary"]["applied"] == 0
assert q["summary"]["human_review"] == 1
assert q["summary"]["failed"] == 0
assert item["executed"] is True
assert item["execution_ok"] is False
assert item["requires_followup"] is True
assert item["evidence"]["mutation_applied"] is False
assert item["evidence"]["residual_count"] == 1
assert "recommended_action=shatter_or_review" in item["reason"]
PY

set +e
"$exe" autonomy "$fixtures/bolt_hole.step" \
  --actions "$fixtures/autonomy_intermediate.json" --mode auto \
  --queue-report "$outdir/intermediate_blocked.json"
intermediate_blocked_rc=$?
set -e
[[ "$intermediate_blocked_rc" == "2" ]] \
  || fail "intermediate split bypassed the explicit opt-in gate"

"$exe" autonomy "$fixtures/bolt_hole.step" \
  --actions "$fixtures/autonomy_intermediate.json" --mode auto \
  --allow-certified-intermediate 1e-8 \
  --max-intermediate-pieces 4 --max-intermediate-residuals 1 \
  --queue-report "$outdir/intermediate_applied.json" \
  --log "$outdir/intermediate_decisions.json" \
  --out-step "$outdir/intermediate.step" \
  || fail "certified intermediate split was not applied"
[[ -s "$outdir/intermediate.step" ]] || fail "intermediate STEP was not exported"
grep -q '"policy_rule":"decomposition.certified_intermediate_progress"' \
  "$outdir/intermediate_decisions.json" \
  || fail "intermediate audit rule was not recorded"
python3 - "$outdir/intermediate_applied.json" <<'PY' \
  || fail "intermediate queue policy was not persisted"
import json, sys
q = json.load(open(sys.argv[1]))
assert q["summary"]["applied"] == 1
assert q["policy"]["allow_certified_intermediate_decomposition"] is True
assert q["policy"]["max_intermediate_pieces"] == 4
assert q["policy"]["max_intermediate_residual_pieces"] == 1
PY

approx_request='{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:budgeted-shatter","source_digest":"dff5aa386cae4b29b05d6da4ac0c2f8f","expected_revision":0,"actor":"llm:budget-test","actions":[{"id":"budgeted-shatter","op":"shatter_with_approximation","target":"solid:1","rationale":"use only the human supplied budget","parameters":{"max_depth":3}}]}'
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json "$approx_request" --mode auto \
  --approx-volume 0.1% --approx-shatter \
  --queue-report "$outdir/approximation.json" \
  --out "$outdir/approximation.i" \
  || fail "explicitly budgeted autonomous shatter failed"
[[ -s "$outdir/approximation.i" ]] \
  || fail "budgeted autonomous shatter wrote no MCNP"
python3 - "$outdir/approximation.json" <<'PY' \
  || fail "autonomy approximation policy/evidence was not persisted"
import json, sys
q = json.load(open(sys.argv[1]))
p = q["policy"]["approximation"]
item = q["items"][0]
assert p["enabled"] is True
assert abs(p["global_limit"] - 0.001) < 1e-12
assert p["allow_numerical_shatter_loss"] is True
assert item["execution_ok"] is True
assert item["evidence"]["certificate_passed"] is True
assert item["evidence"]["mutation_applied"] is True
assert item["effective_parameters"]["max_depth"] == 3
PY

"$exe" autonomy "$fixtures/l_shape.step" \
  --resume "$outdir/approximation.json" \
  --queue-report "$outdir/approximation-resumed.json" \
  --out "$outdir/approximation-resumed.i" \
  || fail "budgeted shatter could not be reconstructed by the shared dispatcher"
[[ -s "$outdir/approximation-resumed.i" ]] \
  || fail "reconstructed budgeted shatter wrote no MCNP"
python3 - "$outdir/approximation-resumed.json" <<'PY' \
  || fail "reconstructed shatter lost its effective parameters"
import json, sys
q = json.load(open(sys.argv[1]))
item = q["items"][0]
assert item["execution_ok"] is True
assert item["effective_parameters"]["max_depth"] == 3
PY

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json '{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:invalid-depth","source_digest":"dff5aa386cae4b29b05d6da4ac0c2f8f","expected_revision":0,"actions":[{"op":"shatter_with_approximation","target":"solid:1","parameters":{"max_depth":3.5}}]}' \
  --mode auto --approx-volume 1% --approx-shatter \
  --queue-report "$outdir/nonintegral-depth.json" >/dev/null 2>&1
nonintegral_depth_rc=$?
set -e
[[ "$nonintegral_depth_rc" == "1" ]] \
  || fail "autonomy accepted a non-integral effective shatter depth"

set +e
"$exe" autonomy "$fixtures/l_shape.step" \
  --actions-json "$approx_request" --mode auto \
  --approx-volume 1 --approx-shatter \
  --queue-report "$outdir/ambiguous-percent.json" >/dev/null 2>&1
ambiguous_percent_rc=$?
set -e
[[ "$ambiguous_percent_rc" == "1" ]] \
  || fail "autonomy accepted an approximation percentage without '%'"

overlap_request='{"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"request:own-small-overlap","source_digest":"417045a8edc16312ff290de54688de8c","expected_revision":0,"actor":"llm:ownership-test","actions":[{"id":"own-small-overlap","op":"resolve_overlap_with_approximation","target":"solid:2","candidate":"solid:1","rationale":"assign the shared region to one stable owner"}]}'
"$exe" autonomy "$fixtures/overlapping_boxes.step" \
  --actions-json "$overlap_request" --mode auto \
  --approx-volume 20% --approx-resolve-overlaps \
  --queue-report "$outdir/overlap_ownership.json" \
  --out-step "$outdir/overlap_ownership.step" \
  || fail "budgeted autonomous overlap ownership failed"
python3 - "$outdir/overlap_ownership.json" <<'PY' \
  || fail "autonomous overlap ownership evidence was not persisted"
import json, sys
q = json.load(open(sys.argv[1]))
p = q["policy"]["approximation"]
item = q["items"][0]
assert p["allow_overlap_resolution"] is True
assert q["summary"]["applied"] == 1
assert item["execution_ok"] is True
assert item["evidence"]["certificate_passed"] is True
assert abs(item["evidence"]["relative_volume_delta"] - 0.125) < 1e-12
PY

"$exe" autonomy "$fixtures/overlapping_boxes.step" \
  --resume "$outdir/overlap_ownership.json" \
  --queue-report "$outdir/overlap_resumed.json" \
  --out-step "$outdir/overlap_resumed.step" \
  || fail "successful overlap ownership action could not be reconstructed"
[[ -s "$outdir/overlap_resumed.step" ]] \
  || fail "resumed overlap ownership did not preserve reconstructed geometry"

echo "autonomy_smoke: OK"
