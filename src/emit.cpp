// Emit stage: turn an already-prepared CadModel into an AleaSystem of
// surfaces, halfspace nodes, and cells. Refuses geometry that isn't ready
// (non-convex without decomposition, splines without replacement, etc.).

#include "csgify/engine.hpp"
#include "csgify/alea_bridge.hpp"

#include "alea_backend.hpp"
#include "analytic_surface.hpp"
#include "decompose.hpp"
#include "engine_internal.hpp"
#include "fnv.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <sstream>
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

namespace csgify {
namespace {

void tag_alea_cell(alea_system_t* system,
                   int cell_index,
                   const std::vector<std::string>& entity_ids,
                   ConversionReport& report) {
    std::ostringstream comment;
    bool first = true;
    for (const std::string& id : entity_ids) {
        if (id.empty()) continue;
        if (!first) comment << ';';
        first = false;
        comment << "csgify:id=" << id;
    }
    if (first) return;
    if (alea_cell_set_inline_comment(system, cell_index,
                                     comment.str().c_str()) != 0) {
        detail::add_diag(
            report, DiagnosticSeverity::warning, "alea_sidecar_tag_failed",
            alea_error()[0] ? alea_error()
                            : "Alea rejected the csgify cell identity tag.",
            entity_ids.front());
    }
}

using namespace csgify::detail;

struct IntersectionLiteral {
    alea_primitive_id_t primitive = ALEA_PRIMITIVE_ID_INVALID;
    int sense = 0;
    alea_node_id_t node = ALEA_NODE_ID_INVALID;
};

struct IntersectionTerm {
    std::vector<IntersectionLiteral> literals;
    alea_node_id_t node = ALEA_NODE_ID_INVALID;
};

using LiteralKey = std::pair<alea_primitive_id_t, int>;

struct MergeKey {
    std::vector<LiteralKey> common;
    alea_primitive_id_t removed_primitive = ALEA_PRIMITIVE_ID_INVALID;

    bool operator<(const MergeKey& other) const {
        if (removed_primitive != other.removed_primitive) {
            return removed_primitive < other.removed_primitive;
        }
        return common < other.common;
    }
};

bool collect_intersection_literals(const alea_system_t* sys,
                                   alea_node_id_t node,
                                   std::map<alea_primitive_id_t,
                                            IntersectionLiteral>* literals) {
    const alea_operation_t op = alea_node_operation(sys, node);
    if (op == ALEA_OP_INTERSECTION) {
        return collect_intersection_literals(sys, alea_node_left(sys, node), literals)
            && collect_intersection_literals(sys, alea_node_right(sys, node), literals);
    }
    if (op != ALEA_OP_PRIMITIVE) return false;

    const alea_primitive_id_t primitive = alea_node_primitive_id(sys, node);
    const int sense = alea_node_sense(sys, node);
    auto found = literals->find(primitive);
    if (found != literals->end()) {
        // Repeated equal literals are harmless. Opposite literals would make
        // the piece empty and must not be coalesced here.
        return found->second.sense == sense;
    }
    literals->emplace(primitive, IntersectionLiteral{primitive, sense, node});
    return true;
}

bool intersection_term(const alea_system_t* sys,
                       alea_node_id_t node,
                       IntersectionTerm* term) {
    std::map<alea_primitive_id_t, IntersectionLiteral> sorted;
    if (!collect_intersection_literals(sys, node, &sorted)) return false;
    term->literals.clear();
    term->literals.reserve(sorted.size());
    for (const auto& entry : sorted) term->literals.push_back(entry.second);
    term->node = node;
    return !term->literals.empty();
}

std::vector<LiteralKey> term_key(const IntersectionTerm& term) {
    std::vector<LiteralKey> key;
    key.reserve(term.literals.size());
    for (const IntersectionLiteral& literal : term.literals) {
        key.emplace_back(literal.primitive, literal.sense);
    }
    return key;
}

// Exact Boolean coalescing for a union of convex half-space intersections:
// (A & +H) | (A & -H) = A. Repeating this rule removes artificial BSP cuts
// and yields maximal intersections without running O(piece_count^2) CAD fuses.
std::vector<alea_node_id_t> coalesce_intersection_terms(
    alea_system_t* sys,
    const std::vector<alea_node_id_t>& nodes) {
    std::vector<IntersectionTerm> terms;
    terms.reserve(nodes.size());
    for (alea_node_id_t node : nodes) {
        IntersectionTerm term;
        if (!intersection_term(sys, node, &term)) return nodes;
        terms.push_back(std::move(term));
    }

    while (terms.size() > 1) {
        struct Candidates {
            std::vector<std::size_t> negative;
            std::vector<std::size_t> positive;
        };
        std::map<MergeKey, Candidates> candidates;
        for (std::size_t term_index = 0; term_index < terms.size(); ++term_index) {
            const IntersectionTerm& term = terms[term_index];
            for (std::size_t removed = 0; removed < term.literals.size(); ++removed) {
                MergeKey key;
                key.removed_primitive = term.literals[removed].primitive;
                key.common.reserve(term.literals.size() - 1);
                for (std::size_t i = 0; i < term.literals.size(); ++i) {
                    if (i != removed) {
                        key.common.emplace_back(term.literals[i].primitive,
                                                term.literals[i].sense);
                    }
                }
                std::vector<std::size_t>& side = term.literals[removed].sense < 0
                    ? candidates[key].negative : candidates[key].positive;
                side.push_back(term_index);
            }
        }

        std::vector<bool> consumed(terms.size(), false);
        std::vector<IntersectionTerm> merged;
        bool made_progress = false;
        for (const auto& entry : candidates) {
            const Candidates& sides = entry.second;
            for (std::size_t neg : sides.negative) {
                if (consumed[neg]) continue;
                const auto pos_it = std::find_if(
                    sides.positive.begin(), sides.positive.end(),
                    [&](std::size_t pos) { return !consumed[pos]; });
                if (pos_it == sides.positive.end()) break;
                const std::size_t pos = *pos_it;
                if (entry.first.common.empty()) continue;

                IntersectionTerm common;
                for (const IntersectionLiteral& literal : terms[neg].literals) {
                    if (literal.primitive != entry.first.removed_primitive) {
                        common.literals.push_back(literal);
                    }
                }
                std::vector<alea_node_id_t> common_nodes;
                common_nodes.reserve(common.literals.size());
                for (const IntersectionLiteral& literal : common.literals) {
                    common_nodes.push_back(literal.node);
                }
                common.node = common_nodes.size() == 1
                    ? common_nodes.front()
                    : alea_intersection_n(sys, common_nodes.data(), common_nodes.size());
                if (common.node == ALEA_NODE_ID_INVALID) continue;

                consumed[neg] = true;
                consumed[pos] = true;
                merged.push_back(std::move(common));
                made_progress = true;
            }
        }
        if (!made_progress) break;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (!consumed[i]) merged.push_back(std::move(terms[i]));
        }

        // Coalescing through different paths can produce identical terms.
        std::map<std::vector<LiteralKey>, std::size_t> unique;
        std::vector<IntersectionTerm> deduplicated;
        deduplicated.reserve(merged.size());
        for (IntersectionTerm& term : merged) {
            const std::vector<LiteralKey> key = term_key(term);
            if (unique.emplace(key, deduplicated.size()).second) {
                deduplicated.push_back(std::move(term));
            }
        }
        terms = std::move(deduplicated);
    }

    std::vector<alea_node_id_t> result;
    result.reserve(terms.size());
    for (const IntersectionTerm& term : terms) result.push_back(term.node);
    return result;
}

bool add_face_halfspace(alea_system_t* sys,
                        const TopoDS_Face& face,
                        const gp_Pnt& interior_point,
                        std::size_t solid_index,
                        std::size_t face_index,
                        const ConversionOptions& options,
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

    const double distance = surface.signed_distance_at(interior_point);
    const int sense = distance <= options.linear_tolerance ? -1 : 1;
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
                   const std::vector<FaceReplacement>* replacements = nullptr,
                   const std::string& bridge_entity_id = {}) {
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
                                   replacements, bridge_entity_id)) {
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
                                solid_index, face_index, options, report, &node,
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

    tag_alea_cell(sys, cell_index,
                  {bridge_entity_id.empty() ? solid_id(solid_index)
                                            : bridge_entity_id},
                  report);

    ++report.solids_converted;
    return true;
}

// Build the intersection-of-halfspaces node for a piece, without creating a
// cell. Returns INVALID on any failure. Used by the union-emission path so a
// group of pieces can be combined into one cell. Diagnostics go to the
// caller's report (which is a scratch report during the union trial).
alea_node_id_t build_piece_intersection_node(
        alea_system_t* sys,
        const TopoDS_Solid& solid,
        const gp_Pnt& interior_point,
        std::size_t solid_index,
        const ConversionOptions& options,
        ConversionReport& report,
        const std::vector<FaceReplacement>* replacements) {
    std::vector<alea_node_id_t> nodes;
    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        alea_node_id_t node = ALEA_NODE_ID_INVALID;
        if (!add_face_halfspace(sys, TopoDS::Face(face_exp.Current()), interior_point,
                                solid_index, face_index, options, report, &node,
                                replacements)) {
            return ALEA_NODE_ID_INVALID;
        }
        nodes.push_back(node);
    }
    if (nodes.empty()) {
        return ALEA_NODE_ID_INVALID;
    }
    return alea_intersection_n(sys, nodes.data(), nodes.size());
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

    tag_alea_cell(sys, cell_index, {body.id}, report);

    const double volume = synthetic_volume_estimate(body);
    report.total_volume += volume;
    report.solid_volumes.push_back({body.label, volume});
    ++report.solids_seen;
    ++report.solids_converted;
    add_diag(report, DiagnosticSeverity::info, "synthetic_body_emitted",
             "Synthetic body was emitted as an Alea cell.", body.label);
    return true;
}

// Emit the model's decomposed pieces. By default pieces from one source solid
// are reunited before Alea simplification. When union_decomposed_pieces is
// explicitly disabled, each piece becomes its own Alea cell (convert_solid).
// Otherwise,
// pieces sharing a source_solid_id are validated, built as intersection
// nodes, and combined into a single union cell, so Alea's simplify/dedup pass
// collapses the cut surfaces shared between adjacent pieces and the original
// CAD solid stays one logical body. If any piece in a group is not a simple
// convex cell, the whole group falls back to per-piece emission so the
// honest per-piece diagnostics still surface.
void emit_decomposed_pieces(alea_system_t* sys,
                            const CadModel& model,
                            const ConversionOptions& options,
                            ConversionReport& report) {
    std::size_t piece_index = 0;
    auto report_piece = [&](const DecomposedSolidPiece& piece) -> std::size_t {
        ++piece_index;
        ++report.solids_seen;
        ++report.decomposition_pieces;
        report.decomposed_pieces.push_back(
            analyze_decomposed_piece(piece, piece_index, 100000 + piece_index, options));
        return 100000 + piece_index;
    };

    if (!options.union_decomposed_pieces) {
        std::size_t active_piece_count = 0;
        for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
            if (!piece.active) continue;
            ++active_piece_count;
            const std::size_t idx = report_piece(piece);
            convert_solid(sys, piece.solid, idx, options, report, 0, nullptr,
                          piece.source_solid_id);
        }
        if (active_piece_count > 0) {
            add_diag(report, DiagnosticSeverity::info,
                     "decomposed_pieces_emitted",
                     std::to_string(active_piece_count)
                         + " explicit decomposed pieces were converted.");
        }
        return;
    }

    // Group active pieces by source_solid_id, preserving first-seen order.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const DecomposedSolidPiece*>> groups;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) continue;
        if (groups.find(piece.source_solid_id) == groups.end()) {
            order.push_back(piece.source_solid_id);
        }
        groups[piece.source_solid_id].push_back(&piece);
    }

    for (const std::string& key : order) {
        const std::vector<const DecomposedSolidPiece*>& pieces = groups[key];

        // Trial: build an intersection node for every piece in the group.
        std::vector<alea_node_id_t> piece_nodes;
        bool all_simple = true;
        ConversionReport scratch;
        for (const DecomposedSolidPiece* piece : pieces) {
            GProp_GProps props;
            BRepGProp::VolumeProperties(piece->solid, props);
            gp_Pnt interior;
            if (!find_interior_point(piece->solid, props, options.linear_tolerance, &interior)) {
                all_simple = false;
                break;
            }
            if (options.require_convex_cells
                && !validate_convex_halfspace_cell(piece->solid, interior, 100000, options,
                                                   scratch, false, false, false,
                                                   &model.face_replacements)) {
                all_simple = false;
                break;
            }
            const alea_node_id_t node = build_piece_intersection_node(
                sys, piece->solid, interior, 100000, options, scratch,
                &model.face_replacements);
            if (node == ALEA_NODE_ID_INVALID) {
                all_simple = false;
                break;
            }
            piece_nodes.push_back(node);
        }

        if (all_simple && !piece_nodes.empty()) {
            for (const DecomposedSolidPiece* piece : pieces) {
                report_piece(*piece);
            }
            const std::vector<alea_node_id_t> coalesced =
                coalesce_intersection_terms(sys, piece_nodes);
            const alea_node_id_t root = coalesced.size() == 1
                ? coalesced.front()
                : alea_union_n(sys, coalesced.data(), coalesced.size());
            if (coalesced.size() < piece_nodes.size()) {
                add_diag(report, DiagnosticSeverity::info,
                         "decomposed_union_coalesced",
                         std::to_string(piece_nodes.size())
                             + " decomposed intersections were reduced to "
                             + std::to_string(coalesced.size())
                             + " exact intersection term(s).",
                         key);
            }
            int material_index = alea_find_material_by_id(sys, options.default_material_id);
            if (material_index < 0) {
                material_index = alea_add_material(sys, options.default_material_id);
            }
            const int cell_index = root != ALEA_NODE_ID_INVALID
                    && material_index >= 0
                ? alea_add_cell(sys, 0, root, material_index,
                                options.default_density, 0)
                : -1;
            if (cell_index >= 0) {
                // Public piece counters are transaction-local and may differ
                // on deterministic recipe replay. Bind the final Alea cell to
                // its stable source entity; the manifest still records the
                // final cell expression and numbering.
                tag_alea_cell(sys, cell_index, {key}, report);
                ++report.solids_converted;
                double group_volume = 0.0;
                for (const DecomposedSolidPiece* piece : pieces) {
                    if (piece->volume > 0.0) {
                        group_volume += piece->volume;
                    } else {
                        GProp_GProps props;
                        BRepGProp::VolumeProperties(piece->solid, props);
                        group_volume += std::abs(props.Mass());
                    }
                }
                report.total_volume += group_volume;
                report.solid_volumes.push_back({key, group_volume});
            } else {
                add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                         alea_error()[0] ? alea_error()
                                         : "Alea failed to add the decomposed-piece union cell.",
                         key);
            }
        } else {
            // Honest fallback: emit each piece on its own so per-piece
            // diagnostics (and any further decomposition) still happen.
            for (const DecomposedSolidPiece* piece : pieces) {
                const std::size_t idx = report_piece(*piece);
                convert_solid(sys, piece->solid, idx, options, report, 0,
                              nullptr, piece->source_solid_id);
            }
        }
    }
}

bool emit_cad_boolean_body(alea_system_t* sys,
                           const CadModel& model,
                           const CadBooleanBody& body,
                           const ConversionOptions& options,
                           ConversionReport& report) {
    auto build_nodes = [&](const std::vector<TopoDS_Solid>& cells,
                           std::vector<alea_node_id_t>* nodes) {
        for (const TopoDS_Solid& cell : cells) {
            GProp_GProps props;
            BRepGProp::VolumeProperties(cell, props);
            gp_Pnt interior;
            if (!find_interior_point(cell, props, options.linear_tolerance,
                                     &interior)) return false;
            GeometryCertificate certificate;
            if (!validate_convex_halfspace_cell(
                    cell, interior, 300000, options, report, false, false,
                    false, &model.face_replacements, &certificate)) {
                return false;
            }
            const alea_node_id_t node = build_piece_intersection_node(
                sys, cell, interior, 300000, options, report,
                &model.face_replacements);
            if (node == ALEA_NODE_ID_INVALID) return false;
            nodes->push_back(node);
        }
        return !nodes->empty();
    };
    std::vector<alea_node_id_t> positive;
    std::vector<alea_node_id_t> negative;
    if (!build_nodes(body.positive_cells, &positive)
        || (!body.negative_cells.empty()
            && !build_nodes(body.negative_cells, &negative))) {
        add_diag(report, DiagnosticSeverity::error,
                 "cad_boolean_cell_failed",
                 "A CAD shell could not be emitted as a convex analytic cell.",
                 body.label);
        return false;
    }
    const alea_node_id_t outer = positive.size() == 1
        ? positive.front() : alea_union_n(sys, positive.data(), positive.size());
    alea_node_id_t root = outer;
    if (!negative.empty()) {
        const alea_node_id_t cavities = negative.size() == 1
            ? negative.front() : alea_union_n(sys, negative.data(), negative.size());
        root = alea_difference(sys, outer, cavities);
    }
    const int requested_material = body.material_id > 0
        ? body.material_id : options.default_material_id;
    const double requested_density = body.material_id > 0
        ? body.density : options.default_density;
    int material_index = alea_find_material_by_id(sys, requested_material);
    if (material_index < 0) {
        material_index = alea_add_material(sys, requested_material);
    }
    const int cell_index = root != ALEA_NODE_ID_INVALID && material_index >= 0
        ? alea_add_cell(sys, 0, root, material_index, requested_density, 0)
        : -1;
    if (cell_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                 "Alea failed to add the CAD shell-difference cell.", body.label);
        return false;
    }
    tag_alea_cell(sys, cell_index, {body.id, body.source_solid_id}, report);
    GProp_GProps props;
    BRepGProp::VolumeProperties(body.materialized_solid, props);
    const double volume = std::abs(props.Mass());
    ++report.solids_seen;
    ++report.solids_converted;
    report.total_volume += volume;
    report.solid_volumes.push_back({body.label, volume});
    add_diag(report, DiagnosticSeverity::info, "cad_boolean_body_emitted",
             negative.empty()
                 ? "Emitted an exact union of certified CAD cells."
                 : "Emitted an exact union of certified CAD cells minus obstacles/cavities.",
             body.label);
    return true;
}

} // namespace

ConversionResult convert_model(const CadModel& model,
                               const ConversionOptions& options) {
    using namespace csgify::detail;
    ConversionResult result;
    result.report.diagnostics = model.diagnostics;
    result.report.source_path = model.source_path;
    result.report.length_unit = model.length_unit;
    result.report.length_unit_to_mm = model.length_unit_to_mm;
    result.report.assembly = model.assembly;
    result.report.strings = model.strings;
    result.report.metadata = model.metadata;
    result.report.component_definitions = model.component_definitions;
    result.report.component_instances = model.component_instances;
    result.report.materials = model.materials;
    result.report.medium_requests = model.medium_requests;
    result.report.derived_regions = model.derived_regions;
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
                      &model.face_replacements, id);
    }

    emit_decomposed_pieces(result.system.get(), model, options, result.report);

    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (body.active && body.emit_as_cell) {
            emit_cad_boolean_body(result.system.get(), model, body, options,
                                  result.report);
        }
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
        std::string finalize_error;
        if (!finalize_alea_system(result.system.get(),
                                  options.split_union_cells,
                                  &finalize_error)) {
            add_diag(result.report, DiagnosticSeverity::error, "alea_finalize_failed",
                     finalize_error);
        }
    }

    std::string source_digest;
    if (!model.source_path.empty()) {
        const std::string raw = fnv1a128_file_hex(model.source_path);
        if (!raw.empty()) source_digest = "fnv1a128:" + raw;
    }
    result.handoff = make_alea_handoff_manifest(
        result.system.get(), result.report, source_digest,
        alea_bridge_policy_digest(options));
    const AleaBridgeNegotiation negotiation = negotiate_alea_bridge(
        result.handoff.capabilities);
    std::string handoff_error;
    const bool handoff_valid = validate_alea_handoff_manifest(
        result.handoff, &handoff_error);
    result.report.alea_handoff.attempted = true;
    result.report.alea_handoff.compatible =
        negotiation.compatible && handoff_valid;
    result.report.alea_handoff.schema_major = result.handoff.schema_major;
    result.report.alea_handoff.schema_minor = result.handoff.schema_minor;
    result.report.alea_handoff.libalea_version = result.handoff.libalea_version;
    result.report.alea_handoff.contract_digest = result.handoff.contract_digest;
    result.report.alea_handoff.cell_count = result.handoff.cells.size();
    result.report.alea_handoff.surface_count = result.handoff.surfaces.size();
    result.report.alea_handoff.universe_count = result.handoff.universes.size();
    result.report.alea_handoff.sidecar_entity_count = result.handoff.sidecar.size();
    result.report.alea_handoff.chunk_count = result.handoff.chunks.size();
    result.report.alea_handoff.missing_capabilities =
        negotiation.missing_capabilities;
    if (!handoff_valid) {
        add_diag(result.report, DiagnosticSeverity::error,
                 "alea_handoff_manifest_invalid", handoff_error);
    }

    return result;
}

ConversionResult convert_step_file(const std::string& step_path,
                                   const ConversionOptions& options) {
    CadModel model = load_step_model(step_path, options);
    return convert_model(model, options);
}

} // namespace csgify
