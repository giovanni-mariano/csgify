#pragma once

#include "csgify/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace csgify {

enum class JobOperation {
    analyze,
    batch_remove,
    fixed_point_decomposition,
    decomposition_preview,
    export_step,
    emit_csg,
    alea_void_generation,
};

enum class JobPhase {
    queued,
    resolving_targets,
    loading_geometry,
    analyzing_solids,
    staging_mutation,
    certifying_solids,
    checking_overlaps,
    terminal_verification,
    committing,
    awaiting_review,
    finished,
};

enum class JobOutcome {
    none,
    committed,
    certified_partial,
    cancelled,
    rolled_back,
    failed,
};

// Immutable, value-type snapshot. Consumers must use stable IDs and copied
// diagnostics; engine and OCCT objects never cross this boundary.
struct JobProgressEvent {
    unsigned schema_version = 1;
    std::string job_id;
    std::uint64_t sequence = 0;
    JobOperation operation = JobOperation::analyze;
    JobPhase phase = JobPhase::queued;
    JobOutcome outcome = JobOutcome::none;
    std::string message_code;
    std::string target_id;
    std::string candidate_id;
    std::string strategy;
    std::size_t completed = 0;
    std::size_t total = 0;
    bool total_known = false;
    double elapsed_ms = 0.0;
    std::size_t base_revision = 0;
    std::size_t committed_revision = 0;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> changed_shape_ids;
    std::string artifact_id;

    // Operation-specific counters retained by the generic fixed-point adapter.
    std::size_t actions_applied = 0;
    std::size_t candidates_evaluated = 0;
    std::size_t splitter_calls = 0;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
    double residual_volume = 0.0;
};

using JobProgressCallback = std::function<bool(const JobProgressEvent&)>;

const char* job_operation_name(JobOperation operation);
const char* job_phase_name(JobPhase phase);
const char* job_outcome_name(JobOutcome outcome);

// Compact JSON suitable for one JSONL record. The parser rejects unknown
// schema versions and malformed enum names.
std::string job_progress_event_json(const JobProgressEvent& event);
bool parse_job_progress_event_json(const std::string& json,
                                   JobProgressEvent* event,
                                   std::string* error = nullptr);

} // namespace csgify
