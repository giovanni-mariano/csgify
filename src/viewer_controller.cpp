#include "viewer_controller.hpp"

#include "session_view.hpp"  // SolidViewEntry, collect_view_entries

#include <cstdlib>
#include <ostream>
#include <sstream>
#include <utility>

namespace csgify {
namespace {

ViewerAction message_action(const std::string& msg) {
    ViewerAction a;
    a.handled = true;
    a.message = msg;
    return a;
}

} // namespace

ViewerController::ViewerController(Session& session,
                                   std::string document_id,
                                   ViewerReviewContext review)
    : session_(session),
      document_id_(std::move(document_id)),
      review_(std::move(review)) {
    removal_set_ =
        review_.kind == "removal_set" && !review_.alternatives.empty();
    for (const ViewerReviewAlternative& a : review_.alternatives) {
        alts_.push_back(ControllerAlternative{a, AltState::pending});
    }
    visible_ = snapshot_visible_();
    if (removal_set_) {
        active_ = -1;
        advance_active_(true); // land on the first pending alternative
    }
}

std::set<std::string> ViewerController::snapshot_visible_() const {
    std::set<std::string> ids;
    for (const SolidViewEntry& e : collect_view_entries(session_)) {
        ids.insert(e.shape_id);
    }
    return ids;
}

void ViewerController::advance_active_(bool forward) {
    const int n = static_cast<int>(alts_.size());
    if (n == 0) { active_ = -1; return; }
    for (int step = 1; step <= n; ++step) {
        const int idx = forward
            ? (active_ + step) % n
            : ((active_ - step) % n + n) % n;
        if (alts_[idx].state == AltState::pending) {
            active_ = idx;
            return;
        }
    }
    active_ = -1; // none pending
}

std::string ViewerController::active_summary() const {
    if (active_ < 0 || active_ >= static_cast<int>(alts_.size())) {
        return "no pending alternatives left";
    }
    const ControllerAlternative& a = alts_[active_];
    std::ostringstream os;
    os << "active " << a.info.review_id << " -> " << a.info.target_id
       << "  (" << a.info.confidence << ", vol " << a.info.volume_fraction
       << ")  [" << (active_ + 1) << "/" << alts_.size() << "]";
    return os.str();
}

ViewerAction ViewerController::run_op_(const std::string& op,
                                       const std::string& target,
                                       const std::string& candidate) {
    ViewerAction act;
    act.handled = true;
    act.mutated = true;
    act.op = op;
    act.target_id = target;
    act.candidate_id = candidate;

    const std::set<std::string> before = snapshot_visible_();

    CommandResult r;
    if (op == "remove_solid") {
        r = remove_solid(session_, target);
    } else if (op == "apply_decomposition_candidate") {
        r = apply_decomposition_candidate(session_, target, candidate);
    } else if (op == "fill_hole") {
        r = fill_hole(session_, document_id_, candidate);
    } else if (op == "apply_surface_replacement_candidate") {
        r = apply_surface_replacement_candidate(session_, target, candidate);
    } else {
        act.mutated = false;
        act.message = "unknown op " + op;
        return act;
    }

    act.ok = r.ok;
    if (!r.ok) {
        act.message = op + " " + target + " failed (see report)";
        return act;
    }

    const std::set<std::string> after = snapshot_visible_();
    for (const std::string& id : before) {
        if (after.find(id) == after.end()) act.erased.push_back(id);
    }
    for (const std::string& id : after) {
        if (before.find(id) == before.end()) act.added.push_back(id);
    }
    visible_ = after;

    std::ostringstream os;
    os << "logged " << op << " " << target;
    if (!candidate.empty()) os << " " << candidate;
    act.stdout_line = os.str();
    return act;
}

ViewerAction ViewerController::accept_active() {
    if (removal_set_) {
        if (active_ < 0) {
            return message_action("no pending removal alternatives to accept");
        }
        ControllerAlternative& a = alts_[active_];
        ViewerAction act = run_op_(review_.action.empty() ? "remove_solid"
                                                           : review_.action,
                                   a.info.target_id, "");
        if (act.ok) {
            act.message = "accepted " + a.info.review_id
                        + " (" + a.info.target_id + ")";
            a.state = AltState::accepted;
            advance_active_(true);
            act.alternatives_changed = true;
        }
        return act;
    }
    if (!review_.action.empty()) {
        return run_op_(review_.action, review_.target_id, review_.candidate_id);
    }
    return message_action("no active candidate to accept");
}

ViewerAction ViewerController::reject_active() {
    if (removal_set_) {
        if (active_ < 0) {
            return message_action("no pending removal alternatives to reject");
        }
        ControllerAlternative& a = alts_[active_];
        a.state = AltState::rejected;
        ViewerAction act;
        act.handled = true;
        act.alternatives_changed = true;
        act.message = "rejected " + a.info.review_id
                    + " (" + a.info.target_id + ")  (no change)";
        advance_active_(true);
        return act;
    }
    if (!review_.review_id.empty()) {
        return message_action("rejected " + review_.kind + " "
                              + review_.review_id + "  (no change)");
    }
    return message_action("nothing to reject");
}

ViewerAction ViewerController::remove_target(const std::string& id) {
    if (id.empty()) {
        return message_action("no selected solid; click a solid first.");
    }
    return run_op_("remove_solid", id, "");
}

ViewerAction ViewerController::cycle(bool forward) {
    if (!removal_set_) {
        return message_action("cycling only applies to removal-set review");
    }
    advance_active_(forward);
    ViewerAction act;
    act.handled = true;
    act.alternatives_changed = true;
    act.message = active_summary();
    return act;
}

bool ViewerController::commit(const std::string& out_path) {
    if (out_path.empty()) return true;
    const CommandResult e = emit_document(session_, document_id_, out_path);
    return e.ok;
}

namespace {

// Split a key string on commas and whitespace into tokens.
std::vector<std::string> tokenize_keys(const std::string& keys) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : keys) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

} // namespace

int run_viewer_headless(Session& session,
                        const std::string& document_id,
                        const ViewerReviewContext& review,
                        const std::string& keys,
                        const std::string& commit_out_path,
                        std::ostream& out,
                        std::ostream& err) {
    ViewerController controller(session, document_id, review);
    if (controller.removal_set_mode()) {
        err << "  " << controller.active_summary() << "\n";
    }

    bool committed = false;
    bool commit_ok = true;
    for (const std::string& tok : tokenize_keys(keys)) {
        ViewerAction a;
        if (tok == "A" || tok == "a") {
            a = controller.accept_active();
        } else if (tok == "X" || tok == "x") {
            a = controller.reject_active();
        } else if (tok == "Tab") {
            a = controller.cycle(true);
        } else if (tok == "ShiftTab" || tok == "BackTab") {
            a = controller.cycle(false);
        } else if (tok.rfind("R:", 0) == 0) {
            a = controller.remove_target(tok.substr(2));
        } else if (tok == "Q" || tok == "q") {
            const bool ok = controller.commit(commit_out_path);
            committed = true;
            commit_ok = ok;
            if (!commit_out_path.empty()) {
                err << (ok ? "  emitted deck -> " : "  emit failed: ")
                    << commit_out_path << "\n";
            }
            break;
        } else {
            err << "  unknown key '" << tok << "'\n";
            continue;
        }
        if (!a.stdout_line.empty()) { out << a.stdout_line << "\n"; out.flush(); }
        if (!a.message.empty()) err << "  " << a.message << "\n";
    }

    if (!committed) {
        const bool ok = controller.commit(commit_out_path);
        commit_ok = ok;
        if (!commit_out_path.empty()) {
            err << (ok ? "  emitted deck -> " : "  emit failed: ")
                << commit_out_path << "\n";
        }
    }
    return commit_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace csgify
