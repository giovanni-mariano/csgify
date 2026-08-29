#include "replay.hpp"

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
    std::vector<RecipeOp> ops;
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
        if (const JsonValue* v = p.get("overlap_volume_threshold")) {
            options.overlap_volume_threshold = v->as_number(options.overlap_volume_threshold);
        }
        if (const JsonValue* v = p.get("overlap_volume_fraction")) {
            options.overlap_volume_fraction = v->as_number(options.overlap_volume_fraction);
        }
        break;
    }
    return options;
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
    if (op.op == "decompose_planar") {
        const CommandResult r = decompose_planar(session, resolved_local_id,
                                                 ReportIdMode::local);
        if (!r.ok) *error_out = "decompose_planar failed";
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
        || op.op == "apply_checked_replacement_operation") {
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
                  const std::vector<std::string>& new_solids_unaddressed) {
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

int replay_recipe(const std::string& recipe_json_path,
                  const std::string& step_override,
                  const std::string& output_path,
                  const std::string& report_path,
                  const std::string& step_output_path,
                  const ReplayOptions& options) {
    RecipeFile recipe;
    try {
        recipe = read_recipe(recipe_json_path);
    } catch (const std::exception& e) {
        std::cerr << "error recipe_parse_failed: " << e.what() << "\n";
        return 2;
    }

    const std::string step_path = step_override.empty() ? recipe.step_path : step_override;
    if (step_path.empty()) {
        std::cerr << "error replay_missing_step\n";
        return 2;
    }

    SessionPtr session_ptr = create_session();
    Session& session = *session_ptr;

    // Reproduce the conversion options the original session loaded with
    // (notably allow_overlaps). The recipe's own load_step op is skipped in
    // the loop below, so we apply its journaled options here, at reload.
    const ConversionOptions replay_options = load_options_from_recipe(recipe);
    const DocumentId doc = load_step(session, step_path, &replay_options);

    // Snapshot of the loaded metadata. Recipe ops were captured against
    // the original-import metadata, so we resolve targets against the
    // freshly-loaded snapshot for consistency. The session API does not
    // expose CadModel directly, so we re-load once via the engine.
    const CadModel snapshot = load_step_model(step_path);

    std::string step_digest_replay;
    {
        const std::string raw = fnv1a128_file_hex(step_path);
        if (!raw.empty()) step_digest_replay = "fnv1a128:" + raw;
    }

    std::vector<OpRecord> records;
    records.reserve(recipe.ops.size());

    std::unordered_set<std::string> addressed_entries;

    bool emit_called = false;
    int exit_status = 0;

    for (const RecipeOp& op : recipe.ops) {
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
            } else if (!step_output_path.empty()) {
                CommandResult sr = export_document_step(session, doc,
                                                       step_output_path,
                                                       ReportIdMode::local);
                if (!sr.ok) exit_status = 1;
            }
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

    if (!emit_called && !output_path.empty()) {
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

    // New solids in the revised STEP that no recipe op addressed.
    std::vector<std::string> new_solids_unaddressed;
    for (const ShapeMetadata& m : snapshot.solid_metadata) {
        if (m.component_entry.empty()) continue;
        if (addressed_entries.count(m.component_entry)) continue;
        // Skip if any recipe op had a fingerprint that soft-matched this
        // entry — that's effectively addressed by the soft path.
        new_solids_unaddressed.push_back(m.shape_id + " (" + m.component_entry + ")");
    }

    write_report(report_path, recipe, step_path, step_digest_replay,
                 records, new_solids_unaddressed);

    return exit_status;
}

} // namespace csgify
