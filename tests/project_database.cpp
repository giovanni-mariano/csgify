#include "csgify/project_store.hpp"
#include "csgify/project_job_progress.hpp"
#include "csgify/engine.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>

#include <sqlite3.h>
#include <unistd.h>

namespace {

csgify::ProjectSnapshot snapshot() {
    csgify::ProjectSnapshot value;
    value.source_path = "/model/iter-port.step";
    value.source_digest = "source-digest-1";

    csgify::EntityMetadata metadata;
    metadata.id = 1;
    metadata.stable_name = value.strings.intern("instance:pipe-a");
    metadata.display_name = value.strings.intern("Cooling pipe A");
    metadata.source_component_path = value.strings.intern("Port/Cooling/PipeA");
    metadata.tags.push_back(value.strings.intern("cooling"));
    metadata.annotations.push_back({
        value.strings.intern("user_comment"),
        value.strings.intern("Primary water circuit"),
        csgify::AnnotationOrigin::user,
        csgify::MetadataReviewState::approved,
        true,
    });
    value.metadata.push_back(metadata);

    csgify::ComponentDefinition definition;
    definition.id = "def:pipe";
    definition.source_definition_key = value.strings.intern("0:1:4");
    definition.geometry_digest = "geometry-digest";
    definition.geometry_digest_verified = true;
    definition.metadata_id = metadata.id;
    value.component_definitions.push_back(definition);

    csgify::ComponentInstance instance;
    instance.id = "inst:pipe-a";
    instance.definition_id = definition.id;
    instance.source_shape_id = "solid:1";
    instance.metadata_id = metadata.id;
    value.component_instances.push_back(instance);

    csgify::MaterialDefinition water;
    water.id = "material:water";
    water.metadata_id = metadata.id;
    water.phase = csgify::MaterialPhase::liquid;
    water.density = 0.997;
    value.materials.push_back(water);
    return value;
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("csgify-project-database-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::string error;

    // Inspection must not create a directory or SQLite database as a side
    // effect. Project initialization is an explicit writable operation.
    {
        csgify::ProjectStore missing(root);
        assert(!missing.open_read_only(&error));
        assert(error.find("not initialized") != std::string::npos);
        assert(!std::filesystem::exists(root));
        error.clear();
    }

    csgify::ArtifactRecord artifact;
    {
        csgify::ProjectStore project(root);
        assert(project.open(&error));
        assert(project.schema_version(&error) == 8);
        assert(project.counts(&error).sources == 0);
        assert(project.counts(&error).autonomy_requests == 0);
        assert(project.counts(&error).agent_conversations == 0);
        assert(project.counts(&error).agent_messages == 0);

        csgify::ProjectAgentConversationRecord conversation;
        conversation.conversation_id = "conversation:default";
        conversation.title = "Project conversation";
        conversation.created_at = 10;
        conversation.updated_at = 10;
        assert(project.upsert_agent_conversation(conversation, &error));
        csgify::ProjectAgentMessageRecord user_message;
        user_message.message_id = "message:user:1";
        user_message.conversation_id = conversation.conversation_id;
        user_message.role = csgify::ProjectAgentMessageRole::user;
        user_message.text = "Why is export blocked?";
        user_message.document_id = "doc:1";
        user_message.document_revision = 4;
        user_message.target_id = "doc:1/solid:2";
        user_message.created_at = 11;
        assert(project.append_agent_message(user_message, &error));
        csgify::ProjectAgentMessageRecord assistant_message;
        assistant_message.message_id = "message:assistant:1";
        assistant_message.conversation_id = conversation.conversation_id;
        assistant_message.parent_message_id = user_message.message_id;
        assistant_message.role = csgify::ProjectAgentMessageRole::assistant;
        assistant_message.text = "A blocking diagnostic remains.";
        assistant_message.context_id = "context:1";
        assistant_message.context_digest = "context-digest";
        assistant_message.document_id = "doc:1";
        assistant_message.document_revision = 4;
        assistant_message.target_id = "doc:1/solid:2";
        assistant_message.provider_id = "scripted";
        assistant_message.model_id = "deterministic-test";
        assistant_message.simulated = true;
        assistant_message.created_at = 12;
        assistant_message.references.push_back(
            {"diagnostic", "diagnostic:blocked", "Blocking diagnostic"});
        assert(project.append_agent_message(assistant_message, &error));
        assert(!project.append_agent_message(assistant_message, &error));
        error.clear();
        const auto conversations = project.list_agent_conversations(10, 0, &error);
        assert(conversations.size() == 1);
        assert(conversations[0].updated_at == 12);
        const auto messages = project.list_agent_messages(
            conversation.conversation_id, 10, 0, &error);
        assert(messages.size() == 2);
        assert(messages[0].ordinal == 1 && messages[1].ordinal == 2);
        assert(messages[1].parent_message_id == user_message.message_id);
        assert(messages[1].context_digest == "context-digest");
        assert(messages[1].simulated);
        assert(messages[1].references.size() == 1);
        assert(messages[1].references[0].id == "diagnostic:blocked");
        assert(project.counts(&error).agent_conversations == 1);
        assert(project.counts(&error).agent_messages == 2);

        const csgify::ProjectSnapshot valid = snapshot();
        assert(project.persist_snapshot(valid, &error));
        csgify::ProjectCounts counts = project.counts(&error);
        assert(counts.sources == 1);
        assert(counts.component_definitions == 1);
        assert(counts.component_instances == 1);
        assert(counts.materials == 1);
        assert(counts.metadata_entities == 1);
        assert(counts.strings == valid.strings.size());

        // Re-importing one source replaces its snapshot atomically.
        assert(project.persist_snapshot(valid, &error));
        counts = project.counts(&error);
        assert(counts.sources == 1);
        assert(counts.component_instances == 1);

        csgify::ProjectSnapshot invalid = valid;
        invalid.component_instances[0].definition_id = "def:missing";
        assert(!project.persist_snapshot(invalid, &error));
        // The failed transaction must restore the preceding complete snapshot.
        counts = project.counts(&error);
        assert(counts.component_definitions == 1);
        assert(counts.component_instances == 1);

        assert(project.artifacts().publish_bytes(
            "semantic-report", "json", "{\"schema\":1}\n", &artifact, &error));
        assert(project.record_artifact(artifact, &error));
        assert(project.counts(&error).artifacts == 1);
        csgify::ConversionCacheEntry cache;
        cache.cache_key = "geometry-digest:policy-digest";
        cache.geometry_digest = "geometry-digest";
        cache.policy_digest = "policy-digest";
        cache.artifact = artifact;
        cache.certificate_digest = "certificate-digest";
        assert(project.record_conversion_cache(cache, &error));
        assert(project.counts(&error).conversion_cache_entries == 1);

        csgify::ConversionReport recognition_report;
        recognition_report.component_definitions = valid.component_definitions;
        recognition_report.component_instances = valid.component_instances;
        csgify::AnalyzedSolid recognized;
        recognized.id = "solid:1";
        recognized.topology.vertex_count = 8;
        recognized.topology.edge_count = 12;
        recognized.topology.face_count = 6;
        recognized.topology.prismatic_candidate = true;
        recognized.topology.constant_cross_section_candidate = true;
        recognized.topology.selected_recognition_kind = "extrusion";
        recognized.topology.recommended_strategy = "prismatic_decomposition";
        recognized.certificate.attempted = true;
        recognized.certificate.certified = true;
        recognized.certificate.faces_checked = 6;
        recognition_report.solids.push_back(recognized);
        csgify::ConversionOptions recognition_options;
        csgify::DefinitionCacheSyncResult first_sync;
        assert(project.sync_definition_recognition_cache(
            recognition_report, recognition_options, &first_sync, &error));
        assert(first_sync.hits == 0);
        assert(first_sync.misses == 1);
        assert(first_sync.published == 1);
        csgify::DefinitionCacheSyncResult second_sync;
        assert(project.sync_definition_recognition_cache(
            recognition_report, recognition_options, &second_sync, &error));
        assert(second_sync.hits == 1);
        assert(second_sync.misses == 0);
        assert(second_sync.published == 0);
        assert(project.counts(&error).artifacts == 2);
        assert(project.counts(&error).conversion_cache_entries == 2);

        const auto sources = project.list_sources(10, 0, &error);
        assert(sources.size() == 1);
        assert(sources[0].source_path == valid.source_path);
        const auto definitions = project.list_component_definitions(10, 0, &error);
        assert(definitions.size() == 1);
        assert(definitions[0].definition_id == "def:pipe");
        assert(definitions[0].display_name == "Cooling pipe A");
        assert(definitions[0].instance_count == 1);
        const auto instances = project.list_component_instances(10, 0, &error);
        assert(instances.size() == 1);
        assert(instances[0].instance_id == "inst:pipe-a");
        assert(instances[0].source_shape_id == "solid:1");
        assert(project.search_component_definitions("cooling", 10, 0, &error).size() == 1);
        assert(project.search_component_instances("PIPE-A", 10, 0, &error).size() == 1);
        assert(project.search_component_instances("not-present", 10, 0, &error).empty());
        const auto artifacts = project.list_artifacts(10, 0, {}, &error);
        assert(artifacts.size() == 2);

        csgify::ProjectJobSpec parent;
        parent.job_id = "job:parent";
        parent.target_id = "solid:parent";
        parent.operation = "decompose";
        parent.input_digest = "parent-input";
        parent.policy_digest = "policy-digest";
        parent.stable_order = 20;
        parent.max_attempts = 2;
        assert(project.enqueue_job(parent, {}, &error));

        csgify::ProjectJobSpec child = parent;
        child.job_id = "job:child";
        child.target_id = "solid:child";
        child.input_digest = "child-input";
        child.stable_order = 0;
        assert(project.enqueue_job(child, {parent.job_id}, &error));

        csgify::ProjectJobSpec independent = parent;
        independent.job_id = "job:independent";
        independent.target_id = "solid:independent";
        independent.input_digest = "independent-input";
        independent.stable_order = 10;
        assert(project.enqueue_job(independent, {}, &error));

        csgify::ProjectJobSpec interactive = parent;
        interactive.job_id = "job:interactive";
        interactive.target_id = "doc:interactive";
        interactive.operation = "qt_batch_remove_v1";
        interactive.input_digest = "interactive-input";
        interactive.stable_order = 100;
        assert(project.enqueue_job(interactive, {}, &error));
        // An interactive client claims its own action even though another
        // ready job has a lower queue order. Blocked dependencies still win.
        assert(!project.claim_job(child.job_id, "worker:qt", 98, &error));
        error.clear();
        auto interactive_claim = project.claim_job(
            interactive.job_id, "worker:qt", 99, &error);
        assert(interactive_claim
               && interactive_claim->spec.job_id == interactive.job_id);
        assert(project.complete_job(interactive.job_id, "worker:qt", artifact,
                                    "interactive-certificate", 99, &error));

        // The lower-order child is blocked, so the independent job claims first.
        auto claimed = project.claim_next_job("worker:a", 100, &error);
        assert(claimed && claimed->spec.job_id == independent.job_id);
        assert(claimed->attempt == 1);
        csgify::JobProgressEvent progress;
        progress.job_id = independent.job_id;
        progress.sequence = 1;
        progress.operation = csgify::JobOperation::fixed_point_decomposition;
        progress.phase = csgify::JobPhase::analyzing_solids;
        progress.message_code = "analysis_started";
        progress.target_id = independent.target_id;
        assert(project.append_job_event(progress, 100, {}, &error));
        progress.sequence = 2;
        progress.phase = csgify::JobPhase::certifying_solids;
        progress.message_code = "certifying";
        progress.completed = 3;
        progress.total = 10;
        progress.total_known = true;
        assert(project.append_job_event(progress, 101, artifact.digest, &error));
        const auto stored_events = project.list_job_events(
            independent.job_id, 0, 10, &error);
        assert(stored_events.size() == 2);
        assert(stored_events[1].event.sequence == 2);
        assert(stored_events[1].event.completed == 3);
        assert(stored_events[1].artifact_digest == artifact.digest);
        assert(project.list_job_events(independent.job_id, 1, 10, &error).size() == 1);
        assert(!project.append_job_event(progress, 102, {}, &error));
        error.clear();
        const auto live_jobs = project.list_jobs(10, 0, "running", &error);
        assert(live_jobs.size() == 1);
        assert(live_jobs[0].latest_sequence == 2);
        assert(live_jobs[0].latest_phase == csgify::JobPhase::certifying_solids);
        assert(live_jobs[0].total_known && live_jobs[0].completed == 3
               && live_jobs[0].total == 10);
        assert(project.fail_job(independent.job_id, "worker:a", "temporary",
                                "retry once", true, 101, &error));
        claimed = project.claim_next_job("worker:a", 102, &error);
        assert(claimed && claimed->spec.job_id == independent.job_id);
        assert(claimed->attempt == 2);
        std::int64_t retry_time = 102;
        csgify::ProjectJobProgressSink retry_sink(
            project, *claimed, 5, [&]() { return retry_time; });
        assert(retry_sink.ready() && retry_sink.latest_sequence() == 2);
        csgify::JobProgressEvent retry_event;
        retry_event.operation = csgify::JobOperation::fixed_point_decomposition;
        retry_event.phase = csgify::JobPhase::resolving_targets;
        retry_event.message_code = "retry_started";
        assert(retry_sink.consume(retry_event));
        assert(retry_sink.latest_sequence() == 3);
        assert(project.complete_job(independent.job_id, "worker:a", artifact,
                                    "certificate:independent", 103, &error));

        claimed = project.claim_next_job("worker:b", 104, &error);
        assert(claimed && claimed->spec.job_id == parent.job_id);
        assert(project.heartbeat_job(parent.job_id, "worker:b", 105, &error));
        assert(project.complete_job(parent.job_id, "worker:b", artifact,
                                    "certificate:parent", 106, &error));
        claimed = project.claim_next_job("worker:b", 107, &error);
        assert(claimed && claimed->spec.job_id == child.job_id);
        assert(project.complete_job(child.job_id, "worker:b", artifact,
                                    "certificate:child", 108, &error));

        csgify::ProjectJobSpec streamed = parent;
        streamed.job_id = "job:streamed";
        streamed.target_id = "solid:streamed";
        streamed.input_digest = "streamed-input";
        streamed.stable_order = 25;
        assert(project.enqueue_job(streamed, {}, &error));
        claimed = project.claim_next_job("worker:stream", 500, &error);
        assert(claimed && claimed->spec.job_id == streamed.job_id);
        std::int64_t sink_time = 500;
        csgify::ProjectJobProgressSink sink(
            project, *claimed, 5, [&]() { return sink_time; });
        assert(sink.ready());
        csgify::JobProgressEvent streamed_event;
        streamed_event.job_id = "engine-local-id";
        streamed_event.sequence = 99;
        streamed_event.operation = csgify::JobOperation::analyze;
        streamed_event.phase = csgify::JobPhase::analyzing_solids;
        streamed_event.message_code = "analysis_started";
        streamed_event.total_known = true;
        streamed_event.total = 4;
        assert(sink.consume(streamed_event));
        streamed_event.completed = 1;
        streamed_event.message_code = "analysis_solid_completed";
        ++sink_time;
        assert(sink.consume(streamed_event));
        auto streamed_rows = project.list_jobs(100, 0, "running", &error);
        auto streamed_row = std::find_if(
            streamed_rows.begin(), streamed_rows.end(),
            [&](const csgify::ProjectJobSummary& row) {
                return row.job_id == streamed.job_id;
            });
        assert(streamed_row != streamed_rows.end());
        assert(streamed_row->heartbeat_at == 500);
        // A separate connection models cancellation arriving from Qt/MCP or
        // another process while the worker owns its original connection.
        {
            csgify::ProjectStore external(root);
            assert(external.open(&error));
            assert(external.request_job_cancellation(
                streamed.job_id, 502, &error));
        }
        sink_time = 502;
        streamed_event.completed = 2;
        assert(!sink.consume(streamed_event));
        assert(sink.cancellation_seen());
        streamed_event.phase = csgify::JobPhase::finished;
        streamed_event.outcome = csgify::JobOutcome::cancelled;
        streamed_event.message_code = "job_cancelled";
        assert(!sink.consume(streamed_event));
        assert(sink.terminal_seen());
        const auto streamed_events = project.list_job_events(
            streamed.job_id, 0, 10, &error);
        assert(streamed_events.size() == 4);
        for (std::size_t i = 0; i < streamed_events.size(); ++i) {
            assert(streamed_events[i].event.job_id == streamed.job_id);
            assert(streamed_events[i].event.sequence == i + 1);
        }
        assert(project.fail_job(streamed.job_id, claimed->worker_id,
                                "job_cancelled", "cancelled by project", false,
                                503, &error));

        csgify::ProjectJobSpec artifact_job = parent;
        artifact_job.job_id = "job:artifact-stream";
        artifact_job.target_id = "solid:artifact-stream";
        artifact_job.input_digest = "artifact-stream-input";
        artifact_job.stable_order = 26;
        assert(project.enqueue_job(artifact_job, {}, &error));
        claimed = project.claim_next_job("worker:artifact", 510, &error);
        assert(claimed && claimed->spec.job_id == artifact_job.job_id);
        sink_time = 510;
        csgify::ProjectJobProgressSink artifact_sink(
            project, *claimed, 5, [&]() { return sink_time; });
        csgify::JobProgressEvent artifact_event;
        artifact_event.operation = csgify::JobOperation::decomposition_preview;
        artifact_event.phase = csgify::JobPhase::awaiting_review;
        artifact_event.message_code = "preview_artifact_ready";
        csgify::ArtifactRecord review_artifact;
        assert(artifact_sink.publish_bytes(
            artifact_event, "job_review_summary", "json",
            "{\"outcome\":\"certified_partial\"}\n", &review_artifact));
        artifact_event.phase = csgify::JobPhase::finished;
        artifact_event.outcome = csgify::JobOutcome::committed;
        artifact_event.message_code = "preview_finished";
        assert(artifact_sink.complete(
            artifact_event, review_artifact, review_artifact.digest));
        const auto artifact_events = project.list_job_events(
            artifact_job.job_id, 0, 10, &error);
        assert(artifact_events.size() == 2);
        assert(artifact_events[0].artifact_digest == review_artifact.digest);
        assert(artifact_events[1].event.outcome
               == csgify::JobOutcome::committed);

        csgify::ProjectJobSpec commit_race = parent;
        commit_race.job_id = "job:commit-race";
        commit_race.target_id = "solid:commit-race";
        commit_race.input_digest = "commit-race-input";
        commit_race.stable_order = 27;
        assert(project.enqueue_job(commit_race, {}, &error));
        claimed = project.claim_next_job("worker:commit-race", 520, &error);
        assert(claimed && claimed->spec.job_id == commit_race.job_id);
        sink_time = 520;
        csgify::ProjectJobProgressSink race_sink(
            project, *claimed, 5, [&]() { return sink_time; });
        csgify::JobProgressEvent race_event;
        race_event.operation = csgify::JobOperation::emit_csg;
        race_event.phase = csgify::JobPhase::committing;
        race_event.message_code = "writing_output";
        assert(race_sink.consume(race_event));
        assert(project.request_job_cancellation(commit_race.job_id, 521, &error));
        race_event.phase = csgify::JobPhase::finished;
        race_event.outcome = csgify::JobOutcome::committed;
        race_event.message_code = "emit_finished";
        assert(!race_sink.complete(
            race_event, review_artifact, review_artifact.digest));
        assert(race_sink.cancellation_seen());
        auto race_events = project.list_job_events(
            commit_race.job_id, 0, 10, &error);
        assert(race_events.size() == 1);
        race_event.outcome = csgify::JobOutcome::cancelled;
        race_event.message_code = "job_cancelled";
        assert(!race_sink.consume(race_event));
        assert(project.fail_job(commit_race.job_id, claimed->worker_id,
                                "job_cancelled", "commit race cancelled",
                                false, 522, &error));
        race_events = project.list_job_events(commit_race.job_id, 0, 10, &error);
        assert(race_events.size() == 2);
        assert(race_events.back().event.outcome
               == csgify::JobOutcome::cancelled);

        csgify::ProjectJobSpec abandoned = parent;
        abandoned.job_id = "job:abandoned";
        abandoned.target_id = "solid:abandoned";
        abandoned.input_digest = "abandoned-input";
        abandoned.stable_order = 30;
        assert(project.enqueue_job(abandoned, {}, &error));
        claimed = project.claim_next_job("worker:lost", 200, &error);
        assert(claimed && claimed->spec.job_id == abandoned.job_id);
        assert(project.recover_abandoned_jobs(201, 202, &error) == 1);
        claimed = project.claim_next_job("worker:lost-again", 203, &error);
        assert(claimed && claimed->attempt == 2);
        assert(project.recover_abandoned_jobs(204, 205, &error) == 1);

        csgify::ProjectJobSpec cancelled = parent;
        cancelled.job_id = "job:cancelled";
        cancelled.target_id = "solid:cancelled";
        cancelled.input_digest = "cancelled-input";
        assert(project.enqueue_job(cancelled, {}, &error));
        assert(project.request_job_cancellation(cancelled.job_id, 300, &error));

        const csgify::ProjectJobCounts jobs = project.job_counts(&error);
        assert(jobs.succeeded == 5);
        assert(jobs.failed == 1);
        assert(jobs.cancelled == 3);
        assert(jobs.queued == 0);
        assert(jobs.running == 0);
        const auto failed_jobs = project.list_jobs(10, 0, "failed", &error);
        assert(failed_jobs.size() == 1);
        assert(failed_jobs[0].job_id == abandoned.job_id);
        assert(project.retry_failed_job(abandoned.job_id, 400, &error));
        assert(project.job_counts(&error).queued == 1);
        claimed = project.claim_next_job("worker:recovery", 401, &error);
        assert(claimed && claimed->spec.job_id == abandoned.job_id);
        assert(claimed->attempt == 3);
        assert(project.complete_job(abandoned.job_id, "worker:recovery", artifact,
                                    "certificate:recovered", 402, &error));

        csgify::ProjectAutonomyRequestRecord autonomy;
        autonomy.request_id = "request:project-idempotency";
        autonomy.request_digest = "submitted-request-digest";
        autonomy.source_digest = "source-digest";
        autonomy.policy_digest = "authority-policy-digest";
        autonomy.conversion_policy_digest = "conversion-policy-digest";
        autonomy.lease_owner = "worker:first";
        autonomy.lease_expires_at = 600;
        autonomy.created_at = 500;
        auto reservation = project.reserve_autonomy_request(autonomy, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::reserved);
        assert(project.renew_autonomy_request_lease(
            autonomy.request_id, autonomy.request_digest, autonomy.lease_owner,
            650, &error));
        const auto renewed = project.find_autonomy_request(
            autonomy.request_id, &error);
        assert(renewed && renewed->lease_expires_at == 650);
        assert(!project.renew_autonomy_request_lease(
            autonomy.request_id, autonomy.request_digest, "worker:wrong",
            700, &error));
        error.clear();
        reservation = project.reserve_autonomy_request(autonomy, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::existing);
        csgify::ProjectAutonomyRequestRecord conflicting = autonomy;
        conflicting.request_digest = "different-request-digest";
        reservation = project.reserve_autonomy_request(conflicting, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::conflict);
        csgify::ProjectAutonomyRequestRecord reclaimed = autonomy;
        reclaimed.lease_owner = "worker:recovered";
        reclaimed.lease_expires_at = 750;
        reclaimed.created_at = 651;
        reservation = project.reserve_autonomy_request(reclaimed, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::reserved);
        assert(reservation.record
               && reservation.record->lease_owner == "worker:recovered");
        csgify::ArtifactRecord queue_artifact;
        assert(project.artifacts().publish_bytes(
            "autonomy-queue", "json", "{\"schema_version\":4}\n",
            &queue_artifact, &error));
        assert(project.finish_autonomy_request(
            autonomy.request_id, autonomy.request_digest,
            "worker:recovered",
            csgify::ProjectAutonomyRequestState::awaiting_decision,
            queue_artifact, 652, &error));
        const auto stored_request = project.find_autonomy_request(
            autonomy.request_id, &error);
        assert(stored_request);
        assert(stored_request->state
               == csgify::ProjectAutonomyRequestState::awaiting_decision);
        assert(stored_request->queue_artifact
               && stored_request->queue_artifact->digest
                      == queue_artifact.digest);
        assert(project.claim_autonomy_request(
            autonomy.request_id, autonomy.request_digest, "worker:resume",
            800, &error));
        assert(!project.finish_autonomy_request(
            autonomy.request_id, autonomy.request_digest,
            "worker:recovered",
            csgify::ProjectAutonomyRequestState::completed,
            queue_artifact, 653, &error));
        error.clear();
        assert(project.rollback_autonomy_request_claim(
            autonomy.request_id, autonomy.request_digest, "worker:resume",
            csgify::ProjectAutonomyRequestState::awaiting_decision, &error));
        const auto restored_request = project.find_autonomy_request(
            autonomy.request_id, &error);
        assert(restored_request);
        assert(restored_request->state
               == csgify::ProjectAutonomyRequestState::awaiting_decision);
        assert(restored_request->lease_owner.empty());
        assert(project.claim_autonomy_request(
            autonomy.request_id, autonomy.request_digest, "worker:resume-2",
            800, &error));
        assert(project.finish_autonomy_request(
            autonomy.request_id, autonomy.request_digest, "worker:resume-2",
            csgify::ProjectAutonomyRequestState::completed,
            queue_artifact, 654, &error));

        csgify::ProjectAutonomyRequestRecord abandoned_before_checkpoint =
            autonomy;
        abandoned_before_checkpoint.request_id =
            "request:rollback-before-checkpoint";
        abandoned_before_checkpoint.request_digest =
            "rollback-before-checkpoint-digest";
        abandoned_before_checkpoint.lease_owner = "worker:preflight";
        abandoned_before_checkpoint.lease_expires_at = 900;
        abandoned_before_checkpoint.created_at = 700;
        reservation = project.reserve_autonomy_request(
            abandoned_before_checkpoint, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::reserved);
        assert(project.rollback_autonomy_request_claim(
            abandoned_before_checkpoint.request_id,
            abandoned_before_checkpoint.request_digest,
            abandoned_before_checkpoint.lease_owner,
            csgify::ProjectAutonomyRequestState::running, &error));
        assert(!project.find_autonomy_request(
            abandoned_before_checkpoint.request_id, &error));
        assert(error.empty());

        csgify::ProjectAutonomyRequestRecord interrupted = autonomy;
        interrupted.request_id = "request:interrupted-checkpoint";
        interrupted.request_digest = "interrupted-request-digest";
        interrupted.lease_owner = "worker:interrupted";
        interrupted.lease_expires_at = 900;
        interrupted.created_at = 700;
        reservation = project.reserve_autonomy_request(interrupted, &error);
        assert(reservation.status
               == csgify::ProjectAutonomyReservationStatus::reserved);
        csgify::ArtifactRecord interrupted_artifact;
        assert(project.artifacts().publish_bytes(
            "autonomy-queue", "json",
            "{\"schema_version\":4,\"interrupted\":true}\n",
            &interrupted_artifact, &error));

        sqlite3* fault_database = nullptr;
        assert(sqlite3_open(project.database_path().string().c_str(),
                            &fault_database) == SQLITE_OK);
        assert(sqlite3_exec(
            fault_database,
            "CREATE TRIGGER fail_autonomy_checkpoint BEFORE UPDATE ON "
            "autonomy_requests WHEN NEW.request_id="
            "'request:interrupted-checkpoint' BEGIN SELECT "
            "RAISE(ABORT,'injected checkpoint failure'); END;",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(fault_database);

        assert(!project.finish_autonomy_request(
            interrupted.request_id, interrupted.request_digest,
            interrupted.lease_owner,
            csgify::ProjectAutonomyRequestState::completed,
            interrupted_artifact, 701, &error));
        error.clear();
        const auto interrupted_after_failure = project.find_autonomy_request(
            interrupted.request_id, &error);
        assert(interrupted_after_failure);
        assert(interrupted_after_failure->state
               == csgify::ProjectAutonomyRequestState::running);
        assert(!project.find_artifact(interrupted_artifact.digest,
                                      interrupted_artifact.kind, &error));
        assert(error.empty());

        fault_database = nullptr;
        assert(sqlite3_open(project.database_path().string().c_str(),
                            &fault_database) == SQLITE_OK);
        assert(sqlite3_exec(fault_database,
                            "DROP TRIGGER fail_autonomy_checkpoint;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(fault_database);
        assert(project.finish_autonomy_request(
            interrupted.request_id, interrupted.request_digest,
            interrupted.lease_owner,
            csgify::ProjectAutonomyRequestState::completed,
            interrupted_artifact, 702, &error));
        assert(project.counts(&error).autonomy_requests == 2);
    }

    // Reopen proves the project index and artifact reference are persistent.
    {
        csgify::ProjectStore reopened(root);
        assert(reopened.open_read_only(&error));
        const csgify::ProjectCounts counts = reopened.counts(&error);
        assert(counts.sources == 1);
        assert(counts.component_instances == 1);
        assert(counts.artifacts == 5);
        assert(counts.conversion_cache_entries == 2);
        assert(counts.autonomy_requests == 2);
        assert(counts.agent_conversations == 1);
        assert(counts.agent_messages == 2);
        const csgify::ProjectJobCounts jobs = reopened.job_counts(&error);
        assert(jobs.succeeded == 6);
        assert(jobs.failed == 0);
        assert(jobs.cancelled == 3);
        const auto found = reopened.find_artifact(
            artifact.digest, "semantic-report", &error);
        assert(found.has_value());
        assert(found->relative_path == artifact.relative_path);
        assert(reopened.artifacts().validate(*found, &error));
        const auto cached = reopened.find_conversion_cache(
            "geometry-digest:policy-digest", &error);
        assert(cached.has_value());
        assert(cached->geometry_digest == "geometry-digest");
        assert(cached->policy_digest == "policy-digest");
        assert(cached->certificate_digest == "certificate-digest");
        assert(cached->artifact.relative_path == artifact.relative_path);
        const auto events = reopened.list_job_events(
            "job:independent", 0, 10, &error);
        assert(events.size() == 3);
        assert(events.back().event.phase == csgify::JobPhase::resolving_targets);
        const auto autonomy = reopened.find_autonomy_request(
            "request:project-idempotency", &error);
        assert(autonomy && autonomy->queue_artifact);
        assert(reopened.artifacts().validate(*autonomy->queue_artifact, &error));
        const auto restored_conversations =
            reopened.list_agent_conversations(10, 0, &error);
        assert(restored_conversations.size() == 1);
        const auto restored_messages = reopened.list_agent_messages(
            restored_conversations[0].conversation_id, 10, 0, &error);
        assert(restored_messages.size() == 2);
        assert(restored_messages.back().references.size() == 1);
    }

    std::filesystem::remove_all(root, ec);

    // A schema-6 project can contain a running reservation from before leases
    // existed. Migration adds the lease columns, and the next exact retry can
    // safely reclaim that legacy reservation.
    const std::filesystem::path migration_root = root.string() + "-migration";
    std::filesystem::remove_all(migration_root, ec);
    std::filesystem::create_directories(migration_root);
    sqlite3* legacy_database = nullptr;
    assert(sqlite3_open((migration_root / "project.sqlite").string().c_str(),
                        &legacy_database) == SQLITE_OK);
    assert(sqlite3_exec(
        legacy_database,
        "CREATE TABLE autonomy_requests("
        "request_id TEXT PRIMARY KEY,request_digest TEXT NOT NULL,"
        "source_digest TEXT NOT NULL,policy_digest TEXT NOT NULL,"
        "conversion_policy_digest TEXT NOT NULL,state TEXT NOT NULL,"
        "queue_artifact_algorithm TEXT,queue_artifact_digest TEXT,"
        "queue_artifact_kind TEXT,created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL);"
        "INSERT INTO autonomy_requests VALUES("
        "'request:legacy-running','legacy-digest','legacy-source',"
        "'legacy-policy','legacy-conversion','running',NULL,NULL,NULL,10,10);"
        "PRAGMA user_version=6;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy_database);
    {
        csgify::ProjectStore migrated(migration_root);
        assert(migrated.open(&error));
        assert(migrated.schema_version(&error) == 8);
        const auto legacy = migrated.find_autonomy_request(
            "request:legacy-running", &error);
        assert(legacy && legacy->lease_owner.empty());
        assert(legacy->lease_expires_at == 0);
        csgify::ProjectAutonomyRequestRecord retry = *legacy;
        retry.lease_owner = "worker:migration-recovery";
        retry.lease_expires_at = 200;
        retry.created_at = 100;
        const auto reclaimed = migrated.reserve_autonomy_request(retry, &error);
        assert(reclaimed.status
               == csgify::ProjectAutonomyReservationStatus::reserved);
        assert(reclaimed.record
               && reclaimed.record->lease_owner
                      == "worker:migration-recovery");
    }
    std::filesystem::remove_all(migration_root, ec);
    return 0;
}
