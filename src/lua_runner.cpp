#include "lua_runner.hpp"

#include "cad2csg/engine.hpp"
#include "cad2csg/session.hpp"

#include <fstream>
#include <iostream>
#include <new>
#include <string>

#if CAD2CSG_ENABLE_LUA

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace cad2csg {
namespace {

constexpr const char* kSessionMeta = "cad2csg.session";

struct LuaSession {
    SessionPtr session;

    LuaSession() : session(create_session()) {}
};

LuaSession* check_session(lua_State* L, int index) {
    void* userdata = luaL_checkudata(L, index, kSessionMeta);
    return static_cast<LuaSession*>(userdata);
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
    set_boolean_field(L, "exact_csg_supported", solid.exact_csg_supported);
    set_boolean_field(L, "needs_decomposition", solid.needs_decomposition);
    set_boolean_field(L, "needs_spline_replacement", solid.needs_spline_replacement);
    set_boolean_field(L, "needs_user_decision", solid.needs_user_decision);

    lua_newtable(L);
    for (std::size_t i = 0; i < solid.face_ids.size(); ++i) {
        lua_pushstring(L, solid.face_ids[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "faces");
}

void push_face_table(lua_State* L, const AnalyzedFace& face) {
    lua_newtable(L);
    set_string_field(L, "id", face.id);
    set_string_field(L, "label", face.label);
    set_string_field(L, "name", face.name);
    set_string_field(L, "hierarchy_path", face.hierarchy_path);
    set_string_field(L, "solid_id", face.solid_id);
    set_string_field(L, "surface_type", face.surface_type);
    set_integer_field(L, "index", face.index);
    set_number_field(L, "area", face.area);
    push_bbox_table(L, face.bbox);
    lua_setfield(L, -2, "bbox");
    set_boolean_field(L, "exact_csg_supported", face.exact_csg_supported);
    set_boolean_field(L, "needs_spline_replacement", face.needs_spline_replacement);
    set_boolean_field(L, "needs_emitter", face.needs_emitter);
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
    set_boolean_field(L, "needs_user_decision", piece.needs_user_decision);
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

void push_report_table(lua_State* L, const ConversionReport& report) {
    lua_newtable(L);
    set_boolean_field(L, "ok", report.ok());
    set_string_field(L, "document_id", report.document_id);
    set_string_field(L, "source_path", report.source_path);
    set_number_field(L, "total_volume", report.total_volume);
    set_integer_field(L, "solids_seen", report.solids_seen);
    set_integer_field(L, "solids_converted", report.solids_converted);
    set_integer_field(L, "faces_seen", report.faces_seen);
    set_integer_field(L, "analytic_faces", report.analytic_faces);
    set_integer_field(L, "spline_faces", report.spline_faces);
    set_string_field(L, "json", report_json_string(report));

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
}

int push_command_result(lua_State* L, const CommandResult& result) {
    if (!result.ok) {
        lua_pushnil(L);
        lua_pushstring(L, report_json_string(result.report).c_str());
        return 2;
    }
    lua_pushstring(L, report_json_string(result.report).c_str());
    return 1;
}

int push_command_result_table(lua_State* L, const CommandResult& result) {
    if (!result.ok) {
        lua_pushnil(L);
        push_report_table(L, result.report);
        return 2;
    }
    push_report_table(L, result.report);
    return 1;
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
    if (op == "bbox") return ReplacementOperation::bbox;
    if (op == "bbox_cylinder") return ReplacementOperation::bbox_cylinder;
    if (op == "fitted_cylinder") return ReplacementOperation::fitted_cylinder;
    if (op == "fitted_sphere") return ReplacementOperation::fitted_sphere;
    if (op == "fitted_cone") return ReplacementOperation::fitted_cone;
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
    void* userdata = lua_newuserdata(L, sizeof(LuaSession));
    new (userdata) LuaSession();
    luaL_getmetatable(L, kSessionMeta);
    lua_setmetatable(L, -2);
    return 1;
}

int l_load_step(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* path = luaL_checkstring(L, 2);
    const DocumentId id = load_step(*s->session, path);
    lua_pushstring(L, id.c_str());
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

int l_remove_solid(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, remove_solid(*s->session, shape_id));
}

int l_replace_solid(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    const ReplacementOperation op = replacement_operation_from_lua(L, 3);

    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        const ReplacementLimits limits = replacement_limits_from_lua(L, 4);
        ReplacementCommandResult result =
            apply_checked_replacement_operation(*s->session, shape_id, op, limits);
        return push_command_result(L, result.command);
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
    ReplacementLimits limits;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_cylinder,
            limits);
        return push_command_result(L, result.command);
    }
    return push_command_result(L, replace_solid_with_fitted_cylinder(*s->session, shape_id));
}

int l_replace_solid_with_fitted_sphere(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    ReplacementLimits limits;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_sphere,
            limits);
        return push_command_result(L, result.command);
    }
    return push_command_result(L, replace_solid_with_fitted_sphere(*s->session, shape_id));
}

int l_replace_solid_with_fitted_cone(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    ReplacementLimits limits;
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        limits = replacement_limits_from_lua(L, 3);
        ReplacementCommandResult result = apply_checked_replacement_operation(
            *s->session,
            shape_id,
            ReplacementOperation::fitted_cone,
            limits);
        return push_command_result(L, result.command);
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

int l_decompose_planar(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* shape_id = luaL_checkstring(L, 2);
    return push_command_result(L, decompose_planar(*s->session, shape_id));
}

int l_decompose_by_face(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* solid_id = luaL_checkstring(L, 2);
    const char* face_id = luaL_checkstring(L, 3);
    return push_command_result(L, decompose_by_face(*s->session, solid_id, face_id));
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
        } else if (!lua_isnil(L, 3)) {
            return luaL_error(L, "auto_decompose_bsp expects a number or { max_depth = number }");
        }
    }
    return push_command_result(L, auto_decompose_bsp(*s->session,
                                                     shape_id,
                                                     max_depth,
                                                     policy));
}

int l_auto_fit_splines(lua_State* L) {
    LuaSession* s = check_session(L, 1);
    const char* document_id = luaL_checkstring(L, 2);
    bool has_max_deviation = false;
    double max_deviation = 0.0;
    if (!optional_deviation_from_lua(L, 3, &has_max_deviation, &max_deviation)) {
        return luaL_error(L, "auto_fit_splines expects a number or { max_deviation = number }");
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
    lua_pushcfunction(L, l_analyze);
    lua_setfield(L, -2, "analyze");
    lua_pushcfunction(L, l_analyze_table);
    lua_setfield(L, -2, "analyze_table");
    lua_pushcfunction(L, l_remove_solid);
    lua_setfield(L, -2, "remove_solid");
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
    lua_pushcfunction(L, l_decompose_planar);
    lua_setfield(L, -2, "decompose_planar");
    lua_pushcfunction(L, l_decompose_by_face);
    lua_setfield(L, -2, "decompose_by_face");
    lua_pushcfunction(L, l_propose_decomposition);
    lua_setfield(L, -2, "propose_decomposition");
    lua_pushcfunction(L, l_apply_decomposition_candidate);
    lua_setfield(L, -2, "apply_decomposition_candidate");
    lua_pushcfunction(L, l_auto_decompose_bsp);
    lua_setfield(L, -2, "auto_decompose_bsp");
    lua_pushcfunction(L, l_auto_fit_splines);
    lua_setfield(L, -2, "auto_fit_splines");
    lua_pushcfunction(L, l_compare_volumes);
    lua_setfield(L, -2, "compare_volumes");
    lua_pushcfunction(L, l_emit);
    lua_setfield(L, -2, "emit");
    lua_pushcfunction(L, l_report_json);
    lua_setfield(L, -2, "report_json");
    lua_pushcfunction(L, l_report_table);
    lua_setfield(L, -2, "report_table");
    lua_pushcfunction(L, l_write_report);
    lua_setfield(L, -2, "write_report");
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);
}

void register_cad2csg(lua_State* L) {
    register_session(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_new_session);
    lua_setfield(L, -2, "session");
    lua_setglobal(L, "cad2csg");
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
    register_cad2csg(L);
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

} // namespace cad2csg

#else

namespace cad2csg {

int run_lua_script(const std::string& /*script_path*/, int /*argc*/, char** /*argv*/) {
    std::cerr << "error lua_not_built: rebuild with CAD2CSG_ENABLE_LUA=ON "
              << "and Lua development files installed\n";
    return 1;
}

} // namespace cad2csg

#endif
