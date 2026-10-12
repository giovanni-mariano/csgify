#include "csgify/session.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int fail(const std::string& message) {
    std::cerr << "job_progress: " << message << '\n';
    return EXIT_FAILURE;
}
bool ordered_and_terminal(const std::vector<csgify::JobProgressEvent>& events,
                          csgify::JobOutcome outcome) {
    if (events.empty()) return false;
    std::size_t terminals = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].sequence != i + 1) return false;
        if (events[i].phase == csgify::JobPhase::finished) ++terminals;
    }
    return terminals == 1 && events.back().phase == csgify::JobPhase::finished
        && events.back().outcome == outcome;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: job_progress FOUR_SOLIDS.step");

    csgify::JobProgressEvent sample;
    sample.job_id = "job:\"unicode-ß";
    sample.sequence = 7;
    sample.operation = csgify::JobOperation::batch_remove;
    sample.phase = csgify::JobPhase::certifying_solids;
    sample.message_code = "certifying";
    sample.target_id = "solid:1";
    sample.completed = 2;
    sample.total = 4;
    sample.total_known = true;
    sample.elapsed_ms = 12.5;
    sample.base_revision = 3;
    sample.changed_shape_ids = {"solid:1", "solid:2"};
    csgify::Diagnostic diagnostic;
    diagnostic.severity = csgify::DiagnosticSeverity::warning;
    diagnostic.code = "review";
    diagnostic.message = "line one\nline two";
    sample.diagnostics.push_back(diagnostic);
    const std::string json = csgify::job_progress_event_json(sample);
    csgify::JobProgressEvent parsed;
    std::string parse_error;
    if (!csgify::parse_job_progress_event_json(json, &parsed, &parse_error))
        return fail("JSON round trip failed: " + parse_error);
    if (parsed.job_id != sample.job_id || parsed.sequence != sample.sequence
        || parsed.phase != sample.phase || parsed.completed != sample.completed
        || parsed.changed_shape_ids != sample.changed_shape_ids
        || parsed.diagnostics.size() != 1
        || parsed.diagnostics[0].message != diagnostic.message)
        return fail("JSON round trip changed event fields");
    std::string bad = json;
    const std::string version = "\"schema_version\":1";
    bad.replace(bad.find(version), version.size(), "\"schema_version\":2");
    if (csgify::parse_job_progress_event_json(bad, &parsed, &parse_error))
        return fail("unsupported schema version was accepted");

    csgify::FixedPointProgressEvent specialized;
    specialized.phase = csgify::FixedPointProgressPhase::candidate_evaluated;
    specialized.candidates_evaluated = 9;
    specialized.splitter_calls = 3;
    specialized.target_id = "solid:3";
    specialized.candidate_id = "candidate:2";
    specialized.state.residual_count = 4;
    std::vector<csgify::JobProgressEvent> adapted;
    auto adapter = csgify::adapt_fixed_point_progress(
        "fixed:1", 5, [&](const csgify::JobProgressEvent& event) {
            adapted.push_back(event);
            return true;
        });
    if (!adapter(specialized) || adapted.size() != 1
        || adapted[0].sequence != 1 || adapted[0].candidates_evaluated != 9
        || adapted[0].splitter_calls != 3 || adapted[0].residual_count != 4
        || adapted[0].candidate_id != specialized.candidate_id)
        return fail("fixed-point adapter lost metrics");

    auto session = csgify::create_session({});
    const auto document = csgify::load_step(*session, argv[1]);
    std::vector<csgify::JobProgressEvent> analysis_events;
    const auto analysis = csgify::analyze(
        *session, document, csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            analysis_events.push_back(event);
            return true;
        });
    if (!analysis.ok || analysis.report.solids.size() < 3)
        return fail("four-solids fixture did not load");
    if (!ordered_and_terminal(analysis_events, csgify::JobOutcome::committed))
        return fail("analysis event stream is not ordered/terminal");
    bool saw_solid_progress = false, saw_analysis_overlap = false;
    for (const auto& event : analysis_events) {
        saw_solid_progress |= event.phase == csgify::JobPhase::analyzing_solids
            && event.total_known && event.completed > 0;
        saw_analysis_overlap |= event.phase == csgify::JobPhase::checking_overlaps;
    }
    if (!saw_solid_progress || !saw_analysis_overlap)
        return fail("analysis omitted per-solid or overlap progress");
    std::vector<csgify::JobProgressEvent> events;
    const auto removed = csgify::remove_solids(
        *session, {analysis.report.solids[0].id, analysis.report.solids[1].id},
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            events.push_back(event);
            return true;
        });
    if (!removed.ok || csgify::document_revision(*session, document) != 1)
        return fail("successful batch did not commit exactly one revision");
    if (!ordered_and_terminal(events, csgify::JobOutcome::committed))
        return fail("successful batch event stream is not ordered/terminal");
    bool saw_certify = false, saw_overlap = false, saw_commit = false;
    for (const auto& event : events) {
        saw_certify |= event.phase == csgify::JobPhase::certifying_solids;
        saw_overlap |= event.phase == csgify::JobPhase::checking_overlaps;
        saw_commit |= event.phase == csgify::JobPhase::committing;
    }
    if (!saw_certify || !saw_overlap || !saw_commit)
        return fail("batch omitted certification phases");

    auto cancelled_session = csgify::create_session({});
    const auto cancelled_document = csgify::load_step(*cancelled_session, argv[1]);
    const auto cancelled_analysis = csgify::analyze(
        *cancelled_session, cancelled_document);
    std::vector<csgify::JobProgressEvent> cancelled_events;
    const auto cancelled = csgify::remove_solids(
        *cancelled_session, {cancelled_analysis.report.solids[0].id},
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            cancelled_events.push_back(event);
            if (event.phase == csgify::JobPhase::certifying_solids)
                throw std::runtime_error("consumer stopped");
            return true;
        });
    if (cancelled.ok
        || csgify::document_revision(*cancelled_session, cancelled_document) != 0)
        return fail("callback exception did not preserve the certified revision");
    if (!ordered_and_terminal(cancelled_events, csgify::JobOutcome::cancelled))
        return fail("cancelled batch event stream is not ordered/terminal");

    auto analysis_cancel_session = csgify::create_session({});
    const auto analysis_cancel_document = csgify::load_step(
        *analysis_cancel_session, argv[1]);
    std::vector<csgify::JobProgressEvent> analysis_cancel_events;
    const auto stopped_analysis = csgify::analyze(
        *analysis_cancel_session, analysis_cancel_document,
        csgify::ReportIdMode::scoped,
        [&](const csgify::JobProgressEvent& event) {
            analysis_cancel_events.push_back(event);
            return !(event.phase == csgify::JobPhase::analyzing_solids
                     && event.completed == 1);
        });
    if (stopped_analysis.ok
        || csgify::document_revision(*analysis_cancel_session,
                                     analysis_cancel_document) != 0)
        return fail("analysis cancellation changed session state");
    if (!ordered_and_terminal(analysis_cancel_events,
                              csgify::JobOutcome::cancelled))
        return fail("cancelled analysis event stream is not ordered/terminal");
    return EXIT_SUCCESS;
}
