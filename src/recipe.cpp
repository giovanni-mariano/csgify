#include "recipe.hpp"

#include "csgify/session.hpp"
#include "json_parser.hpp"
#include "operation_registry.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace csgify {
namespace {

// JSON parsing helpers live in src/json_parser.hpp so the replay
// command can share the same parser.

// ---- Compaction ---------------------------------------------------------

struct FingerprintRecord {
    bool valid = false;
    double volume = 0.0;
    double surface_area = 0.0;
    double bbox_diagonal = 0.0;
    int face_count = 0;
    double centroid_x = 0.0;
    double centroid_y = 0.0;
    double centroid_z = 0.0;
};

struct Decision {
    std::size_t seq = 0;
    std::string kind;             // "decision" or "undo"
    std::string op;
    unsigned operation_version = 1; // legacy unversioned logs mean v1
    std::string result;
    std::string target_kind;      // "solid" | "face" | "document"
    std::string target_shape_id;  // scoped, e.g. "doc:1/solid:5"
    std::string component_entry;
    FingerprintRecord fingerprint;
    std::unordered_map<std::string, std::shared_ptr<JsonValue>> params;
    std::size_t undoes_seq = 0;   // only for "undo"
    std::string review_target_id;
    std::string proposal_id;
    std::string review_decision;
    std::string review_kind;
    std::string review_action;
    std::string review_candidate_id;
    std::size_t review_base_revision = 0;
    DecisionAuditContext audit;
};

struct RecipePostconditions {
    bool present = false;
    bool has_decomposition_state = false;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
    std::size_t max_overlap_errors = 0;
    bool has_approximation_ledger = false;
    double cumulative_changed_volume = 0.0;
    double cumulative_added_volume = 0.0;
    double cumulative_removed_volume = 0.0;
};

RecipePostconditions derive_postconditions(const std::vector<Decision>& ops) {
    RecipePostconditions out;
    for (const Decision& op : ops) {
        const auto residual = op.params.find("residual_count_after");
        const auto pieces = op.params.find("active_piece_count_after");
        if (residual == op.params.end() || pieces == op.params.end()
            || !residual->second || !pieces->second) {
            const OperationDescriptor* descriptor = find_operation(op.op);
            if (out.present && descriptor && descriptor->mutating) {
                // A later unrelated mutation invalidates the coordinator's
                // terminal counts. Omit rather than encode stale evidence.
                out = {};
            }
            continue;
        }
        out.present = true;
        out.has_decomposition_state = true;
        out.residual_count = static_cast<std::size_t>(
            residual->second->as_number());
        out.active_piece_count = static_cast<std::size_t>(
            pieces->second->as_number());
        out.max_overlap_errors = 0;
    }
    for (const Decision& op : ops) {
        if (op.op != "apply_budgeted_replacement_operation"
            && op.op != "shatter_with_approximation"
            && op.op != "remove_residual_with_approximation"
            && op.op != "resolve_overlap_with_approximation"
            && op.op != "apply_budgeted_surface_replacement_candidate") continue;
        auto changed = op.params.find("changed_volume");
        auto added = op.params.find("added_volume");
        auto removed = op.params.find("removed_volume");
        if (changed == op.params.end() || added == op.params.end()
            || removed == op.params.end()) continue;
        out.present = true;
        out.has_approximation_ledger = true;
        out.cumulative_changed_volume += changed->second->as_number();
        out.cumulative_added_volume += added->second->as_number();
        out.cumulative_removed_volume += removed->second->as_number();
    }
    return out;
}

// Category groups ops that supersede one another on the same target. A
// later op of the same category replaces an earlier one in the recipe.
std::string category_for(const std::string& op) {
    if (const OperationDescriptor* descriptor = find_operation(op)) {
        return descriptor->category;
    }
    return op;
}

// Translates the operation enum recorded by apply_replacement_operation
// back into the specific named method, so the recipe reads naturally.
const char* replacement_enum_to_method(int operation) {
    const OperationDescriptor* descriptor = find_replacement(
        static_cast<ReplacementOperation>(operation));
    return descriptor ? descriptor->name : nullptr;
}

const char* spline_fit_kind_to_lua(int kind) {
    switch (kind) {
    case 1: return "plane";
    case 2: return "sphere";
    case 3: return "cylinder";
    case 4: return "cone";
    default: return nullptr;
    }
}

std::string lua_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

std::string format_double(double v) {
    std::ostringstream out;
    out.precision(17);
    out << v;
    return out.str();
}

Decision read_record(const JsonValue& obj) {
    Decision d;
    if (const JsonValue* v = obj.get("kind")) d.kind = v->as_string();
    if (const JsonValue* v = obj.get("seq")) {
        d.seq = static_cast<std::size_t>(v->as_number());
    }
    if (const JsonValue* v = obj.get("op")) d.op = v->as_string();
    if (const JsonValue* v = obj.get("operation_version")) {
        d.operation_version = static_cast<unsigned>(v->as_number());
    }
    if (const JsonValue* v = obj.get("result")) d.result = v->as_string();
    if (const JsonValue* v = obj.get("undoes_seq")) {
        d.undoes_seq = static_cast<std::size_t>(v->as_number());
    }
    if (const JsonValue* v = obj.get("target_id"))
        d.review_target_id = v->as_string();
    if (const JsonValue* v = obj.get("proposal_id"))
        d.proposal_id = v->as_string();
    if (const JsonValue* v = obj.get("decision"))
        d.review_decision = v->as_string();
    if (const JsonValue* v = obj.get("review_kind"))
        d.review_kind = v->as_string();
    if (const JsonValue* v = obj.get("action"))
        d.review_action = v->as_string();
    if (const JsonValue* v = obj.get("candidate_id"))
        d.review_candidate_id = v->as_string();
    if (const JsonValue* v = obj.get("base_revision"))
        d.review_base_revision = static_cast<std::size_t>(v->as_number());
    if (const JsonValue* audit = obj.get("audit")) {
        if (const JsonValue* v = audit->get("actor"))
            d.audit.actor = v->as_string();
        if (const JsonValue* v = audit->get("proposal_actor"))
            d.audit.proposal_actor = v->as_string();
        if (const JsonValue* v = audit->get("proposal_id"))
            d.audit.proposal_id = v->as_string();
        if (const JsonValue* v = audit->get("rationale"))
            d.audit.rationale = v->as_string();
        if (const JsonValue* v = audit->get("policy_rule"))
            d.audit.policy_rule = v->as_string();
        if (const JsonValue* v = audit->get("approval_mode"))
            d.audit.approval_mode = v->as_string();
    }
    if (const JsonValue* t = obj.get("target")) {
        if (const JsonValue* v = t->get("kind")) d.target_kind = v->as_string();
        if (const JsonValue* v = t->get("shape_id")) d.target_shape_id = v->as_string();
        if (const JsonValue* v = t->get("component_entry")) d.component_entry = v->as_string();
        if (const JsonValue* fp = t->get("fingerprint")) {
            d.fingerprint.valid = true;
            if (const JsonValue* v = fp->get("volume"))        d.fingerprint.volume        = v->as_number();
            if (const JsonValue* v = fp->get("surface_area"))  d.fingerprint.surface_area  = v->as_number();
            if (const JsonValue* v = fp->get("bbox_diagonal")) d.fingerprint.bbox_diagonal = v->as_number();
            if (const JsonValue* v = fp->get("face_count"))    d.fingerprint.face_count    = static_cast<int>(v->as_number());
            if (const JsonValue* c = fp->get("centroid")) {
                if (c->kind == JsonKind::array && c->arr.size() == 3) {
                    d.fingerprint.centroid_x = c->arr[0]->as_number();
                    d.fingerprint.centroid_y = c->arr[1]->as_number();
                    d.fingerprint.centroid_z = c->arr[2]->as_number();
                }
            }
        }
    }
    if (const JsonValue* p = obj.get("params")) {
        if (p->kind == JsonKind::object) {
            for (const auto& kv : p->obj) {
                d.params.emplace(kv.first, kv.second);
            }
        }
    }
    return d;
}

std::vector<Decision> compact_reviews(const std::vector<Decision>& records) {
    std::map<std::size_t, Decision> latest;
    std::unordered_map<std::string, std::size_t> sequence_by_identity;
    for (const Decision& record : records) {
        if (record.kind != "review" || record.review_target_id.empty()
            || record.proposal_id.empty() || record.review_decision.empty()) {
            continue;
        }
        const std::string identity = record.review_target_id + "\n"
            + record.proposal_id;
        Decision normalized = record;
        if (normalized.audit.proposal_id.empty())
            normalized.audit.proposal_id = normalized.proposal_id;
        const auto previous = sequence_by_identity.find(identity);
        if (previous != sequence_by_identity.end())
            latest.erase(previous->second);
        sequence_by_identity[identity] = record.seq;
        latest[record.seq] = std::move(normalized);
    }
    std::vector<Decision> reviews;
    reviews.reserve(latest.size());
    for (const auto& entry : latest) reviews.push_back(entry.second);
    return reviews;
}

struct Manifest {
    std::string step_path;
    std::string step_digest;
    std::string csgify_version;
    std::string occt_version;
    std::string created_at;
};

// Build the compacted list of ops to emit, in causal order.
//
// MVP rules (subset of docs/DECISION_LOG_AND_RECIPE.md "Compaction Rules"):
//   1. Drop failed/skipped records.
//   2. Drop records that were undone by a later "undo" record.
//   3. For each (target, category), keep only the latest surviving op.
//   4. Drop ops whose target is later remove_solid'd. (A "removal" of
//      itself survives — the model genuinely has that solid removed.)
//   5. load_step is always first; export_step and emit_mcnp are last.
//
// Steps deferred to step 3:
//   - Intermediate-decomposition pieces: this MVP keeps ops on pieces
//     verbatim. Replay against a revised STEP is where this starts to
//     matter; covered there.
//   - Fingerprint-based stable ordering across runs that arrive at the
//     same final model.
std::vector<Decision> compact(const std::vector<Decision>& records,
                              bool preserve_step_exports) {
    std::unordered_set<std::size_t> undone;
    for (const Decision& d : records) {
        if (d.kind == "undo" && d.undoes_seq != 0) {
            undone.insert(d.undoes_seq);
        }
    }

    // Pass 1: collect surviving decisions by (target_key, category).
    // Later seq overwrites earlier — std::map by seq gives causal order
    // for the second pass.
    std::map<std::size_t, Decision> kept;

    // Per-target latest-seq index, so a new op on the same target evicts
    // the older one.
    std::unordered_map<std::string, std::size_t> latest_by_key;

    for (const Decision& d : records) {
        if (d.kind != "decision") continue;
        if (d.result != "applied") continue;
        if (undone.count(d.seq)) continue;
        if (d.op == "export_step" && !preserve_step_exports) continue;
        if (d.op == "load_step" || d.op == "export_step"
            || d.op == "emit_mcnp") {
            kept[d.seq] = d;
            continue;
        }
        const std::string id = d.component_entry.empty()
            ? d.target_shape_id
            : d.component_entry;
        const std::string key = category_for(d.op) + "|" + id;
        auto it = latest_by_key.find(key);
        if (it != latest_by_key.end()) {
            kept.erase(it->second);
            it->second = d.seq;
        } else {
            latest_by_key.emplace(key, d.seq);
        }
        kept[d.seq] = d;
    }

    // Pass 2: drop solid ops whose target was later removed. We keep the
    // remove_solid itself.
    std::unordered_set<std::string> removed_keys;
    for (const auto& kv : kept) {
        if (kv.second.op == "remove_solid") {
            const std::string id = kv.second.component_entry.empty()
                ? kv.second.target_shape_id
                : kv.second.component_entry;
            removed_keys.insert(id);
        }
    }

    std::vector<Decision> out;
    out.reserve(kept.size());
    Decision load_step_op;
    Decision emit_mcnp_op;
    Decision export_step_op;
    bool have_load = false;
    bool have_emit = false;
    bool have_export = false;

    for (auto& kv : kept) {
        Decision& d = kv.second;
        if (d.op == "load_step") {
            load_step_op = d;
            have_load = true;
            continue;
        }
        if (d.op == "emit_mcnp") {
            emit_mcnp_op = d;
            have_emit = true;
            continue;
        }
        if (d.op == "export_step") {
            export_step_op = d;
            have_export = true;
            continue;
        }
        if (category_for(d.op) == "solid" && d.op != "remove_solid") {
            const std::string id = d.component_entry.empty()
                ? d.target_shape_id
                : d.component_entry;
            if (removed_keys.count(id)) continue;
        }
        out.push_back(std::move(d));
    }

    std::vector<Decision> ordered;
    ordered.reserve(out.size() + 3);
    if (have_load) ordered.push_back(load_step_op);
    for (Decision& d : out) ordered.push_back(std::move(d));
    if (have_export) ordered.push_back(export_step_op);
    if (have_emit) ordered.push_back(emit_mcnp_op);
    return ordered;
}

// ---- Lua emission -------------------------------------------------------

void emit_comment(std::ostream& out, const Decision& d) {
    if (!d.target_shape_id.empty()) {
        out << "-- " << d.target_shape_id;
        if (!d.component_entry.empty()) {
            out << "  (component_entry " << d.component_entry << ")";
        }
        out << "\n";
    }
    if (d.fingerprint.valid) {
        out << "--   fingerprint: V=" << format_double(d.fingerprint.volume)
            << " A=" << format_double(d.fingerprint.surface_area)
            << " bbox_diag=" << format_double(d.fingerprint.bbox_diagonal)
            << " faces=" << d.fingerprint.face_count
            << " centroid=("
            << format_double(d.fingerprint.centroid_x) << ","
            << format_double(d.fingerprint.centroid_y) << ","
            << format_double(d.fingerprint.centroid_z) << ")\n";
    }
}

bool emit_solid_op(std::ostream& out, const Decision& d) {
    const std::string& id = d.target_shape_id;
    const std::string quoted_id = std::string("\"") + lua_escape(id) + "\"";

    if (d.op == "remove_solid") {
        emit_comment(out, d);
        out << "s:remove_solid(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "remove_residual_with_approximation") {
        auto number = [&](const char* key, double fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_number(fallback);
        };
        auto boolean = [&](const char* key, bool fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_bool(fallback);
        };
        emit_comment(out, d);
        out << "s:remove_solid(" << quoted_id
            << ", { approx_global_limit = "
            << format_double(number("approximation_global_limit", 0.0))
            << ", approx_body_limit = "
            << format_double(number("approximation_body_limit", 0.0))
            << ", approx_action_limit = "
            << format_double(number("approximation_action_limit", 0.0))
            << ", approx_global_added_limit = "
            << format_double(number("approximation_global_added_limit", 0.0))
            << ", approx_global_removed_limit = "
            << format_double(number("approximation_global_removed_limit", 0.0))
            << ", approx_body_added_limit = "
            << format_double(number("approximation_body_added_limit", 0.0))
            << ", approx_body_removed_limit = "
            << format_double(number("approximation_body_removed_limit", 0.0))
            << ", allow_sliver_removal = true"
            << ", require_containment_preservation = "
            << (boolean("require_containment_preservation", true) ? "true" : "false")
            << ", require_topology_preservation = "
            << (boolean("require_topology_preservation", true) ? "true" : "false")
            << " })\n";
        return true;
    }
    if (d.op == "resolve_overlap_with_approximation") {
        auto owner = d.params.find("owner_shape_id");
        if (owner == d.params.end()) {
            std::cerr << "recipe: overlap resolution is missing owner_shape_id\n";
            return false;
        }
        auto number = [&](const char* key, double fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_number(fallback);
        };
        auto boolean = [&](const char* key, bool fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_bool(fallback);
        };
        emit_comment(out, d);
        out << "s:resolve_overlap(" << quoted_id << ", \""
            << lua_escape(owner->second->as_string())
            << "\", { approx_global_limit = "
            << format_double(number("approximation_global_limit", 0.0))
            << ", approx_body_limit = "
            << format_double(number("approximation_body_limit", 0.0))
            << ", approx_action_limit = "
            << format_double(number("approximation_action_limit", 0.0))
            << ", approx_global_added_limit = "
            << format_double(number("approximation_global_added_limit", 0.0))
            << ", approx_global_removed_limit = "
            << format_double(number("approximation_global_removed_limit", 0.0))
            << ", approx_body_added_limit = "
            << format_double(number("approximation_body_added_limit", 0.0))
            << ", approx_body_removed_limit = "
            << format_double(number("approximation_body_removed_limit", 0.0))
            << ", allow_overlap_resolution = true"
            << ", require_containment_preservation = "
            << (boolean("require_containment_preservation", true)
                    ? "true" : "false")
            << ", require_topology_preservation = "
            << (boolean("require_topology_preservation", true)
                    ? "true" : "false")
            << " })\n";
        return true;
    }
    if (d.op == "decompose_planar") {
        emit_comment(out, d);
        out << "s:decompose_planar(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "decompose_prismatic") {
        emit_comment(out, d);
        out << "s:decompose_prismatic(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "decompose_revolution") {
        emit_comment(out, d);
        out << "s:decompose_revolution(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "reuse_definition_conversion") {
        const auto source = d.params.find("source_solid_id");
        const std::string source_id = source == d.params.end()
            || !source->second ? std::string{} : source->second->as_string();
        if (source_id.empty()) return false;
        emit_comment(out, d);
        out << "s:reuse_definition_conversion(" << quoted_id << ", "
            << "\"" << lua_escape(source_id) << "\")\n";
        return true;
    }
    if (d.op == "decompose_shell_difference") {
        emit_comment(out, d);
        out << "s:decompose_shell_difference(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "decompose_pipe_annulus") {
        emit_comment(out, d);
        out << "s:decompose_pipe_annulus(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "shatter" || d.op == "shatter_with_approximation") {
        int max_depth = 8;
        bool allow_drift = false;
        if (auto it = d.params.find("max_depth"); it != d.params.end()) {
            max_depth = static_cast<int>(it->second->as_number(8.0));
        }
        if (auto it = d.params.find("allow_volume_drift"); it != d.params.end()) {
            allow_drift = it->second->as_bool(false);
        }
        emit_comment(out, d);
        out << "s:shatter(" << quoted_id << ", { max_depth = " << max_depth;
        if (d.op == "shatter") {
            out << ", allow_volume_drift = "
                << (allow_drift ? "true" : "false");
        } else {
            auto number = [&](const char* key, double fallback) {
                auto it = d.params.find(key);
                return it == d.params.end() ? fallback
                                            : it->second->as_number(fallback);
            };
            auto boolean = [&](const char* key, bool fallback) {
                auto it = d.params.find(key);
                return it == d.params.end() ? fallback
                                            : it->second->as_bool(fallback);
            };
            out << ", approx_global_limit = "
                << format_double(number("approximation_global_limit", 0.0))
                << ", approx_body_limit = "
                << format_double(number("approximation_body_limit", 0.0))
                << ", approx_action_limit = "
                << format_double(number("approximation_action_limit", 0.0))
                << ", approx_global_added_limit = "
                << format_double(number("approximation_global_added_limit", 0.0))
                << ", approx_global_removed_limit = "
                << format_double(number("approximation_global_removed_limit", 0.0))
                << ", approx_body_added_limit = "
                << format_double(number("approximation_body_added_limit", 0.0))
                << ", approx_body_removed_limit = "
                << format_double(number("approximation_body_removed_limit", 0.0))
                << ", allow_numerical_shatter_loss = true"
                << ", require_containment_preservation = "
                << (boolean("require_containment_preservation", true)
                        ? "true" : "false")
                << ", require_topology_preservation = "
                << (boolean("require_topology_preservation", true)
                        ? "true" : "false");
        }
        out << " })\n";
        return true;
    }
    if (d.op == "decompose_by_face") {
        auto it = d.params.find("face_id");
        if (it == d.params.end() || it->second->kind != JsonKind::string) {
            std::cerr << "recipe: missing 'face_id' param on decompose_by_face\n";
            return false;
        }
        emit_comment(out, d);
        out << "s:decompose_by_face(" << quoted_id << ", \""
            << lua_escape(it->second->as_string()) << "\")\n";
        return true;
    }
    if (d.op == "decompose_by_plane") {
        const char* keys[] = {
            "origin_x", "origin_y", "origin_z",
            "normal_x", "normal_y", "normal_z",
        };
        double values[6] = {};
        for (int i = 0; i < 6; ++i) {
            auto it = d.params.find(keys[i]);
            if (it == d.params.end() || it->second->kind != JsonKind::number) {
                std::cerr << "recipe: missing '" << keys[i]
                          << "' param on decompose_by_plane\n";
                return false;
            }
            values[i] = it->second->as_number();
        }
        emit_comment(out, d);
        out << "s:decompose_by_plane(" << quoted_id
            << ", " << values[0] << ", " << values[1] << ", " << values[2]
            << ", " << values[3] << ", " << values[4] << ", " << values[5]
            << ")\n";
        return true;
    }
    if (d.op == "decompose_by_planes") {
        auto count_it = d.params.find("plane_count");
        if (count_it == d.params.end()
            || count_it->second->kind != JsonKind::number) {
            std::cerr << "recipe: missing 'plane_count' param on decompose_by_planes\n";
            return false;
        }
        const int plane_count = static_cast<int>(count_it->second->as_number());
        emit_comment(out, d);
        out << "s:decompose_by_planes(" << quoted_id << ", {\n";
        for (int i = 0; i < plane_count; ++i) {
            const std::string idx = std::to_string(i);
            const std::string keys[] = {
                "plane_" + idx + "_ox", "plane_" + idx + "_oy",
                "plane_" + idx + "_oz", "plane_" + idx + "_nx",
                "plane_" + idx + "_ny", "plane_" + idx + "_nz",
            };
            double values[6] = {};
            for (int k = 0; k < 6; ++k) {
                auto it = d.params.find(keys[k]);
                if (it == d.params.end()
                    || it->second->kind != JsonKind::number) {
                    std::cerr << "recipe: missing '" << keys[k]
                              << "' param on decompose_by_planes\n";
                    return false;
                }
                values[k] = it->second->as_number();
            }
            out << "  { origin = {" << values[0] << ", " << values[1]
                << ", " << values[2] << "}, normal = {" << values[3]
                << ", " << values[4] << ", " << values[5] << "} }";
            if (i + 1 < plane_count) out << ",";
            out << "\n";
        }
        out << "})\n";
        return true;
    }
    if (d.op == "apply_decomposition_candidate") {
        auto it = d.params.find("face_id");
        if (it == d.params.end()) {
            const char* keys[] = {
                "origin_x", "origin_y", "origin_z",
                "normal_x", "normal_y", "normal_z",
            };
            double values[6] = {};
            bool has_plane = true;
            for (int i = 0; i < 6; ++i) {
                auto pit = d.params.find(keys[i]);
                if (pit == d.params.end()
                    || pit->second->kind != JsonKind::number) {
                    has_plane = false;
                    break;
                }
                values[i] = pit->second->as_number();
            }
            if (has_plane) {
                emit_comment(out, d);
                out << "s:decompose_by_plane(" << quoted_id
                    << ", " << values[0] << ", " << values[1] << ", " << values[2]
                    << ", " << values[3] << ", " << values[4] << ", " << values[5]
                    << ")\n";
                return true;
            }
        }
        if (it == d.params.end() || it->second->kind != JsonKind::string) {
            std::cerr << "recipe: missing 'face_id' param on apply_decomposition_candidate\n";
            return false;
        }
        emit_comment(out, d);
        out << "s:decompose_by_face(" << quoted_id << ", \""
            << lua_escape(it->second->as_string()) << "\")\n";
        return true;
    }
    if (d.op.rfind("replace_solid_with_", 0) == 0) {
        emit_comment(out, d);
        out << "s:" << d.op << "(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "apply_replacement_operation"
        || d.op == "apply_checked_replacement_operation"
        || d.op == "apply_budgeted_replacement_operation") {
        auto it = d.params.find("operation");
        if (it == d.params.end() || it->second->kind != JsonKind::number) {
            std::cerr << "recipe: missing 'operation' param on " << d.op << "\n";
            return false;
        }
        const int op_enum = static_cast<int>(it->second->as_number());
        const char* method = replacement_enum_to_method(op_enum);
        if (!method) {
            std::cerr << "recipe: unknown replacement enum " << op_enum << "\n";
            return false;
        }
        emit_comment(out, d);
        if (d.op == "apply_budgeted_replacement_operation") {
            const OperationDescriptor* descriptor =
                find_replacement(static_cast<ReplacementOperation>(op_enum));
            if (!descriptor) {
                std::cerr << "recipe: missing replacement descriptor for enum "
                          << op_enum << "\n";
                return false;
            }
            out << "s:replace_solid(" << quoted_id << ", \""
                << descriptor->lua_value << "\"";
        } else {
            out << "s:" << method << "(" << quoted_id;
        }
        if (d.op == "apply_budgeted_replacement_operation") {
            auto number = [&](const char* key, double fallback) {
                auto pit = d.params.find(key);
                return pit == d.params.end() ? fallback
                                             : pit->second->as_number(fallback);
            };
            auto boolean = [&](const char* key, bool fallback) {
                auto pit = d.params.find(key);
                return pit == d.params.end() ? fallback
                                             : pit->second->as_bool(fallback);
            };
            out << ", { approx_global_limit = "
                << format_double(number("approximation_global_limit", 0.0))
                << ", approx_body_limit = "
                << format_double(number("approximation_body_limit", 0.0))
                << ", approx_action_limit = "
                << format_double(number("approximation_action_limit", 0.0))
                << ", approx_global_added_limit = "
                << format_double(number("approximation_global_added_limit", 0.0))
                << ", approx_global_removed_limit = "
                << format_double(number("approximation_global_removed_limit", 0.0))
                << ", approx_body_added_limit = "
                << format_double(number("approximation_body_added_limit", 0.0))
                << ", approx_body_removed_limit = "
                << format_double(number("approximation_body_removed_limit", 0.0))
                << ", approx_surface_deviation_limit = "
                << format_double(number("approximation_surface_deviation_limit", 0.0))
                << ", allow_surface_replacement = "
                << (boolean("allow_surface_replacement", false) ? "true" : "false")
                << ", allow_bbox_replacement = "
                << (boolean("allow_bbox_replacement", false) ? "true" : "false")
                << ", allow_sliver_removal = "
                << (boolean("allow_sliver_removal", false) ? "true" : "false")
                << ", require_containment_preservation = "
                << (boolean("require_containment_preservation", true) ? "true" : "false")
                << ", require_topology_preservation = "
                << (boolean("require_topology_preservation", true) ? "true" : "false")
                << " }";
        } else if (d.op == "apply_checked_replacement_operation") {
            bool wrote = false;
            auto has_abs = d.params.find("has_max_abs_delta");
            auto max_abs = d.params.find("max_abs_delta");
            auto has_rel = d.params.find("has_max_rel_delta");
            auto max_rel = d.params.find("max_rel_delta");
            out << ", {";
            if (has_abs != d.params.end() && has_abs->second->as_bool()
                && max_abs != d.params.end()) {
                out << "max_abs_delta = "
                    << format_double(max_abs->second->as_number());
                wrote = true;
            }
            if (has_rel != d.params.end() && has_rel->second->as_bool()
                && max_rel != d.params.end()) {
                if (wrote) out << ", ";
                out << "max_rel_delta = "
                    << format_double(max_rel->second->as_number());
            }
            out << "}";
        }
        out << ")\n";
        return true;
    }
    std::cerr << "recipe: unknown solid op " << d.op << "\n";
    return false;
}

bool emit_face_op(std::ostream& out, const Decision& d) {
    if (d.op == "apply_budgeted_surface_replacement_candidate") {
        auto candidate = d.params.find("candidate_id");
        if (candidate == d.params.end()) return false;
        auto number = [&](const char* key, double fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_number(fallback);
        };
        auto boolean = [&](const char* key, bool fallback) {
            auto it = d.params.find(key);
            return it == d.params.end() ? fallback
                                        : it->second->as_bool(fallback);
        };
        emit_comment(out, d);
        out << "s:apply_surface_replacement_candidate(\""
            << lua_escape(d.target_shape_id) << "\", \""
            << lua_escape(candidate->second->as_string())
            << "\", { approx_global_limit = "
            << format_double(number("approximation_global_limit", 0.0))
            << ", approx_body_limit = "
            << format_double(number("approximation_body_limit", 0.0))
            << ", approx_action_limit = "
            << format_double(number("approximation_action_limit", 0.0))
            << ", approx_global_added_limit = "
            << format_double(number("approximation_global_added_limit", 0.0))
            << ", approx_global_removed_limit = "
            << format_double(number("approximation_global_removed_limit", 0.0))
            << ", approx_body_added_limit = "
            << format_double(number("approximation_body_added_limit", 0.0))
            << ", approx_body_removed_limit = "
            << format_double(number("approximation_body_removed_limit", 0.0))
            << ", approx_surface_deviation_limit = "
            << format_double(number("approximation_surface_deviation_limit", 0.0))
            << ", allow_surface_replacement = true"
            << ", require_containment_preservation = "
            << (boolean("require_containment_preservation", true) ? "true" : "false")
            << ", require_topology_preservation = "
            << (boolean("require_topology_preservation", true) ? "true" : "false")
            << " })\n";
        return true;
    }
    if (d.op == "apply_surface_replacement_candidate") {
        auto kind_it = d.params.find("kind");
        auto budget_it = d.params.find("max_deviation_budget");
        if (kind_it == d.params.end()
            || kind_it->second->kind != JsonKind::string
            || budget_it == d.params.end()
            || budget_it->second->kind != JsonKind::number) {
            std::cerr << "recipe: missing replay parameters on " << d.op << "\n";
            return false;
        }
        emit_comment(out, d);
        out << "s:replace_face_with_fit(\"" << lua_escape(d.target_shape_id)
            << "\", \"" << lua_escape(kind_it->second->as_string())
            << "\", { max_deviation = "
            << format_double(budget_it->second->as_number()) << " })\n";
        return true;
    }
    if (d.op != "replace_face_with_fit") {
        std::cerr << "recipe: unknown face op " << d.op << "\n";
        return false;
    }
    auto kind_it = d.params.find("kind");
    if (kind_it == d.params.end()) {
        std::cerr << "recipe: missing 'kind' param on replace_face_with_fit\n";
        return false;
    }
    const int kind = static_cast<int>(kind_it->second->as_number());
    const char* kind_name = spline_fit_kind_to_lua(kind);
    if (!kind_name) {
        std::cerr << "recipe: unknown spline fit kind " << kind << "\n";
        return false;
    }
    emit_comment(out, d);
    out << "s:replace_face_with_fit(\"" << lua_escape(d.target_shape_id)
        << "\", \"" << kind_name << "\"";
    auto has_dev = d.params.find("has_max_deviation");
    auto dev = d.params.find("max_deviation");
    if (has_dev != d.params.end() && has_dev->second->as_bool()
        && dev != d.params.end()) {
        out << ", { max_deviation = " << format_double(dev->second->as_number()) << " }";
    }
    out << ")\n";
    return true;
}

// Build a Lua options table from the conversion options journaled on a
// load_step decision. Returns "" when none are present, so legacy ordinary
// recipes keep the bare s:load_step(path) form. Mirrors
// the params written in session.cpp and consumed by replay.cpp.
std::string load_step_options_table(const Decision& d) {
    std::vector<std::string> fields;
    auto add_bool = [&](const char* key) {
        auto it = d.params.find(key);
        if (it != d.params.end() && it->second->kind == JsonKind::boolean) {
            fields.push_back(std::string(key) + " = "
                             + (it->second->as_bool() ? "true" : "false"));
        }
    };
    auto add_num = [&](const char* key) {
        auto it = d.params.find(key);
        if (it != d.params.end() && it->second->kind == JsonKind::number) {
            fields.push_back(std::string(key) + " = "
                             + format_double(it->second->as_number()));
        }
    };
    auto add_string = [&](const char* key) {
        auto it = d.params.find(key);
        if (it != d.params.end() && it->second->kind == JsonKind::string) {
            fields.push_back(std::string(key) + " = \""
                             + lua_escape(it->second->as_string()) + "\"");
        }
    };
    add_bool("allow_overlaps");
    add_bool("check_overlaps");
    add_bool("heal_shape");
    add_string("unit_override");
    add_num("unit_scale_to_mm_override");
    add_string("normalize_to_unit");
    add_num("overlap_volume_threshold");
    add_num("overlap_volume_fraction");
    add_string("mcnp_domain_id");
    add_bool("require_domain_coverage");
    if (fields.empty()) return "";
    std::string table = "{ ";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) table += ", ";
        table += fields[i];
    }
    table += " }";
    return table;
}

bool emit_document_op(std::ostream& out, const Decision& d, const std::string& doc_var) {
    if (d.op == "load_step") {
        auto it = d.params.find("step_path");
        const std::string step = it != d.params.end() ? it->second->as_string() : "";
        out << "local " << doc_var << " = s:load_step(\""
            << lua_escape(step) << "\"";
        const std::string opts = load_step_options_table(d);
        if (!opts.empty()) out << ", " << opts;
        out << ")\n";
        return true;
    }
    if (d.op == "emit_mcnp") {
        auto it = d.params.find("output_path");
        const std::string outpath = it != d.params.end() ? it->second->as_string() : "";
        out << "s:emit(" << doc_var << ", \"" << lua_escape(outpath) << "\")\n";
        return true;
    }
    if (d.op == "export_step") {
        auto it = d.params.find("output_path");
        const std::string outpath = it != d.params.end() ? it->second->as_string() : "";
        out << "s:export_step(" << doc_var << ", \""
            << lua_escape(outpath) << "\")\n";
        return true;
    }
    if (d.op == "auto_fit_splines") {
        out << "s:auto_fit_splines(" << doc_var;
        auto has_dev = d.params.find("has_max_deviation");
        auto dev = d.params.find("max_deviation");
        if (has_dev != d.params.end() && has_dev->second->as_bool()
            && dev != d.params.end()) {
            out << ", { max_deviation = " << format_double(dev->second->as_number()) << " }";
        }
        out << ")\n";
        return true;
    }
    if (d.op == "define_material") {
        const auto id = d.params.find("material_id");
        const auto phase = d.params.find("phase");
        const auto density = d.params.find("density");
        if (id == d.params.end() || phase == d.params.end()
            || density == d.params.end()) return false;
        out << "s:define_material(" << doc_var << ", { id = \""
            << lua_escape(id->second->as_string()) << "\", phase = \""
            << lua_escape(phase->second->as_string()) << "\", density = "
            << format_double(density->second->as_number()) << " })\n";
        return true;
    }
    if (d.op == "apply_contained_medium"
        || d.op == "stage_contained_medium_request") {
        auto value = [&](const char* key) -> std::string {
            const auto it = d.params.find(key);
            return it == d.params.end() || !it->second
                ? std::string{} : it->second->as_string();
        };
        auto number = [&](const char* key, double fallback) {
            const auto it = d.params.find(key);
            return it == d.params.end() || !it->second
                ? fallback : it->second->as_number(fallback);
        };
        out << "s:" << (d.op == "apply_contained_medium"
                ? "apply_contained_medium" : "stage_contained_medium_request")
            << "(" << doc_var << ", { id = \""
            << lua_escape(value("request_id")) << "\", material_id = \""
            << lua_escape(value("material_id"))
            << "\", complement_material_id = \""
            << lua_escape(value("complement_material_id"))
            << "\", fill_mode = \"" << lua_escape(value("fill_mode"))
            << "\", host_component_ids = { ";
        const int host_count = static_cast<int>(number("host_count", 0));
        if (host_count > 0) {
            for (int index = 1; index <= host_count; ++index) {
                if (index > 1) out << ", ";
                out << '"' << lua_escape(value(
                    ("host_" + std::to_string(index) + "_component_id").c_str()))
                    << '"';
            }
        } else if (!value("host_component_id").empty()) {
            out << '"' << lua_escape(value("host_component_id")) << '"';
        }
        out << " }, ports = { ";
        bool wrote_port = false;
        const int port_count = static_cast<int>(number("port_count", 2));
        for (int index = 1; index <= port_count; ++index) {
            const std::string prefix = "port_" + std::to_string(index);
            const std::string port_id = value((prefix + "_id").c_str());
            if (port_id.empty()) continue;
            if (wrote_port) out << ", ";
            wrote_port = true;
            out << "{ port_id = \"" << lua_escape(port_id)
                << "\", disposition = \""
                << lua_escape(value((prefix + "_disposition").c_str()))
                << "\", peer_port_id = \""
                << lua_escape(value((prefix + "_peer_id").c_str()))
                << "\", valve_state = \""
                << lua_escape(value((prefix + "_valve_state").c_str()))
                << "\" }";
        }
        out << " }, internal_components_to_subtract = { ";
        const int internal_count = static_cast<int>(
            number("internal_component_count", 0));
        for (int index = 1; index <= internal_count; ++index) {
            if (index > 1) out << ", ";
            out << '"' << lua_escape(value(
                ("internal_" + std::to_string(index) + "_component_id").c_str()))
                << '"';
        }
        out << " }, seed_points_or_faces = { ";
        const int seed_count = static_cast<int>(number("seed_count", 0));
        for (int index = 1; index <= seed_count; ++index) {
            if (index > 1) out << ", ";
            const std::string prefix = "seed_" + std::to_string(index);
            out << "{ kind = \""
                << lua_escape(value((prefix + "_kind").c_str()))
                << "\", entity_id = \""
                << lua_escape(value((prefix + "_entity_id").c_str()))
                << "\", point = { "
                << format_double(number((prefix + "_x").c_str(), 0.0)) << ", "
                << format_double(number((prefix + "_y").c_str(), 0.0)) << ", "
                << format_double(number((prefix + "_z").c_str(), 0.0))
                << " } }";
        }
        out << " }, max_worker_count = "
            << static_cast<int>(number("max_worker_count", 1));
        const auto fill_level = d.params.find("fill_level");
        if (fill_level != d.params.end())
            out << ", fill_level = "
                << format_double(fill_level->second->as_number());
        const auto fraction = d.params.find("target_volume_fraction");
        if (fraction != d.params.end())
            out << ", target_volume_fraction = "
                << format_double(fraction->second->as_number());
        const auto pockets = d.params.find("allow_disconnected_pockets");
        out << ", allow_disconnected_pockets = "
            << (pockets != d.params.end() && pockets->second->as_bool()
                    ? "true" : "false");
        if (d.op == "apply_contained_medium") {
            const auto approved = d.params.find("user_approved");
            out << ", user_approved = "
                << (approved != d.params.end() && approved->second->as_bool()
                        ? "true" : "false");
        }
        out << " })\n";
        return true;
    }
    if (d.op == "undo_last_mutation") {
        out << "s:undo_last_mutation(" << doc_var << ")\n";
        return true;
    }
    std::cerr << "recipe: unknown document op " << d.op << "\n";
    return false;
}

// JSON emitter for the machine-replayable sibling. Format matches the
// decision-log record shape so the same parser and matching code (see
// src/replay.cpp) can consume both.
std::string json_escape(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 2);
    for (char c : v) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    return out;
}

void emit_json_string(std::ostream& out, const std::string& v) {
    out << "\"" << json_escape(v) << "\"";
}

void emit_param_value(std::ostream& out, const JsonValue& v) {
    switch (v.kind) {
    case JsonKind::string: emit_json_string(out, v.str); return;
    case JsonKind::number: out << format_double(v.num); return;
    case JsonKind::boolean: out << (v.b ? "true" : "false"); return;
    case JsonKind::null:   out << "null"; return;
    default: out << "null"; return;  // arrays/objects not emitted in params
    }
}

void emit_target_json(std::ostream& out, const Decision& d) {
    out << "\"target\":{"
        << "\"kind\":";       emit_json_string(out, d.target_kind);
    out << ",\"shape_id\":";  emit_json_string(out, d.target_shape_id);
    out << ",\"component_entry\":"; emit_json_string(out, d.component_entry);
    if (d.fingerprint.valid) {
        out << ",\"fingerprint\":{"
            << "\"volume\":"        << format_double(d.fingerprint.volume)
            << ",\"surface_area\":" << format_double(d.fingerprint.surface_area)
            << ",\"bbox_diagonal\":"<< format_double(d.fingerprint.bbox_diagonal)
            << ",\"face_count\":"   << d.fingerprint.face_count
            << ",\"centroid\":["    << format_double(d.fingerprint.centroid_x)
            << ","                  << format_double(d.fingerprint.centroid_y)
            << ","                  << format_double(d.fingerprint.centroid_z)
            << "]}";
    }
    out << "}";
}

void emit_op_json(std::ostream& out, const Decision& d) {
    out << "{\"op\":";  emit_json_string(out, d.op);
    const OperationDescriptor* descriptor = find_operation(d.op);
    out << ",\"operation_version\":"
        << (descriptor ? descriptor->version : 0);
    out << ",";         emit_target_json(out, d);
    out << ",\"params\":{";
    bool first = true;
    for (const auto& kv : d.params) {
        if (!first) out << ",";
        first = false;
        emit_json_string(out, kv.first);
        out << ":";
        emit_param_value(out, *kv.second);
    }
    out << "}}";
}

bool emit_recipe_json(std::ostream& out,
                      const Manifest& manifest,
                      const std::vector<Decision>& ops,
                      const std::vector<Decision>& reviews,
                      const RecipePostconditions& postconditions) {
    out << "{\n";
    out << "  \"schema\": \"csgify.recipe\",\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"manifest\": {";
    out << "\"step_path\":";       emit_json_string(out, manifest.step_path);
    out << ",\"step_digest\":";    emit_json_string(out, manifest.step_digest);
    out << ",\"csgify_version\":";emit_json_string(out, manifest.csgify_version);
    out << ",\"occt_version\":";   emit_json_string(out, manifest.occt_version);
    out << "},\n";
    if (postconditions.present) {
        out << "  \"postconditions\": {";
        bool wrote = false;
        if (postconditions.has_decomposition_state) {
            out << "\"residual_count\":" << postconditions.residual_count
                << ",\"active_piece_count\":"
                << postconditions.active_piece_count
                << ",\"max_overlap_errors\":"
                << postconditions.max_overlap_errors;
            wrote = true;
        }
        if (postconditions.has_approximation_ledger) {
            if (wrote) out << ",";
            out << "\"approximation_ledger\":{"
                << "\"cumulative_changed_volume\":"
                << format_double(postconditions.cumulative_changed_volume)
                << ",\"cumulative_added_volume\":"
                << format_double(postconditions.cumulative_added_volume)
                << ",\"cumulative_removed_volume\":"
                << format_double(postconditions.cumulative_removed_volume)
                << "}";
        }
        out << "},\n";
    }
    out << "  \"reviews\": [\n";
    for (std::size_t i = 0; i < reviews.size(); ++i) {
        const Decision& review = reviews[i];
        out << "    {\"target_id\":";
        emit_json_string(out, review.review_target_id);
        out << ",\"proposal_id\":";
        emit_json_string(out, review.proposal_id);
        out << ",\"decision\":";
        emit_json_string(out, review.review_decision);
        out << ",\"review_kind\":";
        emit_json_string(out, review.review_kind);
        out << ",\"action\":";
        emit_json_string(out, review.review_action);
        out << ",\"candidate_id\":";
        emit_json_string(out, review.review_candidate_id);
        out << ",\"base_revision\":" << review.review_base_revision;
        out << ",\"audit\":{\"actor\":";
        emit_json_string(out, review.audit.actor);
        out << ",\"proposal_actor\":";
        emit_json_string(out, review.audit.proposal_actor);
        out << ",\"proposal_id\":";
        emit_json_string(out, review.audit.proposal_id);
        out << ",\"rationale\":";
        emit_json_string(out, review.audit.rationale);
        out << ",\"policy_rule\":";
        emit_json_string(out, review.audit.policy_rule);
        out << ",\"approval_mode\":";
        emit_json_string(out, review.audit.approval_mode);
        out << "}}";
        if (i + 1 < reviews.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"ops\": [\n";
    for (std::size_t i = 0; i < ops.size(); ++i) {
        out << "    ";
        emit_op_json(out, ops[i]);
        if (i + 1 < ops.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return true;
}

bool emit_recipe(std::ostream& out,
                 const Manifest& manifest,
                 const std::vector<Decision>& ops,
                 const CompactRecipeOptions& options) {
    if (options.include_provenance_header) {
        out << "-- csgify recipe (compacted)\n";
        out << "-- step_path:        " << manifest.step_path << "\n";
        if (!manifest.step_digest.empty()) {
            out << "-- step_digest:      " << manifest.step_digest << "\n";
        }
        out << "-- csgify_version:  " << manifest.csgify_version << "\n";
        out << "-- occt_version:     " << manifest.occt_version << "\n";
        out << "-- generated_from:   decision log\n";
        out << "\n";
    }
    out << "local s = csgify.session()\n";
    const std::string doc_var = "doc1";
    bool ok = true;
    for (std::size_t i = 0; i < ops.size();) {
        const Decision& d = ops[i];
        if (d.target_kind == "solid" && d.op == "remove_solid") {
            std::size_t end = i + 1;
            while (end < ops.size()
                   && ops[end].target_kind == "solid"
                   && ops[end].op == "remove_solid") {
                ++end;
            }
            if (end - i > 1) {
                for (std::size_t j = i; j < end; ++j) {
                    emit_comment(out, ops[j]);
                }
                out << "s:remove_solids({\n";
                for (std::size_t j = i; j < end; ++j) {
                    out << "    \"" << lua_escape(ops[j].target_shape_id)
                        << "\"" << (j + 1 < end ? "," : "") << "\n";
                }
                out << "})\n";
                i = end;
                continue;
            }
        }
        if (d.target_kind == "document") {
            ok = emit_document_op(out, d, doc_var) && ok;
        } else if (d.target_kind == "solid") {
            ok = emit_solid_op(out, d) && ok;
        } else if (d.target_kind == "face") {
            ok = emit_face_op(out, d) && ok;
        } else {
            std::cerr << "recipe: unknown target kind " << d.target_kind << "\n";
            ok = false;
        }
        ++i;
    }
    return ok;
}

std::vector<std::shared_ptr<JsonValue>> load_decision_log_entries(
    const std::string& log_path) {
    try {
        std::shared_ptr<JsonValue> root = load_json_file(log_path);
        if (root && root->kind == JsonKind::array) {
            return root->arr;
        }
    } catch (const std::exception&) {
        // Fall through to newline-delimited JSON. This keeps compacting
        // old array logs and new crash-tolerant logs with one code path.
    }

    std::ifstream in(log_path);
    if (!in) {
        throw std::runtime_error("could not open " + log_path);
    }
    std::vector<std::shared_ptr<JsonValue>> entries;
    std::string line;
    while (std::getline(in, line)) {
        std::size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        if (line[first] != '{') continue;
        std::size_t last = line.find_last_not_of(" \t\r\n");
        if (last != std::string::npos && line[last] == ',') {
            line.erase(last);
        }
        std::shared_ptr<JsonValue> value = JsonParser(line).parse();
        if (value && value->kind == JsonKind::object) {
            entries.push_back(value);
        }
    }
    return entries;
}

} // namespace

int compact_log_to_recipe(const std::string& log_path,
                          const std::string& recipe_path,
                          const CompactRecipeOptions& options) {
    std::vector<std::shared_ptr<JsonValue>> entries;
    try {
        entries = load_decision_log_entries(log_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    if (entries.empty()) {
        std::cerr << "error: decision log is empty or invalid\n";
        return 1;
    }

    Manifest manifest;
    std::vector<Decision> records;
    for (const auto& entry : entries) {
        if (!entry || entry->kind != JsonKind::object) continue;
        const std::string kind = entry->get("kind") ? entry->get("kind")->as_string() : "";
        if (kind == "manifest") {
            if (const JsonValue* v = entry->get("schema")) {
                if (v->as_string() != "csgify.decision-log") {
                    std::cerr << "error: unsupported decision-log schema\n";
                    return 1;
                }
            }
            if (const JsonValue* v = entry->get("schema_version")) {
                if (static_cast<int>(v->as_number()) != 1) {
                    std::cerr << "error: unsupported decision-log schema_version\n";
                    return 1;
                }
            }
            if (const JsonValue* v = entry->get("step_path"))       manifest.step_path = v->as_string();
            if (const JsonValue* v = entry->get("step_digest"))     manifest.step_digest = v->as_string();
            if (const JsonValue* v = entry->get("csgify_version")) manifest.csgify_version = v->as_string();
            if (const JsonValue* v = entry->get("occt_version"))    manifest.occt_version = v->as_string();
            if (const JsonValue* v = entry->get("created_at"))      manifest.created_at = v->as_string();
            continue;
        }
        records.push_back(read_record(*entry));
    }

    const std::vector<Decision> ops = compact(
        records, options.preserve_step_exports);
    const std::vector<Decision> reviews = compact_reviews(records);
    const RecipePostconditions postconditions = derive_postconditions(ops);
    for (const Decision& op : ops) {
        const OperationDescriptor* descriptor = find_operation(op.op);
        if (!descriptor) {
            std::cerr << "error: operation is not present in this csgify "
                      << "operation registry: " << op.op << "\n";
            return 1;
        }
        if (op.operation_version != descriptor->version) {
            std::cerr << "error: unsupported operation version for "
                      << op.op << "\n";
            return 1;
        }
    }

    std::ofstream out(recipe_path);
    if (!out) {
        std::cerr << "error: could not open " << recipe_path << " for writing\n";
        return 1;
    }
    bool ok = emit_recipe(out, manifest, ops, options);

    // Also emit the machine-replayable JSON sibling. If recipe_path ends
    // in ".lua", swap to ".json"; otherwise append ".json".
    std::string json_path = recipe_path;
    const std::string lua_suffix = ".lua";
    if (json_path.size() > lua_suffix.size() &&
        json_path.compare(json_path.size() - lua_suffix.size(),
                          lua_suffix.size(), lua_suffix) == 0) {
        json_path.replace(json_path.size() - lua_suffix.size(),
                          lua_suffix.size(), ".json");
    } else {
        json_path += ".json";
    }
    std::ofstream jout(json_path);
    if (!jout) {
        std::cerr << "error: could not open " << json_path << " for writing\n";
        return 1;
    }
    ok = emit_recipe_json(jout, manifest, ops, reviews, postconditions) && ok;

    return ok ? 0 : 1;
}

} // namespace csgify
