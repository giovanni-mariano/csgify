#pragma once

// Headless decision logic for the interactive viewer.
//
// This translation unit is ALWAYS built (it does not depend on TKOpenGl, AIS,
// V3d, or X11). It owns the viewer's state machine and every session mutation
// the viewer can issue, so the decision logic is testable without a window:
// `run_viewer_headless` replays a key string and the same code path that the
// GUI's X11 loop drives. The GUI (src/viewer.cpp, GL-gated) is a thin shell
// that renders controller state and forwards key events to these methods.

#include "csgify/engine.hpp"   // HoleFillCandidate, SurfaceReplacementCandidate
#include "csgify/session.hpp"  // Session, CommandResult, mutators

#include <iosfwd>
#include <set>
#include <string>
#include <vector>

namespace csgify {

// One entry in a multi-alternative review (e.g. the full set of removal
// suggestions shown together so the user can compare and pick in-window).
struct ViewerReviewAlternative {
    std::string review_id;          // removal:N
    std::string target_id;          // scoped solid id to highlight and act on
    std::string component_instance_id;
    std::string component_definition_id;
    std::string label;
    std::string name;
    std::string hierarchy_path;
    std::string confidence;         // high, medium, low
    double volume_fraction = 0.0;
    double score = 0.0;
    std::vector<std::string> reason_codes;
    std::vector<std::string> blockers;
    std::string suggested_action;
};

struct ViewerReviewContext {
    std::string kind;         // removal, removal_set, hole_fill, decomposition, surface_replacement, contained_medium, or empty
    std::string review_id;    // removal:N, hole-fill:N, candidate:N
    std::string action;       // remove_solid, fill_hole, apply_decomposition_candidate, ...
    std::string target_id;    // scoped solid id when available
    std::string candidate_id; // candidate id when applicable
    HoleFillCandidate hole_fill;
    bool has_hole_fill = false;
    SurfaceReplacementCandidate surface_replacement;
    bool has_surface_replacement = false;
    MediumGenerationProposal medium_proposal;
    MediumGenerationRequest medium_request;
    bool has_medium = false;
    // When non-empty (kind == "removal_set"), the viewer shows every
    // alternative at once and lets the user Tab through them; A/X act on the
    // active one. `action` still names the command emitted on accept.
    std::vector<ViewerReviewAlternative> alternatives;
    DecisionAuditContext audit;
};

enum class AltState { pending, accepted, rejected };

struct ControllerAlternative {
    ViewerReviewAlternative info;
    AltState state = AltState::pending;
};

// Result of handling one decision: what mutated (for the decision log, already
// written by the session call), plus the display delta the renderer applies.
// Tests assert on `op`/`ok`/`stdout_line`; the GUI uses `erased`/`added`.
struct ViewerAction {
    bool handled = false;           // a known key/decision was processed
    bool mutated = false;           // a session op ran
    bool ok = false;                // ...and succeeded
    std::string op;                 // "remove_solid", "apply_decomposition_candidate", ...
    std::string target_id;
    std::string candidate_id;
    std::string message;            // one human line (stderr)
    std::string stdout_line;        // machine line, e.g. "logged remove_solid solid:2"
    std::vector<std::string> erased;  // shape ids no longer present after the op
    std::vector<std::string> added;   // shape ids newly present after the op
    bool alternatives_changed = false; // removal-set needs a recolor
};

// The viewer's decision state machine. Holds no rendering state. Every
// mutating method calls the same session function the CLI uses, so an attached
// decision log records each accepted decision and it replays.
class ViewerController {
public:
    ViewerController(Session& session,
                     std::string document_id,
                     ViewerReviewContext review);

    // State the renderer reads.
    bool removal_set_mode() const { return removal_set_; }
    int active_index() const { return active_; }
    const std::vector<ControllerAlternative>& alternatives() const { return alts_; }
    const ViewerReviewContext& review() const { return review_; }
    std::string active_summary() const;

    // Decisions (no rendering, no X11).
    ViewerAction accept_active();                       // A
    ViewerAction reject_active();                       // X
    ViewerAction remove_target(const std::string& id);  // R (GUI passes selection)
    ViewerAction cycle(bool forward);                   // Tab / Shift-Tab
    ViewerAction select_candidate(const std::string& id); // GUI candidate table
    void configure_decomposition_review(const std::string& target_id,
                                        const std::string& candidate_id,
                                        const DecisionAuditContext& audit = {});
    ViewerAction undo();                                // U
    bool commit(const std::string& out_path);           // Q + --out -> emit deck

private:
    ViewerAction run_op_(const std::string& op,
                         const std::string& target,
                         const std::string& candidate);
    std::set<std::string> snapshot_visible_() const;
    void advance_active_(bool forward);

    Session& session_;
    std::string document_id_;
    ViewerReviewContext review_;
    bool removal_set_ = false;
    std::vector<ControllerAlternative> alts_;
    int active_ = -1;
    std::set<std::string> visible_;  // model-side displayable ids (truth)
};

// Replay a key string against the controller with no window. Keys are
// separated by commas or whitespace:
//   A         accept active            X    reject active
//   Tab       cycle next               BackTab / ShiftTab   cycle previous
//   R:solid:N remove that solid        Q    quit (emit deck if commit_out set)
// Returns a process exit code. Used by `csgify view --keys ...` and tests.
int run_viewer_headless(Session& session,
                        const std::string& document_id,
                        const ViewerReviewContext& review,
                        const std::string& keys,
                        const std::string& commit_out_path,
                        std::ostream& out,
                        std::ostream& err);

} // namespace csgify
