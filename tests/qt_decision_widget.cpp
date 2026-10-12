#include "viewer_qt.hpp"

#include "csgify/session.hpp"

#include <cassert>

int main(int argc, char** argv) {
    assert(argc == 2);
    csgify::SessionPtr session = csgify::create_session({});
    const csgify::DocumentId document = csgify::load_step(*session, argv[1]);
    const csgify::CommandResult analysis = csgify::analyze(
        *session, document, csgify::ReportIdMode::scoped);
    assert(analysis.ok);
    assert(analysis.report.solids_requiring_decomposition > 0);
    return csgify::run_qt_decision_widget_smoke(
        *session, argv[1], document);
}
