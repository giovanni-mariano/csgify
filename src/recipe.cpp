#include "recipe.hpp"

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
};

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
//   5. load_step is always first; emit_mcnp is always last.
//
// Steps deferred to step 3:
//   - Intermediate-decomposition pieces: this MVP keeps ops on pieces
//     verbatim. Replay against a revised STEP is where this starts to
//     matter; covered there.
//   - Fingerprint-based stable ordering across runs that arrive at the
//     same final model.
std::vector<Decision> compact(const std::vector<Decision>& records) {
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
        // export_step writes the simplified STEP, a side-output rather than a
        // step in the MCNP conversion pipeline. The recipe pins no STEP output
        // path; `csgify replay --out-step` is the knob for that. Dropping it
        // keeps recipe.lua, recipe.json, and replay free of an op none of them
        // can act on.
        if (d.op == "export_step") continue;
        if (d.op == "load_step" || d.op == "emit_mcnp") {
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
    bool have_load = false;
    bool have_emit = false;

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
        if (category_for(d.op) == "solid" && d.op != "remove_solid") {
            const std::string id = d.component_entry.empty()
                ? d.target_shape_id
                : d.component_entry;
            if (removed_keys.count(id)) continue;
        }
        out.push_back(std::move(d));
    }

    std::vector<Decision> ordered;
    ordered.reserve(out.size() + 2);
    if (have_load) ordered.push_back(load_step_op);
    for (Decision& d : out) ordered.push_back(std::move(d));
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
    if (d.op == "decompose_planar") {
        emit_comment(out, d);
        out << "s:decompose_planar(" << quoted_id << ")\n";
        return true;
    }
    if (d.op == "shatter") {
        int max_depth = 8;
        bool allow_drift = false;
        if (auto it = d.params.find("max_depth"); it != d.params.end()) {
            max_depth = static_cast<int>(it->second->as_number(8.0));
        }
        if (auto it = d.params.find("allow_volume_drift"); it != d.params.end()) {
            allow_drift = it->second->as_bool(false);
        }
        emit_comment(out, d);
        out << "s:shatter(" << quoted_id << ", { max_depth = " << max_depth
            << ", allow_volume_drift = " << (allow_drift ? "true" : "false")
            << " })\n";
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
        || d.op == "apply_checked_replacement_operation") {
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
        out << "s:" << method << "(" << quoted_id;
        if (d.op == "apply_checked_replacement_operation") {
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
// load_step decision (currently the overlap knobs). Returns "" when none are
// present, so ordinary recipes keep the bare s:load_step(path) form. Mirrors
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
    add_bool("allow_overlaps");
    add_bool("check_overlaps");
    add_num("overlap_volume_threshold");
    add_num("overlap_volume_fraction");
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
                      const std::vector<Decision>& ops) {
    out << "{\n";
    out << "  \"schema\": \"csgify.recipe\",\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"manifest\": {";
    out << "\"step_path\":";       emit_json_string(out, manifest.step_path);
    out << ",\"step_digest\":";    emit_json_string(out, manifest.step_digest);
    out << ",\"csgify_version\":";emit_json_string(out, manifest.csgify_version);
    out << ",\"occt_version\":";   emit_json_string(out, manifest.occt_version);
    out << "},\n";
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
    for (const Decision& d : ops) {
        if (d.target_kind == "document") {
            ok = emit_document_op(out, d, doc_var) && ok;
            continue;
        }
        if (d.target_kind == "solid") {
            ok = emit_solid_op(out, d) && ok;
            continue;
        }
        if (d.target_kind == "face") {
            ok = emit_face_op(out, d) && ok;
            continue;
        }
        std::cerr << "recipe: unknown target kind " << d.target_kind << "\n";
        ok = false;
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

    const std::vector<Decision> ops = compact(records);
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
    ok = emit_recipe_json(jout, manifest, ops) && ok;

    return ok ? 0 : 1;
}

} // namespace csgify
