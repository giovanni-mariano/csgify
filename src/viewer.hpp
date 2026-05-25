#pragma once

#if CAD2CSG_HAVE_TKOPENGL

#include "cad2csg/engine.hpp"
#include "cad2csg/session.hpp"

#include <string>

namespace cad2csg {

struct ViewerReviewContext {
    std::string kind;         // removal, hole_fill, decomposition, surface_replacement, or empty
    std::string review_id;    // removal:N, hole-fill:N, candidate:N
    std::string action;       // remove_solid, fill_hole, apply_decomposition_candidate
    std::string target_id;    // scoped solid id when available
    std::string candidate_id; // candidate id when applicable
    HoleFillCandidate hole_fill;
    bool has_hole_fill = false;
    SurfaceReplacementCandidate surface_replacement;
    bool has_surface_replacement = false;
};

// Run the interactive 3D AIS/V3d viewer on the current session state.
//
// session        — must have at least one document loaded (load_step called).
//                  analyze() should be called first for status coloring.
// step_path      — shown in the window title only.
// preselect_id   — if non-empty, this solid is highlighted on startup.
// proposal       — if non-null, used to look up the cut plane for candidate_id.
// candidate_id   — if non-empty and proposal is non-null, the candidate's
//                  cut plane is shown as a translucent overlay.
//
// Interaction:
//   left-drag   — orbit      middle-drag — pan
//   scroll      — zoom       F           — fit all
//   click       — select (prints "selected <shape_id>" to stdout)
//   R           — print "command remove_solid <shape_id>"
//   A           — print accept command for the active candidate/selection
//   X           — print reject command for the active candidate/selection
//   H           — hide selected
//   I           — isolate selected
//   S           — show all
//   T           — toggle transparency
//   Q / Esc     — quit
//
// Returns EXIT_SUCCESS or EXIT_FAILURE.
int run_viewer_command(const Session& session,
                       const std::string& step_path,
                       const std::string& preselect_id,
                       const DecompositionProposal* proposal,
                       const std::string& candidate_id,
                       const ViewerReviewContext& review = {});

} // namespace cad2csg

#endif // CAD2CSG_HAVE_TKOPENGL
