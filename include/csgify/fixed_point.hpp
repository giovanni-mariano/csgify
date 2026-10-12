#pragma once

#include "csgify/engine.hpp"
#include "csgify/progress.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace csgify {

struct DecompositionProgressState {
    std::size_t unsupported_face_count = 0;
    double residual_volume = 0.0;
    std::size_t halfspace_violation_count = 0;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
    std::size_t exact_body_count = 0;
    std::size_t overlap_error_count = 0;
    double total_volume = 0.0;
};

struct DecompositionProgressScore {
    std::size_t unsupported_face_count = 0;
    std::uint64_t residual_volume_bucket = 0;
    std::size_t halfspace_violation_count = 0;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
};

struct FixedPointDecompositionPolicy {
    DecompositionPolicy candidate_policy;
    // Match the exact decomposition certificate by default. Callers may
    // tighten these independently, but the coordinator must not reject a
    // partition already accepted by the engine's 1e-6 numerical tolerance.
    double max_action_relative_volume_delta = 1.0e-6;
    double max_cumulative_relative_volume_delta = 1.0e-6;
    double progress_volume_epsilon = 1.0e-10;
    double absolute_volume_epsilon = 1.0e-9;
    double min_piece_volume_fraction = 1.0e-8;
    std::size_t max_actions = 64;
    std::size_t max_candidate_evaluations = 4096;
    std::size_t max_splitter_calls = 4096;
    std::size_t max_active_pieces = 4096;
    std::size_t max_residual_pieces = 256;
    // Bound expensive OCCT proposal/certificate work before yielding to the
    // next residual body. Large assemblies should make broad progress instead
    // of exhausting the global time budget on one difficult component.
    std::size_t max_candidates_per_target = 64;
    std::size_t max_stagnant_target_visits = 2;
    int max_target_depth = 16;
    std::uint64_t max_elapsed_ms = 0;
    bool allow_medium_certified_progress = true;
    bool allow_low_certified_progress = false;
    bool allow_bbox_fallback = false;
    bool commit_certified_partial_progress = true;
    bool run_prismatic_strategy = true;
    bool run_shell_difference_strategy = true;
    bool run_shatter_fallback = false;
    bool remove_residual_slivers = true;
    // Evaluate ordinary BSP candidates as immutable local partitions and run
    // one authoritative whole-model reconciliation only after an accepted
    // commit. The legacy copied-model path remains as a qualification oracle.
    bool use_incremental_trials = true;
    std::size_t max_local_boolean_calls = 2048;
    bool verify_incremental_state_each_action = false;
    bool run_parallel_plane_batches = true;
    std::size_t max_parallel_families_per_target = 8;
    std::size_t max_planes_per_batch = 64;
};

enum class FixedPointStopReason {
    exact,
    already_exact,
    no_improving_candidate,
    unsupported_geometry,
    action_budget,
    candidate_budget,
    splitter_budget,
    piece_budget,
    residual_budget,
    depth_budget,
    elapsed_time,
    overlap,
    volume_drift,
    cancelled,
    internal_error,
};

enum class FixedPointProgressPhase {
    started,
    initial_analysis_completed,
    target_started,
    candidate_evaluated,
    action_accepted,
    terminal_verification_started,
    finished,
};

// Immutable snapshot delivered at safe boundaries between OCCT operations.
// Returning false requests cancellation. An OCCT Boolean already in flight is
// allowed to finish; the last certified model state is then preserved.
struct FixedPointProgressEvent {
    FixedPointProgressPhase phase = FixedPointProgressPhase::started;
    std::size_t actions_applied = 0;
    std::size_t candidates_evaluated = 0;
    std::size_t splitter_calls = 0;
    std::string target_id;
    std::string candidate_id;
    std::string strategy;
    DecompositionProgressState state;
    double elapsed_ms = 0.0;
    std::size_t base_revision = 0;
    std::size_t committed_revision = 0;
    FixedPointStopReason stop_reason = FixedPointStopReason::internal_error;
};

using FixedPointProgressCallback =
    std::function<bool(const FixedPointProgressEvent&)>;

// Lossless bridge for consumers migrating to the general job stream. Sequence
// starts at one and exactly mirrors the specialized callback's event count.
FixedPointProgressCallback adapt_fixed_point_progress(
    std::string job_id,
    std::size_t base_revision,
    JobProgressCallback progress);

struct FixedPointIterationRecord {
    std::size_t iteration = 0;
    std::string target_id;
    std::string candidate_id;
    std::string strategy;
    std::string confidence;
    DecompositionProgressState before;
    DecompositionProgressState after;
    double action_relative_volume_delta = 0.0;
    double cumulative_relative_volume_delta = 0.0;
    std::size_t pieces_before_coalesce = 0;
    std::size_t pieces_after_coalesce = 0;
    std::size_t candidates_evaluated = 0;
    bool accepted = false;
    std::string rejection_reason;
};

struct FixedPointDecompositionSummary {
    bool exact = false;
    bool mutated = false;
    FixedPointStopReason stop_reason = FixedPointStopReason::internal_error;
    DecompositionProgressState baseline;
    DecompositionProgressState final_state;
    std::size_t actions_applied = 0;
    std::size_t candidates_evaluated = 0;
    std::size_t splitter_calls = 0;
    std::size_t peak_active_pieces = 0;
    double elapsed_ms = 0.0;
    std::vector<FixedPointIterationRecord> iterations;
};

DecompositionProgressScore make_decomposition_progress_score(
    const DecompositionProgressState& state,
    double baseline_volume,
    const FixedPointDecompositionPolicy& policy);

bool decomposition_progress_less(const DecompositionProgressScore& left,
                                 const DecompositionProgressScore& right);

const char* fixed_point_stop_reason_name(FixedPointStopReason reason);

const char* fixed_point_progress_phase_name(FixedPointProgressPhase phase);

} // namespace csgify
