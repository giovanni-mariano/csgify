#pragma once

#include "csgify/assembly.hpp"
#include "csgify/progress.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace csgify {

struct ConversionReport;
struct ConversionOptions;

struct ArtifactRecord {
    std::string digest_algorithm;
    std::string digest;
    std::string kind;
    std::string relative_path;
    std::uint64_t size = 0;
    bool deduplicated = false;
};

// Immutable, content-addressed payload storage used by the resumable project
// database. This class deliberately owns files only below root()/objects and
// root()/tmp; callers retain ownership of source and output files.
class ArtifactStore {
public:
    explicit ArtifactStore(std::filesystem::path root);

    bool initialize(std::string* error = nullptr) const;

    bool publish_bytes(const std::string& kind,
                       const std::string& extension,
                       const std::string& contents,
                       ArtifactRecord* record,
                       std::string* error = nullptr) const;

    bool publish_file(const std::string& kind,
                      const std::string& extension,
                      const std::filesystem::path& source,
                      ArtifactRecord* record,
                      std::string* error = nullptr) const;

    bool validate(const ArtifactRecord& record,
                  std::string* error = nullptr) const;

    std::filesystem::path absolute_path(const ArtifactRecord& record) const;
    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

struct ProjectSnapshot {
    std::string source_path;
    std::string source_digest;
    StringTable strings;
    std::vector<EntityMetadata> metadata;
    std::vector<ComponentDefinition> component_definitions;
    std::vector<ComponentInstance> component_instances;
    std::vector<MaterialDefinition> materials;
};

struct ProjectCounts {
    std::uint64_t sources = 0;
    std::uint64_t strings = 0;
    std::uint64_t metadata_entities = 0;
    std::uint64_t component_definitions = 0;
    std::uint64_t component_instances = 0;
    std::uint64_t materials = 0;
    std::uint64_t artifacts = 0;
    std::uint64_t conversion_cache_entries = 0;
    std::uint64_t spatial_entities = 0;
    std::uint64_t autonomy_requests = 0;
    std::uint64_t agent_conversations = 0;
    std::uint64_t agent_messages = 0;
};

enum class ProjectAgentMessageRole { user, assistant };
enum class ProjectAgentMessageState { complete, interrupted, failed };

struct ProjectAgentEvidenceReference {
    std::string kind;
    std::string id;
    std::string label;
};

struct ProjectAgentConversationRecord {
    std::string conversation_id;
    std::string title;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

struct ProjectAgentMessageRecord {
    std::string message_id;
    std::string conversation_id;
    std::string parent_message_id;
    ProjectAgentMessageRole role = ProjectAgentMessageRole::user;
    ProjectAgentMessageState state = ProjectAgentMessageState::complete;
    std::string text;
    std::string context_id;
    std::string context_digest;
    std::string document_id;
    std::size_t document_revision = 0;
    std::string target_id;
    std::string run_id;
    std::string provider_id;
    std::string model_id;
    bool simulated = false;
    std::int64_t created_at = 0;
    std::uint64_t ordinal = 0;
    std::vector<ProjectAgentEvidenceReference> references;
};

const char* project_agent_message_role_name(ProjectAgentMessageRole role);
const char* project_agent_message_state_name(ProjectAgentMessageState state);

enum class ProjectAutonomyRequestState {
    running,
    awaiting_decision,
    completed,
    failed,
};

struct ProjectAutonomyRequestRecord {
    std::string request_id;
    std::string request_digest;
    std::string source_digest;
    std::string policy_digest;
    std::string conversion_policy_digest;
    ProjectAutonomyRequestState state = ProjectAutonomyRequestState::running;
    std::optional<ArtifactRecord> queue_artifact;
    std::string lease_owner;
    std::int64_t lease_expires_at = 0;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

enum class ProjectAutonomyReservationStatus {
    reserved,
    existing,
    conflict,
    error,
};

struct ProjectAutonomyReservation {
    ProjectAutonomyReservationStatus status =
        ProjectAutonomyReservationStatus::error;
    std::optional<ProjectAutonomyRequestRecord> record;
};

const char* project_autonomy_request_state_name(
    ProjectAutonomyRequestState state);

struct ProjectSourceSummary {
    std::string source_path;
    std::string source_digest;
};

struct ProjectDefinitionSummary {
    std::string source_digest;
    std::string definition_id;
    std::string display_name;
    std::string geometry_digest;
    bool geometry_digest_verified = false;
    std::uint64_t instance_count = 0;
};

struct ProjectInstanceSummary {
    std::string source_digest;
    std::string instance_id;
    std::string definition_id;
    std::string parent_assembly_id;
    std::string source_shape_id;
    std::string display_name;
    std::string material_override;
    bool active = true;
};

struct SpatialBounds {
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    bool valid = false;
};

struct SpatialEntry {
    std::string entity_id;
    std::string owner_id;
    std::string kind;
    SpatialBounds bounds;
};

using SpatialCandidatePair = std::pair<std::string, std::string>;

std::vector<SpatialEntry> spatial_entries_from_report(
    const ConversionReport& report);

struct ConversionCacheEntry {
    std::string cache_key;
    std::string geometry_digest;
    std::string policy_digest;
    ArtifactRecord artifact;
    std::string certificate_digest;
};

struct DefinitionCacheSyncResult {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t published = 0;
    std::size_t skipped_unverified = 0;
};

enum class ProjectJobState {
    queued,
    running,
    succeeded,
    failed,
    cancelled,
};

struct ProjectJobLimits {
    std::uint64_t memory_bytes = 0;
    std::uint64_t wall_time_ms = 0;
    std::uint64_t cpu_time_seconds = 0;
    std::uint64_t file_size_bytes = 0;
};

struct ProjectJobSpec {
    std::string job_id;
    std::string target_id;
    std::string operation;
    std::string input_digest;
    std::string policy_digest;
    std::int64_t stable_order = 0;
    std::uint32_t max_attempts = 1;
    ProjectJobLimits limits;
};

struct ClaimedProjectJob {
    ProjectJobSpec spec;
    ProjectJobState state = ProjectJobState::queued;
    std::uint32_t attempt = 0;
    std::string worker_id;
    bool cancel_requested = false;
};

struct ProjectJobCounts {
    std::uint64_t queued = 0;
    std::uint64_t running = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
};

struct ProjectJobSummary {
    std::string job_id;
    std::string target_id;
    std::string operation;
    ProjectJobState state = ProjectJobState::queued;
    std::uint32_t attempt = 0;
    std::uint32_t max_attempts = 0;
    std::string worker_id;
    std::string error_code;
    std::string error_message;
    std::int64_t started_at = 0;
    std::int64_t heartbeat_at = 0;
    std::int64_t finished_at = 0;
    std::uint64_t latest_sequence = 0;
    JobPhase latest_phase = JobPhase::queued;
    std::size_t completed = 0;
    std::size_t total = 0;
    bool total_known = false;
};

struct StoredJobProgressEvent {
    JobProgressEvent event;
    std::int64_t emitted_at = 0;
    std::string artifact_digest;
};

const char* project_job_state_name(ProjectJobState state);

ProjectSnapshot project_snapshot_from_report(const ConversionReport& report,
                                             std::string source_digest = {});

// Transactional index for resumable projects. Payload files are owned by the
// ArtifactStore; SQLite contains semantic identities, dependency metadata, and
// immutable artifact references.
class ProjectStore {
public:
    explicit ProjectStore(std::filesystem::path root);
    ~ProjectStore();

    ProjectStore(const ProjectStore&) = delete;
    ProjectStore& operator=(const ProjectStore&) = delete;
    ProjectStore(ProjectStore&&) noexcept;
    ProjectStore& operator=(ProjectStore&&) noexcept;

    bool open(std::string* error = nullptr);
    // Open an already initialized project without creating directories,
    // databases, journals, or schema migrations. Intended for status/event
    // inspection paths whose user-facing contract is read-only.
    bool open_read_only(std::string* error = nullptr);
    bool is_open() const;
    int schema_version(std::string* error = nullptr) const;

    bool persist_snapshot(const ProjectSnapshot& snapshot,
                          std::string* error = nullptr);
    bool record_artifact(const ArtifactRecord& artifact,
                         std::string* error = nullptr);
    std::optional<ArtifactRecord> find_artifact(
        const std::string& digest,
        const std::string& kind,
        std::string* error = nullptr) const;
    bool record_conversion_cache(const ConversionCacheEntry& entry,
                                 std::string* error = nullptr);
    std::optional<ConversionCacheEntry> find_conversion_cache(
        const std::string& cache_key,
        std::string* error = nullptr) const;
    // Publish one immutable recognition certificate per verified geometry
    // definition. Cache hits require both the geometry and policy digest to
    // match; occurrences never become independent cache entries.
    bool sync_definition_recognition_cache(
        const ConversionReport& report,
        const ConversionOptions& options,
        DefinitionCacheSyncResult* result = nullptr,
        std::string* error = nullptr);
    // Conversations are durable project records, independent from geometry
    // runs. Messages are append-only and bind any model answer to the context
    // revision and evidence references that supported it.
    bool upsert_agent_conversation(
        const ProjectAgentConversationRecord& conversation,
        std::string* error = nullptr);
    bool append_agent_message(const ProjectAgentMessageRecord& message,
                              std::string* error = nullptr);
    std::vector<ProjectAgentConversationRecord> list_agent_conversations(
        std::size_t limit = 100, std::size_t offset = 0,
        std::string* error = nullptr) const;
    std::vector<ProjectAgentMessageRecord> list_agent_messages(
        const std::string& conversation_id,
        std::size_t limit = 100, std::size_t offset = 0,
        std::string* error = nullptr) const;
    // Atomically reserve one external autonomy request ID. Reusing the ID with
    // different bound input is a conflict; an exact retry returns the existing
    // durable outcome instead of creating another logical operation.
    ProjectAutonomyReservation reserve_autonomy_request(
        const ProjectAutonomyRequestRecord& request,
        std::string* error = nullptr);
    bool claim_autonomy_request(
        const std::string& request_id,
        const std::string& request_digest,
        const std::string& lease_owner,
        std::int64_t lease_expires_at,
        std::string* error = nullptr);
    bool renew_autonomy_request_lease(
        const std::string& request_id,
        const std::string& request_digest,
        const std::string& lease_owner,
        std::int64_t lease_expires_at,
        std::string* error = nullptr);
    // Undo ownership when execution stops before a durable queue checkpoint.
    // A fresh/reclaimed reservation is deleted; a claimed review or failed
    // request is restored to its preceding state.
    bool rollback_autonomy_request_claim(
        const std::string& request_id,
        const std::string& request_digest,
        const std::string& lease_owner,
        ProjectAutonomyRequestState previous_state,
        std::string* error = nullptr);
    std::optional<ProjectAutonomyRequestRecord> find_autonomy_request(
        const std::string& request_id,
        std::string* error = nullptr) const;
    // Publish/index the immutable queue and transition the matching reservation
    // in one SQLite transaction.
    bool finish_autonomy_request(
        const std::string& request_id,
        const std::string& request_digest,
        const std::string& lease_owner,
        ProjectAutonomyRequestState state,
        const ArtifactRecord& queue_artifact,
        std::int64_t now_epoch_seconds,
        std::string* error = nullptr);
    bool enqueue_job(const ProjectJobSpec& job,
                     const std::vector<std::string>& dependency_job_ids = {},
                     std::string* error = nullptr);
    std::optional<ClaimedProjectJob> claim_next_job(
        const std::string& worker_id,
        std::int64_t now_epoch_seconds,
        std::string* error = nullptr);
    // Atomically claim one known ready job. Interactive clients use this to
    // avoid consuming an unrelated queued worker job.
    std::optional<ClaimedProjectJob> claim_job(
        const std::string& job_id,
        const std::string& worker_id,
        std::int64_t now_epoch_seconds,
        std::string* error = nullptr);
    bool heartbeat_job(const std::string& job_id,
                       const std::string& worker_id,
                       std::int64_t now_epoch_seconds,
                       std::string* error = nullptr);
    // Append-only audit stream. Duplicate (job_id, sequence) pairs are
    // rejected; callers may safely resume by listing after a known sequence.
    bool append_job_event(const JobProgressEvent& event,
                          std::int64_t emitted_at,
                          const std::string& artifact_digest = {},
                          std::string* error = nullptr);
    std::vector<StoredJobProgressEvent> list_job_events(
        const std::string& job_id,
        std::uint64_t after_sequence = 0,
        std::size_t limit = 1000,
        std::string* error = nullptr) const;
    std::uint64_t latest_job_event_sequence(
        const std::string& job_id,
        std::string* error = nullptr) const;
    bool complete_job(const std::string& job_id,
                      const std::string& worker_id,
                      const ArtifactRecord& artifact,
                      const std::string& certificate_digest,
                      std::int64_t now_epoch_seconds,
                      std::string* error = nullptr);
    // Atomically append the artifact-linked terminal event and transition the
    // still-owned, non-cancelled job to succeeded.
    bool complete_job_with_event(const std::string& job_id,
                                 const std::string& worker_id,
                                 const ArtifactRecord& artifact,
                                 const std::string& certificate_digest,
                                 const JobProgressEvent& terminal_event,
                                 std::int64_t now_epoch_seconds,
                                 std::string* error = nullptr);
    bool fail_job(const std::string& job_id,
                  const std::string& worker_id,
                  const std::string& error_code,
                  const std::string& message,
                  bool retryable,
                  std::int64_t now_epoch_seconds,
                  std::string* error = nullptr);
    bool request_job_cancellation(const std::string& job_id,
                                  std::int64_t now_epoch_seconds,
                                  std::string* error = nullptr);
    bool job_cancellation_requested(const std::string& job_id,
                                    const std::string& worker_id,
                                    bool* requested,
                                    std::string* error = nullptr) const;
    std::uint64_t recover_abandoned_jobs(
        std::int64_t stale_before_epoch_seconds,
        std::int64_t now_epoch_seconds,
        std::string* error = nullptr);
    // Explicit operator action: grant one additional attempt to a failed job
    // and return it to the deterministic queue. Successful/cancelled/running
    // jobs are never changed by this operation.
    bool retry_failed_job(const std::string& job_id,
                          std::int64_t now_epoch_seconds,
                          std::string* error = nullptr);
    ProjectJobCounts job_counts(std::string* error = nullptr) const;
    std::vector<ProjectSourceSummary> list_sources(
        std::size_t limit = 100, std::size_t offset = 0,
        std::string* error = nullptr) const;
    std::vector<ProjectDefinitionSummary> list_component_definitions(
        std::size_t limit = 100, std::size_t offset = 0,
        std::string* error = nullptr) const;
    std::vector<ProjectInstanceSummary> list_component_instances(
        std::size_t limit = 100, std::size_t offset = 0,
        std::string* error = nullptr) const;
    std::vector<ProjectDefinitionSummary> search_component_definitions(
        const std::string& query, std::size_t limit = 100,
        std::size_t offset = 0, std::string* error = nullptr) const;
    std::vector<ProjectInstanceSummary> search_component_instances(
        const std::string& query, std::size_t limit = 100,
        std::size_t offset = 0, std::string* error = nullptr) const;
    std::vector<ProjectJobSummary> list_jobs(
        std::size_t limit = 100, std::size_t offset = 0,
        const std::string& state = {}, std::string* error = nullptr) const;
    std::vector<ArtifactRecord> list_artifacts(
        std::size_t limit = 100, std::size_t offset = 0,
        const std::string& kind = {}, std::string* error = nullptr) const;
    std::vector<std::string> list_spatial_namespaces(
        std::string* error = nullptr) const;
    std::vector<SpatialEntry> list_spatial_entries(
        const std::string& model_namespace, std::size_t limit = 100,
        std::size_t offset = 0, const std::string& kind = {},
        const std::string& search = {}, std::string* error = nullptr) const;
    bool replace_spatial_entries(const std::string& model_namespace,
                                 const std::vector<SpatialEntry>& entries,
                                 std::string* error = nullptr);
    std::vector<SpatialEntry> query_spatial_entries(
        const std::string& model_namespace,
        const SpatialBounds& bounds,
        const std::string& kind = {},
        double expansion = 0.0,
        std::string* error = nullptr) const;
    std::vector<SpatialCandidatePair> spatial_overlap_candidates(
        const std::string& model_namespace,
        const std::string& kind,
        double expansion = 0.0,
        std::size_t maximum_pairs = 0,
        std::string* error = nullptr) const;
    ProjectCounts counts(std::string* error = nullptr) const;

    ArtifactStore& artifacts() { return artifacts_; }
    const ArtifactStore& artifacts() const { return artifacts_; }
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path database_path() const;

private:
    struct Impl;
    std::filesystem::path root_;
    ArtifactStore artifacts_;
    std::unique_ptr<Impl> impl_;
};

} // namespace csgify
