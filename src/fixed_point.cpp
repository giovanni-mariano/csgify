#include "csgify/fixed_point.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <tuple>

namespace csgify {

DecompositionProgressScore make_decomposition_progress_score(
    const DecompositionProgressState& state,
    double baseline_volume,
    const FixedPointDecompositionPolicy& policy) {
    DecompositionProgressScore score;
    score.unsupported_face_count = state.unsupported_face_count;
    score.halfspace_violation_count = state.halfspace_violation_count;
    score.residual_count = state.residual_count;
    score.active_piece_count = state.active_piece_count;

    const double relative_width = std::abs(baseline_volume)
        * std::max(0.0, policy.progress_volume_epsilon);
    const double width = std::max(relative_width,
                                  std::max(0.0, policy.absolute_volume_epsilon));
    if (!std::isfinite(state.residual_volume) || state.residual_volume < 0.0
        || !std::isfinite(width) || width <= 0.0) {
        score.residual_volume_bucket =
            std::numeric_limits<std::uint64_t>::max();
        return score;
    }
    const long double bucket = std::ceil(
        static_cast<long double>(state.residual_volume)
        / static_cast<long double>(width));
    const long double maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    score.residual_volume_bucket = bucket >= maximum
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(bucket);
    return score;
}

bool decomposition_progress_less(const DecompositionProgressScore& left,
                                 const DecompositionProgressScore& right) {
    return std::tie(left.unsupported_face_count,
                    left.residual_volume_bucket,
                    left.halfspace_violation_count,
                    left.residual_count,
                    left.active_piece_count)
        < std::tie(right.unsupported_face_count,
                   right.residual_volume_bucket,
                   right.halfspace_violation_count,
                   right.residual_count,
                   right.active_piece_count);
}

const char* fixed_point_stop_reason_name(FixedPointStopReason reason) {
    switch (reason) {
    case FixedPointStopReason::exact: return "exact";
    case FixedPointStopReason::already_exact: return "already_exact";
    case FixedPointStopReason::no_improving_candidate: return "no_improving_candidate";
    case FixedPointStopReason::unsupported_geometry: return "unsupported_geometry";
    case FixedPointStopReason::action_budget: return "action_budget";
    case FixedPointStopReason::candidate_budget: return "candidate_budget";
    case FixedPointStopReason::splitter_budget: return "splitter_budget";
    case FixedPointStopReason::piece_budget: return "piece_budget";
    case FixedPointStopReason::residual_budget: return "residual_budget";
    case FixedPointStopReason::depth_budget: return "depth_budget";
    case FixedPointStopReason::elapsed_time: return "elapsed_time";
    case FixedPointStopReason::overlap: return "overlap";
    case FixedPointStopReason::volume_drift: return "volume_drift";
    case FixedPointStopReason::cancelled: return "cancelled";
    case FixedPointStopReason::internal_error: return "internal_error";
    }
    return "internal_error";
}

const char* fixed_point_progress_phase_name(FixedPointProgressPhase phase) {
    switch (phase) {
    case FixedPointProgressPhase::started: return "started";
    case FixedPointProgressPhase::initial_analysis_completed:
        return "initial_analysis_completed";
    case FixedPointProgressPhase::target_started: return "target_started";
    case FixedPointProgressPhase::candidate_evaluated: return "candidate_evaluated";
    case FixedPointProgressPhase::action_accepted: return "action_accepted";
    case FixedPointProgressPhase::terminal_verification_started:
        return "terminal_verification_started";
    case FixedPointProgressPhase::finished: return "finished";
    }
    return "unknown";
}

FixedPointProgressCallback adapt_fixed_point_progress(
    std::string job_id,
    std::size_t base_revision,
    JobProgressCallback progress) {
    struct State {
        std::string job_id;
        std::size_t base_revision = 0;
        std::uint64_t sequence = 0;
        JobProgressCallback progress;
    };
    auto state = std::make_shared<State>();
    state->job_id = std::move(job_id);
    state->base_revision = base_revision;
    state->progress = std::move(progress);
    return [state](const FixedPointProgressEvent& source) {
        if (!state->progress) return true;
        JobProgressEvent event;
        event.job_id = state->job_id;
        event.sequence = ++state->sequence;
        event.operation = JobOperation::fixed_point_decomposition;
        event.base_revision = state->base_revision;
        event.committed_revision = source.committed_revision == 0
            && state->base_revision != 0
            ? state->base_revision : source.committed_revision;
        event.elapsed_ms = source.elapsed_ms;
        event.target_id = source.target_id;
        event.candidate_id = source.candidate_id;
        event.strategy = source.strategy;
        event.actions_applied = source.actions_applied;
        event.candidates_evaluated = source.candidates_evaluated;
        event.splitter_calls = source.splitter_calls;
        event.completed = source.candidates_evaluated;
        event.total_known = false;
        event.residual_count = source.state.residual_count;
        event.active_piece_count = source.state.active_piece_count;
        event.residual_volume = source.state.residual_volume;
        switch (source.phase) {
        case FixedPointProgressPhase::started:
            event.phase = JobPhase::analyzing_solids;
            event.message_code = "fixed_point_started";
            break;
        case FixedPointProgressPhase::initial_analysis_completed:
            event.phase = JobPhase::certifying_solids;
            event.message_code = "fixed_point_initial_analysis_completed";
            break;
        case FixedPointProgressPhase::target_started:
            event.phase = JobPhase::certifying_solids;
            event.message_code = "fixed_point_target_started";
            break;
        case FixedPointProgressPhase::candidate_evaluated:
            event.phase = JobPhase::certifying_solids;
            event.message_code = "fixed_point_candidate_evaluated";
            break;
        case FixedPointProgressPhase::action_accepted:
            event.phase = JobPhase::staging_mutation;
            event.message_code = "fixed_point_action_accepted";
            event.changed_shape_ids.push_back(source.target_id);
            break;
        case FixedPointProgressPhase::terminal_verification_started:
            event.phase = JobPhase::terminal_verification;
            event.message_code = "fixed_point_terminal_verification_started";
            break;
        case FixedPointProgressPhase::finished:
            event.phase = JobPhase::finished;
            event.message_code = "fixed_point_finished";
            if (source.stop_reason == FixedPointStopReason::cancelled) {
                event.outcome = JobOutcome::cancelled;
            } else if (source.actions_applied > 0
                       && source.state.residual_count > 0) {
                event.outcome = JobOutcome::certified_partial;
            } else if (source.state.residual_count == 0) {
                event.outcome = JobOutcome::committed;
            } else {
                event.outcome = JobOutcome::failed;
            }
            break;
        }
        try {
            return state->progress(event);
        } catch (...) {
            return false;
        }
    };
}

} // namespace csgify
