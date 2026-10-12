#include "csgify/session.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << "fixed_point_callback: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: fixed_point_callback L_SHAPE.step");

    csgify::SessionPtr session = csgify::create_session();
    const csgify::DocumentId document = csgify::load_step(*session, argv[1]);
    const std::size_t revision_before =
        csgify::document_revision(*session, document);
    std::vector<csgify::FixedPointProgressEvent> events;
    const csgify::FixedPointDecompositionResult result =
        csgify::decompose_to_fixed_point(*session, document, {},
            csgify::ReportIdMode::scoped,
            [&events](const csgify::FixedPointProgressEvent& event) {
                events.push_back(event);
                return event.phase != csgify::FixedPointProgressPhase::started;
            });

    if (result.summary.stop_reason != csgify::FixedPointStopReason::cancelled)
        return fail("callback cancellation did not set the cancelled stop reason");
    if (result.summary.mutated || result.summary.actions_applied != 0)
        return fail("cancellation before search mutated the document");
    if (csgify::document_revision(*session, document) != revision_before)
        return fail("cancelled run changed the document revision");
    if (events.size() != 2
        || events.front().phase != csgify::FixedPointProgressPhase::started
        || events.back().phase != csgify::FixedPointProgressPhase::finished)
        return fail("callback did not receive stable start/finish boundaries");
    if (events.back().stop_reason != csgify::FixedPointStopReason::cancelled)
        return fail("terminal callback did not carry the stop reason");

    {
        csgify::SessionPtr depth_session = csgify::create_session();
        const csgify::DocumentId depth_doc =
            csgify::load_step(*depth_session, argv[1]);
        csgify::FixedPointDecompositionPolicy policy;
        policy.max_target_depth = 0;
        const auto depth = csgify::decompose_to_fixed_point(
            *depth_session, depth_doc, policy);
        if (depth.summary.stop_reason
                != csgify::FixedPointStopReason::depth_budget
            || depth.summary.mutated) {
            return fail("target-depth budget did not stop before mutation");
        }
    }

    {
        csgify::SessionPtr piece_session = csgify::create_session();
        const csgify::DocumentId piece_doc =
            csgify::load_step(*piece_session, argv[1]);
        csgify::FixedPointDecompositionPolicy policy;
        policy.max_active_pieces = 1;
        const auto pieces = csgify::decompose_to_fixed_point(
            *piece_session, piece_doc, policy);
        if (pieces.summary.stop_reason
                != csgify::FixedPointStopReason::piece_budget
            || pieces.summary.mutated) {
            return fail("active-piece budget was not reported precisely");
        }
    }

    {
        // Regression: enabling the expensive fallback must not let shatter
        // outrank an available bounded BSP action.  Disable the L-shape's
        // structural shortcut so this exercises the generic ordering.
        csgify::SessionPtr fallback_session = csgify::create_session();
        const csgify::DocumentId fallback_doc =
            csgify::load_step(*fallback_session, argv[1]);
        csgify::FixedPointDecompositionPolicy policy;
        policy.run_prismatic_strategy = false;
        policy.run_shell_difference_strategy = false;
        policy.run_shatter_fallback = true;
        const auto fallback = csgify::decompose_to_fixed_point(
            *fallback_session, fallback_doc, policy);
        if (!fallback.summary.exact
            || fallback.summary.iterations.empty()) {
            return fail("BSP-before-shatter regression run did not converge");
        }
        if (fallback.summary.iterations.front().strategy != "bsp") {
            return fail("shatter ran before an available BSP improvement");
        }
        if (fallback.command.report.performance.whole_model_trial_copies != 0) {
            return fail("incremental BSP evaluation copied the whole model");
        }
        if (fallback.command.report.performance.local_trials_certified == 0
            || fallback.command.report.performance.full_reconciliations > 3) {
            return fail("incremental BSP certificate/reconciliation counters are wrong");
        }
        if (fallback.command.report.performance
                .local_overlap_containment_proofs == 0) {
            return fail("incremental BSP did not record its containment overlap proof");
        }
        if (fallback.command.report.performance.parallel_plane_batches == 0) {
            return fail("parallel face-plane batch was not evaluated first");
        }
    }

    {
        // Proposal generation itself must observe the hard bound.  Checking
        // only after an exhaustive proposal lets OCCT perform expensive work
        // beyond the advertised limit on large components.
        csgify::SessionPtr budget_session = csgify::create_session();
        const csgify::DocumentId budget_doc =
            csgify::load_step(*budget_session, argv[1]);
        csgify::FixedPointDecompositionPolicy policy;
        policy.run_prismatic_strategy = false;
        policy.run_shell_difference_strategy = false;
        policy.max_candidate_evaluations = 1;
        policy.max_splitter_calls = 1;
        const auto bounded = csgify::decompose_to_fixed_point(
            *budget_session, budget_doc, policy);
        if (bounded.summary.candidates_evaluated > 1
            || bounded.summary.splitter_calls > 1
            || bounded.command.report.performance.splitter_calls > 1) {
            return fail("proposal generation exceeded its hard candidate/splitter budget");
        }
    }
    return EXIT_SUCCESS;
}
