#include "replay.hpp"

#include "csgify/engine.hpp"
#include "csgify/session.hpp"
#include "engine_model.hpp"
#include "fnv.hpp"
#include "json_parser.hpp"
#include "operation_registry.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace csgify {
namespace {

struct Fingerprint {
    bool valid = false;
    double volume = 0.0;
    double surface_area = 0.0;
    double bbox_diagonal = 0.0;
    int face_count = 0;
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
};

struct RecipeOp {
    std::string op;
    unsigned operation_version = 1;
    std::string target_kind;     // "solid" | "face" | "document"
    std::string target_shape_id; // scoped or local
    std::string component_entry;
    Fingerprint fingerprint;
    std::shared_ptr<JsonValue> params; // raw object
};

struct RecipeFile {
    std::string step_path;
    std::string step_digest;
    std::string csgify_version;
    std::string occt_version;
    std::vector<RecipeOp> ops;
    std::vector<DecisionReviewRecord> reviews;
    bool has_postconditions = false;
    bool has_decomposition_postconditions = false;
    std::size_t expected_residual_count = 0;
    std::size_t expected_active_piece_count = 0;
    std::size_t max_overlap_errors = 0;
    bool has_approximation_postconditions = false;
    double expected_cumulative_changed_volume = 0.0;
    double expected_cumulative_added_volume = 0.0;
    double expected_cumulative_removed_volume = 0.0;
};

Fingerprint read_fingerprint(const JsonValue& fp_obj) {
    Fingerprint fp;
    fp.valid = true;
    if (const JsonValue* v = fp_obj.get("volume"))        fp.volume        = v->as_number();
    if (const JsonValue* v = fp_obj.get("surface_area"))  fp.surface_area  = v->as_number();
    if (const JsonValue* v = fp_obj.get("bbox_diagonal")) fp.bbox_diagonal = v->as_number();
    if (const JsonValue* v = fp_obj.get("face_count"))    fp.face_count    = static_cast<int>(v->as_number());
    if (const JsonValue* c = fp_obj.get("centroid")) {
        if (c->kind == JsonKind::array && c->arr.size() == 3) {
            fp.cx = c->arr[0]->as_number();
            fp.cy = c->arr[1]->as_number();
            fp.cz = c->arr[2]->as_number();
        }
    }
    return fp;
}

RecipeFile read_recipe(const std::string& path) {
    auto root = load_json_file(path);
    RecipeFile out;
    if (!root || root->kind != JsonKind::object) {
        throw std::runtime_error("recipe is not a JSON object");
    }
    if (const JsonValue* schema = root->get("schema")) {
        if (schema->as_string() != "csgify.recipe") {
            throw std::runtime_error("unsupported recipe schema: "
                                     + schema->as_string());
        }
    }
    if (const JsonValue* version = root->get("schema_version")) {
        if (static_cast<int>(version->as_number()) != 1) {
            throw std::runtime_error("unsupported recipe schema_version");
        }
    }
    if (const JsonValue* m = root->get("manifest")) {
        if (const JsonValue* v = m->get("step_path"))   out.step_path = v->as_string();
        if (const JsonValue* v = m->get("step_digest")) out.step_digest = v->as_string();
        if (const JsonValue* v = m->get("csgify_version"))
            out.csgify_version = v->as_string();
        if (const JsonValue* v = m->get("occt_version"))
            out.occt_version = v->as_string();
    }
    if (const JsonValue* p = root->get("postconditions")) {
        out.has_postconditions = true;
        if (const JsonValue* v = p->get("residual_count")) {
            out.has_decomposition_postconditions = true;
            out.expected_residual_count =
                static_cast<std::size_t>(v->as_number());
        }
        if (const JsonValue* v = p->get("active_piece_count"))
            out.expected_active_piece_count =
                static_cast<std::size_t>(v->as_number());
        if (const JsonValue* v = p->get("max_overlap_errors"))
            out.max_overlap_errors =
                static_cast<std::size_t>(v->as_number());
        if (const JsonValue* a = p->get("approximation_ledger")) {
            out.has_approximation_postconditions = true;
            if (const JsonValue* v = a->get("cumulative_changed_volume"))
                out.expected_cumulative_changed_volume = v->as_number();
            if (const JsonValue* v = a->get("cumulative_added_volume"))
                out.expected_cumulative_added_volume = v->as_number();
            if (const JsonValue* v = a->get("cumulative_removed_volume"))
                out.expected_cumulative_removed_volume = v->as_number();
        }
    }
    if (const JsonValue* reviews = root->get("reviews")) {
        if (reviews->kind != JsonKind::array)
            throw std::runtime_error("recipe 'reviews' is not an array");
        for (const auto& entry : reviews->arr) {
            if (!entry || entry->kind != JsonKind::object) continue;
            DecisionReviewRecord review;
            if (const JsonValue* v = entry->get("target_id"))
                review.target_id = v->as_string();
            if (const JsonValue* v = entry->get("proposal_id"))
                review.proposal_id = v->as_string();
            if (const JsonValue* v = entry->get("decision"))
                review.decision = v->as_string();
            if (const JsonValue* v = entry->get("review_kind"))
                review.review_kind = v->as_string();
            if (const JsonValue* v = entry->get("action"))
                review.action = v->as_string();
            if (const JsonValue* v = entry->get("candidate_id"))
                review.candidate_id = v->as_string();
            if (const JsonValue* v = entry->get("base_revision"))
                review.base_revision = static_cast<std::size_t>(v->as_number());
            if (const JsonValue* audit = entry->get("audit")) {
                if (const JsonValue* v = audit->get("actor"))
                    review.audit.actor = v->as_string();
                if (const JsonValue* v = audit->get("proposal_actor"))
                    review.audit.proposal_actor = v->as_string();
                if (const JsonValue* v = audit->get("proposal_id"))
                    review.audit.proposal_id = v->as_string();
                if (const JsonValue* v = audit->get("rationale"))
                    review.audit.rationale = v->as_string();
                if (const JsonValue* v = audit->get("policy_rule"))
                    review.audit.policy_rule = v->as_string();
                if (const JsonValue* v = audit->get("approval_mode"))
                    review.audit.approval_mode = v->as_string();
            }
            if (review.target_id.empty() || review.proposal_id.empty()
                || review.decision.empty()) {
                throw std::runtime_error(
                    "recipe review is missing target, proposal, or decision");
            }
            out.reviews.push_back(std::move(review));
        }
    }
    const JsonValue* ops = root->get("ops");
    if (!ops || ops->kind != JsonKind::array) {
        throw std::runtime_error("recipe missing 'ops' array");
    }
    for (const auto& entry : ops->arr) {
        if (!entry || entry->kind != JsonKind::object) continue;
        RecipeOp o;
        if (const JsonValue* v = entry->get("op")) o.op = v->as_string();
        if (const JsonValue* v = entry->get("operation_version")) {
            o.operation_version = static_cast<unsigned>(v->as_number());
        }
        if (const JsonValue* t = entry->get("target")) {
            if (const JsonValue* v = t->get("kind"))            o.target_kind = v->as_string();
            if (const JsonValue* v = t->get("shape_id"))        o.target_shape_id = v->as_string();
            if (const JsonValue* v = t->get("component_entry")) o.component_entry = v->as_string();
            if (const JsonValue* fp = t->get("fingerprint"))    o.fingerprint = read_fingerprint(*fp);
        }
        if (const JsonValue* p = entry->get("params")) {
            o.params = std::make_shared<JsonValue>(*p);
        }
        out.ops.push_back(std::move(o));
    }
    return out;
}

// Reconstruct the conversion options the original session used, from the
// options journaled on the recipe's load_step op. Mirrors the params written
// in session.cpp and re-emitted by recipe.cpp. Absent params keep defaults.
ConversionOptions load_options_from_recipe(const RecipeFile& recipe) {
    ConversionOptions options;
    for (const RecipeOp& op : recipe.ops) {
        if (op.op != "load_step" || !op.params
            || op.params->kind != JsonKind::object) {
            continue;
        }
        const JsonValue& p = *op.params;
        if (const JsonValue* v = p.get("allow_overlaps")) {
            options.allow_overlaps = v->as_bool(options.allow_overlaps);
        }
        if (const JsonValue* v = p.get("check_overlaps")) {
            options.check_overlaps = v->as_bool(options.check_overlaps);
        }
        if (const JsonValue* v = p.get("heal_shape"))
            options.heal_shape = v->as_bool(options.heal_shape);
        if (const JsonValue* v = p.get("unit_override"))
            options.unit_override = v->as_string();
        if (const JsonValue* v = p.get("unit_scale_to_mm_override"))
            options.unit_scale_to_mm_override = v->as_number();
        if (const JsonValue* v = p.get("normalize_to_unit"))
            options.normalize_to_unit = v->as_string();
        if (const JsonValue* v = p.get("overlap_volume_threshold")) {
            options.overlap_volume_threshold = v->as_number(options.overlap_volume_threshold);
        }
        if (const JsonValue* v = p.get("overlap_volume_fraction")) {
            options.overlap_volume_fraction = v->as_number(options.overlap_volume_fraction);
        }
        if (const JsonValue* v = p.get("mcnp_domain_id"))
            options.mcnp_domain_id = v->as_string();
        if (const JsonValue* v = p.get("require_domain_coverage"))
            options.require_domain_coverage = v->as_bool(false);
        break;
    }
    return options;
}

ApproximationPolicy approximation_policy_from_params(const JsonValue* params) {
    ApproximationPolicy p;
    p.enabled = true;
    if (!params || params->kind != JsonKind::object) return p;
    auto number = [&](const char* key, double fallback) {
        const JsonValue* v = params->get(key);
        return v ? v->as_number(fallback) : fallback;
    };
    auto boolean = [&](const char* key, bool fallback) {
        const JsonValue* v = params->get(key);
        return v ? v->as_bool(fallback) : fallback;
    };
    p.max_global_changed_volume_fraction =
        number("approximation_global_limit", 0.0);
    p.max_body_changed_volume_fraction =
        number("approximation_body_limit",
               p.max_global_changed_volume_fraction);
    p.max_action_changed_volume_fraction =
        number("approximation_action_limit",
               p.max_body_changed_volume_fraction);
    p.max_global_added_volume_fraction =
        number("approximation_global_added_limit",
               p.max_global_changed_volume_fraction);
    p.max_global_removed_volume_fraction =
        number("approximation_global_removed_limit",
               p.max_global_changed_volume_fraction);
    p.max_body_added_volume_fraction =
        number("approximation_body_added_limit",
               p.max_body_changed_volume_fraction);
    p.max_body_removed_volume_fraction =
        number("approximation_body_removed_limit",
               p.max_body_changed_volume_fraction);
    p.max_surface_deviation =
        number("approximation_surface_deviation_limit", 0.0);
    p.allow_surface_replacement =
        boolean("allow_surface_replacement", false);
    p.allow_sliver_removal = boolean("allow_sliver_removal", false);
    p.allow_bbox_replacement = boolean("allow_bbox_replacement", false);
    p.allow_numerical_shatter_loss =
        boolean("allow_numerical_shatter_loss", false);
    p.allow_overlap_resolution =
        boolean("allow_overlap_resolution", false);
    p.require_containment_preservation =
        boolean("require_containment_preservation", true);
    p.require_topology_preservation =
        boolean("require_topology_preservation", true);
    return p;
}

// ---- Matching -----------------------------------------------------------

bool close_rel(double a, double b, double rel_tol) {
    const double m = std::max(std::abs(a), std::abs(b));
    if (m == 0.0) return std::abs(a - b) <= rel_tol;
    return std::abs(a - b) <= rel_tol * m;
}

bool fingerprint_match(const Fingerprint& a, const ShapeFingerprint& b,
                       double rel_tol) {
    if (!a.valid || !b.valid) return false;
    if (a.face_count != b.face_count) return false;
    if (!close_rel(a.volume, b.volume, rel_tol)) return false;
    if (!close_rel(a.surface_area, b.surface_area, rel_tol)) return false;
    if (!close_rel(a.bbox_diagonal, b.bbox_diagonal, rel_tol)) return false;
    const double dx = a.cx - b.centroid_x;
    const double dy = a.cy - b.centroid_y;
    const double dz = a.cz - b.centroid_z;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double scale = std::max(a.bbox_diagonal, b.bbox_diagonal);
    return dist <= rel_tol * (scale > 0.0 ? scale : 1.0);
}

enum class Outcome { applied, soft_applied, unmatched, ambiguous, dispatch_failed };

struct ResolvedTarget {
    Outcome outcome = Outcome::unmatched;
    std::string resolved_shape_id; // local id in the loaded model
};

ResolvedTarget resolve_solid_target(const RecipeOp& op,
                                    const std::vector<ShapeMetadata>& solids,
                                    double rel_tol) {
    ResolvedTarget r;

    auto local_shape_id = [](const std::string& value) {
        if (value.compare(0, 4, "doc:") != 0) {
            return value;
        }
        const std::size_t slash = value.find('/');
        if (slash == std::string::npos || slash + 1 >= value.size()) {
            return value;
        }
        return value.substr(slash + 1);
    };

    const std::string target_local_id = local_shape_id(op.target_shape_id);
    if (target_local_id.compare(0, 6, "piece:") == 0) {
        r.outcome = Outcome::applied;
        r.resolved_shape_id = target_local_id;
        return r;
    }

    // Primary match: component_entry. Must be unique to count as a clean
    // applied. If two solids share the entry (shouldn't, given the import
    // fix, but defensive) treat as ambiguous.
    if (!op.component_entry.empty()) {
        const ShapeMetadata* hit = nullptr;
        int count = 0;
        for (const ShapeMetadata& m : solids) {
            if (m.component_entry == op.component_entry) {
                hit = &m;
                ++count;
            }
        }
        if (count == 1) {
            r.outcome = Outcome::applied;
            r.resolved_shape_id = hit->shape_id;
            return r;
        }
        if (count > 1) {
            r.outcome = Outcome::ambiguous;
            return r;
        }
    }

    // Soft match: fingerprint. Must be unique.
    if (op.fingerprint.valid) {
        const ShapeMetadata* hit = nullptr;
        int count = 0;
        for (const ShapeMetadata& m : solids) {
            if (fingerprint_match(op.fingerprint, m.fingerprint, rel_tol)) {
                hit = &m;
                ++count;
            }
        }
        if (count == 1) {
            r.outcome = Outcome::soft_applied;
            r.resolved_shape_id = hit->shape_id;
            return r;
        }
        if (count > 1) {
            r.outcome = Outcome::ambiguous;
            return r;
        }
    }

    r.outcome = Outcome::unmatched;
    return r;
}

// ---- Dispatch -----------------------------------------------------------

bool dispatch_solid_op(Session& session,
                       const RecipeOp& op,
                       const std::string& resolved_local_id,
                       std::string* error_out) {
    if (op.op == "remove_solid") {
        const CommandResult r = remove_solid(session, resolved_local_id,
                                             ReportIdMode::local);
        if (!r.ok) *error_out = "remove_solid failed";
        return r.ok;
    }
    if (op.op == "remove_residual_with_approximation") {
        ApproximationPolicy policy =
            approximation_policy_from_params(op.params.get());
        policy.allow_sliver_removal = true;
        const ApproximationCommandResult r =
            remove_residual_with_approximation(
                session, resolved_local_id, policy, ReportIdMode::local);
        if (!r.command.ok)
            *error_out = "remove_residual_with_approximation failed: "
                         + r.budget.code;
        return r.command.ok;
    }
    if (op.op == "resolve_overlap_with_approximation") {
        std::string owner_id;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("owner_shape_id"))
                owner_id = v->as_string();
        }
        if (owner_id.empty()) {
            *error_out = "missing owner_shape_id on overlap resolution";
            return false;
        }
        ApproximationPolicy policy =
            approximation_policy_from_params(op.params.get());
        policy.allow_overlap_resolution = true;
        const ApproximationCommandResult r =
            resolve_overlap_with_approximation(
                session, resolved_local_id, owner_id, policy,
                ReportIdMode::local);
        if (!r.command.ok)
            *error_out = "resolve_overlap_with_approximation failed: "
                         + r.budget.code;
        return r.command.ok;
    }
    if (op.op == "decompose_planar") {
        const CommandResult r = decompose_planar(session, resolved_local_id,
                                                 ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_planar failed";
        return r.ok;
    }
    if (op.op == "decompose_prismatic") {
        const CommandResult r = decompose_prismatic(session, resolved_local_id,
                                                    ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_prismatic failed";
        return r.ok;
    }
    if (op.op == "decompose_revolution") {
        const CommandResult r = decompose_revolution(
            session, resolved_local_id, ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_revolution failed";
        return r.ok;
    }
    if (op.op == "reuse_definition_conversion") {
        std::string source_id;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* value = op.params->get("source_solid_id"))
                source_id = value->as_string();
        }
        if (source_id.empty()) {
            *error_out = "missing source_solid_id on definition conversion reuse";
            return false;
        }
        const CommandResult r = reuse_definition_conversion(
            session, resolved_local_id, source_id, ReportIdMode::local);
        if (!r.ok) *error_out = "reuse_definition_conversion failed";
        return r.ok;
    }
    if (op.op == "decompose_shell_difference") {
        const CommandResult r = decompose_shell_difference(
            session, resolved_local_id, ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_shell_difference failed";
        return r.ok;
    }
    if (op.op == "decompose_pipe_annulus") {
        const CommandResult r = decompose_pipe_annulus(
            session, resolved_local_id, ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_pipe_annulus failed";
        return r.ok;
    }
    if (op.op == "shatter") {
        int max_depth = 8;
        bool allow_volume_drift = false;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("max_depth")) {
                max_depth = static_cast<int>(v->as_number(8.0));
            }
            if (const JsonValue* v = op.params->get("allow_volume_drift")) {
                allow_volume_drift = v->as_bool(false);
            }
        }
        const CommandResult r = shatter(session, resolved_local_id,
                                        max_depth, allow_volume_drift,
                                        ReportIdMode::local);
        if (!r.ok) *error_out = "shatter failed";
        return r.ok;
    }
    if (op.op == "shatter_with_approximation") {
        int max_depth = 8;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("max_depth"))
                max_depth = static_cast<int>(v->as_number(8.0));
        }
        ApproximationPolicy policy =
            approximation_policy_from_params(op.params.get());
        policy.allow_numerical_shatter_loss = true;
        const ApproximationCommandResult r = shatter_with_approximation(
            session, resolved_local_id, max_depth, policy,
            ReportIdMode::local);
        if (!r.command.ok)
            *error_out = "shatter_with_approximation failed: " + r.budget.code;
        return r.command.ok;
    }
    if (op.op == "decompose_by_face") {
        std::string face_id;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("face_id")) {
                face_id = v->as_string();
            }
        }
        if (face_id.empty()) {
            *error_out = "missing face_id on decompose_by_face";
            return false;
        }
        const CommandResult r = decompose_by_face(session, resolved_local_id,
                                                  face_id,
                                                  ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_by_face failed";
        return r.ok;
    }
    if (op.op == "decompose_by_plane") {
        if (!op.params || op.params->kind != JsonKind::object) {
            *error_out = "missing params on decompose_by_plane";
            return false;
        }
        const JsonValue* ox = op.params->get("origin_x");
        const JsonValue* oy = op.params->get("origin_y");
        const JsonValue* oz = op.params->get("origin_z");
        const JsonValue* nx = op.params->get("normal_x");
        const JsonValue* ny = op.params->get("normal_y");
        const JsonValue* nz = op.params->get("normal_z");
        if (!ox || !oy || !oz || !nx || !ny || !nz) {
            *error_out = "missing plane parameters on decompose_by_plane";
            return false;
        }
        const CommandResult r = decompose_by_plane(
            session,
            resolved_local_id,
            Vec3{ox->as_number(), oy->as_number(), oz->as_number()},
            Vec3{nx->as_number(), ny->as_number(), nz->as_number()},
            ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_by_plane failed";
        return r.ok;
    }
    if (op.op == "decompose_by_planes") {
        if (!op.params || op.params->kind != JsonKind::object) {
            *error_out = "missing params on decompose_by_planes";
            return false;
        }
        const JsonValue* count = op.params->get("plane_count");
        if (!count) {
            *error_out = "missing plane_count on decompose_by_planes";
            return false;
        }
        std::vector<PlaneSpec> planes;
        const int plane_count = static_cast<int>(count->as_number());
        for (int i = 0; i < plane_count; ++i) {
            const std::string idx = std::to_string(i);
            const JsonValue* ox = op.params->get("plane_" + idx + "_ox");
            const JsonValue* oy = op.params->get("plane_" + idx + "_oy");
            const JsonValue* oz = op.params->get("plane_" + idx + "_oz");
            const JsonValue* nx = op.params->get("plane_" + idx + "_nx");
            const JsonValue* ny = op.params->get("plane_" + idx + "_ny");
            const JsonValue* nz = op.params->get("plane_" + idx + "_nz");
            if (!ox || !oy || !oz || !nx || !ny || !nz) {
                *error_out = "missing indexed plane parameter on decompose_by_planes";
                return false;
            }
            planes.push_back({Vec3{ox->as_number(), oy->as_number(), oz->as_number()},
                              Vec3{nx->as_number(), ny->as_number(), nz->as_number()}});
        }
        const CommandResult r = decompose_by_planes(session,
                                                    resolved_local_id,
                                                    planes,
                                                    ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_by_planes failed";
        return r.ok;
    }
    if (op.op == "apply_decomposition_candidate") {
        std::string face_id;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("face_id")) {
                face_id = v->as_string();
            }
            const JsonValue* ox = op.params->get("origin_x");
            const JsonValue* oy = op.params->get("origin_y");
            const JsonValue* oz = op.params->get("origin_z");
            const JsonValue* nx = op.params->get("normal_x");
            const JsonValue* ny = op.params->get("normal_y");
            const JsonValue* nz = op.params->get("normal_z");
            if (ox && oy && oz && nx && ny && nz) {
                const CommandResult r = decompose_by_plane(
                    session,
                    resolved_local_id,
                    Vec3{ox->as_number(), oy->as_number(), oz->as_number()},
                    Vec3{nx->as_number(), ny->as_number(), nz->as_number()},
                    ReportIdMode::local);
                if (!r.ok) *error_out = "apply_decomposition_candidate plane failed";
                return r.ok;
            }
        }
        if (face_id.empty()) {
            *error_out = "missing face_id on apply_decomposition_candidate";
            return false;
        }
        const CommandResult r = decompose_by_face(session, resolved_local_id,
                                                  face_id,
                                                  ReportIdMode::local);
        if (!r.ok) *error_out = "apply_decomposition_candidate failed";
        return r.ok;
    }
    if (op.op == "apply_replacement_operation"
        || op.op == "apply_checked_replacement_operation"
        || op.op == "apply_budgeted_replacement_operation") {
        int op_enum = -1;
        ReplacementLimits limits;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("operation")) {
                op_enum = static_cast<int>(v->as_number());
            }
            if (const JsonValue* v = op.params->get("has_max_abs_delta")) {
                limits.has_absolute_delta = v->as_bool(false);
            }
            if (const JsonValue* v = op.params->get("max_abs_delta")) {
                limits.max_absolute_delta = v->as_number();
            }
            if (const JsonValue* v = op.params->get("has_max_rel_delta")) {
                limits.has_relative_delta = v->as_bool(false);
            }
            if (const JsonValue* v = op.params->get("max_rel_delta")) {
                limits.max_relative_delta = v->as_number();
            }
        }
        if (op_enum < 0 || op_enum > 4) {
            *error_out = "missing or out-of-range replacement enum";
            return false;
        }
        if (op.op == "apply_budgeted_replacement_operation") {
            const ApproximationCommandResult r =
                apply_budgeted_replacement_operation(
                    session, resolved_local_id,
                    static_cast<ReplacementOperation>(op_enum),
                    approximation_policy_from_params(op.params.get()),
                    ReportIdMode::local);
            if (!r.command.ok)
                *error_out = "apply_budgeted_replacement_operation failed: "
                             + r.budget.code;
            return r.command.ok;
        }
        if (op.op == "apply_checked_replacement_operation") {
            ReplacementCommandResult r = apply_checked_replacement_operation(
                session, resolved_local_id,
                static_cast<ReplacementOperation>(op_enum),
                limits,
                ReportIdMode::local);
            if (!r.command.ok) *error_out = "apply_checked_replacement_operation failed";
            return r.command.ok;
        }
        const CommandResult r = apply_replacement_operation(
            session, resolved_local_id,
            static_cast<ReplacementOperation>(op_enum),
            ReportIdMode::local);
        if (!r.ok) *error_out = "apply_replacement_operation failed";
        return r.ok;
    }
    if (op.op == "replace_solid_with_bbox") {
        const CommandResult r = replace_solid_with_bbox(session, resolved_local_id,
                                                        ReportIdMode::local);
        if (!r.ok) *error_out = "replace_solid_with_bbox failed";
        return r.ok;
    }
    if (op.op == "replace_solid_with_bbox_cylinder") {
        const CommandResult r = replace_solid_with_bbox_cylinder(session, resolved_local_id,
                                                                 ReportIdMode::local);
        if (!r.ok) *error_out = "replace_solid_with_bbox_cylinder failed";
        return r.ok;
    }
    if (op.op == "replace_solid_with_fitted_cylinder") {
        const CommandResult r = replace_solid_with_fitted_cylinder(session, resolved_local_id,
                                                                   ReportIdMode::local);
        if (!r.ok) *error_out = "replace_solid_with_fitted_cylinder failed";
        return r.ok;
    }
    if (op.op == "replace_solid_with_fitted_sphere") {
        const CommandResult r = replace_solid_with_fitted_sphere(session, resolved_local_id,
                                                                 ReportIdMode::local);
        if (!r.ok) *error_out = "replace_solid_with_fitted_sphere failed";
        return r.ok;
    }
    if (op.op == "replace_solid_with_fitted_cone") {
        const CommandResult r = replace_solid_with_fitted_cone(session, resolved_local_id,
                                                               ReportIdMode::local);
        if (!r.ok) *error_out = "replace_solid_with_fitted_cone failed";
        return r.ok;
    }
    *error_out = "unknown solid op: " + op.op;
    return false;
}

bool dispatch_face_op(Session& session,
                      const RecipeOp& op,
                      std::string* error_out) {
    if (op.op == "apply_budgeted_surface_replacement_candidate") {
        std::string candidate_id;
        if (op.params && op.params->kind == JsonKind::object)
            if (const JsonValue* v = op.params->get("candidate_id"))
                candidate_id = v->as_string();
        if (candidate_id.empty()) {
            *error_out = "missing candidate_id on budgeted surface replacement";
            return false;
        }
        const ApproximationCommandResult r =
            apply_budgeted_surface_replacement_candidate(
                session, op.target_shape_id, candidate_id,
                approximation_policy_from_params(op.params.get()),
                ReportIdMode::local);
        if (!r.command.ok)
            *error_out = "apply_budgeted_surface_replacement_candidate failed: "
                         + r.budget.code;
        return r.command.ok;
    }
    if (op.op == "apply_surface_replacement_candidate") {
        std::string kind_name;
        double budget = 0.0;
        bool has_budget = false;
        if (op.params && op.params->kind == JsonKind::object) {
            if (const JsonValue* v = op.params->get("kind"))
                kind_name = v->as_string();
            if (const JsonValue* v = op.params->get("max_deviation_budget")) {
                budget = v->as_number();
                has_budget = true;
            }
        }
        SplineFitKind kind = SplineFitKind::none;
        if (kind_name == "plane") kind = SplineFitKind::plane;
        else if (kind_name == "sphere") kind = SplineFitKind::sphere;
        else if (kind_name == "cylinder") kind = SplineFitKind::cylinder;
        else if (kind_name == "cone") kind = SplineFitKind::cone;
        if (kind == SplineFitKind::none || !has_budget
            || !std::isfinite(budget) || budget < 0.0) {
            *error_out = "missing or invalid replay parameters on " + op.op;
            return false;
        }
        const CommandResult r = replace_face_with_fit(
            session, op.target_shape_id, kind, true, budget,
            ReportIdMode::local);
        if (!r.ok) *error_out = "apply_surface_replacement_candidate failed";
        return r.ok;
    }
    if (op.op != "replace_face_with_fit") {
        *error_out = "unknown face op: " + op.op;
        return false;
    }
    int kind = -1;
    bool has_dev = false;
    double dev = 0.0;
    if (op.params && op.params->kind == JsonKind::object) {
        if (const JsonValue* v = op.params->get("kind"))             kind = static_cast<int>(v->as_number());
        if (const JsonValue* v = op.params->get("has_max_deviation")) has_dev = v->as_bool();
        if (const JsonValue* v = op.params->get("max_deviation"))     dev = v->as_number();
    }
    if (kind < 0) {
        *error_out = "missing 'kind' on replace_face_with_fit";
        return false;
    }
    const CommandResult r = replace_face_with_fit(session, op.target_shape_id,
                                                  static_cast<SplineFitKind>(kind),
                                                  has_dev, dev,
                                                  ReportIdMode::local);
    if (!r.ok) *error_out = "replace_face_with_fit failed";
    return r.ok;
}

// ---- Report -------------------------------------------------------------

std::string json_escape(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 2);
    for (char c : v) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c;
        }
    }
    return out;
}

const char* outcome_name(Outcome o) {
    switch (o) {
    case Outcome::applied:        return "applied";
    case Outcome::soft_applied:   return "soft_applied";
    case Outcome::unmatched:      return "unmatched";
    case Outcome::ambiguous:      return "ambiguous";
    case Outcome::dispatch_failed:return "dispatch_failed";
    }
    return "unknown";
}

struct OpRecord {
    std::string op;
    std::string target_kind;
    std::string original_shape_id;
    std::string component_entry;
    Outcome outcome = Outcome::unmatched;
    std::string resolved_shape_id;
    std::string error;
};

void write_report(const std::string& path,
                  const RecipeFile& recipe,
                  const std::string& step_path,
                  const std::string& step_digest,
                  const std::vector<OpRecord>& records,
                  const std::vector<std::string>& new_solids_unaddressed,
                  bool postconditions_checked,
                  bool postconditions_passed,
                  const std::string& postcondition_error) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "warning: could not open report " << path << "\n";
        return;
    }
    out << "{\n";
    out << "  \"schema\": \"csgify.replay-report\",\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"recipe_step_path\": \""    << json_escape(recipe.step_path) << "\",\n";
    out << "  \"recipe_step_digest\": \""  << json_escape(recipe.step_digest) << "\",\n";
    out << "  \"replay_step_path\": \""    << json_escape(step_path) << "\",\n";
    out << "  \"replay_step_digest\": \""  << json_escape(step_digest) << "\",\n";
    out << "  \"same_step\": "
        << (recipe.step_digest == step_digest && !step_digest.empty() ? "true" : "false")
        << ",\n";
    out << "  \"postconditions\": {\"checked\":"
        << (postconditions_checked ? "true" : "false")
        << ",\"passed\":" << (postconditions_passed ? "true" : "false");
    if (!postcondition_error.empty()) {
        out << ",\"error\":\"" << json_escape(postcondition_error) << "\"";
    }
    out << "},\n";

    int n_applied = 0, n_soft = 0, n_unmatched = 0, n_ambig = 0, n_fail = 0;
    for (const auto& r : records) {
        switch (r.outcome) {
        case Outcome::applied:        ++n_applied; break;
        case Outcome::soft_applied:   ++n_soft; break;
        case Outcome::unmatched:      ++n_unmatched; break;
        case Outcome::ambiguous:      ++n_ambig; break;
        case Outcome::dispatch_failed:++n_fail; break;
        }
    }
    out << "  \"counts\": {"
        << "\"applied\":"        << n_applied
        << ",\"soft_applied\":"  << n_soft
        << ",\"unmatched\":"     << n_unmatched
        << ",\"ambiguous\":"     << n_ambig
        << ",\"dispatch_failed\":" << n_fail
        << "},\n";

    out << "  \"ops\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        const OpRecord& r = records[i];
        out << "    {"
            << "\"op\":\""              << json_escape(r.op) << "\""
            << ",\"target_kind\":\""    << json_escape(r.target_kind) << "\""
            << ",\"original_shape_id\":\"" << json_escape(r.original_shape_id) << "\""
            << ",\"component_entry\":\"" << json_escape(r.component_entry) << "\""
            << ",\"resolved_shape_id\":\"" << json_escape(r.resolved_shape_id) << "\""
            << ",\"outcome\":\""        << outcome_name(r.outcome) << "\"";
        if (!r.error.empty()) {
            out << ",\"error\":\"" << json_escape(r.error) << "\"";
        }
        out << "}";
        if (i + 1 < records.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"new_solids_unaddressed\": [";
    for (std::size_t i = 0; i < new_solids_unaddressed.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape(new_solids_unaddressed[i]) << "\"";
    }
    out << "]\n}\n";
}

} // namespace

namespace {

ReconstructedSession execute_recipe(const std::string& recipe_json_path,
                                    const std::string& step_override,
                                    const std::string& output_path,
                                    const std::string& report_path,
                                    const std::string& step_output_path,
                                    const ReplayOptions& options,
                                    bool execute_outputs,
                                    const ConversionOptions* expected_options,
                                    bool require_bound_preparation) {
    ReconstructedSession replay_result;
    RecipeFile recipe;
    try {
        recipe = read_recipe(recipe_json_path);
    } catch (const std::exception& e) {
        std::cerr << "error recipe_parse_failed: " << e.what() << "\n";
        replay_result.error = std::string("recipe_parse_failed: ") + e.what();
        return replay_result;
    }

    const std::string step_path = step_override.empty() ? recipe.step_path : step_override;
    if (step_path.empty()) {
        std::cerr << "error replay_missing_step\n";
        replay_result.error = "replay_missing_step";
        return replay_result;
    }

    bool preparation_bound = false;
    for (const RecipeOp& op : recipe.ops) {
        if (op.op != "load_step" || !op.params
            || op.params->kind != JsonKind::object) continue;
        preparation_bound = op.params->get("heal_shape")
            && op.params->get("unit_override")
            && op.params->get("unit_scale_to_mm_override")
            && op.params->get("normalize_to_unit");
        break;
    }
    replay_result.preparation_bound = preparation_bound;
    if (require_bound_preparation
        && (recipe.csgify_version.empty()
            || recipe.csgify_version != csgify_version())) {
        replay_result.error =
            "csgify version differs from the recorded decision history";
        return replay_result;
    }
    if (require_bound_preparation
        && (recipe.occt_version.empty()
            || recipe.occt_version != occt_version())) {
        replay_result.error =
            "OCCT version differs from the recorded decision history";
        return replay_result;
    }
    const ConversionOptions replay_options = load_options_from_recipe(recipe);
    if (require_bound_preparation && !preparation_bound) {
        replay_result.error =
            "decision history predates preparation binding; reimport is required";
        return replay_result;
    }
    if (expected_options && preparation_bound
        && (replay_options.heal_shape != expected_options->heal_shape
            || replay_options.unit_override != expected_options->unit_override
            || replay_options.unit_scale_to_mm_override
                != expected_options->unit_scale_to_mm_override
            || replay_options.normalize_to_unit
                != expected_options->normalize_to_unit)) {
        replay_result.error =
            "project preparation differs from the recorded decision history";
        return replay_result;
    }

    std::string step_digest_replay;
    {
        const std::string raw = fnv1a128_file_hex(step_path);
        if (!raw.empty()) step_digest_replay = "fnv1a128:" + raw;
    }
    replay_result.same_source = !step_digest_replay.empty()
        && recipe.step_digest == step_digest_replay;
    if (require_bound_preparation && !replay_result.same_source) {
        replay_result.error =
            "source contents differ from the recorded decision history";
        return replay_result;
    }

    SessionPtr session_ptr = create_session();
    Session& session = *session_ptr;

    // Reproduce the conversion options the original session loaded with
    // (notably allow_overlaps). The recipe's own load_step op is skipped in
    // the loop below, so we apply its journaled options here, at reload.
    const DocumentId doc = load_step(session, step_path, &replay_options);

    // Snapshot of the loaded metadata. Recipe ops were captured against
    // the original-import metadata, so we resolve targets against the
    // freshly-loaded snapshot for consistency. The session API does not
    // expose CadModel directly, so we re-load once via the engine.
    const CadModel snapshot = load_step_model(step_path);

    std::vector<OpRecord> records;
    records.reserve(recipe.ops.size());

    std::unordered_set<std::string> addressed_entries;

    bool emit_called = false;
    bool step_export_called = false;
    bool postconditions_checked = false;
    bool postconditions_passed = !recipe.has_postconditions;
    std::string postcondition_error;
    int exit_status = 0;

    const auto check_postconditions = [&]() {
        if (!recipe.has_postconditions) return true;
        postconditions_checked = true;
        std::ostringstream error;
        if (recipe.has_decomposition_postconditions) {
            const DecompositionProgressState state =
                decomposition_progress_state(session, doc);
            if (state.residual_count != recipe.expected_residual_count) {
            error << "residual_count expected="
                  << recipe.expected_residual_count << " actual="
                  << state.residual_count;
            }
            if (state.active_piece_count != recipe.expected_active_piece_count) {
            if (error.tellp() > 0) error << "; ";
            error << "active_piece_count expected="
                  << recipe.expected_active_piece_count << " actual="
                  << state.active_piece_count;
            }
            if (state.overlap_error_count > recipe.max_overlap_errors) {
            if (error.tellp() > 0) error << "; ";
            error << "overlap_errors maximum=" << recipe.max_overlap_errors
                  << " actual=" << state.overlap_error_count;
            }
        }
        if (recipe.has_approximation_postconditions) {
            const ApproximationLedger ledger =
                document_approximation_ledger(session, doc);
            const double scale = std::max(
                1.0, std::max(recipe.expected_cumulative_changed_volume,
                              ledger.cumulative_changed_volume));
            const double tolerance = 1.0e-8 * scale;
            auto compare = [&](const char* name, double expected, double actual) {
                if (std::abs(expected - actual) <= tolerance) return;
                if (error.tellp() > 0) error << "; ";
                error << name << " expected=" << expected
                      << " actual=" << actual;
            };
            compare("approximation_changed_volume",
                    recipe.expected_cumulative_changed_volume,
                    ledger.cumulative_changed_volume);
            compare("approximation_added_volume",
                    recipe.expected_cumulative_added_volume,
                    ledger.cumulative_added_volume);
            compare("approximation_removed_volume",
                    recipe.expected_cumulative_removed_volume,
                    ledger.cumulative_removed_volume);
        }
        postcondition_error = error.str();
        postconditions_passed = postcondition_error.empty();
        return postconditions_passed;
    };

    for (std::size_t op_index = 0;
         op_index < recipe.ops.size();
         ++op_index) {
        const RecipeOp& op = recipe.ops[op_index];
        OpRecord rec;
        rec.op = op.op;
        rec.target_kind = op.target_kind;
        rec.original_shape_id = op.target_shape_id;
        rec.component_entry = op.component_entry;

        const OperationDescriptor* operation_descriptor = find_operation(op.op);
        if (!operation_descriptor) {
            rec.outcome = Outcome::dispatch_failed;
            rec.error = "operation is not present in this csgify operation registry: "
                + op.op;
            records.push_back(std::move(rec));
            exit_status = 1;
            continue;
        }
        if (op.operation_version != operation_descriptor->version) {
            rec.outcome = Outcome::dispatch_failed;
            rec.error = "unsupported operation version for " + op.op;
            records.push_back(std::move(rec));
            exit_status = 1;
            continue;
        }

        // Skip the recipe's own load_step (we already loaded above with
        // the override). Mark it as applied for the report.
        if (op.op == "load_step") {
            rec.outcome = Outcome::applied;
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            continue;
        }

        if (op.op == "emit_mcnp") {
            if (!check_postconditions()) {
                rec.outcome = Outcome::dispatch_failed;
                rec.error = "recipe postcondition failed: "
                    + postcondition_error;
                rec.resolved_shape_id = doc;
                emit_called = true;
                records.push_back(std::move(rec));
                exit_status = 1;
                continue;
            }
            if (!execute_outputs) {
                rec.outcome = Outcome::applied;
                rec.resolved_shape_id = doc;
                records.push_back(std::move(rec));
                continue;
            }
            // Run analyze first so the overlap gate is live during replay.
            // emit_document's convert_model only reads model.diagnostics; the
            // pairwise overlap pass populates them here. Without this, replay
            // would happily emit a deck for overlapping geometry the recipe
            // never allowed. allow_overlaps (applied at load) downgrades the
            // overlap diagnostic to a warning, so the gate passes by intent.
            analyze(session, doc, ReportIdMode::local);
            CommandResult r = emit_document(session, doc, output_path,
                                            ReportIdMode::local);
            rec.outcome = r.ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!r.ok) rec.error = "emit failed";
            rec.resolved_shape_id = doc;
            emit_called = true;
            records.push_back(std::move(rec));
            if (!r.ok) {
                exit_status = 1;
            } else if (!step_output_path.empty() && !step_export_called) {
                CommandResult sr = export_document_step(session, doc,
                                                       step_output_path,
                                                       ReportIdMode::local);
                if (!sr.ok) exit_status = 1;
            }
            continue;
        }

        if (op.op == "export_step") {
            if (!check_postconditions()) {
                rec.outcome = Outcome::dispatch_failed;
                rec.error = "recipe postcondition failed: "
                    + postcondition_error;
                rec.resolved_shape_id = doc;
                records.push_back(std::move(rec));
                exit_status = 1;
                continue;
            }
            if (!execute_outputs) {
                rec.outcome = Outcome::applied;
                rec.resolved_shape_id = doc;
                records.push_back(std::move(rec));
                continue;
            }
            std::string recorded_path;
            if (op.params && op.params->kind == JsonKind::object) {
                if (const JsonValue* value = op.params->get("output_path")) {
                    recorded_path = value->as_string();
                }
            }
            const std::string path = step_output_path.empty()
                ? recorded_path : step_output_path;
            if (path.empty()) {
                rec.outcome = Outcome::dispatch_failed;
                rec.error = "export_step missing output_path";
                exit_status = 1;
            } else {
                CommandResult exported = export_document_step(
                    session, doc, path, ReportIdMode::local);
                rec.outcome = exported.ok ? Outcome::applied
                                          : Outcome::dispatch_failed;
                if (!exported.ok) {
                    rec.error = "export_step failed";
                    exit_status = 1;
                } else {
                    step_export_called = true;
                }
            }
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            continue;
        }

        if (op.op == "auto_fit_splines") {
            bool has_dev = false;
            double dev = 0.0;
            if (op.params && op.params->kind == JsonKind::object) {
                if (const JsonValue* v = op.params->get("has_max_deviation")) has_dev = v->as_bool();
                if (const JsonValue* v = op.params->get("max_deviation"))     dev = v->as_number();
            }
            CommandResult r = auto_fit_splines(session, doc, has_dev, dev,
                                               ReportIdMode::local);
            rec.outcome = r.ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!r.ok) rec.error = "auto_fit_splines failed";
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            continue;
        }

        if (op.op == "define_material") {
            MaterialDefinition material;
            std::string phase;
            if (op.params && op.params->kind == JsonKind::object) {
                if (const JsonValue* value = op.params->get("material_id"))
                    material.id = value->as_string();
                if (const JsonValue* value = op.params->get("phase"))
                    phase = value->as_string();
                if (const JsonValue* value = op.params->get("density"))
                    material.density = value->as_number();
            }
            material.phase = phase == "liquid" ? MaterialPhase::liquid
                : phase == "gas" ? MaterialPhase::gas
                : phase == "solid" ? MaterialPhase::solid
                : phase == "vacuum" ? MaterialPhase::vacuum
                                      : MaterialPhase::unknown;
            const CommandResult r = define_material(
                session, doc, material, ReportIdMode::local);
            rec.outcome = r.ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!r.ok) { rec.error = "define_material failed"; exit_status = 1; }
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            continue;
        }

        if (op.op == "apply_contained_medium"
            || op.op == "stage_contained_medium_request") {
            MediumGenerationRequest request;
            bool approved = false;
            auto disposition = [](const std::string& value) {
                return value == "cap_here" ? MediumPortDisposition::cap_here
                    : value == "interface_to_external_model"
                        ? MediumPortDisposition::interface_to_external_model
                    : value == "open_to_domain"
                        ? MediumPortDisposition::open_to_domain
                    : value == "connect_to_port"
                        ? MediumPortDisposition::connect_to_port
                        : MediumPortDisposition::unresolved;
            };
            auto valve = [](const std::string& value) {
                return value == "open" ? MediumValveState::open
                    : value == "closed" ? MediumValveState::closed
                    : value == "unresolved" ? MediumValveState::unresolved
                                              : MediumValveState::not_applicable;
            };
            if (op.params && op.params->kind == JsonKind::object) {
                if (const JsonValue* value = op.params->get("request_id"))
                    request.id = value->as_string();
                if (const JsonValue* value = op.params->get("material_id"))
                    request.material_id = value->as_string();
                if (const JsonValue* value = op.params->get(
                        "complement_material_id"))
                    request.complement_material_id = value->as_string();
                if (const JsonValue* value = op.params->get("fill_mode")) {
                    const std::string mode = value->as_string();
                    request.fill_mode = mode == "level" ? MediumFillMode::level
                        : mode == "bounded_segment"
                            ? MediumFillMode::bounded_segment
                            : MediumFillMode::full;
                }
                int host_count = 0;
                if (const JsonValue* value = op.params->get("host_count"))
                    host_count = static_cast<int>(value->as_number());
                for (int index = 1; index <= host_count; ++index) {
                    if (const JsonValue* value = op.params->get(
                            "host_" + std::to_string(index)
                            + "_component_id"))
                        request.host_component_ids.push_back(value->as_string());
                }
                if (host_count == 0) {
                    if (const JsonValue* value = op.params->get(
                            "host_component_id"))
                        request.host_component_ids.push_back(value->as_string());
                }
                if (const JsonValue* value = op.params->get("user_approved"))
                    approved = value->as_bool();
                if (const JsonValue* value = op.params->get("fill_level"))
                    request.fill_level = value->as_number();
                if (const JsonValue* value = op.params->get(
                        "target_volume_fraction"))
                    request.target_volume_fraction = value->as_number();
                if (const JsonValue* value = op.params->get(
                        "allow_disconnected_pockets"))
                    request.allow_disconnected_pockets = value->as_bool();
                if (const JsonValue* value = op.params->get("max_worker_count"))
                    request.max_worker_count = static_cast<std::size_t>(
                        value->as_number(1.0));
                int port_count = 2;
                if (const JsonValue* value = op.params->get("port_count"))
                    port_count = static_cast<int>(value->as_number());
                for (int index = 1; index <= port_count; ++index) {
                    const std::string prefix = "port_" + std::to_string(index);
                    const JsonValue* id = op.params->get(prefix + "_id");
                    const JsonValue* kind = op.params->get(
                        prefix + "_disposition");
                    if (id && kind) {
                        MediumPortDecision port;
                        port.port_id = id->as_string();
                        port.disposition = disposition(kind->as_string());
                        if (const JsonValue* peer = op.params->get(
                                prefix + "_peer_id"))
                            port.peer_port_id = peer->as_string();
                        if (const JsonValue* state = op.params->get(
                                prefix + "_valve_state"))
                            port.valve_state = valve(state->as_string());
                        request.ports.push_back(std::move(port));
                    }
                }
                int internal_count = 0;
                if (const JsonValue* value = op.params->get(
                        "internal_component_count"))
                    internal_count = static_cast<int>(value->as_number());
                for (int index = 1; index <= internal_count; ++index) {
                    if (const JsonValue* value = op.params->get(
                            "internal_" + std::to_string(index)
                            + "_component_id"))
                        request.internal_components_to_subtract.push_back(
                            value->as_string());
                }
                int seed_count = 0;
                if (const JsonValue* value = op.params->get("seed_count"))
                    seed_count = static_cast<int>(value->as_number());
                for (int index = 1; index <= seed_count; ++index) {
                    const std::string prefix = "seed_" + std::to_string(index);
                    GeometricSeed seed;
                    if (const JsonValue* value = op.params->get(prefix + "_kind"))
                        seed.kind = value->as_string() == "face"
                            ? GeometricSeedKind::face
                            : GeometricSeedKind::point;
                    if (const JsonValue* value = op.params->get(
                            prefix + "_entity_id"))
                        seed.entity_id = value->as_string();
                    const char* coordinates[] = {"_x", "_y", "_z"};
                    for (std::size_t coordinate = 0; coordinate < 3;
                         ++coordinate)
                        if (const JsonValue* value = op.params->get(
                                prefix + coordinates[coordinate]))
                            seed.point[coordinate] = value->as_number();
                    request.seed_points_or_faces.push_back(std::move(seed));
                }
            }
            const CommandResult r = op.op == "apply_contained_medium"
                ? apply_contained_medium(
                    session, doc, request, approved, ReportIdMode::local)
                : stage_contained_medium_request(
                    session, doc, request, ReportIdMode::local);
            rec.outcome = r.ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!r.ok) {
                rec.error = op.op + " failed";
                exit_status = 1;
            }
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            continue;
        }

        if (op.op == "undo_last_mutation") {
            CommandResult r = undo_last_mutation(session, doc,
                                                  ReportIdMode::local);
            rec.outcome = r.ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!r.ok) rec.error = "undo_last_mutation failed";
            rec.resolved_shape_id = doc;
            records.push_back(std::move(rec));
            if (!r.ok) exit_status = 1;
            continue;
        }

        if (op.target_kind == "solid") {
            if (op.op == "remove_solid") {
                std::size_t batch_end = op_index + 1;
                while (batch_end < recipe.ops.size()
                       && recipe.ops[batch_end].target_kind == "solid"
                       && recipe.ops[batch_end].op == "remove_solid") {
                    ++batch_end;
                }

                std::vector<OpRecord> batch_records;
                std::vector<std::string> resolved_ids;
                batch_records.reserve(batch_end - op_index);
                resolved_ids.reserve(batch_end - op_index);
                bool batch_resolved = true;

                for (std::size_t i = op_index; i < batch_end; ++i) {
                    const RecipeOp& removal = recipe.ops[i];
                    OpRecord removal_record;
                    removal_record.op = removal.op;
                    removal_record.target_kind = removal.target_kind;
                    removal_record.original_shape_id = removal.target_shape_id;
                    removal_record.component_entry = removal.component_entry;

                    const OperationDescriptor* descriptor =
                        find_operation(removal.op);
                    if (!descriptor
                        || removal.operation_version != descriptor->version) {
                        removal_record.outcome = Outcome::dispatch_failed;
                        removal_record.error =
                            "unsupported operation version for remove_solid";
                        batch_resolved = false;
                    } else {
                        ResolvedTarget resolved = resolve_solid_target(
                            removal,
                            snapshot.solid_metadata,
                            options.rel_tolerance);
                        removal_record.outcome = resolved.outcome;
                        removal_record.resolved_shape_id =
                            resolved.resolved_shape_id;
                        if (resolved.outcome == Outcome::applied
                            || resolved.outcome == Outcome::soft_applied) {
                            addressed_entries.insert(removal.component_entry);
                            resolved_ids.push_back(resolved.resolved_shape_id);
                        } else {
                            batch_resolved = false;
                        }
                    }
                    batch_records.push_back(std::move(removal_record));
                }

                if (batch_resolved) {
                    const CommandResult result = remove_solids(
                        session, resolved_ids, ReportIdMode::local);
                    if (!result.ok) {
                        for (OpRecord& removal_record : batch_records) {
                            removal_record.outcome = Outcome::dispatch_failed;
                            removal_record.error = "remove_solids failed";
                        }
                        exit_status = 1;
                    }
                } else {
                    for (OpRecord& removal_record : batch_records) {
                        if (removal_record.outcome == Outcome::applied
                            || removal_record.outcome
                                == Outcome::soft_applied) {
                            removal_record.outcome = Outcome::dispatch_failed;
                            removal_record.error =
                                "removal batch contained an unresolved target";
                        }
                    }
                    exit_status = 1;
                }

                for (OpRecord& removal_record : batch_records) {
                    records.push_back(std::move(removal_record));
                }
                op_index = batch_end - 1;
                continue;
            }

            ResolvedTarget rt = resolve_solid_target(op, snapshot.solid_metadata,
                                                     options.rel_tolerance);
            rec.outcome = rt.outcome;
            rec.resolved_shape_id = rt.resolved_shape_id;
            if (rt.outcome == Outcome::applied || rt.outcome == Outcome::soft_applied) {
                addressed_entries.insert(op.component_entry);
                std::string err;
                if (!dispatch_solid_op(session, op, rt.resolved_shape_id, &err)) {
                    rec.outcome = Outcome::dispatch_failed;
                    rec.error = err;
                    exit_status = 1;
                }
            } else {
                exit_status = 1;
            }
            records.push_back(std::move(rec));
            continue;
        }

        if (op.target_kind == "face") {
            std::string err;
            const bool ok = dispatch_face_op(session, op, &err);
            rec.outcome = ok ? Outcome::applied : Outcome::dispatch_failed;
            if (!ok) { rec.error = err; exit_status = 1; }
            rec.resolved_shape_id = op.target_shape_id;
            records.push_back(std::move(rec));
            continue;
        }

        // Unknown target kind.
        rec.outcome = Outcome::dispatch_failed;
        rec.error = "unknown target kind: " + op.target_kind;
        records.push_back(std::move(rec));
        exit_status = 1;
    }

    if (execute_outputs && !emit_called && !output_path.empty()) {
        if (!check_postconditions()) {
            std::cerr << "error replay_postcondition_failed: "
                      << postcondition_error << '\n';
            exit_status = 1;
        } else {
        // Recipe omitted an explicit emit (rare); fire one so --out has
        // meaning. Analyze first for the same overlap-gate reason as above.
        analyze(session, doc, ReportIdMode::local);
        CommandResult r = emit_document(session, doc, output_path,
                                        ReportIdMode::local);
        if (!r.ok) {
            exit_status = 1;
        } else if (!step_output_path.empty()) {
            CommandResult sr = export_document_step(session, doc,
                                                   step_output_path,
                                                   ReportIdMode::local);
            if (!sr.ok) exit_status = 1;
        }
        }
    }

    if (!postconditions_checked && !check_postconditions()) {
        exit_status = 1;
    }

    // New solids in the revised STEP that no recipe op addressed.
    std::vector<std::string> new_solids_unaddressed;
    for (const ShapeMetadata& m : snapshot.solid_metadata) {
        if (m.component_entry.empty()) continue;
        if (addressed_entries.count(m.component_entry)) continue;
        // Skip if any recipe op had a fingerprint that soft-matched this
        // entry — that's effectively addressed by the soft path.
        new_solids_unaddressed.push_back(m.shape_id + " (" + m.component_entry + ")");
    }

    if (!report_path.empty()) {
        write_report(report_path, recipe, step_path, step_digest_replay,
                     records, new_solids_unaddressed,
                     postconditions_checked, postconditions_passed,
                     postcondition_error);
    }
    for (const OpRecord& record : records) {
        switch (record.outcome) {
        case Outcome::applied: ++replay_result.applied_count; break;
        case Outcome::soft_applied: ++replay_result.soft_applied_count; break;
        case Outcome::unmatched: ++replay_result.unmatched_count; break;
        case Outcome::ambiguous: ++replay_result.ambiguous_count; break;
        case Outcome::dispatch_failed: ++replay_result.dispatch_failed_count; break;
        }
    }
    replay_result.status = exit_status;
    if (exit_status == 0) {
        if (!execute_outputs) {
            for (const DecisionReviewRecord& review : recipe.reviews) {
                record_decision_review(*session_ptr, review.target_id,
                    review.proposal_id, review.decision, review.audit,
                    review.review_kind, review.action, review.candidate_id,
                    review.base_revision);
            }
        }
        replay_result.session = std::move(session_ptr);
        replay_result.document_id = doc;
    } else if (replay_result.error.empty()) {
        replay_result.error = "one or more saved operations could not be reconstructed";
    }
    return replay_result;
}

} // namespace

ReconstructedSession reconstruct_recipe_session(
    const std::string& recipe_json_path,
    const std::string& step_override,
    const ConversionOptions& expected_options,
    const std::string& report_path,
    const ReplayOptions& options,
    bool require_preparation_match) {
    return execute_recipe(recipe_json_path, step_override, {}, report_path, {},
                          options, false, &expected_options,
                          require_preparation_match);
}

int replay_recipe(const std::string& recipe_json_path,
                  const std::string& step_override,
                  const std::string& output_path,
                  const std::string& report_path,
                  const std::string& step_output_path,
                  const ReplayOptions& options) {
    return execute_recipe(recipe_json_path, step_override, output_path,
                          report_path, step_output_path, options, true,
                          nullptr, false).status;
}

} // namespace csgify
