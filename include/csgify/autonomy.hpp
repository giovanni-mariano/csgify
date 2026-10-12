#pragma once

#include "csgify/alea_void_bridge.hpp"
#include "csgify/session.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace csgify {

enum class AutonomyMode { explain, propose, automatic };
enum class AutonomyDisposition { auto_apply, human_review, denied };

struct AutonomyPolicy {
    AutonomyMode mode = AutonomyMode::propose;
    bool allow_high_confidence_decomposition = true;
    bool allow_certified_intermediate_decomposition = false;
    bool allow_exact_surface_replacement = true;
    bool allow_approximate_surface_replacement = false;
    bool allow_removal = false;
    bool allow_contained_medium_generation = false;
    // Immutable human/project configuration. An LLM action can reference one
    // of these definitions but cannot introduce or alter a material.
    std::vector<MaterialDefinition> contained_medium_materials;
    double max_relative_volume_delta = 1.0e-8;
    double max_intermediate_relative_volume_delta = 0.0;
    double min_piece_volume_fraction = 1.0e-8;
    double max_surface_deviation = 0.0;
    double max_removal_volume_fraction = 0.0;
    std::size_t max_auto_actions = 32;
    std::size_t max_intermediate_pieces = 64;
    std::size_t max_intermediate_residual_pieces = 4;
    // This policy is supplied by the human/tool invocation. Proposals cannot
    // carry or modify it; execution re-measures geometry transactionally.
    ApproximationPolicy approximation;
};

// Import/preparation settings affect the geometry seen by every later
// candidate. They are persisted separately from authority policy so a queue
// can restore the same model before replaying accepted operations.
struct AutonomyPreparationPolicy {
    bool heal_shape = true;
    std::string unit_override;
    double unit_scale_to_mm_override = 0.0;
    std::string normalize_to_unit;
};

// Human-owned, versioned configuration shared by context generation and
// autonomy execution. Descriptive fields travel with the project policy but
// do not grant additional execution authority.
struct AutonomyProjectPolicy {
    std::string project_id;
    std::string intended_analysis;
    AutonomyPreparationPolicy preparation;
    AutonomyPolicy authority;
};

struct AutonomyOperationParameters {
    // Normalized effective value, including when omitted by the proposer.
    int shatter_max_depth = 8;
};

struct AutonomyProposal {
    std::string id;
    std::string actor = "llm";
    std::string op;
    std::string target_id;
    std::string candidate_id;
    std::string rationale;
    AutonomyOperationParameters parameters;
    std::optional<MediumGenerationRequest> medium_request;
};

struct AutonomyEvidence {
    std::string confidence;
    bool usable = false;
    bool exact = false;
    bool certificate_passed = false;
    double relative_volume_delta = 0.0;
    double max_deviation = 0.0;
    double volume_fraction = 0.0;
    std::size_t piece_count = 0;
    std::size_t residual_count = 0;
    std::size_t sliver_count = 0;
    bool mutation_applied = false;
};

struct AutonomyQueueItem {
    AutonomyProposal proposal;
    AutonomyDisposition disposition = AutonomyDisposition::denied;
    std::string policy_rule;
    std::string reason;
    std::size_t evaluated_revision = 0;
    AutonomyEvidence evidence;
    bool executed = false;
    bool execution_ok = false;
    bool requires_followup = false;
    bool rejected = false;
    std::string review_rationale;
};

struct AutonomyQueue {
    // Stable identity of the external request. The digest binds the ID,
    // source/revision precondition, and normalized action payloads.
    std::string request_id;
    std::string request_digest;
    std::size_t expected_revision = 0;
    std::string document_id;
    std::string source_path;
    // Content identity of source_path. Required before geometry actions may
    // be reconstructed in a later process.
    std::string source_digest;
    // Digests detect accidental edits and prevent a resume from silently
    // inheriting different geometry or authority policy.
    std::string policy_digest;
    std::string conversion_policy_digest;
    AutonomyPreparationPolicy preparation;
    std::string coverage_certificate_path;
    std::string coverage_certificate_digest;
    AutonomyMode mode = AutonomyMode::propose;
    AutonomyPolicy policy;
    std::vector<AutonomyQueueItem> items;
    std::size_t auto_apply_count = 0;
    std::size_t human_review_count = 0;
    std::size_t denied_count = 0;
    std::size_t applied_count = 0;
    std::size_t failed_count = 0;
};

struct AutonomyRequest {
    std::string request_id;
    std::string source_digest;
    std::size_t expected_revision = 0;
    std::string actor = "llm";
    std::vector<AutonomyProposal> actions;
};

struct AutonomyOperationExecution {
    CommandResult command;
    // Some coordinators report useful successful completion (for example,
    // already exact) without a mutating CommandResult. This is the common
    // logical success used by both live execution and restart replay.
    bool ok = false;
};

struct AutonomyContextOptions {
    std::string project_id;
    std::string intended_analysis;
    AutonomyPreparationPolicy preparation;
    std::string target_id;
    std::string history_path;
    std::size_t max_diagnostics = 128;
    std::size_t max_history_records = 32;
    std::size_t max_candidates = 32;
    std::size_t candidate_offset = 0;
    std::size_t solid_offset = 0;
    std::size_t max_solids = 32;
    std::size_t max_faces_per_solid = 32;
    std::size_t occurrence_offset = 0;
    std::size_t max_occurrences = 32;
    std::size_t max_materials = 32;
    std::size_t max_interfaces = 32;
    std::size_t overlap_offset = 0;
    std::size_t max_local_overlaps = 32;
    std::size_t max_serialized_bytes = 1024 * 1024;
    bool include_candidates = true;
    bool include_removal_candidates = true;
};

AutonomyRequest read_autonomy_request_json(const std::string& path);
AutonomyRequest parse_autonomy_request_json(const std::string& json);
AutonomyProjectPolicy read_autonomy_project_policy_json(
    const std::string& path);
// Atomically writes the complete versioned policy. Every field understood by
// the reader is emitted so a UI editing a small supported subset cannot erase
// materials, protection lists, or authority limits it did not modify.
int write_autonomy_project_policy_json(
    const std::string& path,
    const AutonomyProjectPolicy& policy);
int write_default_autonomy_project_policy_json(
    const std::string& path,
    const std::string& project_id,
    const std::string& intended_analysis = "geometry-prototype");
AutonomyQueue read_autonomy_queue_json(const std::string& path);
bool reject_autonomy_queue_item(AutonomyQueue& queue,
                                const std::string& item_id,
                                const std::string& rationale);
void refresh_autonomy_queue_counts(AutonomyQueue& queue);
void reevaluate_pending_autonomy_queue(Session& session,
                                       AutonomyQueue& queue,
                                       const AutonomyPolicy& policy);
AutonomyQueue evaluate_autonomy_queue(Session& session,
                                      const DocumentId& document_id,
                                      const std::vector<AutonomyProposal>& proposals,
                                      const AutonomyPolicy& policy);
AutonomyQueue make_coverage_autonomy_queue(
    const AleaCoverageCertificate& certificate,
    std::string certificate_path,
    std::string source_path = {},
    const AutonomyPolicy& policy = {});
void reconcile_coverage_autonomy_queue(
    AutonomyQueue& queue,
    const AleaCoverageCertificate& replacement,
    std::string replacement_path);
void execute_autonomy_queue(Session& session,
                            AutonomyQueue& queue,
                            const AutonomyPolicy& policy);
AutonomyOperationExecution execute_autonomy_operation(
    Session& session,
    const DocumentId& document_id,
    AutonomyQueueItem& item,
    const AutonomyPolicy& policy);
int write_autonomy_queue_json(const AutonomyQueue& queue,
                              const std::string& path);
// Builds exactly the same bounded context used by file/stdout export, but
// keeps it in memory for an embedded agent provider.
std::string build_autonomy_context_json(
    Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AutonomyContextOptions& options);
int write_autonomy_context_json(Session& session,
                                const DocumentId& document_id,
                                const AutonomyPolicy& policy,
                                const AutonomyContextOptions& options,
                                const std::string& path);

const char* autonomy_mode_name(AutonomyMode mode);
const char* autonomy_disposition_name(AutonomyDisposition disposition);
std::string autonomy_policy_digest(const AutonomyPolicy& policy);
std::string autonomy_conversion_policy_digest(
    const ConversionOptions& options);
std::string autonomy_request_digest(const AutonomyRequest& request);

} // namespace csgify
