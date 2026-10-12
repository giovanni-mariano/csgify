#include "csgify/session.hpp"
#include "session_view.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << "decomposition_preview: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return fail("usage: decomposition_preview L_SHAPE.step OVERLAP.step");

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document = csgify::load_step(*session, argv[1]);
    csgify::analyze(*session, document);

    const std::size_t revision_before =
        csgify::document_revision(*session, document);
    const std::vector<csgify::SolidViewEntry> entries_before =
        csgify::collect_view_entries(*session);
    const csgify::DecompositionProposal proposal =
        csgify::propose_decomposition(*session, "doc:1/solid:1");
    if (!proposal.ok || proposal.best_candidate_id.empty()) {
        return fail("fixture produced no usable decomposition candidate");
    }

    const csgify::DecompositionCandidate* candidate = nullptr;
    for (const csgify::DecompositionCandidate& item : proposal.candidates) {
        if (item.id == proposal.best_candidate_id) {
            candidate = &item;
            break;
        }
    }
    if (!candidate) return fail("best candidate was not present in proposal");

    std::vector<csgify::JobProgressEvent> preview_events;
    const csgify::DecompositionPreview preview =
        csgify::collect_decomposition_preview(
            *session, *candidate,
            [&](const csgify::JobProgressEvent& event) {
                preview_events.push_back(event);
                return true;
            });
    if (!preview.ok || !preview.summary_matches
        || preview.pieces.size() != candidate->piece_count) {
        return fail("preview shapes do not match candidate summary");
    }
    if (preview.exact_count + preview.residual_count + preview.sliver_count
        != preview.pieces.size()) {
        return fail("preview classifications do not cover every piece");
    }
    if (!preview.certificate_attempted || !preview.partition_exact
        || !preview.valid_children || !preview.contained_in_parent
        || !preview.covers_parent || !preview.siblings_disjoint
        || !preview.volume_within_tolerance
        || preview.certificate_boolean_calls == 0
        || !preview.overlap_safe || preview.overlap_unknown
        || preview.recommended_next_action.empty()) {
        return fail("preview did not expose an exact local safety certificate");
    }
    if (preview_events.empty()
        || preview_events.back().phase != csgify::JobPhase::finished
        || preview_events.back().outcome
               != csgify::JobOutcome::certified_partial) {
        return fail("preview progress omitted its reviewable terminal event");
    }
    bool saw_piece = false;
    bool saw_overlap_phase = false;
    std::size_t terminals = 0;
    for (std::size_t i = 0; i < preview_events.size(); ++i) {
        if (preview_events[i].sequence != i + 1)
            return fail("preview progress sequence is not contiguous");
        saw_piece |= preview_events[i].phase
                         == csgify::JobPhase::certifying_solids
            && preview_events[i].total_known
            && preview_events[i].completed > 0;
        saw_overlap_phase |= preview_events[i].phase
            == csgify::JobPhase::checking_overlaps;
        terminals += preview_events[i].phase == csgify::JobPhase::finished;
    }
    if (!saw_piece || !saw_overlap_phase || terminals != 1)
        return fail("preview progress omitted piece/overlap boundaries");
    const double scale = std::max(1.0, std::abs(candidate->pieces_volume));
    if (std::abs(preview.total_volume - candidate->pieces_volume) > 1.0e-9 * scale) {
        return fail("preview volume differs from candidate volume");
    }
    if (csgify::document_revision(*session, document) != revision_before
        || csgify::collect_view_entries(*session).size() != entries_before.size()) {
        return fail("preview mutated the live session");
    }

    std::vector<csgify::JobProgressEvent> cancelled_preview_events;
    const csgify::DecompositionPreview cancelled_preview =
        csgify::collect_decomposition_preview(
            *session, *candidate,
            [&](const csgify::JobProgressEvent& event) {
                cancelled_preview_events.push_back(event);
                return !(event.phase == csgify::JobPhase::certifying_solids
                         && event.completed == 1);
            });
    if (cancelled_preview.ok || cancelled_preview_events.empty()
        || cancelled_preview_events.back().outcome
               != csgify::JobOutcome::cancelled
        || csgify::document_revision(*session, document) != revision_before) {
        return fail("preview cancellation did not preserve the live revision");
    }

    csgify::SessionPtr overlap_session = csgify::create_session(options);
    const csgify::DocumentId overlap_document =
        csgify::load_step(*overlap_session, argv[2]);
    const csgify::CommandResult overlap_analysis =
        csgify::analyze(*overlap_session, overlap_document);
    if (overlap_analysis.report.overlaps.empty())
        return fail("overlap fixture did not report an overlap");
    std::size_t conflict_count = 0;
    for (const csgify::SolidViewEntry& entry
         : csgify::collect_view_entries(*overlap_session)) {
        if (entry.overlap_conflict) ++conflict_count;
    }
    if (conflict_count < 2)
        return fail("viewer entries did not tag both overlap participants");
    return EXIT_SUCCESS;
}
