#pragma once

#if CSGIFY_HAVE_QT6_VIEWER

#include "csgify/engine.hpp"
#include "csgify/autonomy.hpp"
#include "csgify/alea_void_bridge.hpp"
#include "csgify/session.hpp"
#include "csgify/project_store.hpp"
#include "viewer_controller.hpp"

#include <string>

namespace csgify {

// Qt 6 decision-review frontend. Geometry is rendered by OCCT AIS/V3d in a
// native Qt widget; every mutation is delegated to ViewerController.
int run_qt_viewer_command(Session& session,
                          const std::string& step_path,
                          const std::string& document_id,
                          const std::string& preselect_id,
                          const DecompositionProposal* proposal,
                          const std::string& candidate_id,
                          const ViewerReviewContext& review,
                          const std::string& commit_out_path);

// Offscreen interaction regression for the real decision window. It avoids
// showing the native OCCT viewport while driving its Qt tree, actions, worker
// completion, cancellation, and close-event policy.
int run_qt_decision_widget_smoke(Session& session,
                                 const std::string& step_path,
                                 const std::string& document_id);

int run_qt_autonomy_queue(Session& session,
                          const std::string& step_path,
                          AutonomyQueue& queue,
                          const std::string& queue_path);

// Read-only project-scale coverage/void review. Unsafe findings are always
// presented as human-review blockers; this window never mutates geometry or
// weakens the MCNP exact-one ownership gate.
int run_qt_coverage_review(const AleaCoverageCertificate& certificate,
                           const std::string& certificate_path);

// Project-scale control plane. Opening this window performs SQLite queries
// only; OCCT geometry is loaded later, when the operator explicitly asks for
// geometric review.
int run_qt_project_workspace(const std::string& project_path,
                             const std::string& source_path,
                             const std::string& coverage_path,
                             const std::string& queue_path,
                             bool smoke_test = false);

// Project entry point used when no project directory was supplied. New
// projects receive a conservative default policy; existing projects are
// validated before the workspace opens.
int run_qt_project_launcher(bool smoke_test = false);

} // namespace csgify

#endif
