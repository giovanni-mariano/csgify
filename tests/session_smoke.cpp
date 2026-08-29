#include "csgify/session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(const char* message) {
    std::cerr << "session_smoke: " << message << "\n";
    return EXIT_FAILURE;
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

    csgify::CommandResult emit =
        csgify::emit_document(*session, document_id, output_path);
    if (!emit.ok || emit.report.solids_converted != 1) {
        return fail("emit failed for box fixture");
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
