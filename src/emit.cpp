// Emit stage: turn an already-prepared CadModel into an AleaSystem of
// surfaces, halfspace nodes, and cells. Refuses geometry that isn't ready
// (non-convex without decomposition, splines without replacement, etc.).

#include "cad2csg/engine.hpp"

#include "analytic_surface.hpp"
#include "decompose.hpp"
#include "engine_internal.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_XYZ.hxx>

#include <alea.h>

namespace cad2csg {
namespace {

using namespace cad2csg::detail;

bool add_face_halfspace(alea_system_t* sys,
                        const TopoDS_Face& face,
                        const gp_Pnt& interior_point,
                        std::size_t solid_index,
                        std::size_t face_index,
                        ConversionReport& report,
                        alea_node_id_t* node_out,
                        const std::vector<FaceReplacement>* replacements = nullptr) {
    const std::string label = face_label(solid_index, face_index);
    AnalyticSurface surface;

    int surface_index = -1;
    if (!analytic_surface_from_face(face, solid_index, face_index, report, true,
                                    &surface, replacements)) {
        return false;
    }

    switch (surface.kind) {
    case AnalyticSurfaceKind::plane: {
        const gp_Pln plane = surface.plane;
        const gp_XYZ n = plane.Axis().Direction().XYZ();
        const double d = -n.Dot(plane.Location().XYZ());
        surface_index = alea_plane_surface(sys, 0, n.X(), n.Y(), n.Z(), d);
        break;
    }
    case AnalyticSurfaceKind::sphere: {
        const gp_Sphere sphere = surface.sphere;
        surface_index = alea_sphere_surface(sys, 0,
                                            sphere.Location().X(),
                                            sphere.Location().Y(),
                                            sphere.Location().Z(),
                                            sphere.Radius());
        break;
    }
    case AnalyticSurfaceKind::quadric: {
        const QuadricCoefficients& q = surface.quadric;
        surface_index = alea_quadric_surface(sys, 0, q.a, q.b, q.c, q.d, q.e,
                                             q.f, q.g, q.h, q.i, q.j);
        break;
    }
    case AnalyticSurfaceKind::torus: {
        const gp_Torus torus = surface.torus;
        const gp_Pnt location = torus.Location();
        const gp_Dir direction = torus.Axis().Direction();
        if (std::abs(std::abs(direction.X()) - 1.0) <= 1.0e-10
            && std::abs(direction.Y()) <= 1.0e-10
            && std::abs(direction.Z()) <= 1.0e-10) {
            surface_index = alea_torus_x_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        } else if (std::abs(std::abs(direction.Y()) - 1.0) <= 1.0e-10
                   && std::abs(direction.X()) <= 1.0e-10
                   && std::abs(direction.Z()) <= 1.0e-10) {
            surface_index = alea_torus_y_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        } else if (std::abs(std::abs(direction.Z()) - 1.0) <= 1.0e-10
                   && std::abs(direction.X()) <= 1.0e-10
                   && std::abs(direction.Y()) <= 1.0e-10) {
            surface_index = alea_torus_z_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        }
        break;
    }
    }

    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the surface.",
                 label);
        return false;
    }

    const double value = surface.value_at(interior_point);
    const int sense = value <= kEps ? -1 : 1;
    const alea_node_id_t node = alea_halfspace(sys, surface_index, sense);
    if (node == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid halfspace node.", label);
        return false;
    }

    *node_out = node;
    return true;
}

bool convert_solid(alea_system_t* sys,
                   const TopoDS_Solid& solid,
                   std::size_t solid_index,
                   const ConversionOptions& options,
                   ConversionReport& report,
                   int depth = 0,
                   const std::vector<FaceReplacement>* replacements = nullptr) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double volume = std::abs(props.Mass());
    if (depth == 0) {
        report.total_volume += volume;
        report.solid_volumes.push_back({solid_label(solid_index), volume});
    }

    if (volume <= options.linear_tolerance) {
        add_diag(report, DiagnosticSeverity::warning, "empty_or_tiny_solid",
                 "Solid has near-zero volume and was skipped.", solid_label(solid_index));
        return false;
    }

    gp_Pnt interior_point;
    if (!find_interior_point(solid, props, options.linear_tolerance, &interior_point)) {
        add_diag(report, DiagnosticSeverity::error, "interior_point_not_found",
                 "Could not find an interior point for selecting face halfspaces.",
                 solid_label(solid_index));
        return false;
    }
    std::vector<alea_node_id_t> nodes;
    std::size_t face_index = 0;

    if (options.require_convex_cells
        && !validate_convex_halfspace_cell(solid, interior_point, solid_index,
                                           options, report, false, depth == 0, true,
                                           replacements)) {
        if (options.try_planar_decomposition && depth == 0) {
            const std::vector<TopoDS_Solid> pieces =
                split_solid_by_planar_faces(solid, options, report, solid_index);
            std::size_t converted_before = report.solids_converted;
            for (const TopoDS_Solid& piece : pieces) {
                if (convert_solid(sys, piece, solid_index, options, report, depth + 1,
                                   replacements)) {
                    ++report.decomposition_pieces;
                }
            }
            if (report.solids_converted > converted_before) {
                return true;
            }
        }

        if (depth > 0) {
            return false;
        }

        add_diag(report, DiagnosticSeverity::error,
                 "nonconvex_solid_requires_decomposition",
                 "At least one vertex lies outside a face halfspace selected from the solid interior point; emitting one Alea intersection cell would remove valid volume.",
                 solid_label(solid_index));
        return false;
    }

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        alea_node_id_t node = ALEA_NODE_ID_INVALID;
        if (!add_face_halfspace(sys, TopoDS::Face(face_exp.Current()), interior_point,
                                solid_index, face_index, report, &node,
                                replacements)) {
            return false;
        }
        nodes.push_back(node);
    }

    if (nodes.empty()) {
        add_diag(report, DiagnosticSeverity::error, "solid_without_faces",
                 "Solid contains no faces.", solid_label(solid_index));
        return false;
    }

    const alea_node_id_t root = alea_intersection_n(sys, nodes.data(), nodes.size());
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the face intersection.",
                 solid_label(solid_index));
        return false;
    }

    int material_index = alea_find_material_by_id(sys, options.default_material_id);
    if (material_index < 0) {
        material_index = alea_add_material(sys, options.default_material_id);
    }
    if (material_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_material_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the default material.",
                 solid_label(solid_index));
        return false;
    }

    const int cell_index = alea_add_cell(sys, 0, root, material_index,
                                         options.default_density, 0);
    if (cell_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the converted solid cell.",
                 solid_label(solid_index));
        return false;
    }

    ++report.solids_converted;
    return true;
}

alea_node_id_t oriented_halfspace(alea_system_t* sys,
                                  int surface_index,
                                  double interior_value,
                                  const std::string& label,
                                  ConversionReport& report) {
    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the synthetic surface.",
                 label);
        return ALEA_NODE_ID_INVALID;
    }
    const int sense = interior_value <= kEps ? -1 : 1;
    const alea_node_id_t node = alea_halfspace(sys, surface_index, sense);
    if (node == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid synthetic halfspace.", label);
    }
    return node;
}

int axis_aligned_cone_surface(alea_system_t* sys,
                              Vec3 apex,
                              Vec3 axis,
                              double tan_angle_squared,
                              double tolerance) {
    if (std::abs(std::abs(axis.x) - 1.0) <= tolerance
        && nearly_zero(axis.y, tolerance)
        && nearly_zero(axis.z, tolerance)) {
        return alea_cone_x_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    if (std::abs(std::abs(axis.y) - 1.0) <= tolerance
        && nearly_zero(axis.x, tolerance)
        && nearly_zero(axis.z, tolerance)) {
        return alea_cone_y_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    if (std::abs(std::abs(axis.z) - 1.0) <= tolerance
        && nearly_zero(axis.x, tolerance)
        && nearly_zero(axis.y, tolerance)) {
        return alea_cone_z_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    return -1;
}

alea_node_id_t emit_synthetic_cone_node(alea_system_t* sys,
                                        const SyntheticBody& body,
                                        ConversionReport& report) {
    const double length = vec_len(body.height);
    if (length <= kEps || body.radius < 0.0 || body.top_radius < 0.0) {
        add_diag(report, DiagnosticSeverity::error, "invalid_synthetic_cone",
                 "Synthetic cone/frustum has invalid dimensions.", body.label);
        return ALEA_NODE_ID_INVALID;
    }

    if (std::abs(body.radius - body.top_radius) <= kEps) {
        const int surface_index = alea_rcc_surface(sys, 0,
                                                   body.base.x, body.base.y, body.base.z,
                                                   body.height.x, body.height.y, body.height.z,
                                                   body.radius);
        return oriented_halfspace(sys, surface_index, -1.0, body.label, report);
    }

    const Vec3 axis = scale(body.height, 1.0 / length);
    const Vec3 top = add(body.base, body.height);
    const Vec3 interior = add(body.base, scale(body.height, 0.5));
    const double radius_delta = body.top_radius - body.radius;
    const double apex_t = -body.radius * length / radius_delta;
    const Vec3 apex = add(body.base, scale(axis, apex_t));
    const double tan_angle_squared = (radius_delta * radius_delta) / (length * length);
    const QuadricCoefficients side = cone_quadric(apex, axis, tan_angle_squared);

    int side_surface = axis_aligned_cone_surface(sys, apex, axis,
                                                 tan_angle_squared, 1.0e-10);
    if (side_surface < 0) {
        side_surface = alea_quadric_surface(sys, 0,
                                            side.a, side.b, side.c,
                                            side.d, side.e, side.f,
                                            side.g, side.h, side.i, side.j);
    }
    const alea_node_id_t side_node =
        oriented_halfspace(sys, side_surface, quadric_value(side, {interior.x, interior.y, interior.z}),
                           body.label, report);
    if (side_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const double base_d = -dot(axis, body.base);
    const int base_surface = alea_plane_surface(sys, 0, axis.x, axis.y, axis.z, base_d);
    const double base_value = dot(axis, interior) + base_d;
    const alea_node_id_t base_node =
        oriented_halfspace(sys, base_surface, base_value, body.label, report);
    if (base_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const double top_d = -dot(axis, top);
    const int top_surface = alea_plane_surface(sys, 0, axis.x, axis.y, axis.z, top_d);
    const double top_value = dot(axis, interior) + top_d;
    const alea_node_id_t top_node =
        oriented_halfspace(sys, top_surface, top_value, body.label, report);
    if (top_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t side_and_base = alea_intersection(sys, side_node, base_node);
    if (side_and_base == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 "Alea failed to build the synthetic cone side/base intersection.",
                 body.label);
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t root = alea_intersection(sys, side_and_base, top_node);
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 "Alea failed to build the synthetic cone cap intersection.",
                 body.label);
    }
    return root;
}

alea_node_id_t emit_synthetic_node(alea_system_t* sys,
                                   const CadModel& model,
                                   const SyntheticBody& body,
                                   ConversionReport& report) {
    int surface_index = -1;

    switch (body.kind) {
    case SyntheticBodyKind::box:
        if (!body.bbox.valid) {
            add_diag(report, DiagnosticSeverity::error, "invalid_synthetic_bbox",
                     "Synthetic box has no valid bounding box.", body.label);
            return ALEA_NODE_ID_INVALID;
        }
        surface_index = alea_box_surface(sys, 0,
                                         body.bbox.xmin, body.bbox.xmax,
                                         body.bbox.ymin, body.bbox.ymax,
                                         body.bbox.zmin, body.bbox.zmax);
        break;
    case SyntheticBodyKind::sphere:
        surface_index = alea_sphere_surface(sys, 0,
                                            body.center.x, body.center.y, body.center.z,
                                            body.radius);
        break;
    case SyntheticBodyKind::cylinder:
        surface_index = alea_rcc_surface(sys, 0,
                                         body.base.x, body.base.y, body.base.z,
                                         body.height.x, body.height.y, body.height.z,
                                         body.radius);
        break;
    case SyntheticBodyKind::cone:
        return emit_synthetic_cone_node(sys, body, report);
    case SyntheticBodyKind::union_body:
    case SyntheticBodyKind::intersection_body:
    case SyntheticBodyKind::difference_body: {
        const SyntheticBody* left = find_synthetic_body(model, body.left_id);
        const SyntheticBody* right = find_synthetic_body(model, body.right_id);
        if (!left || !right) {
            add_diag(report, DiagnosticSeverity::error, "synthetic_child_not_found",
                     "Synthetic CSG body references a missing child.", body.label);
            return ALEA_NODE_ID_INVALID;
        }
        const alea_node_id_t left_node = emit_synthetic_node(sys, model, *left, report);
        const alea_node_id_t right_node = emit_synthetic_node(sys, model, *right, report);
        if (left_node == ALEA_NODE_ID_INVALID || right_node == ALEA_NODE_ID_INVALID) {
            return ALEA_NODE_ID_INVALID;
        }
        if (body.kind == SyntheticBodyKind::union_body) {
            return alea_union(sys, left_node, right_node);
        }
        if (body.kind == SyntheticBodyKind::intersection_body) {
            return alea_intersection(sys, left_node, right_node);
        }
        return alea_difference(sys, left_node, right_node);
    }
    }

    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the synthetic body.",
                 body.label);
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t root = alea_halfspace(sys, surface_index, -1);
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid synthetic body halfspace.", body.label);
        return ALEA_NODE_ID_INVALID;
    }
    return root;
}

bool emit_synthetic_body(alea_system_t* sys,
                         const CadModel& model,
                         const SyntheticBody& body,
                         ConversionReport& report) {
    const alea_node_id_t root = emit_synthetic_node(sys, model, body, report);
    if (root == ALEA_NODE_ID_INVALID) {
        return false;
    }

    int material_index = alea_find_material_by_id(sys, body.material_id);
    if (material_index < 0) {
        material_index = alea_add_material(sys, body.material_id);
    }
    if (material_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_material_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the synthetic body material.",
                 body.label);
        return false;
    }

    const int cell_index = alea_add_cell(sys, 0, root, material_index, body.density, 0);
    if (cell_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the synthetic box cell.",
                 body.label);
        return false;
    }

    const double volume = synthetic_volume_estimate(body);
    report.total_volume += volume;
    report.solid_volumes.push_back({body.label, volume});
    ++report.solids_seen;
    ++report.solids_converted;
    add_diag(report, DiagnosticSeverity::info, "synthetic_body_emitted",
             "Synthetic body was emitted as an Alea cell.", body.label);
    return true;
}

} // namespace

ConversionResult convert_model(const CadModel& model,
                               const ConversionOptions& options) {
    using namespace cad2csg::detail;
    ConversionResult result;
    result.report.diagnostics = model.diagnostics;
    if (model.root.IsNull()) {
        return result;
    }

    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) {
            add_diag(result.report, DiagnosticSeverity::info, "solid_removed",
                     "Solid is suppressed by preprocessing.", solid_label(solid_index));
            continue;
        }
        ++result.report.solids_seen;
        convert_solid(result.system.get(), TopoDS::Solid(solid_exp.Current()),
                      solid_index, options, result.report, 0,
                      &model.face_replacements);
    }

    std::size_t piece_index = 0;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) {
            continue;
        }
        ++piece_index;
        ++result.report.solids_seen;
        ++result.report.decomposition_pieces;
        result.report.decomposed_pieces.push_back(
            analyze_decomposed_piece(piece, piece_index, 100000 + piece_index, options));
        add_diag(result.report, DiagnosticSeverity::info, "decomposed_piece_emitted",
                 "Decomposed solid piece is being converted as an explicit preprocessing result.",
                 piece.label);
        convert_solid(result.system.get(), piece.solid,
                      100000 + piece_index, options, result.report);
    }

    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) {
            continue;
        }
        emit_synthetic_body(result.system.get(), model, body, result.report);
    }

    if (result.report.solids_seen == 0) {
        add_diag(result.report, DiagnosticSeverity::error, "no_active_solids",
                 "No active solids remain after import and preprocessing.");
        return result;
    }

    if (result.report.solids_converted > 0) {
        if (options.split_union_cells) {
            alea_split_union_cells(result.system.get());
        }
        alea_flatten_all_cells(result.system.get(), nullptr);
        if (alea_build_universe_index(result.system.get()) != 0) {
            add_diag(result.report, DiagnosticSeverity::error, "alea_index_failed",
                     alea_error()[0] ? alea_error() : "Alea failed to build the universe index.");
        }
    }

    return result;
}

ConversionResult convert_step_file(const std::string& step_path,
                                   const ConversionOptions& options) {
    CadModel model = load_step_model(step_path, options);
    return convert_model(model, options);
}

} // namespace cad2csg
