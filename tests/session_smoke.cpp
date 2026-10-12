#include "csgify/session.hpp"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << "session_smoke: " << message << "\n";
    return EXIT_FAILURE;
}

bool ordered_terminal(const std::vector<csgify::JobProgressEvent>& events,
                      csgify::JobOutcome outcome) {
    if (events.empty()) return false;
    std::size_t terminal_count = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].sequence != i + 1) return false;
        if (events[i].phase == csgify::JobPhase::finished) ++terminal_count;
    }
    return terminal_count == 1
        && events.back().phase == csgify::JobPhase::finished
        && events.back().outcome == outcome;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return fail("usage: session_smoke STEP OUT");
    }

    const std::string step_path = argv[1];
    const std::string output_path = argv[2];

    csgify::ConversionOptions options;
    auto session = csgify::create_session(options);
    const csgify::DocumentId document_id = csgify::load_step(*session, step_path);
    if (document_id != "doc:1") {
        return fail("first loaded document should be doc:1");
    }
    if (csgify::document_count(*session) != 1) {
        return fail("session should contain one document");
    }

    csgify::CommandResult analysis = csgify::analyze(*session, document_id);
    if (!analysis.ok || analysis.report.solids_seen != 1) {
        return fail("analysis failed for box fixture");
    }
    if (analysis.report.document_id != "doc:1"
        || analysis.report.solids.empty()
        || analysis.report.solids[0].id != "doc:1/solid:1") {
        return fail("session analysis report should expose document-scoped IDs");
    }

    std::vector<csgify::JobProgressEvent> emit_events;
    csgify::CommandResult emit = csgify::emit_document(
        *session, document_id, output_path, csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            emit_events.push_back(event);
            return true;
        });
    if (!emit.ok || emit.report.solids_converted != 1) {
        return fail("emit failed for box fixture");
    }
    if (!ordered_terminal(emit_events, csgify::JobOutcome::committed)) {
        return fail("emit progress was not ordered with one terminal event");
    }
    bool saw_emit_analysis = false;
    bool saw_emit_verification = false;
    bool saw_emit_commit = false;
    for (const auto& event : emit_events) {
        saw_emit_analysis |= event.phase == csgify::JobPhase::analyzing_solids;
        saw_emit_verification |=
            event.phase == csgify::JobPhase::terminal_verification;
        saw_emit_commit |= event.phase == csgify::JobPhase::committing;
    }
    if (!saw_emit_analysis || !saw_emit_verification || !saw_emit_commit) {
        return fail("emit progress omitted a real processing phase");
    }

    std::vector<csgify::JobProgressEvent> step_events;
    const csgify::CommandResult step_export = csgify::export_document_step(
        *session, document_id, output_path + ".step",
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            step_events.push_back(event);
            return true;
        });
    if (!step_export.ok
        || !ordered_terminal(step_events, csgify::JobOutcome::committed)) {
        return fail("STEP export progress was not ordered/terminal");
    }
    bool saw_step_body = false;
    bool saw_step_write = false;
    for (const auto& event : step_events) {
        saw_step_body |= event.message_code == "step_export_body_collected"
            && event.total_known && event.completed > 0;
        saw_step_write |= event.message_code == "step_export_writing";
    }
    if (!saw_step_body || !saw_step_write) {
        return fail("STEP export omitted body or writer progress");
    }

    const std::string cancelled_step_path = output_path + ".cancelled.step";
    std::remove(cancelled_step_path.c_str());
    std::vector<csgify::JobProgressEvent> cancelled_step_events;
    const csgify::CommandResult cancelled_step = csgify::export_document_step(
        *session, document_id, cancelled_step_path,
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            cancelled_step_events.push_back(event);
            return event.message_code != "step_export_body_collected";
        });
    if (cancelled_step.ok
        || !ordered_terminal(cancelled_step_events,
                             csgify::JobOutcome::cancelled)
        || std::ifstream(cancelled_step_path).good()) {
        return fail("cancelled STEP export wrote output or lost terminal state");
    }

    const std::string cancelled_emit_path = output_path + ".cancelled.i";
    std::remove(cancelled_emit_path.c_str());
    std::vector<csgify::JobProgressEvent> cancelled_emit_events;
    const csgify::CommandResult cancelled_emit = csgify::emit_document(
        *session, document_id, cancelled_emit_path,
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            cancelled_emit_events.push_back(event);
            return event.phase != csgify::JobPhase::committing;
        });
    if (cancelled_emit.ok
        || !ordered_terminal(cancelled_emit_events,
                             csgify::JobOutcome::cancelled)
        || std::ifstream(cancelled_emit_path).good()) {
        return fail("cancelled CSG emission wrote output or lost terminal state");
    }

    const csgify::DocumentId second_document = csgify::load_step(*session, step_path);
    if (second_document != "doc:2") {
        return fail("second loaded document should be doc:2");
    }

    csgify::CommandResult ambiguous =
        csgify::replace_solid_with_bbox(*session, "solid:1");
    if (ambiguous.ok) {
        return fail("unscoped shape ID should be ambiguous with two documents");
    }
    if (csgify::last_report(*session).diagnostics.empty()
        || csgify::last_report(*session).diagnostics[0].code != "ambiguous_shape_id") {
        return fail("failed command should update last_report");
    }

    csgify::CommandResult scoped =
        csgify::replace_solid_with_bbox(*session, "doc:2/solid:1");
    if (!scoped.ok) {
        return fail("scoped shape ID should target the second document");
    }
    if (scoped.report.document_id != "doc:2") {
        return fail("scoped command report should identify the second document");
    }
    if (csgify::document_revision(*session, "doc:2") != 1
        || scoped.report.document_revision != 1) {
        return fail("successful mutation should create revision 1");
    }

    csgify::CommandResult undone =
        csgify::undo_last_mutation(*session, "doc:2");
    if (!undone.ok
        || csgify::document_revision(*session, "doc:2") != 2
        || undone.report.document_revision != 2
        || undone.report.solids_seen != 1) {
        return fail("undo should restore the source solid as revision 2");
    }

    csgify::CommandResult no_more_history =
        csgify::undo_last_mutation(*session, "doc:2");
    if (no_more_history.ok) {
        return fail("undo should fail when revision history is empty");
    }

    csgify::VolumeComparison comparison =
        csgify::compare_document_volumes(*session,
                                          "doc:1",
                                          "doc:2",
                                          0.0,
                                          0.0,
                                          csgify::ReportIdMode::scoped);
    if (!comparison.within_tolerance
        || comparison.original.document_id != "doc:1"
        || comparison.candidate.document_id != "doc:2") {
        return fail("session volume comparison should compare scoped documents");
    }

    csgify::CommandResult second_emit =
        csgify::emit_document(*session, second_document, output_path + ".doc2.i");
    if (!second_emit.ok || second_emit.report.solids_converted != 1) {
        return fail("emit failed after scoped replacement");
    }

    return EXIT_SUCCESS;
}
