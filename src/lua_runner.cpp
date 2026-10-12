#include "lua_runner.hpp"

#include "csgify/engine.hpp"
#include "csgify/session.hpp"
#include "decision_pack.hpp"
#include "recipe.hpp"
#include "replay.hpp"
#include "engine_model.hpp"
#include "operation_registry.hpp"
#include "session_impl.hpp"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#if CSGIFY_ENABLE_LUA

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace csgify {
namespace {

constexpr const char* kSessionMeta = "csgify.session";

struct LuaSession {
    SessionPtr session;

    explicit LuaSession(SessionPtr s) : session(std::move(s)) {}
};

LuaSession* check_session(lua_State* L, int index) {
    void* userdata = luaL_checkudata(L, index, kSessionMeta);
    return static_cast<LuaSession*>(userdata);
}

void reject_unknown_table_keys(lua_State* L,
                               int index,
                               std::initializer_list<const char*> allowed,
                               const char* context) {
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "%s keys must be strings", context);
        }
        const char* key = lua_tostring(L, -2);
        bool known = false;
        for (const char* candidate : allowed) {
            if (std::string(key) == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            luaL_error(L, "unknown %s option: %s", context, key);
        }
        lua_pop(L, 1);
    }
}

// Parse a ConversionOptions table. Recognized keys mirror the CLI
// flags introduced in M2 (units, scale, normalize-units) plus the
// existing engine tuning knobs. Unknown keys are rejected: audited scripts
// must not silently run with a misspelled safety or tolerance option.
ConversionOptions conversion_options_from_lua(lua_State* L, int index) {
    ConversionOptions options;
    if (lua_isnoneornil(L, index)) {
        return options;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    reject_unknown_table_keys(
        L, index,
        {"unit_override", "units", "normalize_to_unit", "normalize_units",
         "unit_scale_to_mm_override", "scale", "linear_tolerance",
         "angular_tolerance", "small_volume_fraction", "small_bbox_fraction",
         "thin_plate_ratio", "long_body_ratio", "heal_shape",
         "require_convex_cells", "analyze_topology_metrics",
         "analyze_face_metrics",
         "analyze_fitted_replacements",
         "defer_curved_halfspace_certificates", "try_planar_decomposition",
         "split_union_cells", "union_decomposed_pieces",
         "decomposition_relative_volume_tolerance",
         "shatter_relative_volume_tolerance",
         "prismatic_max_pieces", "prismatic_max_splitter_calls",
         "check_overlaps", "overlap_parallelism", "allow_overlaps",
         "overlap_volume_threshold",
         "overlap_volume_fraction", "mcnp_domain_id",
         "require_domain_coverage"},
        "conversion");

    auto take_string = [&](const char* key, std::string* out) {
        lua_getfield(L, index, key);
        if (lua_isstring(L, -1)) {
            *out = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    };
    auto take_double = [&](const char* key, double* out) {
        lua_getfield(L, index, key);
        if (lua_isnumber(L, -1)) {
            *out = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    };
    auto take_bool = [&](const char* key, bool* out) {
        lua_getfield(L, index, key);
        if (!lua_isnil(L, -1)) {
            *out = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    };
    auto take_size = [&](const char* key, std::size_t* out) {
        lua_getfield(L, index, key);
        if (lua_isinteger(L, -1)) {
            const lua_Integer value = lua_tointeger(L, -1);
            if (value >= 0) *out = static_cast<std::size_t>(value);
        }
        lua_pop(L, 1);
    };

    take_string("unit_override", &options.unit_override);
    take_string("units",        &options.unit_override);
    take_string("normalize_to_unit", &options.normalize_to_unit);
    take_string("normalize_units",   &options.normalize_to_unit);
    take_double("unit_scale_to_mm_override", &options.unit_scale_to_mm_override);
    take_double("scale",                     &options.unit_scale_to_mm_override);
    take_double("linear_tolerance",   &options.linear_tolerance);
    take_double("angular_tolerance",  &options.angular_tolerance);
    take_double("small_volume_fraction", &options.small_volume_fraction);
    take_double("small_bbox_fraction",   &options.small_bbox_fraction);
    take_double("thin_plate_ratio",      &options.thin_plate_ratio);
    take_double("long_body_ratio",       &options.long_body_ratio);
    take_bool("heal_shape",             &options.heal_shape);
    take_bool("require_convex_cells",   &options.require_convex_cells);
    take_bool("analyze_topology_metrics",
              &options.analyze_topology_metrics);
    take_bool("analyze_face_metrics", &options.analyze_face_metrics);
    take_bool("analyze_fitted_replacements",
              &options.analyze_fitted_replacements);
    take_bool("defer_curved_halfspace_certificates",
              &options.defer_curved_halfspace_certificates);
    take_bool("try_planar_decomposition", &options.try_planar_decomposition);
    take_bool("split_union_cells",      &options.split_union_cells);
    take_bool("union_decomposed_pieces", &options.union_decomposed_pieces);
    take_double("shatter_relative_volume_tolerance",
                &options.shatter_relative_volume_tolerance);
    take_double("decomposition_relative_volume_tolerance",
                &options.decomposition_relative_volume_tolerance);
    take_size("prismatic_max_pieces", &options.prismatic_max_pieces);
    take_size("prismatic_max_splitter_calls",
              &options.prismatic_max_splitter_calls);
    take_bool("check_overlaps",         &options.check_overlaps);
    take_size("overlap_parallelism",    &options.overlap_parallelism);
    take_bool("allow_overlaps",         &options.allow_overlaps);
    take_double("overlap_volume_threshold", &options.overlap_volume_threshold);
    take_double("overlap_volume_fraction",  &options.overlap_volume_fraction);
    take_string("mcnp_domain_id", &options.mcnp_domain_id);
    take_bool("require_domain_coverage", &options.require_domain_coverage);
    return options;
}

void set_string_field(lua_State* L, const char* key, const std::string& value) {
    lua_pushstring(L, value.c_str());
    lua_setfield(L, -2, key);
}

void set_number_field(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void set_integer_field(lua_State* L, const char* key, std::size_t value) {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, key);
}

void set_signed_integer_field(lua_State* L, const char* key, long long value) {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, key);
}

void set_boolean_field(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, key);
}

const char* severity_lua_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::info: return "info";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::error: return "error";
    }
    return "unknown";
}

void push_bbox_table(lua_State* L, const BBox& bbox) {
    if (!bbox.valid) {
        lua_pushnil(L);
        return;
    }
    lua_newtable(L);
    lua_newtable(L);
    lua_pushnumber(L, bbox.xmin); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, bbox.ymin); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, bbox.zmin); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "min");
    lua_newtable(L);
    lua_pushnumber(L, bbox.xmax); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, bbox.ymax); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, bbox.zmax); lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "max");
}

void push_vec3_table(lua_State* L, const Vec3& value) {
    lua_newtable(L);
    lua_pushnumber(L, value.x); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, value.y); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, value.z); lua_rawseti(L, -2, 3);
}

void push_diagnostic_table(lua_State* L, const Diagnostic& diagnostic) {
    lua_newtable(L);
    set_string_field(L, "severity", severity_lua_name(diagnostic.severity));
    set_string_field(L, "code", diagnostic.code);
    set_string_field(L, "message", diagnostic.message);
    set_string_field(L, "shape_label", diagnostic.shape_label);
    set_string_field(L, "shape_id", diagnostic.shape_id);
    set_string_field(L, "document_id", diagnostic.document_id);
    set_string_field(L, "local_shape_id", diagnostic.local_shape_id);
}

void push_geometry_certificate_table(lua_State* L,
                                     const GeometryCertificate& certificate) {
    lua_newtable(L);
    set_boolean_field(L, "attempted", certificate.attempted);
    set_boolean_field(L, "certified", certificate.certified);
    set_integer_field(L, "faces_checked", certificate.faces_checked);
    set_integer_field(L, "boolean_halfspace_checks",
                      certificate.boolean_halfspace_checks);
    set_number_field(L, "source_volume", certificate.source_volume);
    set_number_field(L, "maximum_outside_volume",
                     certificate.maximum_outside_volume);
    set_number_field(L, "allowed_outside_volume",
                     certificate.allowed_outside_volume);
    set_string_field(L, "failure_code", certificate.failure_code);
}

void push_topology_table(lua_State* L, const SolidTopology& topology) {
    lua_newtable(L);
    set_integer_field(L, "vertex_count", topology.vertex_count);
    set_integer_field(L, "edge_count", topology.edge_count);
    set_integer_field(L, "face_count", topology.face_count);
    set_integer_field(L, "shell_count", topology.shell_count);
    set_integer_field(L, "closed_shell_count", topology.closed_shell_count);
    set_integer_field(L, "cavity_shell_count", topology.cavity_shell_count);
    set_integer_field(L, "face_inner_wire_count", topology.face_inner_wire_count);
    set_integer_field(L, "manifold_edge_count", topology.manifold_edge_count);
    set_integer_field(L, "boundary_edge_count", topology.boundary_edge_count);
    set_integer_field(L, "nonmanifold_edge_count", topology.nonmanifold_edge_count);
    set_integer_field(L, "linear_edge_count", topology.linear_edge_count);
    set_integer_field(L, "planar_face_count", topology.planar_face_count);
    set_integer_field(L, "concave_edge_count", topology.concave_edge_count);
    set_integer_field(L, "convex_edge_count", topology.convex_edge_count);
    set_integer_field(L, "tangent_or_unknown_edge_count",
                      topology.tangent_or_unknown_edge_count);
    set_signed_integer_field(L, "raw_euler_characteristic",
                             topology.raw_euler_characteristic);
    set_signed_integer_field(L, "euler_characteristic",
                             topology.euler_characteristic);
    set_integer_field(L, "unique_plane_count", topology.unique_plane_count);
    set_integer_field(L, "normal_direction_count",
                      topology.normal_direction_count);
    set_number_field(L, "bbox_fill_ratio", topology.bbox_fill_ratio);
    set_boolean_field(L, "prismatic_candidate", topology.prismatic_candidate);
    push_vec3_table(L, topology.prism_axis);
    lua_setfield(L, -2, "prism_axis");
    set_integer_field(L, "axial_level_count", topology.axial_level_count);
    set_integer_field(L, "axial_end_face_count", topology.axial_end_face_count);
    set_integer_field(L, "extrusion_profile_hole_count",
                      topology.extrusion_profile_hole_count);
    set_boolean_field(L, "constant_cross_section_candidate",
                      topology.constant_cross_section_candidate);
    const PipeAnnulusTopology& pipe = topology.pipe_annulus;
    lua_newtable(L);
    set_boolean_field(L, "candidate", pipe.candidate);
    set_boolean_field(L, "conical", pipe.conical);
    push_vec3_table(L, pipe.axis_origin);
    lua_setfield(L, -2, "axis_origin");
    push_vec3_table(L, pipe.axis_direction);
    lua_setfield(L, -2, "axis_direction");
    set_number_field(L, "inner_radius", pipe.inner_radius);
    set_number_field(L, "outer_radius", pipe.outer_radius);
    set_number_field(L, "top_inner_radius", pipe.top_inner_radius);
    set_number_field(L, "top_outer_radius", pipe.top_outer_radius);
    set_number_field(L, "axial_length", pipe.axial_length);
    set_integer_field(L, "cylindrical_face_count", pipe.cylindrical_face_count);
    set_integer_field(L, "conical_face_count", pipe.conical_face_count);
    set_integer_field(L, "planar_end_face_count", pipe.planar_end_face_count);
    lua_newtable(L);
    for (std::size_t i = 0; i < pipe.ports.size(); ++i) {
        const PipePortTopology& port = pipe.ports[i];
        lua_newtable(L);
        set_string_field(L, "id", port.id);
        push_vec3_table(L, port.center);
        lua_setfield(L, -2, "center");
        push_vec3_table(L, port.normal);
        lua_setfield(L, -2, "normal");
        set_number_field(L, "radius", port.radius);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "ports");
    lua_setfield(L, -2, "pipe_annulus");
    const RevolutionTopology& revolution = topology.revolution;
    lua_newtable(L);
    set_boolean_field(L, "candidate", revolution.candidate);
    set_boolean_field(L, "full_revolution", revolution.full_revolution);
    push_vec3_table(L, revolution.axis_origin);
    lua_setfield(L, -2, "axis_origin");
    push_vec3_table(L, revolution.axis_direction);
    lua_setfield(L, -2, "axis_direction");
    set_number_field(L, "angular_extent", revolution.angular_extent);
    set_integer_field(L, "axial_level_count", revolution.axial_level_count);
    set_integer_field(L, "cylindrical_face_count",
                      revolution.cylindrical_face_count);
    set_integer_field(L, "conical_face_count", revolution.conical_face_count);
    set_integer_field(L, "toroidal_face_count",
                      revolution.toroidal_face_count);
    set_integer_field(L, "spherical_face_count",
                      revolution.spherical_face_count);
    set_integer_field(L, "radial_seam_face_count",
                      revolution.radial_seam_face_count);
    lua_setfield(L, -2, "revolution");
    lua_newtable(L);
    for (std::size_t i = 0; i < topology.recognition_candidates.size(); ++i) {
        const RecognitionCandidate& candidate = topology.recognition_candidates[i];
        lua_newtable(L);
        set_string_field(L, "kind", candidate.kind);
        set_string_field(L, "strategy", candidate.strategy);
        set_string_field(L, "confidence", candidate.confidence);
        set_number_field(L, "score", candidate.score);
        set_boolean_field(L, "exact", candidate.exact);
        lua_newtable(L);
        for (std::size_t j = 0; j < candidate.reason_codes.size(); ++j) {
            lua_pushstring(L, candidate.reason_codes[j].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
        }
        lua_setfield(L, -2, "reason_codes");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "recognition_candidates");
    set_string_field(L, "selected_recognition_kind",
                     topology.selected_recognition_kind);
    set_string_field(L, "recommended_strategy", topology.recommended_strategy);
    lua_newtable(L);
    for (std::size_t i = 0; i < topology.reason_codes.size(); ++i) {
        lua_pushstring(L, topology.reason_codes[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "reason_codes");
    lua_newtable(L);
    for (std::size_t i = 0; i < topology.shells.size(); ++i) {
        const ShellTopology& shell = topology.shells[i];
        lua_newtable(L);
        set_integer_field(L, "index", shell.index);
        set_string_field(L, "role", shell.role);
        set_integer_field(L, "vertex_count", shell.vertex_count);
        set_integer_field(L, "edge_count", shell.edge_count);
        set_integer_field(L, "face_count", shell.face_count);
        set_integer_field(L, "face_inner_wire_count",
                          shell.face_inner_wire_count);
        set_signed_integer_field(L, "raw_euler_characteristic",
                                 shell.raw_euler_characteristic);
        set_signed_integer_field(L, "euler_characteristic",
                                 shell.euler_characteristic);
        set_boolean_field(L, "closed", shell.closed);
        set_boolean_field(L, "genus_known", shell.genus_known);
        set_signed_integer_field(L, "genus", shell.genus);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "shells");
}

void push_solid_table(lua_State* L, const AnalyzedSolid& solid) {
    lua_newtable(L);
    set_string_field(L, "id", solid.id);
    set_string_field(L, "label", solid.label);
    set_string_field(L, "name", solid.name);
    set_string_field(L, "hierarchy_path", solid.hierarchy_path);
    set_integer_field(L, "index", solid.index);
    set_number_field(L, "volume", solid.volume);
    set_number_field(L, "volume_fraction", solid.volume_fraction);
    push_bbox_table(L, solid.bbox);
    lua_setfield(L, -2, "bbox");
    set_number_field(L, "bbox_diagonal", solid.bbox_diagonal);
    set_number_field(L, "bbox_diagonal_fraction", solid.bbox_diagonal_fraction);
    set_number_field(L, "bbox_min_extent", solid.bbox_min_extent);
    set_number_field(L, "bbox_mid_extent", solid.bbox_mid_extent);
    set_number_field(L, "bbox_max_extent", solid.bbox_max_extent);
    set_number_field(L, "bbox_aspect_ratio", solid.bbox_aspect_ratio);
    set_boolean_field(L, "small_volume_candidate", solid.small_volume_candidate);
    set_boolean_field(L, "small_bbox_candidate", solid.small_bbox_candidate);
    set_boolean_field(L, "thin_plate_candidate", solid.thin_plate_candidate);
    set_boolean_field(L, "long_body_candidate", solid.long_body_candidate);
    set_integer_field(L, "face_count", solid.face_count);
    set_boolean_field(L, "exact_csg_supported", solid.exact_csg_supported);
    set_boolean_field(L, "needs_decomposition", solid.needs_decomposition);
    set_boolean_field(L, "needs_spline_replacement", solid.needs_spline_replacement);
    set_boolean_field(L, "needs_surface_replacement", solid.needs_surface_replacement);
    set_boolean_field(L, "needs_user_decision", solid.needs_user_decision);
    set_boolean_field(L, "convexity_certificate_deferred",
                      solid.convexity_certificate_deferred);
    push_geometry_certificate_table(L, solid.certificate);
    lua_setfield(L, -2, "geometry_certificate");
    push_topology_table(L, solid.topology);
    lua_setfield(L, -2, "topology");

    lua_newtable(L);
    for (std::size_t i = 0; i < solid.face_ids.size(); ++i) {
        lua_pushstring(L, solid.face_ids[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "faces");

    // replacement_candidates mirrors the JSON report shape so Lua
    // scripts can decide between bbox / cylinder / sphere / cone fits
    // without re-parsing the JSON. Each metrics entry has
    // available + volume + absolute_delta + relative_delta + fill_ratio.
    auto push_metrics = [&](const char* key, const ReplacementMetrics& m) {
        lua_newtable(L);
        set_boolean_field(L, "available", m.available);
        set_number_field(L, "volume", m.volume);
        set_number_field(L, "absolute_delta", m.absolute_delta);
        set_number_field(L, "relative_delta", m.relative_delta);
        set_number_field(L, "fill_ratio", m.fill_ratio);
        lua_setfield(L, -2, key);
    };
    lua_newtable(L);
    push_metrics("bbox",           solid.bbox_replacement);
    push_metrics("bbox_cylinder",  solid.bbox_cylinder_replacement);
    push_metrics("fitted_cylinder", solid.fitted_cylinder_replacement.metrics);
    push_metrics("fitted_sphere",  solid.fitted_sphere_replacement.metrics);
    push_metrics("fitted_cone",    solid.fitted_cone_replacement.metrics);
    lua_setfield(L, -2, "replacement_candidates");
}

void push_face_table(lua_State* L, const AnalyzedFace& face) {
    lua_newtable(L);
    set_string_field(L, "id", face.id);
    set_string_field(L, "label", face.label);
    set_string_field(L, "name", face.name);
    set_string_field(L, "hierarchy_path", face.hierarchy_path);
    set_string_field(L, "solid_id", face.solid_id);
    set_string_field(L, "surface_type", face.surface_type);
    set_string_field(L, "support_kind", face.support_kind);
    set_integer_field(L, "index", face.index);
    set_number_field(L, "area", face.area);
    push_bbox_table(L, face.bbox);
    lua_setfield(L, -2, "bbox");
    set_boolean_field(L, "exact_csg_supported", face.exact_csg_supported);
    set_boolean_field(L, "needs_spline_replacement", face.needs_spline_replacement);
    set_boolean_field(L, "needs_surface_replacement", face.needs_surface_replacement);
    set_boolean_field(L, "needs_emitter", face.needs_emitter);
}

void push_decomposed_piece_table(lua_State* L,
                                  const AnalyzedDecomposedPiece& piece) {
    lua_newtable(L);
    set_string_field(L, "id", piece.id);
    set_string_field(L, "label", piece.label);
    set_string_field(L, "source_solid_id", piece.source_solid_id);
    set_integer_field(L, "index", piece.index);
    set_number_field(L, "volume", piece.volume);
    push_bbox_table(L, piece.bbox);
    lua_setfield(L, -2, "bbox");
    set_integer_field(L, "face_count", piece.face_count);
    set_boolean_field(L, "exact_csg_supported", piece.exact_csg_supported);
    set_boolean_field(L, "needs_decomposition", piece.needs_decomposition);
    set_boolean_field(L, "needs_spline_replacement", piece.needs_spline_replacement);
    set_boolean_field(L, "needs_surface_replacement", piece.needs_surface_replacement);
    set_boolean_field(L, "needs_user_decision", piece.needs_user_decision);
    push_geometry_certificate_table(L, piece.certificate);
    lua_setfield(L, -2, "geometry_certificate");
}

void push_performance_table(lua_State* L, const PerformanceMetrics& perf) {
    lua_newtable(L);
    set_number_field(L, "load_step_ms", perf.load_step_ms);
    set_number_field(L, "analyze_ms",   perf.analyze_ms);
    set_number_field(L, "propose_ms",   perf.propose_ms);
    set_number_field(L, "split_ms",     perf.split_ms);
    set_number_field(L, "emit_ms",      perf.emit_ms);
    set_number_field(L, "export_step_ms", perf.export_step_ms);
    set_integer_field(L, "candidates_evaluated", perf.candidates_evaluated);
    set_integer_field(L, "splitter_calls",       perf.splitter_calls);
    set_integer_field(L, "pieces_produced",      perf.pieces_produced);
    set_integer_field(L, "definition_recognition_cache_hits",
                      perf.definition_recognition_cache_hits);
    set_integer_field(L, "definition_recognition_cache_misses",
                      perf.definition_recognition_cache_misses);
    set_integer_field(L, "definition_conversion_cache_hits",
                      perf.definition_conversion_cache_hits);
    set_integer_field(L, "definition_conversion_cache_published",
                      perf.definition_conversion_cache_published);
}

void push_assembly_table(lua_State* L, const AssemblyNode& node) {
    lua_newtable(L);
    set_string_field(L, "id", node.id);
    set_string_field(L, "parent_id", node.parent_id);
    set_string_field(L, "name", node.name);
    set_string_field(L, "label_entry", node.label_entry);
    set_string_field(L, "shape_id", node.shape_id);
    set_string_field(L, "kind", node.kind);
    set_string_field(L, "path", node.path);
}

void push_decomposition_candidate_piece_table(
    lua_State* L,
    const DecompositionCandidatePiece& piece) {
    lua_newtable(L);
    set_integer_field(L, "index", piece.index);
    set_number_field(L, "volume", piece.volume);
    set_number_field(L, "volume_fraction", piece.volume_fraction);
    push_bbox_table(L, piece.bbox);
    lua_setfield(L, -2, "bbox");
    set_integer_field(L, "face_count", piece.face_count);
    set_boolean_field(L, "exact_csg_supported", piece.exact_csg_supported);
    set_boolean_field(L, "needs_decomposition", piece.needs_decomposition);
    set_boolean_field(L, "needs_spline_replacement", piece.needs_spline_replacement);
    set_boolean_field(L, "needs_surface_replacement", piece.needs_surface_replacement);
    set_boolean_field(L, "needs_user_decision", piece.needs_user_decision);
    set_integer_field(L, "halfspace_violation_count",
                      piece.halfspace_violation_count);
}

void push_decomposition_candidate_table(lua_State* L,
                                        const DecompositionCandidate& candidate) {
    lua_newtable(L);
    set_string_field(L, "id", candidate.id);
    set_string_field(L, "target_id", candidate.target_id);
    set_string_field(L, "source_face_id", candidate.source_face_id);
    set_string_field(L, "source_face_label", candidate.source_face_label);
    set_string_field(L, "surface_type", candidate.surface_type);
    set_string_field(L, "tool_kind", candidate.tool_kind);
    set_string_field(L, "reason", candidate.reason);
    set_boolean_field(L, "plane_valid", candidate.plane_valid);
    push_vec3_table(L, candidate.plane_origin);
    lua_setfield(L, -2, "plane_origin");
    push_vec3_table(L, candidate.plane_normal);
    lua_setfield(L, -2, "plane_normal");
    lua_newtable(L);
    for (std::size_t i = 0; i < candidate.planes.size(); ++i) {
        lua_newtable(L);
        push_vec3_table(L, candidate.planes[i].origin);
        lua_setfield(L, -2, "origin");
        push_vec3_table(L, candidate.planes[i].normal);
        lua_setfield(L, -2, "normal");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "planes");
    set_boolean_field(L, "usable", candidate.usable);
    set_string_field(L, "confidence", candidate.confidence);
    set_number_field(L, "score", candidate.score);
    set_string_field(L, "rejection_reason", candidate.rejection_reason);
    set_integer_field(L, "piece_count", candidate.piece_count);
    set_integer_field(L, "exact_piece_count", candidate.exact_piece_count);
    set_integer_field(L, "pieces_requiring_decomposition",
                      candidate.pieces_requiring_decomposition);
    set_integer_field(L, "pieces_requiring_spline_replacement",
                      candidate.pieces_requiring_spline_replacement);
    set_integer_field(L, "pieces_requiring_user_decision",
                      candidate.pieces_requiring_user_decision);
    set_integer_field(L, "sliver_piece_count", candidate.sliver_piece_count);
    set_number_field(L, "original_volume", candidate.original_volume);
    set_number_field(L, "pieces_volume", candidate.pieces_volume);
    set_number_field(L, "absolute_delta", candidate.absolute_delta);
    set_number_field(L, "relative_delta", candidate.relative_delta);
    set_number_field(L, "min_piece_volume_fraction",
                     candidate.min_piece_volume_fraction);
    set_number_field(L, "max_piece_volume_fraction",
                     candidate.max_piece_volume_fraction);
    set_number_field(L, "volume_balance", candidate.volume_balance);
    set_integer_field(L, "parent_halfspace_violation_count",
                      candidate.parent_halfspace_violation_count);
    set_integer_field(L, "max_piece_halfspace_violation_count",
                      candidate.max_piece_halfspace_violation_count);
    set_number_field(L, "progress_ratio", candidate.progress_ratio);

    lua_newtable(L);
    for (std::size_t i = 0; i < candidate.pieces.size(); ++i) {
        push_decomposition_candidate_piece_table(L, candidate.pieces[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "pieces");

    lua_newtable(L);
    for (std::size_t i = 0; i < candidate.diagnostics.size(); ++i) {
        push_diagnostic_table(L, candidate.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

void push_decomposition_proposal_table(lua_State* L,
                                       const DecompositionProposal& proposal) {
    lua_newtable(L);
    set_boolean_field(L, "ok", proposal.ok);
    set_string_field(L, "target_id", proposal.target_id);
    set_string_field(L, "best_candidate_id", proposal.best_candidate_id);
    set_string_field(L, "json", decomposition_proposal_json_string(proposal));

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        push_decomposition_candidate_table(L, proposal.candidates[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "candidates");

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        push_diagnostic_table(L, proposal.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

void push_string_array(lua_State* L, const std::vector<std::string>& values) {
    lua_newtable(L);
    for (std::size_t i = 0; i < values.size(); ++i) {
        lua_pushstring(L, values[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

void push_decision_metric_table(lua_State* L, const DecisionMetric& metric) {
    lua_newtable(L);
    set_string_field(L, "name", metric.name);
    set_number_field(L, "value", metric.value);
    set_string_field(L, "unit", metric.unit);
}

void push_decision_metrics_table(lua_State* L,
                                 const std::vector<DecisionMetric>& metrics) {
    lua_newtable(L);
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        push_decision_metric_table(L, metrics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

void push_decision_candidate_view_table(lua_State* L,
                                        const DecisionCandidateView& candidate) {
    lua_newtable(L);
    set_string_field(L, "id", candidate.id);
    set_string_field(L, "action", candidate.action);
    set_string_field(L, "confidence", candidate.confidence);
    set_string_field(L, "reason", candidate.reason);
    push_decision_metrics_table(L, candidate.metrics);
    lua_setfield(L, -2, "metrics");
}

void push_decision_request_table(lua_State* L,
                                 const DecisionRequest& request) {
    lua_newtable(L);
    set_string_field(L, "id", request.id);
    set_string_field(L, "kind", request.kind);
    set_string_field(L, "target_id", request.target_id);
    set_string_field(L, "reason", request.reason);
    set_string_field(L, "recommended_action", request.recommended_action);
    set_string_field(L, "summary", request.summary);
    push_decision_metrics_table(L, request.metrics);
    lua_setfield(L, -2, "metrics");

    lua_newtable(L);
    for (std::size_t i = 0; i < request.candidates.size(); ++i) {
        push_decision_candidate_view_table(L, request.candidates[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "candidates");
}

void push_removal_candidate_table(lua_State* L,
                                  const RemovalCandidate& candidate) {
    lua_newtable(L);
    set_string_field(L, "id", candidate.id);
    set_string_field(L, "solid_id", candidate.solid_id);
    set_string_field(L, "component_instance_id",
                     candidate.component_instance_id);
    set_string_field(L, "component_definition_id",
                     candidate.component_definition_id);
    set_string_field(L, "label", candidate.label);
    set_string_field(L, "name", candidate.name);
    set_string_field(L, "hierarchy_path", candidate.hierarchy_path);
    set_number_field(L, "volume", candidate.volume);
    set_number_field(L, "volume_fraction", candidate.volume_fraction);
    push_bbox_table(L, candidate.bbox);
    lua_setfield(L, -2, "bbox");
    set_number_field(L, "bbox_diagonal_fraction",
                     candidate.bbox_diagonal_fraction);
    push_string_array(L, candidate.reason_codes);
    lua_setfield(L, -2, "reason_codes");
    push_string_array(L, candidate.blockers);
    lua_setfield(L, -2, "blockers");
    set_string_field(L, "confidence", candidate.confidence);
    set_number_field(L, "score", candidate.score);
    set_string_field(L, "suggested_action", candidate.suggested_action);
}

void push_removal_proposal_table(lua_State* L,
                                 const RemovalProposal& proposal) {
    lua_newtable(L);
    set_boolean_field(L, "ok", proposal.ok);
    set_number_field(L, "total_volume", proposal.total_volume);
    set_integer_field(L, "high_confidence_count",
                      proposal.high_confidence_count);
    set_number_field(L, "high_confidence_volume",
                     proposal.high_confidence_volume);
    set_number_field(L, "high_confidence_volume_fraction",
                     proposal.high_confidence_volume_fraction);
    set_string_field(L, "json", removal_proposal_json_string(proposal));

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        push_removal_candidate_table(L, proposal.candidates[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "candidates");

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        push_diagnostic_table(L, proposal.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

const char* hole_fill_kind_lua_name(HoleFillKind kind) {
    switch (kind) {
    case HoleFillKind::cylindrical_through: return "cylindrical_through";
    case HoleFillKind::cylindrical_blind: return "cylindrical_blind";
    case HoleFillKind::coaxial_stack: return "coaxial_stack";
    case HoleFillKind::planar_wire_patch: return "planar_wire_patch";
    case HoleFillKind::unknown: return "unknown";
    }
    return "unknown";
}

void push_hole_fill_candidate_table(lua_State* L,
                                    const HoleFillCandidate& candidate) {
    lua_newtable(L);
    set_string_field(L, "id", candidate.id);
    set_string_field(L, "host_solid_id", candidate.host_solid_id);
    push_string_array(L, candidate.seed_removed_solid_ids);
    lua_setfield(L, -2, "seed_removed_solid_ids");
    push_string_array(L, candidate.face_ids);
    lua_setfield(L, -2, "face_ids");
    set_string_field(L, "kind", hole_fill_kind_lua_name(candidate.kind));
    push_vec3_table(L, candidate.axis_origin);
    lua_setfield(L, -2, "axis_origin");
    push_vec3_table(L, candidate.axis_direction);
    lua_setfield(L, -2, "axis_direction");
    push_vec3_table(L, candidate.base);
    lua_setfield(L, -2, "base");
    push_vec3_table(L, candidate.height);
    lua_setfield(L, -2, "height");
    set_number_field(L, "radius", candidate.radius);
    set_number_field(L, "top_radius", candidate.top_radius);
    set_number_field(L, "length", candidate.length);
    set_number_field(L, "plug_volume", candidate.plug_volume);
    set_number_field(L, "host_volume", candidate.host_volume);
    set_number_field(L, "plug_volume_fraction_of_host",
                     candidate.plug_volume_fraction_of_host);
    set_number_field(L, "estimated_host_overlap_fraction",
                     candidate.estimated_host_overlap_fraction);
    set_number_field(L, "estimated_other_overlap_fraction",
                     candidate.estimated_other_overlap_fraction);
    set_string_field(L, "confidence", candidate.confidence);
    set_number_field(L, "score", candidate.score);
    push_string_array(L, candidate.reason_codes);
    lua_setfield(L, -2, "reason_codes");
    push_string_array(L, candidate.blockers);
    lua_setfield(L, -2, "blockers");
    set_string_field(L, "suggested_action", candidate.suggested_action);
}

void push_hole_fill_proposal_table(lua_State* L,
                                   const HoleFillProposal& proposal) {
    lua_newtable(L);
    set_boolean_field(L, "ok", proposal.ok);
    set_string_field(L, "json", hole_fill_proposal_json_string(proposal));

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        push_hole_fill_candidate_table(L, proposal.candidates[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "candidates");

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.decision_requests.size(); ++i) {
        push_decision_request_table(L, proposal.decision_requests[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "decision_requests");

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        push_diagnostic_table(L, proposal.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

MaterialPhase material_phase_from_string(const std::string& value) {
    if (value == "solid") return MaterialPhase::solid;
    if (value == "liquid") return MaterialPhase::liquid;
    if (value == "gas") return MaterialPhase::gas;
    if (value == "vacuum") return MaterialPhase::vacuum;
    return MaterialPhase::unknown;
}

MediumPortDisposition medium_disposition_from_string(
    const std::string& value) {
    if (value == "cap_here") return MediumPortDisposition::cap_here;
    if (value == "interface_to_external_model")
        return MediumPortDisposition::interface_to_external_model;
    if (value == "open_to_domain")
        return MediumPortDisposition::open_to_domain;
    if (value == "connect_to_port")
        return MediumPortDisposition::connect_to_port;
    return MediumPortDisposition::unresolved;
}

MediumValveState medium_valve_from_string(const std::string& value) {
    if (value == "open") return MediumValveState::open;
    if (value == "closed") return MediumValveState::closed;
    if (value == "unresolved") return MediumValveState::unresolved;
    return MediumValveState::not_applicable;
}

MediumGenerationRequest medium_request_from_lua(lua_State* L, int index) {
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    MediumGenerationRequest request;
    auto string_field = [&](const char* key) {
        lua_getfield(L, index, key);
        const std::string value = lua_isstring(L, -1)
            ? lua_tostring(L, -1) : std::string{};
        lua_pop(L, 1);
        return value;
    };
    request.id = string_field("id");
    request.material_id = string_field("material_id");
    request.complement_material_id = string_field("complement_material_id");
    const std::string fill_mode = string_field("fill_mode");
    request.fill_mode = fill_mode == "level" ? MediumFillMode::level
        : fill_mode == "bounded_segment" ? MediumFillMode::bounded_segment
                                           : MediumFillMode::full;
    lua_getfield(L, index, "gravity_direction");
    if (lua_istable(L, -1) && lua_rawlen(L, -1) == 3) {
        for (lua_Integer i = 1; i <= 3; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isnumber(L, -1))
                request.gravity_direction[static_cast<std::size_t>(i - 1)] =
                    lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "fill_level");
    if (lua_isnumber(L, -1)) request.fill_level = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "target_volume_fraction");
    if (lua_isnumber(L, -1))
        request.target_volume_fraction = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "host_component_ids");
    if (lua_istable(L, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1))
                request.host_component_ids.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "ports");
    if (lua_istable(L, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                MediumPortDecision port;
                lua_getfield(L, -1, "port_id");
                if (lua_isstring(L, -1)) port.port_id = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "disposition");
                if (lua_isstring(L, -1))
                    port.disposition = medium_disposition_from_string(
                        lua_tostring(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "peer_port_id");
                if (lua_isstring(L, -1))
                    port.peer_port_id = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "valve_state");
                if (lua_isstring(L, -1))
                    port.valve_state = medium_valve_from_string(
                        lua_tostring(L, -1));
                lua_pop(L, 1);
                request.ports.push_back(std::move(port));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "seed_points_or_faces");
    if (lua_istable(L, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                GeometricSeed seed;
                lua_getfield(L, -1, "kind");
                if (lua_isstring(L, -1)
                    && std::string(lua_tostring(L, -1)) == "face")
                    seed.kind = GeometricSeedKind::face;
                lua_pop(L, 1);
                lua_getfield(L, -1, "entity_id");
                if (lua_isstring(L, -1))
                    seed.entity_id = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "point");
                if (lua_istable(L, -1) && lua_rawlen(L, -1) == 3) {
                    for (lua_Integer coordinate = 1; coordinate <= 3;
                         ++coordinate) {
                        lua_rawgeti(L, -1, coordinate);
                        if (lua_isnumber(L, -1))
                            seed.point[static_cast<std::size_t>(coordinate - 1)] =
                                lua_tonumber(L, -1);
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
                request.seed_points_or_faces.push_back(std::move(seed));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "internal_components_to_subtract");
    if (lua_istable(L, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1))
                request.internal_components_to_subtract.emplace_back(
                    lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "allow_disconnected_pockets");
    if (lua_isboolean(L, -1))
        request.allow_disconnected_pockets = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "max_worker_count");
    if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0)
        request.max_worker_count = static_cast<std::size_t>(
            lua_tointeger(L, -1));
    lua_pop(L, 1);
    return request;
}

void push_medium_generation_proposal_table(
    lua_State* L,
    const MediumGenerationProposal& proposal) {
    lua_newtable(L);
    set_boolean_field(L, "ok", proposal.ok);
    set_boolean_field(L, "conical", proposal.conical);
    set_string_field(L, "host_shape_id", proposal.host_shape_id);
    set_string_field(L, "host_component_id", proposal.host_component_id);
    push_vec3_table(L, proposal.base);
    lua_setfield(L, -2, "base");
    push_vec3_table(L, proposal.height);
    lua_setfield(L, -2, "height");
    set_number_field(L, "radius", proposal.radius);
    set_number_field(L, "top_radius", proposal.top_radius);
    lua_newtable(L);
    set_boolean_field(L, "solid_valid", proposal.certificate.solid_valid);
    set_boolean_field(L, "closed", proposal.certificate.closed);
    set_boolean_field(L, "leak_test_passed",
                      proposal.certificate.leak_test_passed);
    set_boolean_field(L, "overlap_test_passed",
                      proposal.certificate.overlap_test_passed);
    set_integer_field(L, "unresolved_port_count",
                      proposal.certificate.unresolved_port_count);
    set_number_field(L, "generated_volume",
                     proposal.certificate.generated_volume);
    set_integer_field(L, "network_segment_count",
                      proposal.certificate.network_segment_count);
    set_integer_field(L, "network_connection_count",
                      proposal.certificate.network_connection_count);
    set_integer_field(L, "network_connected_component_count",
                      proposal.certificate.network_connected_component_count);
    set_boolean_field(L, "obstacle_subtraction_passed",
                      proposal.certificate.obstacle_subtraction_passed);
    set_boolean_field(L, "phase_coverage_passed",
                      proposal.certificate.phase_coverage_passed);
    set_boolean_field(L, "protected_interface_passed",
                      proposal.certificate.protected_interface_passed);
    set_number_field(L, "liquid_volume",
                     proposal.certificate.liquid_volume);
    set_number_field(L, "gas_volume", proposal.certificate.gas_volume);
    set_number_field(L, "phase_coverage_delta",
                     proposal.certificate.phase_coverage_delta);
    set_number_field(L, "phase_overlap_volume",
                     proposal.certificate.phase_overlap_volume);
    set_integer_field(L, "liquid_pocket_count",
                      proposal.certificate.liquid_pocket_count);
    set_integer_field(L, "gas_pocket_count",
                      proposal.certificate.gas_pocket_count);
    set_integer_field(L, "worker_count_used",
                      proposal.certificate.worker_count_used);
    set_number_field(L, "material_overlap_volume",
                     proposal.certificate.material_overlap_volume);
    lua_setfield(L, -2, "certificate");
    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.decision_requests.size(); ++i) {
        push_decision_request_table(L, proposal.decision_requests[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "decision_requests");
    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        push_diagnostic_table(L, proposal.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

void push_surface_replacement_candidate_table(
    lua_State* L,
    const SurfaceReplacementCandidate& candidate) {
    lua_newtable(L);
    set_string_field(L, "id", candidate.id);
    set_string_field(L, "face_id", candidate.face_id);
    set_string_field(L, "owner_target_id", candidate.owner_target_id);
    set_string_field(L, "kind", candidate.kind);
    set_string_field(L, "action", candidate.action);
    set_string_field(L, "confidence", candidate.confidence);
    set_string_field(L, "reason", candidate.reason);
    set_boolean_field(L, "exact", candidate.exact);
    set_boolean_field(L, "deviation_certified",
                      candidate.deviation_certified);
    set_boolean_field(L, "usable", candidate.usable);
    set_number_field(L, "score", candidate.score);
    set_number_field(L, "max_deviation", candidate.max_deviation);
    set_number_field(L, "rms_deviation", candidate.rms_deviation);
    set_number_field(L, "relative_max_deviation",
                     candidate.relative_max_deviation);
    set_number_field(L, "owner_relative_volume_delta",
                     candidate.owner_relative_volume_delta);
    set_integer_field(L, "segment_count", candidate.segment_count);
    push_vec3_table(L, candidate.origin);
    lua_setfield(L, -2, "origin");
    push_vec3_table(L, candidate.direction);
    lua_setfield(L, -2, "direction");
    set_number_field(L, "radius", candidate.radius);
    set_number_field(L, "half_angle", candidate.half_angle);

    lua_newtable(L);
    for (std::size_t i = 0; i < candidate.diagnostics.size(); ++i) {
        push_diagnostic_table(L, candidate.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

void push_surface_replacement_proposal_table(
    lua_State* L,
    const SurfaceReplacementProposal& proposal) {
    lua_newtable(L);
    set_boolean_field(L, "ok", proposal.ok);
    set_string_field(L, "face_id", proposal.face_id);
    set_string_field(L, "owner_target_id", proposal.owner_target_id);
    set_string_field(L, "support_kind", proposal.support_kind);
    set_string_field(L, "best_candidate_id", proposal.best_candidate_id);
    set_string_field(L, "json",
                     surface_replacement_proposal_json_string(proposal));

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        push_surface_replacement_candidate_table(L, proposal.candidates[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "candidates");

    lua_newtable(L);
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        push_diagnostic_table(L, proposal.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");
}

void push_report_table(lua_State* L, const ConversionReport& report) {
    lua_newtable(L);
    set_boolean_field(L, "ok", report.ok());
    set_integer_field(L, "schema_version", report.schema_version);
    set_integer_field(L, "document_revision", report.document_revision);
    set_string_field(L, "document_id", report.document_id);
    set_string_field(L, "source_path", report.source_path);
    set_number_field(L, "total_volume", report.total_volume);
    set_integer_field(L, "solids_seen", report.solids_seen);
    set_integer_field(L, "solids_converted", report.solids_converted);
    set_integer_field(L, "solids_requiring_decomposition",
                      report.solids_requiring_decomposition);
    set_integer_field(L, "decomposition_attempts", report.decomposition_attempts);
    set_integer_field(L, "decomposition_pieces", report.decomposition_pieces);
    set_integer_field(L, "faces_seen", report.faces_seen);
    set_integer_field(L, "analytic_faces", report.analytic_faces);
    set_integer_field(L, "spline_faces", report.spline_faces);
    set_string_field(L, "json", report_json_string(report));

    // Units block (M2 addition). length_unit_to_mm is the factor that
    // converts a value in length_unit to millimetres. Defaults to
    // ("", 1.0) when the STEP did not declare a unit.
    lua_newtable(L);
    set_string_field(L, "length_unit", report.length_unit);
    set_number_field(L, "length_unit_to_mm", report.length_unit_to_mm);
    lua_setfield(L, -2, "units");

    push_performance_table(L, report.performance);
    lua_setfield(L, -2, "performance");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
        push_diagnostic_table(L, report.diagnostics[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "diagnostics");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.solids.size(); ++i) {
        push_solid_table(L, report.solids[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "solids");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.faces.size(); ++i) {
        push_face_table(L, report.faces[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "faces");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.assembly.size(); ++i) {
        push_assembly_table(L, report.assembly[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "assembly");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.decomposed_pieces.size(); ++i) {
        push_decomposed_piece_table(L, report.decomposed_pieces[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "decomposed_pieces");

    // Overlap pairs (interpenetrating solids). Empty when none were
    // detected, or when options.check_overlaps is false.
    lua_newtable(L);
    for (std::size_t i = 0; i < report.overlaps.size(); ++i) {
        const SolidOverlap& overlap = report.overlaps[i];
        lua_newtable(L);
        set_string_field(L, "solid_a_id", overlap.solid_a_id);
        set_string_field(L, "solid_b_id", overlap.solid_b_id);
        set_number_field(L, "volume_a", overlap.volume_a);
        set_number_field(L, "volume_b", overlap.volume_b);
        set_number_field(L, "intersection_volume", overlap.intersection_volume);
        set_number_field(L, "min_fraction", overlap.min_fraction);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "overlaps");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.provenance.size(); ++i) {
        const ProvenanceRecord& record = report.provenance[i];
        lua_newtable(L);
        set_string_field(L, "entity_id", record.entity_id);
        set_string_field(L, "operation", record.operation);
        set_boolean_field(L, "exact", record.exact);
        set_boolean_field(L, "approximation", record.approximation);
        set_boolean_field(L, "has_tolerance_budget",
                          record.has_tolerance_budget);
        set_number_field(L, "tolerance_budget", record.tolerance_budget);
        lua_newtable(L);
        for (std::size_t j = 0; j < record.source_ids.size(); ++j) {
            lua_pushstring(L, record.source_ids[j].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
        }
        lua_setfield(L, -2, "source_ids");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "provenance");

    lua_newtable(L);
    for (std::size_t i = 0; i < report.decision_requests.size(); ++i) {
        push_decision_request_table(L, report.decision_requests[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "decision_requests");
}

// All command-result bindings return a structured table (success) or
// (nil, table) on failure. The table carries `ok`, `diagnostics`,
// per-entity arrays AND a `json` field with the canonical JSON for
// callers that prefer the JSON shape. Previously two variants existed
// (a JSON-string returner and a table returner) which produced
// inconsistent return shapes across the API; both are now the same.
int push_command_result_table(lua_State* L, const CommandResult& result) {
    if (!result.ok) {
        lua_pushnil(L);
        push_report_table(L, result.report);
        return 2;
    }
    push_report_table(L, result.report);
    return 1;
}

int push_command_result(lua_State* L, const CommandResult& result) {
    return push_command_result_table(L, result);
}

// Push a ReplacementCommandResult. Shape is identical to
// push_command_result_table but also attaches a `check` field on the
// returned report table so callers can inspect the volume-guard
// outcome (ok flag, metrics, diagnostic) without re-parsing
// diagnostics.
int push_replacement_command_result(lua_State* L,
                                    const ReplacementCommandResult& result) {
    const int nret = push_command_result_table(L, result.command);
    // Top of stack is either the success table or, for (nil, table),
    // the table at -1. Either way, augment the table at -1 with the
    // check info.
    lua_newtable(L);
    set_boolean_field(L, "ok", result.check.ok);
    // metrics
    lua_newtable(L);
    set_boolean_field(L, "available", result.check.metrics.available);
    set_number_field(L, "volume", result.check.metrics.volume);
    set_number_field(L, "absolute_delta", result.check.metrics.absolute_delta);
    set_number_field(L, "relative_delta", result.check.metrics.relative_delta);
    set_number_field(L, "fill_ratio", result.check.metrics.fill_ratio);
    lua_setfield(L, -2, "metrics");
    // diagnostic (may be empty)
    push_diagnostic_table(L, result.check.diagnostic);
    lua_setfield(L, -2, "diagnostic");
    lua_setfield(L, -2, "check");
    return nret;
}

SplineFitKind fit_kind_from_lua(lua_State* L, int index) {
    const std::string kind = luaL_checkstring(L, index);
    if (kind == "plane") return SplineFitKind::plane;
    if (kind == "sphere") return SplineFitKind::sphere;
    if (kind == "cylinder") return SplineFitKind::cylinder;
    if (kind == "cone") return SplineFitKind::cone;
    luaL_error(L, "unknown fit kind: %s", kind.c_str());
    return SplineFitKind::none;
}

ReplacementOperation replacement_operation_from_lua(lua_State* L, int index) {
    const std::string op = luaL_checkstring(L, index);
    const OperationDescriptor* descriptor = find_replacement_by_lua_value(op);
    if (descriptor) return descriptor->replacement_operation;
    luaL_error(L, "unknown replacement operation: %s", op.c_str());
    return ReplacementOperation::bbox;
}

ReplacementLimits replacement_limits_from_lua(lua_State* L, int index) {
    ReplacementLimits limits;
    if (!lua_istable(L, index)) {
        return limits;
    }
    lua_getfield(L, index, "max_abs_delta");
    if (!lua_isnil(L, -1)) {
        limits.has_absolute_delta = true;
        limits.max_absolute_delta = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "max_rel_delta");
    if (!lua_isnil(L, -1)) {
        limits.has_relative_delta = true;
        limits.max_relative_delta = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);
    return limits;
}

bool lua_table_has_field(lua_State* L, int index, const char* key) {
    lua_getfield(L, index, key);
    const bool present = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return present;
}

double lua_optional_number(lua_State* L, int index, const char* key,
                           double fallback) {
    lua_getfield(L, index, key);
    const double value = lua_isnil(L, -1) ? fallback
                                          : luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}

bool lua_optional_bool(lua_State* L, int index, const char* key,
                       bool fallback) {
    lua_getfield(L, index, key);
    const bool value = lua_isnil(L, -1) ? fallback
                                        : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}

ApproximationPolicy approximation_policy_from_lua(lua_State* L, int index) {
    ApproximationPolicy p;
    p.enabled = true;
    p.max_global_changed_volume_fraction =
        lua_optional_number(L, index, "approx_global_limit", 0.0);
    p.max_body_changed_volume_fraction =
        lua_optional_number(L, index, "approx_body_limit",
                            p.max_global_changed_volume_fraction);
    p.max_action_changed_volume_fraction =
        lua_optional_number(L, index, "approx_action_limit",
                            p.max_body_changed_volume_fraction);
    p.max_global_added_volume_fraction =
        lua_optional_number(L, index, "approx_global_added_limit",
                            p.max_global_changed_volume_fraction);
    p.max_global_removed_volume_fraction =
        lua_optional_number(L, index, "approx_global_removed_limit",
                            p.max_global_changed_volume_fraction);
    p.max_body_added_volume_fraction =
        lua_optional_number(L, index, "approx_body_added_limit",
                            p.max_body_changed_volume_fraction);
    p.max_body_removed_volume_fraction =
        lua_optional_number(L, index, "approx_body_removed_limit",
                            p.max_body_changed_volume_fraction);
    p.max_surface_deviation =
        lua_optional_number(L, index, "approx_surface_deviation_limit", 0.0);
    p.allow_surface_replacement =
        lua_optional_bool(L, index, "allow_surface_replacement", false);
    p.allow_bbox_replacement =
        lua_optional_bool(L, index, "allow_bbox_replacement", false);
    p.allow_sliver_removal =
        lua_optional_bool(L, index, "allow_sliver_removal", false);
    p.allow_numerical_shatter_loss =
        lua_optional_bool(L, index, "allow_numerical_shatter_loss", false);
    p.allow_overlap_resolution =
        lua_optional_bool(L, index, "allow_overlap_resolution", false);
    p.require_containment_preservation =
        lua_optional_bool(L, index, "require_containment_preservation", true);
    p.require_topology_preservation =
        lua_optional_bool(L, index, "require_topology_preservation", true);
    lua_getfield(L, index, "protected_target_ids");
    if (lua_istable(L, -1)) {
        const std::size_t count = lua_rawlen(L, -1);
        for (std::size_t i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            p.protected_target_ids.emplace_back(luaL_checkstring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, index, "protected_material_ids");
    if (lua_istable(L, -1)) {
        const std::size_t count = lua_rawlen(L, -1);
        for (std::size_t i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            p.protected_material_ids.push_back(
                static_cast<int>(luaL_checkinteger(L, -1)));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return p;
}

HoleFillOptions hole_fill_options_from_lua(lua_State* L, int index) {
    HoleFillOptions options;
    if (!lua_istable(L, index)) {
        return options;
    }
    lua_getfield(L, index, "host");
    if (!lua_isnil(L, -1)) {
        options.host_solid_id = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "host_solid_id");
    if (!lua_isnil(L, -1)) {
        options.host_solid_id = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "near_removed");
    if (!lua_isnil(L, -1)) {
        options.near_removed_solid_id = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "near_removed_solid_id");
    if (!lua_isnil(L, -1)) {
        options.near_removed_solid_id = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "max_radius");
    if (!lua_isnil(L, -1)) {
        options.has_max_radius = true;
        options.max_radius = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "max_volume_fraction");
    if (!lua_isnil(L, -1)) {
        options.has_max_volume_fraction = true;
        options.max_volume_fraction = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "max_candidates");
    if (!lua_isnil(L, -1)) {
        const lua_Integer n = luaL_checkinteger(L, -1);
        if (n > 0) {
            options.max_candidates = static_cast<std::size_t>(n);
        }
    }
    lua_pop(L, 1);
    return options;
}

bool optional_deviation_from_lua(lua_State* L,
                                 int index,
                                 bool* has_max_deviation,
                                 double* max_deviation) {
    *has_max_deviation = false;
    *max_deviation = 0.0;
    if (lua_gettop(L) < index || lua_isnil(L, index)) {
        return true;
    }
    if (lua_isnumber(L, index)) {
        *has_max_deviation = true;
        *max_deviation = luaL_checknumber(L, index);
        return true;
    }
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "max_deviation");
        if (!lua_isnil(L, -1)) {
            *has_max_deviation = true;
            *max_deviation = luaL_checknumber(L, -1);
        }
        lua_pop(L, 1);
        return true;
    }
    return false;
}

int l_session_gc(lua_State* L) {
    LuaSession* session = check_session(L, 1);
    session->~LuaSession();
    return 0;
}

int l_new_session(lua_State* L) {
    // Parse options and create the C++ session BEFORE allocating Lua
    // userdata. If either step throws, Lua sees a normal error path
    // and no half-constructed userdata can survive without its
    // destructor running.
    const ConversionOptions options = lua_gettop(L) >= 1
                                          ? conversion_options_from_lua(L, 1)
                                          : ConversionOptions{};
    SessionPtr session = create_session(options);
    void* userdata = lua_newuserdata(L, sizeof(LuaSession));
    new (userdata) LuaSession(std::move(session));
    luaL_getmetatable(L, kSessionMeta);
    lua_setmetatable(L, -2);
    return 1;
}

int l_load_step(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* path = luaL_checkstring(L, 2);
    // Optional 3rd argument: a ConversionOptions table to override the
    // session defaults for this document only (per the C++
    // `options_override` parameter on load_step).
    DocumentId id;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        const ConversionOptions override = conversion_options_from_lua(L, 3);
        id = load_step(*s->session, path, &override);
    } else {
        id = load_step(*s->session, path);
    }
    lua_pushstring(L, id.c_str());
    return 1;
}

// s:attach_decision_log(path[, step_hint])
// Opens an append-only decision log. Throws a Lua error on failure.
// Optional second argument is the STEP path hint stored in the log
// manifest (defaults to empty -- the next load_step recording is
// used as the hint for downstream replay).
int l_attach_decision_log(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* path = luaL_checkstring(L, 2);
    const char* hint = lua_gettop(L) >= 3 && !lua_isnil(L, 3)
                           ? luaL_checkstring(L, 3)
                           : "";
    try {
        attach_decision_log(*s->session, path, hint);
    } catch (const std::exception& e) {
        return luaL_error(L,
                          "attach_decision_log failed: %s",
                          e.what());
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_analyze(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    return push_command_result(L, analyze(*s->session, document_id));
}

int l_analyze_table(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    return push_command_result_table(L, analyze(*s->session, document_id));
}

int l_document_revision(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(
        document_revision(*s->session, document_id)));
    return 1;
}

int l_undo_last_mutation(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    return push_command_result(L,
                               undo_last_mutation(*s->session, document_id));
}

int l_suggest_removals(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const RemovalProposal proposal = suggest_removals(*s->session, document_id);
    push_removal_proposal_table(L, proposal);
    return 1;
}

int l_propose_hole_fills(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    HoleFillOptions options;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        options = hole_fill_options_from_lua(L, 3);
    }
    const HoleFillProposal proposal =
        propose_hole_fills(*s->session, document_id, options);
    push_hole_fill_proposal_table(L, proposal);
    return 1;
}

int l_define_material(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    MaterialDefinition material;
    lua_getfield(L, 3, "id");
    if (lua_isstring(L, -1)) material.id = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 3, "phase");
    if (lua_isstring(L, -1))
        material.phase = material_phase_from_string(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 3, "density");
    if (lua_isnumber(L, -1)) material.density = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return push_command_result_table(
        L, define_material(*s->session, document_id, material));
}

int l_propose_contained_medium(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const MediumGenerationRequest request = medium_request_from_lua(L, 3);
    push_medium_generation_proposal_table(
        L, propose_contained_medium(*s->session, document_id, request));
    return 1;
}

int l_stage_contained_medium_request(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const MediumGenerationRequest request = medium_request_from_lua(L, 3);
    return push_command_result_table(L, stage_contained_medium_request(
        *s->session, document_id, request));
}

int l_apply_contained_medium(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const MediumGenerationRequest request = medium_request_from_lua(L, 3);
    lua_getfield(L, 3, "user_approved");
    const bool approved = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return push_command_result_table(L, apply_contained_medium(
        *s->session, document_id, request, approved));
}

int l_fill_hole(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const char* candidate_id = luaL_checkstring(L, 3);
    return push_command_result_table(
        L,
        fill_hole(*s->session, document_id, candidate_id));
}

int l_remove_solid(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)
        && lua_table_has_field(L, 3, "approx_global_limit")) {
        ApproximationPolicy policy = approximation_policy_from_lua(L, 3);
        policy.allow_sliver_removal = true;
        const ApproximationCommandResult result =
            remove_residual_with_approximation(*s->session, shape_id, policy);
        return push_command_result(L, result.command);
    }
    return push_command_result(L, remove_solid(*s->session, shape_id));
}

int l_remove_solids(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const lua_Integer count = luaL_len(L, 2);
    std::vector<std::string> shape_ids;
    shape_ids.reserve(static_cast<std::size_t>(count));
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, i);
        shape_ids.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    return push_command_result(L, remove_solids(*s->session, shape_ids));
}

int l_resolve_overlap(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* loser_id = luaL_checkstring(L, 2);
    const char* owner_id = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    ApproximationPolicy policy = approximation_policy_from_lua(L, 4);
    policy.allow_overlap_resolution = true;
    const ApproximationCommandResult result =
        resolve_overlap_with_approximation(*s->session, loser_id, owner_id,
                                           policy);
    return push_command_result(L, result.command);
}

int l_replace_solid(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    const ReplacementOperation op = replacement_operation_from_lua(L, 3);

    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        if (lua_table_has_field(L, 4, "approx_global_limit")) {
            ApproximationPolicy policy = approximation_policy_from_lua(L, 4);
            const ApproximationCommandResult result =
                apply_budgeted_replacement_operation(*s->session, shape_id,
                                                       op, policy);
            return push_command_result(L, result.command);
        }
        const ReplacementLimits limits = replacement_limits_from_lua(L, 4);
        ReplacementCommandResult result =
            apply_checked_replacement_operation(*s->session, shape_id, op, limits);
        return push_replacement_command_result(L, result);
    }

    return push_command_result(L, apply_replacement_operation(*s->session, shape_id, op));
}

int l_replace_solid_with_bbox(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, replace_solid_with_bbox(*s->session, shape_id));
}

int l_replace_solid_with_bbox_cylinder(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, replace_solid_with_bbox_cylinder(*s->session, shape_id));
}

int l_replace_solid_with_fitted_cylinder(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        const ReplacementLimits limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_cylinder,
            limits);
        return push_replacement_command_result(L, result);
    }
    return push_command_result(L, replace_solid_with_fitted_cylinder(*s->session, shape_id));
}

int l_replace_solid_with_fitted_sphere(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        const ReplacementLimits limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_sphere,
            limits);
        return push_replacement_command_result(L, result);
    }
    return push_command_result(L, replace_solid_with_fitted_sphere(*s->session, shape_id));
}

int l_replace_solid_with_fitted_cone(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        const ReplacementLimits limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_cone,
            limits);
        return push_replacement_command_result(L, result);
    }
    return push_command_result(L, replace_solid_with_fitted_cone(*s->session, shape_id));
}

int l_replace_face_with_fit(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    const SplineFitKind kind = fit_kind_from_lua(L, 3);
    bool has_max_deviation = false;
    double max_deviation = 0.0;
    if (!optional_deviation_from_lua(L, 4, &has_max_deviation, &max_deviation)) {
        return luaL_error(L, "replace_face_with_fit expects a number or { max_deviation = number }");
    }
    return push_command_result(
        L,
        replace_face_with_fit(*s->session,
                              shape_id,
                              kind,
                              has_max_deviation,
                              max_deviation));
}

int l_propose_surface_replacement(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* face_id = luaL_checkstring(L, 2);
    const SurfaceReplacementProposal proposal =
        propose_surface_replacement(*s->session, face_id);
    push_surface_replacement_proposal_table(L, proposal);
    return 1;
}

int l_apply_surface_replacement_candidate(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* face_id = luaL_checkstring(L, 2);
    const char* candidate_id = luaL_checkstring(L, 3);
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)
        && lua_table_has_field(L, 4, "approx_global_limit")) {
        ApproximationPolicy policy = approximation_policy_from_lua(L, 4);
        policy.allow_surface_replacement = true;
        const ApproximationCommandResult result =
            apply_budgeted_surface_replacement_candidate(
                *s->session, face_id, candidate_id, policy);
        return push_command_result(L, result.command);
    }
    return push_command_result(L, apply_surface_replacement_candidate(
        *s->session, face_id, candidate_id));
}

int l_decompose_planar(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, decompose_planar(*s->session, shape_id));
}

int l_decompose_prismatic(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, decompose_prismatic(*s->session, shape_id));
}

int l_decompose_revolution(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, decompose_revolution(*s->session, shape_id));
}

int l_reuse_definition_conversion(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* target_id = luaL_checkstring(L, 2);
    const char* source_id = luaL_checkstring(L, 3);
    return push_command_result(L, reuse_definition_conversion(
        *s->session, target_id, source_id));
}

int l_decompose_shell_difference(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(
        L, decompose_shell_difference(*s->session, shape_id));
}

int l_decompose_pipe_annulus(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, decompose_pipe_annulus(*s->session, shape_id));
}

int l_decompose_by_face(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* solid_id = luaL_checkstring(L, 2);
    const char* face_id = luaL_checkstring(L, 3);
    return push_command_result(L, decompose_by_face(*s->session, solid_id, face_id));
}

int l_decompose_by_plane(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* solid_id = luaL_checkstring(L, 2);
    Vec3 origin{luaL_checknumber(L, 3),
                luaL_checknumber(L, 4),
                luaL_checknumber(L, 5)};
    Vec3 normal{luaL_checknumber(L, 6),
                luaL_checknumber(L, 7),
                luaL_checknumber(L, 8)};
    return push_command_result(L, decompose_by_plane(*s->session,
                                                     solid_id,
                                                     origin,
                                                     normal));
}

int l_decompose_by_planes(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* solid_id = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const lua_Integer n = luaL_len(L, 3);
    std::vector<PlaneSpec> planes;
    planes.reserve(static_cast<std::size_t>(n));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "origin");
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -2, "normal");
        luaL_checktype(L, -1, LUA_TTABLE);
        PlaneSpec spec;
        lua_rawgeti(L, -2, 1); spec.origin.x = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -2, 2); spec.origin.y = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -2, 3); spec.origin.z = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 1); spec.normal.x = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); spec.normal.y = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); spec.normal.z = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_pop(L, 3);
        planes.push_back(spec);
    }
    return push_command_result(L, decompose_by_planes(*s->session,
                                                      solid_id,
                                                      planes));
}

int l_shatter(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    int max_depth = 8;
    bool allow_volume_drift = false;
    if (lua_gettop(L) >= 3) {
        if (lua_isnumber(L, 3)) {
            max_depth = static_cast<int>(luaL_checkinteger(L, 3));
        } else if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "max_depth");
            if (!lua_isnil(L, -1)) {
                max_depth = static_cast<int>(luaL_checkinteger(L, -1));
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "allow_volume_drift");
            if (!lua_isnil(L, -1)) {
                allow_volume_drift = lua_toboolean(L, -1) != 0;
            }
            lua_pop(L, 1);
            if (lua_table_has_field(L, 3, "approx_global_limit")) {
                ApproximationPolicy policy = approximation_policy_from_lua(L, 3);
                policy.allow_numerical_shatter_loss = true;
                const ApproximationCommandResult result =
                    shatter_with_approximation(*s->session, shape_id,
                                               max_depth, policy);
                return push_command_result(L, result.command);
            }
        } else if (!lua_isnil(L, 3)) {
            return luaL_error(L, "shatter expects a number or "
                                 "{ max_depth = number, allow_volume_drift = bool }");
        }
    }
    return push_command_result(L, shatter(*s->session, shape_id,
                                          max_depth, allow_volume_drift));
}

int l_propose_decomposition(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* solid_id = luaL_checkstring(L, 2);
    const DecompositionProposal proposal =
        propose_decomposition(*s->session, solid_id);
    push_decomposition_proposal_table(L, proposal);
    return 1;
}

int l_apply_decomposition_candidate(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    const char* candidate_id = luaL_checkstring(L, 3);
    return push_command_result(
        L,
        apply_decomposition_candidate(*s->session, shape_id, candidate_id));
}

int l_auto_decompose_bsp(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    int max_depth = 4;
    DecompositionPolicy policy;
    if (lua_gettop(L) >= 3) {
        if (lua_isnumber(L, 3)) {
            max_depth = static_cast<int>(luaL_checkinteger(L, 3));
        } else if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "max_depth");
            if (!lua_isnil(L, -1)) {
                max_depth = static_cast<int>(luaL_checkinteger(L, -1));
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "allow_bbox_fallback_auto");
            if (!lua_isnil(L, -1)) {
                policy.allow_bbox_fallback_auto = lua_toboolean(L, -1) != 0;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "allow_medium_confidence_auto");
            if (!lua_isnil(L, -1)) {
                policy.allow_medium_confidence_auto = lua_toboolean(L, -1) != 0;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "allow_low_confidence_auto");
            if (!lua_isnil(L, -1)) {
                policy.allow_low_confidence_auto = lua_toboolean(L, -1) != 0;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "max_relative_delta");
            if (!lua_isnil(L, -1)) {
                policy.max_relative_delta = luaL_checknumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "min_piece_volume_fraction");
            if (!lua_isnil(L, -1)) {
                policy.min_piece_volume_fraction = luaL_checknumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "max_splits_per_target");
            if (!lua_isnil(L, -1)) {
                policy.max_splits_per_target =
                    static_cast<std::size_t>(luaL_checkinteger(L, -1));
            }
            lua_pop(L, 1);
        } else if (!lua_isnil(L, 3)) {
            return luaL_error(L, "auto_decompose_bsp expects a number or { max_depth = number }");
        }
    }
    return push_command_result(L, auto_decompose_bsp(*s->session,
                                                     shape_id,
                                                     max_depth,
                                                     policy));
}

// s:decompose_to_fixed_point(doc_id[, opts])
int l_decompose_to_fixed_point(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    FixedPointDecompositionPolicy policy;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        auto take_size = [&](const char* name, std::size_t* out) {
            lua_getfield(L, 3, name);
            if (lua_isnumber(L, -1)) {
                const lua_Integer value = lua_tointeger(L, -1);
                if (value > 0) *out = static_cast<std::size_t>(value);
            }
            lua_pop(L, 1);
        };
        auto take_number = [&](const char* name, double* out) {
            lua_getfield(L, 3, name);
            if (lua_isnumber(L, -1)) *out = lua_tonumber(L, -1);
            lua_pop(L, 1);
        };
        auto take_bool = [&](const char* name, bool* out) {
            lua_getfield(L, 3, name);
            if (!lua_isnil(L, -1)) *out = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        };
        take_size("max_actions", &policy.max_actions);
        take_size("max_candidate_evaluations", &policy.max_candidate_evaluations);
        take_size("max_candidates_per_target",
                  &policy.max_candidates_per_target);
        take_size("max_splitter_calls", &policy.max_splitter_calls);
        take_size("max_active_pieces", &policy.max_active_pieces);
        take_size("max_residual_pieces", &policy.max_residual_pieces);
        take_size("max_parallel_families_per_target",
                  &policy.max_parallel_families_per_target);
        take_size("max_planes_per_batch", &policy.max_planes_per_batch);
        take_size("max_local_boolean_calls",
                  &policy.max_local_boolean_calls);
        take_size("max_stagnant_target_visits",
                  &policy.max_stagnant_target_visits);
        take_number("max_action_relative_volume_delta",
                    &policy.max_action_relative_volume_delta);
        take_number("max_cumulative_relative_volume_delta",
                    &policy.max_cumulative_relative_volume_delta);
        take_bool("allow_medium_certified_progress",
                  &policy.allow_medium_certified_progress);
        take_bool("allow_low_certified_progress",
                  &policy.allow_low_certified_progress);
        take_bool("allow_bbox_fallback", &policy.allow_bbox_fallback);
        take_bool("run_shatter_fallback", &policy.run_shatter_fallback);
        take_bool("commit_certified_partial_progress",
                  &policy.commit_certified_partial_progress);
        take_bool("remove_residual_slivers", &policy.remove_residual_slivers);
        take_bool("use_incremental_trials", &policy.use_incremental_trials);
        take_bool("run_parallel_plane_batches",
                  &policy.run_parallel_plane_batches);
        take_bool("verify_incremental_state_each_action",
                  &policy.verify_incremental_state_each_action);
    }
    FixedPointDecompositionResult result = decompose_to_fixed_point(
        *s->session, document_id, policy, ReportIdMode::local);
    lua_newtable(L);
    set_boolean_field(L, "ok", result.command.ok);
    set_boolean_field(L, "exact", result.summary.exact);
    set_boolean_field(L, "mutated", result.summary.mutated);
    set_string_field(L, "stop_reason",
                     fixed_point_stop_reason_name(result.summary.stop_reason));
    set_integer_field(L, "actions_applied", result.summary.actions_applied);
    set_integer_field(L, "candidates_evaluated",
                      result.summary.candidates_evaluated);
    set_integer_field(L, "splitter_calls", result.summary.splitter_calls);
    set_integer_field(L, "peak_active_pieces",
                      result.summary.peak_active_pieces);
    set_integer_field(L, "residual_count",
                      result.summary.final_state.residual_count);
    set_number_field(L, "residual_volume",
                     result.summary.final_state.residual_volume);
    set_number_field(L, "total_volume",
                     result.summary.final_state.total_volume);
    return 1;
}

int l_auto_fit_splines(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    bool has_max_deviation = false;
    double max_deviation = 0.0;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        reject_unknown_table_keys(L, 3, {"max_deviation"},
                                  "auto_fit_splines");
    }
    if (!optional_deviation_from_lua(L, 3, &has_max_deviation, &max_deviation)) {
        return luaL_error(L, "auto_fit_splines expects a number or { max_deviation = number }");
    }
    if (!has_max_deviation) {
        return luaL_argerror(
            L, 3,
            "auto_fit_splines requires an explicit max_deviation budget");
    }
    if (!std::isfinite(max_deviation) || max_deviation < 0.0) {
        return luaL_argerror(
            L, 3, "max_deviation must be a finite non-negative number");
    }
    return push_command_result(
        L,
        auto_fit_splines(*s->session, document_id, has_max_deviation, max_deviation));
}

int l_compare_volumes(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* original_id = luaL_checkstring(L, 2);
    const char* candidate_id = luaL_checkstring(L, 3);
    double abs_tol = 0.0;
    double rel_tol = 0.0;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "abs_tol");
        if (!lua_isnil(L, -1)) {
            abs_tol = luaL_checknumber(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "rel_tol");
        if (!lua_isnil(L, -1)) {
            rel_tol = luaL_checknumber(L, -1);
        }
        lua_pop(L, 1);
    }
    const VolumeComparison comparison =
        compare_document_volumes(*s->session, original_id, candidate_id, abs_tol, rel_tol);

    lua_newtable(L);
    lua_pushnumber(L, comparison.original.total_volume);
    lua_setfield(L, -2, "original_volume");
    lua_pushnumber(L, comparison.candidate.total_volume);
    lua_setfield(L, -2, "candidate_volume");
    lua_pushnumber(L, comparison.absolute_delta);
    lua_setfield(L, -2, "absolute_delta");
    lua_pushnumber(L, comparison.relative_delta);
    lua_setfield(L, -2, "relative_delta");
    lua_pushboolean(L, comparison.within_tolerance);
    lua_setfield(L, -2, "within_tolerance");
    return 1;
}

int l_emit(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const char* output_path = luaL_checkstring(L, 3);
    return push_command_result(L, emit_document(*s->session, document_id, output_path));
}

// s:preprocess_safe_all(doc_id[, opts])
// Composed equivalent of CLI `preprocess --safe-all`:
//   1. analyze + overlap precheck (refuses unless opts.allow_overlaps)
//   2. auto-fit qualifying spline faces only when opts.max_deviation is set
//   3. removals only when opts.apply_removal_candidates is explicitly set
//   4. safe BSP decomposition of non-convex solids
// Returns a summary table:
//   { ok, auto_fit_applied, removed, decomposed, attempted,
//     needs_decision, remaining, overlaps_aborted }
// `ok` is false if the overlap precheck aborted (overlaps_aborted=true)
// or any underlying op fatally failed.
int l_preprocess_safe_all(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* doc_id = luaL_checkstring(L, 2);

    bool allow_overlaps = false;
    std::string apply_removal_confidence;
    int max_depth = 4;
    DecompositionPolicy policy;
    bool has_max_deviation = false;
    double max_deviation = 0.0;

    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        reject_unknown_table_keys(
            L, 3,
            {"allow_overlaps", "apply_removal_candidates", "max_depth",
             "max_deviation", "allow_medium_confidence_auto",
             "allow_low_confidence_auto",
             "allow_bbox_fallback_auto"},
            "preprocess_safe_all");
        lua_getfield(L, 3, "allow_overlaps");
        if (!lua_isnil(L, -1)) allow_overlaps = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "apply_removal_candidates");
        if (lua_isstring(L, -1)) apply_removal_confidence = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_depth");
        if (lua_isnumber(L, -1)) {
            max_depth = static_cast<int>(luaL_checkinteger(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_deviation");
        if (lua_isnumber(L, -1)) {
            has_max_deviation = true;
            max_deviation = luaL_checknumber(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "allow_medium_confidence_auto");
        if (!lua_isnil(L, -1)) {
            policy.allow_medium_confidence_auto = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "allow_low_confidence_auto");
        if (!lua_isnil(L, -1)) {
            policy.allow_low_confidence_auto = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "allow_bbox_fallback_auto");
        if (!lua_isnil(L, -1)) {
            policy.allow_bbox_fallback_auto = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    }
    if (has_max_deviation
        && (!std::isfinite(max_deviation) || max_deviation < 0.0)) {
        return luaL_argerror(L, 3,
                             "max_deviation must be a finite non-negative number");
    }

    std::size_t auto_fit_applied = 0;
    std::size_t removed = 0;
    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t needs_decision = 0;
    bool overlaps_aborted = false;

    auto push_summary = [&](bool ok,
                              std::size_t remaining_count) {
        lua_newtable(L);
        set_boolean_field(L, "ok", ok);
        set_integer_field(L, "auto_fit_applied", auto_fit_applied);
        set_integer_field(L, "removed", removed);
        set_integer_field(L, "attempted", attempted);
        set_integer_field(L, "decomposed", succeeded);
        set_integer_field(L, "needs_decision", needs_decision);
        set_integer_field(L, "remaining", remaining_count);
        set_boolean_field(L, "overlaps_aborted", overlaps_aborted);
    };

    // 1. Overlap precheck.
    if (!allow_overlaps) {
        CommandResult precheck =
            analyze(*s->session, doc_id, ReportIdMode::local);
        if (!precheck.report.overlaps.empty()) {
            overlaps_aborted = true;
            push_summary(false, precheck.report.overlaps.size());
            return 1;
        }
    }

    // 2. Auto-fit spline faces only under an explicit approximation budget.
    // A missing max_deviation means "do not approximate", not "unbounded".
    if (has_max_deviation) {
        AutoFitCommandResult af = auto_fit_splines_with_summary(
            *s->session, doc_id, true, max_deviation, ReportIdMode::local);
        auto_fit_applied = af.summary.applied;
    }

    // 3. Explicitly authorized removal candidates.
    if (!apply_removal_confidence.empty()) {
        RemovalProposal removals = suggest_removals(*s->session, doc_id,
                                                     ReportIdMode::local);
        std::vector<std::string> removal_targets;
        for (const RemovalCandidate& c : removals.candidates) {
            const bool allowed = c.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && c.confidence == "medium");
            if (!allowed || c.suggested_action == "manual") continue;
            removal_targets.push_back(c.solid_id);
        }
        if (!removal_targets.empty()) {
            CommandResult r = remove_solids(*s->session,
                                            removal_targets,
                                            ReportIdMode::local);
            if (r.ok) removed += removal_targets.size();
        }
    }

    // 4. Safe BSP decomposition of non-convex solids.
    CommandResult analysis = analyze(*s->session, doc_id, ReportIdMode::local);
    std::vector<std::string> targets;
    for (const AnalyzedSolid& sol : analysis.report.solids) {
        if (sol.needs_decomposition) targets.push_back(sol.id);
    }
    for (const std::string& id : targets) {
        ++attempted;
        CommandResult r = auto_decompose_bsp(*s->session, id, max_depth,
                                              policy, ReportIdMode::local,
                                              /*defer_analysis=*/true);
        if (r.ok) {
            ++succeeded;
        } else {
            for (const Diagnostic& d : r.report.diagnostics) {
                if (d.code == "auto_decompose_bsp_needs_decision") {
                    ++needs_decision; break;
                }
            }
        }
    }

    // Final analyze to count what's left.
    CommandResult final_analysis =
        analyze(*s->session, doc_id, ReportIdMode::local);
    std::size_t remaining = 0;
    for (const AnalyzedSolid& sol : final_analysis.report.solids) {
        if (!sol.exact_csg_supported) ++remaining;
    }

    push_summary(true, remaining);
    return 1;
}

// s:write_decision_pack(doc_id, output_dir[, opts])
// Bundles human/agent review packets (preview.html + targets) for the
// document. opts may carry { target_id, max_packets }. Returns a
// table { ok, packets_written, error }.
int l_write_decision_pack(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* doc_id = luaL_checkstring(L, 2);
    const char* out_dir = luaL_checkstring(L, 3);

    DecisionPackOptions options;
    options.output_dir = out_dir;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "target_id");
        if (lua_isstring(L, -1)) options.target_id = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "max_packets");
        if (lua_isnumber(L, -1)) {
            options.max_packets = static_cast<std::size_t>(
                luaL_checkinteger(L, -1));
        }
        lua_pop(L, 1);
    }

    CadDocument* document = nullptr;
    for (CadDocument& candidate : s->session->documents) {
        if (candidate.id == doc_id) {
            document = &candidate;
            break;
        }
    }
    if (!document) {
        lua_newtable(L);
        set_boolean_field(L, "ok", false);
        set_integer_field(L, "packets_written", 0);
        set_string_field(L, "error",
                         "document_not_found: cannot resolve session document");
        return 1;
    }

    const CommandResult analysis =
        analyze(*s->session, doc_id, ReportIdMode::local);

    DecisionPackResult result =
        write_decision_packs(analysis.report, document->model, options);

    lua_newtable(L);
    set_boolean_field(L, "ok", result.ok);
    set_integer_field(L, "packets_written", result.packets_written);
    set_string_field(L, "error", result.error);
    return 1;
}

// s:explain(shape_id[, doc_id])
// Returns a structured table describing why the target solid /
// piece / face is not emit-ready: status, blockers, suggested
// actions, and top decomposition / surface-replacement candidates.
// Mirrors the data the CLI `explain` verb prints. `doc_id` defaults
// to "doc:1" when omitted.
int l_explain(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    const std::string doc_id = lua_gettop(L) >= 3 && !lua_isnil(L, 3)
                                   ? luaL_checkstring(L, 3)
                                   : "doc:1";

    // Run analyze (local mode -- so ids in the resulting report match
    // the local shape_id form the engine uses internally; the caller
    // can still pass scoped ids and we strip the doc prefix below).
    CommandResult analysis = analyze(*s->session, doc_id, ReportIdMode::local);
    const ConversionReport& report = analysis.report;

    auto strip_doc_prefix = [&](std::string id) {
        const std::string prefix = doc_id + "/";
        if (id.rfind(prefix, 0) == 0) id.erase(0, prefix.size());
        return id;
    };
    const std::string target_id = strip_doc_prefix(shape_id);

    const AnalyzedSolid* solid_hit = nullptr;
    const AnalyzedDecomposedPiece* piece_hit = nullptr;
    const AnalyzedFace* face_hit = nullptr;
    for (const AnalyzedSolid& sol : report.solids) {
        if (sol.id == target_id) { solid_hit = &sol; break; }
    }
    if (!solid_hit) {
        for (const AnalyzedDecomposedPiece& p : report.decomposed_pieces) {
            if (p.id == target_id) { piece_hit = &p; break; }
        }
    }
    if (!solid_hit && !piece_hit) {
        for (const AnalyzedFace& f : report.faces) {
            if (f.id == target_id) { face_hit = &f; break; }
        }
    }

    lua_newtable(L);
    set_string_field(L, "target_id", shape_id);
    if (!solid_hit && !piece_hit && !face_hit) {
        set_string_field(L, "kind", "unknown");
        set_string_field(L, "error", "explain_target_not_found");
        set_boolean_field(L, "ready", false);
        return 1;
    }

    auto status_label = [](const AnalyzedSolid& sol) -> const char* {
        if (sol.exact_csg_supported) return "ready";
        if (sol.needs_spline_replacement) return "spline";
        if (sol.needs_decomposition) return "decompose";
        if (sol.needs_user_decision) return "decision";
        return "review";
    };

    if (solid_hit) {
        set_string_field(L, "kind", "solid");
        set_string_field(L, "status", status_label(*solid_hit));
        set_number_field(L, "volume", solid_hit->volume);
        set_boolean_field(L, "ready", solid_hit->exact_csg_supported);
        set_boolean_field(L, "needs_decomposition", solid_hit->needs_decomposition);
        set_boolean_field(L, "needs_spline_replacement",
                          solid_hit->needs_spline_replacement);
        set_boolean_field(L, "needs_surface_replacement",
                          solid_hit->needs_surface_replacement);
        set_boolean_field(L, "needs_user_decision",
                          solid_hit->needs_user_decision);
        // Blocking faces on this solid.
        lua_newtable(L);
        std::size_t idx = 0;
        for (const AnalyzedFace& f : report.faces) {
            if (f.solid_id != solid_hit->id) continue;
            if (f.exact_csg_supported) continue;
            ++idx;
            push_face_table(L, f);
            lua_rawseti(L, -2, static_cast<lua_Integer>(idx));
        }
        lua_setfield(L, -2, "blocking_faces");
        // Top decomposition candidates (if non-convex).
        if (solid_hit->needs_decomposition) {
            DecompositionProposal prop =
                propose_decomposition(*s->session, solid_hit->id,
                                       ReportIdMode::local);
            push_decomposition_proposal_table(L, prop);
            lua_setfield(L, -2, "decomposition");
        }
    } else if (piece_hit) {
        set_string_field(L, "kind", "piece");
        set_string_field(L, "source_solid_id", piece_hit->source_solid_id);
        set_number_field(L, "volume", piece_hit->volume);
        set_boolean_field(L, "ready", piece_hit->exact_csg_supported);
        set_boolean_field(L, "needs_decomposition", piece_hit->needs_decomposition);
        set_boolean_field(L, "needs_spline_replacement",
                          piece_hit->needs_spline_replacement);
        set_boolean_field(L, "needs_surface_replacement",
                          piece_hit->needs_surface_replacement);
        set_boolean_field(L, "needs_user_decision",
                          piece_hit->needs_user_decision);
    } else { // face_hit
        set_string_field(L, "kind", "face");
        set_string_field(L, "surface_type", face_hit->surface_type);
        set_string_field(L, "owner_solid_id", face_hit->solid_id);
        set_number_field(L, "area", face_hit->area);
        set_boolean_field(L, "ready", face_hit->exact_csg_supported);
        set_boolean_field(L, "needs_spline_replacement",
                          face_hit->needs_spline_replacement);
        set_boolean_field(L, "needs_surface_replacement",
                          face_hit->needs_surface_replacement);
        set_boolean_field(L, "needs_emitter", face_hit->needs_emitter);
        if (!face_hit->exact_csg_supported) {
            SurfaceReplacementProposal prop =
                propose_surface_replacement(*s->session, face_hit->id,
                                             ReportIdMode::local);
            push_surface_replacement_proposal_table(L, prop);
            lua_setfield(L, -2, "surface_replacement");
        }
    }
    return 1;
}

// s:export_step(doc, "simplified.step")
// Writes the as-simplified geometry to STEP for visual review (the
// CLI equivalent is --out-step). Original solids + decomposed pieces +
// primitive synthetic bodies are emitted; boolean synthetic nodes are
// emitted as bbox placeholders for now (same coverage as the C++
// API).
int l_export_step(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    const char* output_path = luaL_checkstring(L, 3);
    return push_command_result(L,
                               export_document_step(*s->session,
                                                    document_id,
                                                    output_path));
}

int l_report_json(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const std::string json = report_json_string(last_report(*s->session));
    lua_pushstring(L, json.c_str());
    return 1;
}

int l_report_table(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    push_report_table(L, last_report(*s->session));
    return 1;
}

int l_write_report(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* path = luaL_checkstring(L, 2);
    std::ofstream out(path);
    if (!out) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "could not open report path: %s", path);
        return 2;
    }
    out << report_json_string(last_report(*s->session));
    lua_pushboolean(L, out ? 1 : 0);
    if (!out) {
        lua_pushstring(L, "report write failed");
        return 2;
    }
    return 1;
}

void register_session(lua_State* L) {
    luaL_newmetatable(L, kSessionMeta);
    lua_pushcfunction(L, l_session_gc);
    lua_setfield(L, -2, "__gc");

    lua_newtable(L);
    lua_pushcfunction(L, l_load_step);
    lua_setfield(L, -2, "load_step");
    lua_pushcfunction(L, l_attach_decision_log);
    lua_setfield(L, -2, "attach_decision_log");
    lua_pushcfunction(L, l_analyze);
    lua_setfield(L, -2, "analyze");
    lua_pushcfunction(L, l_analyze_table);
    lua_setfield(L, -2, "analyze_table");
    lua_pushcfunction(L, l_document_revision);
    lua_setfield(L, -2, "document_revision");
    lua_pushcfunction(L, l_undo_last_mutation);
    lua_setfield(L, -2, "undo_last_mutation");
    lua_pushcfunction(L, l_suggest_removals);
    lua_setfield(L, -2, "suggest_removals");
    lua_pushcfunction(L, l_propose_hole_fills);
    lua_setfield(L, -2, "propose_hole_fills");
    lua_pushcfunction(L, l_define_material);
    lua_setfield(L, -2, "define_material");
    lua_pushcfunction(L, l_propose_contained_medium);
    lua_setfield(L, -2, "propose_contained_medium");
    lua_pushcfunction(L, l_stage_contained_medium_request);
    lua_setfield(L, -2, "stage_contained_medium_request");
    lua_pushcfunction(L, l_apply_contained_medium);
    lua_setfield(L, -2, "apply_contained_medium");
    lua_pushcfunction(L, l_fill_hole);
    lua_setfield(L, -2, "fill_hole");
    lua_pushcfunction(L, l_remove_solid);
    lua_setfield(L, -2, "remove_solid");
    lua_pushcfunction(L, l_remove_solids);
    lua_setfield(L, -2, "remove_solids");
    lua_pushcfunction(L, l_resolve_overlap);
    lua_setfield(L, -2, "resolve_overlap");
    lua_pushcfunction(L, l_replace_solid);
    lua_setfield(L, -2, "replace_solid");
    lua_pushcfunction(L, l_replace_solid_with_bbox);
    lua_setfield(L, -2, "replace_solid_with_bbox");
    lua_pushcfunction(L, l_replace_solid_with_bbox_cylinder);
    lua_setfield(L, -2, "replace_solid_with_bbox_cylinder");
    lua_pushcfunction(L, l_replace_solid_with_fitted_cylinder);
    lua_setfield(L, -2, "replace_solid_with_fitted_cylinder");
    lua_pushcfunction(L, l_replace_solid_with_fitted_sphere);
    lua_setfield(L, -2, "replace_solid_with_fitted_sphere");
    lua_pushcfunction(L, l_replace_solid_with_fitted_cone);
    lua_setfield(L, -2, "replace_solid_with_fitted_cone");
    lua_pushcfunction(L, l_replace_face_with_fit);
    lua_setfield(L, -2, "replace_face_with_fit");
    lua_pushcfunction(L, l_propose_surface_replacement);
    lua_setfield(L, -2, "propose_surface_replacement");
    lua_pushcfunction(L, l_apply_surface_replacement_candidate);
    lua_setfield(L, -2, "apply_surface_replacement_candidate");
    lua_pushcfunction(L, l_decompose_planar);
    lua_setfield(L, -2, "decompose_planar");
    lua_pushcfunction(L, l_decompose_prismatic);
    lua_setfield(L, -2, "decompose_prismatic");
    lua_pushcfunction(L, l_decompose_revolution);
    lua_setfield(L, -2, "decompose_revolution");
    lua_pushcfunction(L, l_reuse_definition_conversion);
    lua_setfield(L, -2, "reuse_definition_conversion");
    lua_pushcfunction(L, l_decompose_shell_difference);
    lua_setfield(L, -2, "decompose_shell_difference");
    lua_pushcfunction(L, l_decompose_pipe_annulus);
    lua_setfield(L, -2, "decompose_pipe_annulus");
    lua_pushcfunction(L, l_decompose_by_face);
    lua_setfield(L, -2, "decompose_by_face");
    lua_pushcfunction(L, l_decompose_by_plane);
    lua_setfield(L, -2, "decompose_by_plane");
    lua_pushcfunction(L, l_decompose_by_planes);
    lua_setfield(L, -2, "decompose_by_planes");
    lua_pushcfunction(L, l_shatter);
    lua_setfield(L, -2, "shatter");
    lua_pushcfunction(L, l_propose_decomposition);
    lua_setfield(L, -2, "propose_decomposition");
    lua_pushcfunction(L, l_apply_decomposition_candidate);
    lua_setfield(L, -2, "apply_decomposition_candidate");
    lua_pushcfunction(L, l_auto_decompose_bsp);
    lua_setfield(L, -2, "auto_decompose_bsp");
    lua_pushcfunction(L, l_decompose_to_fixed_point);
    lua_setfield(L, -2, "decompose_to_fixed_point");
    lua_pushcfunction(L, l_auto_fit_splines);
    lua_setfield(L, -2, "auto_fit_splines");
    lua_pushcfunction(L, l_compare_volumes);
    lua_setfield(L, -2, "compare_volumes");
    lua_pushcfunction(L, l_emit);
    lua_setfield(L, -2, "emit");
    lua_pushcfunction(L, l_export_step);
    lua_setfield(L, -2, "export_step");
    lua_pushcfunction(L, l_explain);
    lua_setfield(L, -2, "explain");
    lua_pushcfunction(L, l_write_decision_pack);
    lua_setfield(L, -2, "write_decision_pack");
    lua_pushcfunction(L, l_preprocess_safe_all);
    lua_setfield(L, -2, "preprocess_safe_all");
    lua_pushcfunction(L, l_report_json);
    lua_setfield(L, -2, "report_json");
    lua_pushcfunction(L, l_report_table);
    lua_setfield(L, -2, "report_table");
    lua_pushcfunction(L, l_write_report);
    lua_setfield(L, -2, "write_report");
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);
}

// csgify.compact(log_path, recipe_path)
// Reads an append-only decision log and writes a deterministic Lua
// recipe. Module-level (not a session method) because it operates on
// files, not session state. Returns true on success, or (nil, errmsg)
// on failure.
int l_module_compact(lua_State* L) {
    const char* log_path = luaL_checkstring(L, 1);
    const char* recipe_path = luaL_checkstring(L, 2);
    const int rc = compact_log_to_recipe(log_path, recipe_path);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "compact_log_to_recipe failed (rc=%d)", rc);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// csgify.replay(recipe_path, step_path, out_path, report_path[, out_step_path])
// Replays a compacted recipe against a STEP file. step_path may be
// empty to use the recorded step path from the recipe header. Returns
// true on success, or (nil, errmsg) on failure.
int l_module_replay(lua_State* L) {
    const char* recipe_path = luaL_checkstring(L, 1);
    const char* step_path = luaL_checkstring(L, 2);
    const char* output_path = luaL_checkstring(L, 3);
    const char* report_path = luaL_checkstring(L, 4);
    const char* step_out_path = lua_gettop(L) >= 5 && !lua_isnil(L, 5)
                                    ? luaL_checkstring(L, 5)
                                    : "";
    const int rc = replay_recipe(recipe_path, step_path, output_path,
                                  report_path, step_out_path);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "replay_recipe failed (rc=%d)", rc);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_module_operations(lua_State* L) {
    lua_newtable(L);
    const auto& registry = operation_registry();
    for (std::size_t i = 0; i < registry.size(); ++i) {
        const OperationDescriptor& descriptor = registry[i];
        lua_newtable(L);
        set_string_field(L, "name", descriptor.name);
        set_string_field(L, "category", descriptor.category);
        const char* target = descriptor.target == OperationTargetKind::document
            ? "document"
            : descriptor.target == OperationTargetKind::solid ? "solid" : "face";
        set_string_field(L, "target_kind", target);
        set_boolean_field(L, "mutating", descriptor.mutating);
        set_boolean_field(L, "approximate", descriptor.approximate);
        set_integer_field(L, "version", descriptor.version);
        set_string_field(L, "cli_flag", descriptor.cli_flag);
        set_string_field(L, "lua_value", descriptor.lua_value);
        set_string_field(L, "autonomy_argument_schema",
                         descriptor.autonomy_argument_schema);
        set_boolean_field(L, "autonomy_supported",
                          descriptor.autonomy_supported);
        set_boolean_field(L, "qt_review_supported",
                          descriptor.qt_review_supported);
        set_boolean_field(L, "restart_supported",
                          descriptor.restart_supported);
        set_string_field(L, "recovery_action", descriptor.recovery_action);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

void register_csgify(lua_State* L) {
    register_session(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_new_session);
    lua_setfield(L, -2, "session");
    lua_pushcfunction(L, l_module_compact);
    lua_setfield(L, -2, "compact");
    lua_pushcfunction(L, l_module_replay);
    lua_setfield(L, -2, "replay");
    lua_pushcfunction(L, l_module_operations);
    lua_setfield(L, -2, "operations");
    lua_setglobal(L, "csgify");
}

void set_arg_table(lua_State* L, const std::string& script_path, int argc, char** argv) {
    lua_createtable(L, argc, 1);
    lua_pushstring(L, script_path.c_str());
    lua_rawseti(L, -2, 0);
    for (int i = 0; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setglobal(L, "arg");
}

} // namespace

int run_lua_script(const std::string& script_path, int argc, char** argv) {
    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "error lua_init_failed\n";
        return 1;
    }
    luaL_openlibs(L);
    register_csgify(L);
    set_arg_table(L, script_path, argc, argv);

    const int status = luaL_dofile(L, script_path.c_str());
    if (status != LUA_OK) {
        std::cerr << "error lua_script_failed: " << lua_tostring(L, -1) << "\n";
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}

} // namespace csgify

#else

namespace csgify {

int run_lua_script(const std::string& /*script_path*/, int /*argc*/, char** /*argv*/) {
    std::cerr << "error lua_not_built: rebuild with CSGIFY_ENABLE_LUA=ON "
              << "and Lua development files installed\n";
    return 1;
}

} // namespace csgify

#endif
