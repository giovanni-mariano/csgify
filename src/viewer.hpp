#pragma once

#if CSGIFY_HAVE_TKOPENGL

#include "csgify/engine.hpp"
#include "csgify/session.hpp"
#include "viewer_controller.hpp"  // ViewerReviewContext, ViewerController

#include <string>
#include <vector>

namespace csgify {

// Run the interactive 3D AIS/V3d viewer on the current session state.
//
// The viewer is a live front end onto the session: accepting a candidate
// calls the same mutating session function the CLI would (remove_solid,
// apply_decomposition_candidate, fill_hole, apply_surface_replacement_candidate),
// so if a decision log is attached every accepted decision is recorded and
// replayable. The model is re-read from the session after each mutation, so
// removed solids disappear and new pieces appear without re-importing the STEP.
//
// session        — mutable; at least one document loaded (load_step called).
//                  analyze() should be called first for status coloring.
//                  Attach a decision log before calling to record decisions.
// step_path      — shown in the window title only.
// document_id    — the document accepts/commit act on (e.g. "doc:1").
// preselect_id   — if non-empty, this solid is highlighted on startup.
// proposal       — if non-null, used to look up the cut plane for candidate_id.
// candidate_id   — if non-empty and proposal is non-null, the candidate's
//                  cut plane is shown as a translucent overlay.
// commit_out_path— if non-empty, emit the MCNP deck here when the user quits.
//
// Interaction:
//   left-drag   — orbit      middle-drag — pan
//   scroll      — zoom       F           — fit all
//   click       — select (prints "selected <shape_id>" to stdout)
//   R           — remove the selected solid (logged); geometry updates live
//   A           — accept the active candidate/removal (logged); updates live
//   X           — reject the active candidate/removal (no mutation)
//   Tab/Shift-Tab — cycle the active alternative (removal-set review)
//   H           — hide selected
//   I           — isolate selected
//   S           — show all
//   T           — toggle transparency
//   Q / Esc     — quit (commit the deck if commit_out_path was given)
//
// Returns EXIT_SUCCESS or EXIT_FAILURE.
int run_viewer_command(Session& session,
                       const std::string& step_path,
                       const std::string& document_id,
                       const std::string& preselect_id,
                       const DecompositionProposal* proposal,
                       const std::string& candidate_id,
                       const ViewerReviewContext& review = {},
                       const std::string& commit_out_path = {});

} // namespace csgify

#endif // CSGIFY_HAVE_TKOPENGL
