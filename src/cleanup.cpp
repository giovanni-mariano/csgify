// Cleanup / Geometric Repair stage. The mutator boundary on `CadModel`:
// every command here either records a decision (face replacement, removal)
// or attaches a synthetic body / decomposed piece. Face replacement records
// analytic overrides; when all faces of a simple spline primitive are
// replaced, the original solid is suppressed and a synthetic analytic body is
// materialized so review/export paths do not keep the source spline surfaces.

#include "csgify/engine.hpp"

#include "cleanup_internal.hpp"

#include "analytic_surface.hpp"
#include "approximation_internal.hpp"
#include "decompose.hpp"
#include "topology_analysis.hpp"
#include "engine_internal.hpp"
#include "fitted_candidates.hpp"
#include "occt_boolean.hpp"
#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass3d.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Cone.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>
#include <gp_Trsf.hxx>

namespace csgify {
namespace {

using namespace csgify::detail;

std::string next_synthetic_id(const CadModel& model) {
    return "synthetic:" + std::to_string(model.synthetic_bodies.size() + 1);
}

std::string next_decomposed_piece_id(const CadModel& model) {
    return "piece:" + std::to_string(model.decomposed_pieces.size() + 1);
}

bool decomposition_volume_guard(CadModel& model,
                                const std::string& shape_id,
                                double relative_delta,
                                const ConversionOptions& options) {
    if (!std::isfinite(options.decomposition_relative_volume_tolerance)
        || options.decomposition_relative_volume_tolerance < 0.0) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_decomposition_volume_tolerance",
            "decomposition_relative_volume_tolerance must be finite and non-negative.",
            shape_id});
        return false;
    }
    if (relative_delta > options.decomposition_relative_volume_tolerance) {
        std::ostringstream message;
        message << "Split relative volume delta " << std::setprecision(17)
                << relative_delta << " exceeds decomposition tolerance "
                << options.decomposition_relative_volume_tolerance << ".";
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "decomposition_volume_drift",
            message.str(),
            shape_id});
        return false;
    }
    return true;
}

bool parse_piece_id(const std::string& id, std::size_t* index_out) {
    constexpr const char* prefix = "piece:";
    const std::size_t prefix_len = 6;
    if (id.compare(0, prefix_len, prefix) != 0 || id.size() == prefix_len) {
        return false;
    }
    std::size_t value = 0;
    for (std::size_t i = prefix_len; i < id.size(); ++i) {
        const char ch = id[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (value == 0) {
        return false;
    }
    *index_out = value;
    return true;
}

bool parse_piece_face_id(const std::string& id,
                         std::string* piece_id_out,
                         std::size_t* face_index_out) {
    constexpr const char* marker = "/face:";
    const std::size_t marker_pos = id.find(marker);
    if (marker_pos == std::string::npos || marker_pos == 0) {
        return false;
    }
    const std::string piece_id = id.substr(0, marker_pos);
    std::size_t piece_index = 0;
    if (!parse_piece_id(piece_id, &piece_index)) {
        return false;
    }
    std::size_t value = 0;
    const std::size_t start = marker_pos + 6;
    if (start >= id.size()) {
        return false;
    }
    for (std::size_t i = start; i < id.size(); ++i) {
        const char ch = id[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (value == 0) {
        return false;
    }
    *piece_id_out = piece_id;
    *face_index_out = value;
    return true;
}

bool find_solid_by_id(const CadModel& model,
                      const std::string& id,
                      TopoDS_Solid* solid_out,
                      std::size_t* index_out) {
    std::size_t target_index = 0;
    if (!parse_solid_id(id, &target_index)) {
        return false;
    }
    const std::string canonical_id = solid_id(target_index);
    if (contains_id(model.removed_solid_ids, canonical_id)
        || contains_id(model.removed_solid_ids, id)) {
        return false;
    }
    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        if (solid_index == target_index) {
            *solid_out = TopoDS::Solid(solid_exp.Current());
            *index_out = solid_index;
            return true;
        }
    }
    return false;
}

DecomposedSolidPiece* find_piece_by_id(CadModel& model, const std::string& id) {
    for (DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (piece.id == id && piece.active) {
            return &piece;
        }
    }
    return nullptr;
}

const DecomposedSolidPiece* find_piece_by_id(const CadModel& model,
                                             const std::string& id) {
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (piece.id == id && piece.active) {
            return &piece;
        }
    }
    return nullptr;
}

struct DecompositionTarget {
    TopoDS_Solid solid;
    std::size_t report_index = 0;
    bool is_piece = false;
    DecomposedSolidPiece* piece = nullptr;
};

std::string decomposition_source_id(const DecompositionTarget& target,
                                    const std::string& requested_id) {
    if (target.is_piece && target.piece
        && !target.piece->source_solid_id.empty()) {
        return target.piece->source_solid_id;
    }
    return requested_id;
}

bool find_decomposition_target(CadModel& model,
                               const std::string& id,
                               DecompositionTarget* target) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (find_solid_by_id(model, id, &solid, &index)) {
        target->solid = solid;
        target->report_index = index;
        target->is_piece = false;
        target->piece = nullptr;
        return true;
    }
    if (DecomposedSolidPiece* piece = find_piece_by_id(model, id)) {
        std::size_t piece_number = 0;
        parse_piece_id(id, &piece_number);
        target->solid = piece->solid;
        target->report_index = 100000 + piece_number;
        target->is_piece = true;
        target->piece = piece;
        return true;
    }
    return false;
}

bool find_decomposition_target(const CadModel& model,
                               const std::string& id,
                               TopoDS_Solid* solid_out,
                               std::size_t* report_index_out,
                               bool* is_piece_out) {
    std::size_t index = 0;
    if (find_solid_by_id(model, id, solid_out, &index)) {
        *report_index_out = index;
        *is_piece_out = false;
        return true;
    }
    if (const DecomposedSolidPiece* piece = find_piece_by_id(model, id)) {
        std::size_t piece_number = 0;
        parse_piece_id(id, &piece_number);
        *solid_out = piece->solid;
        *report_index_out = 100000 + piece_number;
        *is_piece_out = true;
        return true;
    }
    return false;
}

const SplineFaceFit* select_fit(const SplineFitCandidates& fits, SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::plane:    return &fits.plane;
    case SplineFitKind::sphere:   return &fits.sphere;
    case SplineFitKind::cylinder: return &fits.cylinder;
    case SplineFitKind::cone:     return &fits.cone;
    case SplineFitKind::none:     return nullptr;
    }
    return nullptr;
}

bool find_face_in_root(const TopoDS_Shape& root,
                       std::size_t target_solid_index,
                       std::size_t target_face_index,
                       TopoDS_Face* face_out) {
    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        if (solid_index != target_solid_index) {
            continue;
        }
        std::size_t face_index = 0;
        for (TopExp_Explorer face_exp(solid_exp.Current(), TopAbs_FACE);
             face_exp.More(); face_exp.Next()) {
            ++face_index;
            if (face_index == target_face_index) {
                *face_out = TopoDS::Face(face_exp.Current());
                return true;
            }
        }
        return false;
    }
    return false;
}

bool find_face_in_solid(const TopoDS_Solid& solid,
                        std::size_t target_face_index,
                        TopoDS_Face* face_out) {
    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
         face_exp.More(); face_exp.Next()) {
        ++face_index;
        if (face_index == target_face_index) {
            *face_out = TopoDS::Face(face_exp.Current());
            return true;
        }
    }
    return false;
}

SplineFitKind analytic_fit_kind_from_face(const TopoDS_Face& face,
                                          SplineFaceFit* fit_out) {
    BRepAdaptor_Surface adaptor(face, false);
    switch (adaptor.GetType()) {
    case GeomAbs_Plane: {
        const gp_Pln plane = adaptor.Plane();
        const gp_Dir normal = plane.Axis().Direction();
        fit_out->kind = SplineFitKind::plane;
        fit_out->available = true;
        fit_out->origin = {plane.Location().X(),
                           plane.Location().Y(),
                           plane.Location().Z()};
        fit_out->direction = {normal.X(), normal.Y(), normal.Z()};
        return SplineFitKind::plane;
    }
    case GeomAbs_Sphere: {
        const gp_Sphere sphere = adaptor.Sphere();
        fit_out->kind = SplineFitKind::sphere;
        fit_out->available = true;
        fit_out->origin = {sphere.Location().X(),
                           sphere.Location().Y(),
                           sphere.Location().Z()};
        fit_out->radius = sphere.Radius();
        return SplineFitKind::sphere;
    }
    case GeomAbs_Cylinder: {
        const gp_Cylinder cylinder = adaptor.Cylinder();
        const gp_Dir axis = cylinder.Axis().Direction();
        fit_out->kind = SplineFitKind::cylinder;
        fit_out->available = true;
        fit_out->origin = {cylinder.Location().X(),
                           cylinder.Location().Y(),
                           cylinder.Location().Z()};
        fit_out->direction = {axis.X(), axis.Y(), axis.Z()};
        fit_out->radius = cylinder.Radius();
        return SplineFitKind::cylinder;
    }
    case GeomAbs_Cone: {
        const gp_Cone cone = adaptor.Cone();
        gp_Dir axis = cone.Axis().Direction();
        double half_angle = cone.SemiAngle();
        if (half_angle < 0.0) {
            axis.Reverse();
            half_angle = -half_angle;
        }
        const gp_Pnt apex = cone.Apex();
        fit_out->kind = SplineFitKind::cone;
        fit_out->available = true;
        fit_out->origin = {apex.X(), apex.Y(), apex.Z()};
        fit_out->direction = {axis.X(), axis.Y(), axis.Z()};
        fit_out->half_angle = half_angle;
        return SplineFitKind::cone;
    }
    default:
        return SplineFitKind::none;
    }
}

bool projected_span(const TopoDS_Solid& solid,
                    Vec3 origin,
                    Vec3 axis,
                    double* min_t_out,
                    double* max_t_out) {
    std::vector<Vec3> points;
    for (const gp_Pnt& p : solid_vertices(solid)) {
        points.push_back({p.X(), p.Y(), p.Z()});
    }
    if (points.empty()) {
        const BBox bbox = bbox_from_shape(solid);
        if (!bbox.valid) {
            return false;
        }
        points = bbox_corners(bbox);
    }

    bool have = false;
    double min_t = 0.0;
    double max_t = 0.0;
    for (Vec3 p : points) {
        const double t = dot(sub(p, origin), axis);
        if (!have) {
            min_t = t;
            max_t = t;
            have = true;
        } else {
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }
    }
    if (!have || max_t - min_t <= kEps) {
        return false;
    }
    *min_t_out = min_t;
    *max_t_out = max_t;
    return true;
}

bool normalize_vec(Vec3* v) {
    const double length = vec_len(*v);
    if (length <= kEps) {
        return false;
    }
    *v = scale(*v, 1.0 / length);
    return true;
}

bool materialize_fully_replaced_primitive(CadModel& model,
                                          const std::string& solid_id_str,
                                          const TopoDS_Solid& solid,
                                          std::size_t solid_index,
                                          const ConversionOptions& options) {
    if (contains_id(model.removed_solid_ids, solid_id_str)) {
        return false;
    }

    std::size_t face_index = 0;
    std::size_t face_count = 0;
    std::size_t plane_count = 0;
    std::size_t cylinder_count = 0;
    std::size_t cone_count = 0;
    std::size_t sphere_count = 0;
    std::size_t replaced_count = 0;
    SplineFaceFit cylinder_fit;
    SplineFaceFit cone_fit;
    SplineFaceFit sphere_fit;

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
         face_exp.More(); face_exp.Next()) {
        ++face_index;
        ++face_count;
        SplineFaceFit fit;
        SplineFitKind kind = SplineFitKind::none;
        if (const FaceReplacement* replacement =
                find_face_replacement(&model.face_replacements,
                                      solid_index,
                                      face_index)) {
            kind = replacement->kind;
            fit = replacement->fit;
            ++replaced_count;
        } else {
            kind = analytic_fit_kind_from_face(TopoDS::Face(face_exp.Current()), &fit);
        }

        switch (kind) {
        case SplineFitKind::plane:
            ++plane_count;
            break;
        case SplineFitKind::sphere:
            ++sphere_count;
            sphere_fit = fit;
            break;
        case SplineFitKind::cylinder:
            ++cylinder_count;
            cylinder_fit = fit;
            break;
        case SplineFitKind::cone:
            ++cone_count;
            cone_fit = fit;
            break;
        case SplineFitKind::none:
            return false;
        }
    }

    if (replaced_count == 0) {
        return false;
    }

    if (face_count == 1 && sphere_count == 1
        && sphere_fit.available && sphere_fit.radius > kEps) {
        remove_solid(model, solid_id_str);
        add_synthetic_sphere(model, sphere_fit.origin, sphere_fit.radius,
                             options.default_material_id,
                             options.default_density,
                             next_synthetic_id(model)
                                 + "/spline_fit_sphere_from_" + solid_id_str);
        model.diagnostics.push_back({
            DiagnosticSeverity::info,
            "spline_solid_materialized",
            "Fully replaced spline solid was materialized as a synthetic sphere.",
            solid_id_str
        });
        return true;
    }

    if (face_count == 3 && cylinder_count == 1 && plane_count == 2
        && cylinder_fit.available && cylinder_fit.radius > kEps) {
        Vec3 axis = cylinder_fit.direction;
        if (!normalize_vec(&axis)) {
            return false;
        }
        double min_t = 0.0;
        double max_t = 0.0;
        if (!projected_span(solid, cylinder_fit.origin, axis, &min_t, &max_t)) {
            return false;
        }
        const Vec3 base = add(cylinder_fit.origin, scale(axis, min_t));
        const Vec3 height = scale(axis, max_t - min_t);
        remove_solid(model, solid_id_str);
        add_synthetic_cylinder(model, base, height, cylinder_fit.radius,
                               options.default_material_id,
                               options.default_density,
                               next_synthetic_id(model)
                                   + "/spline_fit_cylinder_from_" + solid_id_str);
        model.diagnostics.push_back({
            DiagnosticSeverity::info,
            "spline_solid_materialized",
            "Fully replaced spline solid was materialized as a synthetic cylinder.",
            solid_id_str
        });
        return true;
    }

    if (face_count == 3 && cone_count == 1 && plane_count == 2
        && cone_fit.available && cone_fit.half_angle > kEps) {
        Vec3 axis = cone_fit.direction;
        if (!normalize_vec(&axis)) {
            return false;
        }
        double min_t = 0.0;
        double max_t = 0.0;
        if (!projected_span(solid, cone_fit.origin, axis, &min_t, &max_t)) {
            return false;
        }
        const double slope = std::tan(cone_fit.half_angle);
        const double base_radius = std::abs(min_t) * slope;
        const double top_radius = std::abs(max_t) * slope;
        if (base_radius <= kEps && top_radius <= kEps) {
            return false;
        }
        const Vec3 base = add(cone_fit.origin, scale(axis, min_t));
        const Vec3 height = scale(axis, max_t - min_t);
        remove_solid(model, solid_id_str);
        add_synthetic_cone(model, base, height, base_radius, top_radius,
                           options.default_material_id,
                           options.default_density,
                           next_synthetic_id(model)
                               + "/spline_fit_cone_from_" + solid_id_str);
        model.diagnostics.push_back({
            DiagnosticSeverity::info,
            "spline_solid_materialized",
            "Fully replaced spline solid was materialized as a synthetic cone.",
            solid_id_str
        });
        return true;
    }

    return false;
}

} // namespace

bool active_shape_for_id(const CadModel& model,
                         const std::string& id,
                         TopoDS_Shape* shape_out,
                         std::string* source_solid_id_out) {
    if (!shape_out) return false;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (find_solid_by_id(model, id, &solid, &index)) {
        *shape_out = solid;
        if (source_solid_id_out) *source_solid_id_out = solid_id(index);
        return true;
    }
    if (const DecomposedSolidPiece* piece = find_piece_by_id(model, id)) {
        *shape_out = piece->solid;
        if (source_solid_id_out) {
            *source_solid_id_out = piece->source_solid_id.empty()
                ? piece->id : piece->source_solid_id;
        }
        return true;
    }
    if (const SyntheticBody* body = detail::find_synthetic_body(model, id)) {
        const TopoDS_Shape shape = detail::build_synthetic_shape(model, *body);
        if (shape.IsNull()) return false;
        *shape_out = shape;
        if (source_solid_id_out) {
            *source_solid_id_out = body->source_solid_id.empty()
                ? body->id : body->source_solid_id;
        }
        return true;
    }
    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (body.id == id && body.active && !body.materialized_solid.IsNull()) {
            *shape_out = body.materialized_solid;
            if (source_solid_id_out) {
                *source_solid_id_out = body.source_solid_id.empty()
                    ? body.id : body.source_solid_id;
            }
            return true;
        }
    }
    return false;
}

bool remove_solid(CadModel& model, const std::string& id) {
    if (id.rfind("piece:", 0) == 0) {
        for (DecomposedSolidPiece& piece : model.decomposed_pieces) {
            if (piece.id == id && piece.active) {
                piece.active = false;
                model.diagnostics.push_back({
                    DiagnosticSeverity::info,
                    "decomposed_piece_removed",
                    "Decomposition piece was explicitly suppressed.",
                    id});
                return true;
            }
        }
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested active decomposition piece was not found.",
            id});
        return false;
    }
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }
    if (!detail::contains_id(model.removed_solid_ids, id)) {
        model.removed_solid_ids.push_back(id);
    }
    return true;
}

bool replace_face_with_fit(CadModel& model,
                           const std::string& face_id_str,
                           SplineFitKind kind,
                           bool has_max_deviation,
                           double max_deviation,
                           Diagnostic* diagnostic_out,
                           const ConversionOptions& options) {
    using namespace csgify::detail;
    std::string diagnostic_label = face_id_str;
    std::string diagnostic_id = face_id_str;
    auto emit = [&](DiagnosticSeverity sev, const char* code, const std::string& msg) {
        Diagnostic d{sev, code, msg, diagnostic_label};
        d.shape_id = diagnostic_id;
        if (diagnostic_out) {
            *diagnostic_out = d;
        }
        model.diagnostics.push_back(d);
    };

    std::size_t solid_index = 0;
    std::size_t face_index = 0;
    if (!parse_face_id(face_id_str, &solid_index, &face_index)) {
        emit(DiagnosticSeverity::error, "invalid_face_id",
             "Face ID must look like face:N.M with positive N and M.");
        return false;
    }
    diagnostic_label = face_label(solid_index, face_index);

    TopoDS_Face face;
    if (!find_face_in_root(model.root, solid_index, face_index, &face)) {
        emit(DiagnosticSeverity::error, "face_not_found",
             "Requested face ID was not found in the loaded model.");
        return false;
    }

    BRepAdaptor_Surface adaptor(face, false);
    const GeomAbs_SurfaceType type = adaptor.GetType();
    if (!surface_type_is_spline(type)) {
        emit(DiagnosticSeverity::error, "face_not_spline",
             "Face replacement is only defined for B-spline or Bezier faces.");
        return false;
    }

    const SplineFitCandidates fits = fit_spline_face(face);
    const SplineFaceFit* selected = select_fit(fits, kind);
    if (!selected || !selected->available) {
        emit(DiagnosticSeverity::error, "fit_unavailable",
             "Selected analytic fit is not available for this face.");
        return false;
    }
    if (has_max_deviation && selected->max_deviation > max_deviation) {
        std::ostringstream msg;
        msg << "Fit max_deviation=" << selected->max_deviation
            << " exceeds budget " << max_deviation << ".";
        emit(DiagnosticSeverity::error, "fit_deviation_exceeded", msg.str());
        return false;
    }

    // Remove any prior replacement for the same face, then record the new one.
    for (auto it = model.face_replacements.begin();
         it != model.face_replacements.end();) {
        if (it->face_id == face_id_str) {
            it = model.face_replacements.erase(it);
        } else {
            ++it;
        }
    }
    FaceReplacement record;
    record.face_id = face_id_str;
    record.kind = kind;
    record.fit = *selected;
    record.has_max_deviation = has_max_deviation;
    record.max_deviation = max_deviation;
    model.face_replacements.push_back(record);

    std::ostringstream info;
    info << "Face replaced with " << spline_fit_kind_name(kind)
         << " (max_deviation=" << selected->max_deviation
         << ", relative=" << selected->relative_max_deviation << ").";
    emit(DiagnosticSeverity::info, "face_replacement_recorded", info.str());

    TopoDS_Solid parent_solid;
    std::size_t parent_index = 0;
    const std::string parent_id = solid_id(solid_index);
    if (find_solid_by_id(model, parent_id, &parent_solid, &parent_index)) {
        materialize_fully_replaced_primitive(model, parent_id, parent_solid,
                                             parent_index, options);
    }
    return true;
}

AutoFitSummary auto_fit_splines(CadModel& model,
                                bool has_max_deviation,
                                double max_deviation,
                                const ConversionOptions& options) {
    using namespace csgify::detail;
    AutoFitSummary summary;
    if (!model.loaded()) {
        return summary;
    }
    const AnalysisReport analysis = analyze_model(model, options);
    for (const AnalyzedFace& face : analysis.faces) {
        if (face.surface_type != "bspline" && face.surface_type != "bezier") {
            continue;
        }
        ++summary.spline_faces_seen;
        const SplineFitCandidates& fits = face.spline_fits;
        const SplineFaceFit* best = nullptr;
        SplineFitKind best_kind = SplineFitKind::none;
        auto consider = [&](const SplineFaceFit& fit, SplineFitKind kind) {
            if (!fit.available) {
                return;
            }
            if (!best || fit.max_deviation < best->max_deviation) {
                best = &fit;
                best_kind = kind;
            }
        };
        consider(fits.plane, SplineFitKind::plane);
        consider(fits.sphere, SplineFitKind::sphere);
        consider(fits.cylinder, SplineFitKind::cylinder);
        consider(fits.cone, SplineFitKind::cone);

        if (!best) {
            ++summary.skipped_no_fit;
            model.diagnostics.push_back({
                DiagnosticSeverity::warning,
                "auto_fit_no_candidate",
                "No analytic fit was available for this spline face.",
                face.id,
            });
            continue;
        }
        if (has_max_deviation && best->max_deviation > max_deviation) {
            ++summary.skipped_over_budget;
            std::ostringstream msg;
            msg << "Best fit (" << spline_fit_kind_name(best_kind)
                << ", max_deviation=" << best->max_deviation
                << ") exceeds budget " << max_deviation << ".";
            model.diagnostics.push_back({
                DiagnosticSeverity::warning,
                "auto_fit_over_budget",
                msg.str(),
                face.id,
            });
            continue;
        }
        Diagnostic diag;
        if (replace_face_with_fit(model, face.id, best_kind,
                                  has_max_deviation, max_deviation,
                                  &diag, options)) {
            ++summary.applied;
        }
    }
    return summary;
}

bool decompose_solid_by_planar_faces(CadModel& model,
                                     const std::string& id,
                                     const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    ConversionReport decomposition_report;
    std::vector<TopoDS_Solid> pieces =
        split_solid_by_planar_faces(solid, options, decomposition_report, index);
    for (const Diagnostic& diagnostic : decomposition_report.diagnostics) {
        model.diagnostics.push_back(diagnostic);
    }
    if (pieces.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "planar_decomposition_failed",
            "Planar decomposition did not produce usable pieces.",
            id
        });
        return false;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);
        pieces_volume += std::abs(piece_props.Mass());
    }

    const double absolute_delta = std::abs(pieces_volume - original_volume);
    double relative_delta = 0.0;
    if (original_volume > kEps) {
        relative_delta = absolute_delta / original_volume;
    }

    if (!decomposition_volume_guard(model, id, relative_delta, options)) {
        return false;
    }

    std::ostringstream msg;
    msg << "Planar decomposition produced " << pieces.size()
        << " pieces. original_volume=" << std::setprecision(10) << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= 1.0e-6 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
        "planar_decomposition",
        msg.str(),
        id
    });

    remove_solid(model, id);
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);

        DecomposedSolidPiece decomposed;
        decomposed.id = next_decomposed_piece_id(model);
        decomposed.label = decomposed.id + "/from_" + id;
        decomposed.source_solid_id = id;
        decomposed.solid = piece;
        decomposed.volume = std::abs(piece_props.Mass());
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

bool decompose_solid_prismatic(CadModel& model,
                               const std::string& solid_id_str,
                               const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)) {
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "decomposition_target_not_found",
            "Requested prismatic decomposition target was not found.",
            solid_id_str});
        return false;
    }
    GProp_GProps original_props;
    BRepGProp::VolumeProperties(target.solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    const SolidTopology topology =
        analyze_solid_topology(target.solid, original_volume, options);
    if (!topology.prismatic_candidate) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "prismatic_not_applicable",
            "Solid did not satisfy the all-planar piecewise-prismatic classifier.",
            solid_id_str});
        return false;
    }

    std::vector<gp_Pln> planes = unique_planar_support_planes(target.solid, options);
    const gp_Dir axis(topology.prism_axis.x, topology.prism_axis.y,
                      topology.prism_axis.z);
    std::stable_sort(planes.begin(), planes.end(), [&](const gp_Pln& a,
                                                       const gp_Pln& b) {
        // Partition the 2D profile before introducing axial slabs.
        return std::abs(a.Axis().Direction().Dot(axis))
             < std::abs(b.Axis().Direction().Dot(axis));
    });

    std::vector<gp_Dir> profile_normals;
    for (const gp_Pln& plane : planes) {
        const gp_Dir normal = plane.Axis().Direction();
        if (std::abs(normal.Dot(axis)) > options.angular_tolerance) continue;
        const bool known = std::any_of(
            profile_normals.begin(), profile_normals.end(),
            [&](const gp_Dir& existing) {
                return std::abs(existing.Dot(normal))
                    >= 1.0 - std::max(options.angular_tolerance, 1.0e-9);
            });
        if (!known) profile_normals.push_back(normal);
    }

    auto straddles = [&](const TopoDS_Solid& solid, const gp_Pln& plane) {
        const gp_Dir n = plane.Axis().Direction();
        const gp_Pnt o = plane.Location();
        double min_value = std::numeric_limits<double>::infinity();
        double max_value = -std::numeric_limits<double>::infinity();
        for (TopExp_Explorer it(solid, TopAbs_VERTEX); it.More(); it.Next()) {
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
            const double value = gp_Vec(o, p).Dot(gp_Vec(n));
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
        }
        return min_value < -options.linear_tolerance
            && max_value > options.linear_tolerance;
    };

    std::vector<TopoDS_Solid> pieces{target.solid};
    std::size_t splitter_calls = 0;
    bool any_split = false;
    bool budget_exhausted = false;
    for (const gp_Pln& plane : planes) {
        std::vector<TopoDS_Solid> next;
        for (const TopoDS_Solid& piece : pieces) {
            if (!straddles(piece, plane)) {
                next.push_back(piece);
                continue;
            }
            if (splitter_calls >= options.prismatic_max_splitter_calls) {
                budget_exhausted = true;
                next.push_back(piece);
                continue;
            }
            ++splitter_calls;
            ConversionReport scratch;
            std::vector<TopoDS_Solid> split = split_solid_by_plane(
                piece, plane, options, scratch, target.report_index,
                "prismatic_support_plane");
            if (split.size() > 1) {
                any_split = true;
                next.insert(next.end(), split.begin(), split.end());
            } else {
                next.push_back(piece);
            }
            if (next.size() > options.prismatic_max_pieces) {
                budget_exhausted = true;
                break;
            }
        }
        pieces.swap(next);
        if (budget_exhausted) break;
    }
    if (!any_split || pieces.empty()) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "prismatic_no_progress",
            "Prismatic support-plane arrangement produced no usable partition.",
            solid_id_str});
        return false;
    }

    // Existing face supports are sufficient for ordinary concave profiles,
    // but not for a profile containing a through-hole: a useful partition
    // line often continues a wall direction through a reflex vertex and is
    // not itself the support of any face.  Generate those cross-profile
    // planes only for residual cells.  This local refinement is bounded by
    // the same piece/split budgets and avoids constructing the Cartesian
    // arrangement for the complete source solid.
    auto is_convex_cell = [&](const TopoDS_Solid& solid,
                              std::size_t probe_index) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        DecomposedSolidPiece record;
        record.id = "prismatic_probe:" + std::to_string(probe_index);
        record.solid = solid;
        record.volume = std::abs(props.Mass());
        return analyze_decomposed_piece(record, probe_index,
                                        target.report_index, options)
            .exact_csg_supported;
    };
    std::vector<TopoDS_Solid> refined;
    std::vector<bool> refined_convex;
    refined.reserve(pieces.size());
    refined_convex.reserve(pieces.size());
    std::size_t refinement_index = 0;
    for (const TopoDS_Solid& piece : pieces) {
        ++refinement_index;
        if (is_convex_cell(piece, refinement_index)) {
            refined.push_back(piece);
            refined_convex.push_back(true);
            continue;
        }
        std::vector<gp_Pln> cross_planes;
        for (TopExp_Explorer vertex_it(piece, TopAbs_VERTEX);
             vertex_it.More(); vertex_it.Next()) {
            const gp_Pnt point = BRep_Tool::Pnt(
                TopoDS::Vertex(vertex_it.Current()));
            for (const gp_Dir& normal : profile_normals) {
                const gp_Pln candidate(point, normal);
                const bool duplicate = std::any_of(
                    cross_planes.begin(), cross_planes.end(),
                    [&](const gp_Pln& existing) {
                        return std::abs(existing.Axis().Direction().Dot(normal))
                                   >= 1.0 - std::max(options.angular_tolerance,
                                                    1.0e-9)
                            && existing.Distance(point)
                                   <= std::max(options.linear_tolerance * 10.0,
                                               1.0e-6);
                    });
                if (!duplicate) cross_planes.push_back(candidate);
            }
        }
        std::vector<TopoDS_Solid> local{piece};
        for (const gp_Pln& plane : cross_planes) {
            std::vector<TopoDS_Solid> next;
            for (const TopoDS_Solid& cell : local) {
                if (!straddles(cell, plane)) {
                    next.push_back(cell);
                    continue;
                }
                if (splitter_calls >= options.prismatic_max_splitter_calls
                    || refined.size() + next.size() + local.size()
                           >= options.prismatic_max_pieces) {
                    budget_exhausted = true;
                    next.push_back(cell);
                    continue;
                }
                ++splitter_calls;
                ConversionReport scratch;
                std::vector<TopoDS_Solid> split = split_solid_by_plane(
                    cell, plane, options, scratch, target.report_index,
                    "prismatic_reflex_vertex_plane");
                if (split.size() > 1) {
                    next.insert(next.end(), split.begin(), split.end());
                } else {
                    next.push_back(cell);
                }
            }
            local.swap(next);
        }
        refined.insert(refined.end(), local.begin(), local.end());
        refined_convex.insert(refined_convex.end(), local.size(), false);
    }
    pieces.swap(refined);

    double pieces_volume = 0.0;
    std::size_t residual = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(pieces[i], props);
        const double volume = std::abs(props.Mass());
        pieces_volume += volume;
        if (!refined_convex[i]) {
            DecomposedSolidPiece record;
            record.id = "prismatic_probe:" + std::to_string(i + 1);
            record.solid = pieces[i];
            record.volume = volume;
            const AnalyzedDecomposedPiece analyzed = analyze_decomposed_piece(
                record, i + 1, target.report_index, options);
            if (!analyzed.exact_csg_supported) ++residual;
        }
    }
    const double relative_delta = std::abs(pieces_volume - original_volume)
        / std::max(original_volume, kEps);
    if (relative_delta > options.decomposition_relative_volume_tolerance) {
        std::ostringstream msg;
        msg << "Prismatic partition relative volume delta " << relative_delta
            << " exceeds tolerance "
            << options.decomposition_relative_volume_tolerance << ".";
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "decomposition_volume_drift", msg.str(), solid_id_str});
        return false;
    }

    if (target.is_piece && target.piece) target.piece->active = false;
    else remove_solid(model, solid_id_str);
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(piece, props);
        DecomposedSolidPiece record;
        record.id = next_decomposed_piece_id(model);
        record.label = record.id + "/from_" + solid_id_str;
        record.source_solid_id = decomposition_source_id(target, solid_id_str);
        record.solid = piece;
        record.volume = std::abs(props.Mass());
        model.decomposed_pieces.push_back(std::move(record));
    }
    std::ostringstream msg;
    msg << "Sequential prismatic arrangement produced " << pieces.size()
        << " piece(s) using " << splitter_calls << " splitter call(s); "
        << residual << " residual piece(s), relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        residual == 0 && !budget_exhausted ? DiagnosticSeverity::info
                                           : DiagnosticSeverity::warning,
        residual == 0 && !budget_exhausted ? "prismatic_decomposition"
                                           : "prismatic_decomposition_partial",
        msg.str(), solid_id_str});
    return true;
}

bool decompose_solid_revolution(CadModel& model,
                                const std::string& solid_id_str,
                                const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)) {
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "revolution_target_not_found",
            "Requested revolution target was not found.", solid_id_str});
        return false;
    }
    GProp_GProps original_props;
    BRepGProp::VolumeProperties(target.solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    const SolidTopology topology =
        analyze_solid_topology(target.solid, original_volume, options);
    if (!topology.revolution.candidate
        || !topology.revolution.full_revolution) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "revolution_not_applicable",
            "The solid is not a supported full axisymmetric revolution.",
            solid_id_str});
        return false;
    }

    const gp_Dir axis(topology.revolution.axis_direction.x,
                      topology.revolution.axis_direction.y,
                      topology.revolution.axis_direction.z);
    std::vector<gp_Pln> axial_planes;
    for (const gp_Pln& plane : unique_planar_support_planes(target.solid,
                                                             options)) {
        if (std::abs(plane.Axis().Direction().Dot(axis))
            >= 1.0 - std::max(options.angular_tolerance, 1.0e-9)) {
            axial_planes.push_back(plane);
        }
    }
    auto straddles = [&](const TopoDS_Solid& solid, const gp_Pln& plane) {
        const gp_Dir normal = plane.Axis().Direction();
        const gp_Pnt origin = plane.Location();
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (TopExp_Explorer it(solid, TopAbs_VERTEX); it.More(); it.Next()) {
            const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
            const double value = gp_Vec(origin, point).Dot(gp_Vec(normal));
            low = std::min(low, value);
            high = std::max(high, value);
        }
        return low < -options.linear_tolerance
            && high > options.linear_tolerance;
    };

    std::vector<TopoDS_Solid> pieces{target.solid};
    std::size_t splitter_calls = 0;
    bool any_split = false;
    bool budget_exhausted = false;
    for (const gp_Pln& plane : axial_planes) {
        std::vector<TopoDS_Solid> next;
        for (const TopoDS_Solid& piece : pieces) {
            if (!straddles(piece, plane)) {
                next.push_back(piece);
                continue;
            }
            if (splitter_calls >= options.prismatic_max_splitter_calls
                || next.size() + pieces.size()
                       >= options.prismatic_max_pieces) {
                budget_exhausted = true;
                next.push_back(piece);
                continue;
            }
            ++splitter_calls;
            ConversionReport scratch;
            std::vector<TopoDS_Solid> split = split_solid_by_plane(
                piece, plane, options, scratch, target.report_index,
                "revolution_axial_event_plane");
            if (split.size() > 1) {
                any_split = true;
                next.insert(next.end(), split.begin(), split.end());
            } else {
                next.push_back(piece);
            }
        }
        pieces.swap(next);
    }
    if (!any_split || pieces.empty()) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "revolution_no_progress",
            "No internal axial meridian-event plane split the solid.",
            solid_id_str});
        return false;
    }

    double pieces_volume = 0.0;
    std::size_t residual = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(pieces[i], props);
        const double piece_volume = std::abs(props.Mass());
        pieces_volume += piece_volume;
        DecomposedSolidPiece probe;
        probe.id = "revolution_probe:" + std::to_string(i + 1);
        probe.solid = pieces[i];
        probe.volume = piece_volume;
        if (!analyze_decomposed_piece(probe, i + 1, target.report_index,
                                      options).exact_csg_supported) {
            ++residual;
        }
    }
    const double relative_delta = std::abs(pieces_volume - original_volume)
        / std::max(original_volume, kEps);
    if (!decomposition_volume_guard(model, solid_id_str, relative_delta,
                                    options)) {
        return false;
    }

    if (target.is_piece && target.piece) target.piece->active = false;
    else remove_solid(model, solid_id_str);
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(piece, props);
        DecomposedSolidPiece record;
        record.id = next_decomposed_piece_id(model);
        record.label = record.id + "/from_" + solid_id_str;
        record.source_solid_id = decomposition_source_id(target, solid_id_str);
        record.solid = piece;
        record.volume = std::abs(props.Mass());
        model.decomposed_pieces.push_back(std::move(record));
    }
    std::ostringstream message;
    message << "Axial revolution arrangement produced " << pieces.size()
            << " piece(s) using " << splitter_calls << " splitter call(s); "
            << residual << " residual piece(s), relative_delta="
            << relative_delta;
    model.diagnostics.push_back({
        residual == 0 && !budget_exhausted ? DiagnosticSeverity::info
                                           : DiagnosticSeverity::warning,
        residual == 0 && !budget_exhausted ? "revolution_decomposition"
                                           : "revolution_decomposition_partial",
        message.str(), solid_id_str});
    return true;
}

bool reuse_definition_decomposition(
    CadModel& model,
    const std::string& source_solid_id,
    const InstanceTransform& source_transform,
    const std::string& target_solid_id,
    const InstanceTransform& target_transform,
    const ConversionOptions& options) {
    if (source_solid_id.empty() || source_solid_id == target_solid_id) {
        return false;
    }
    auto is_rigid = [](const InstanceTransform& transform) {
        const auto& m = transform.values;
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                double dot = 0.0;
                for (int row = 0; row < 3; ++row)
                    dot += m[4 * row + a] * m[4 * row + b];
                const double expected = a == b ? 1.0 : 0.0;
                if (std::abs(dot - expected) > 1.0e-9) return false;
            }
        }
        return true;
    };
    if (!is_rigid(source_transform) || !is_rigid(target_transform)) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "definition_cache_non_rigid_transform",
            "Definition conversion reuse was rejected because an occurrence "
            "transform contains scale or shear.", target_solid_id});
        return false;
    }
    TopoDS_Shape target_shape;
    if (!active_shape_for_id(model, target_solid_id, &target_shape)
        || target_shape.ShapeType() != TopAbs_SOLID) {
        return false;
    }
    std::vector<const DecomposedSolidPiece*> source_pieces;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (piece.active && piece.source_solid_id == source_solid_id) {
            source_pieces.push_back(&piece);
        }
    }
    std::vector<const CadBooleanBody*> source_bodies;
    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (body.active && body.source_solid_id == source_solid_id) {
            source_bodies.push_back(&body);
        }
    }
    if (source_pieces.empty() && source_bodies.empty()) return false;

    auto to_trsf = [](const InstanceTransform& value) {
        const auto& m = value.values;
        gp_Trsf result;
        result.SetValues(m[0], m[1], m[2], m[3],
                         m[4], m[5], m[6], m[7],
                         m[8], m[9], m[10], m[11]);
        return result;
    };
    gp_Trsf inverse_source = to_trsf(source_transform);
    try {
        inverse_source.Invert();
    } catch (const Standard_Failure&) {
        return false;
    }
    const gp_Trsf relative = to_trsf(target_transform) * inverse_source;
    std::vector<TopoDS_Solid> transformed;
    transformed.reserve(source_pieces.size());
    std::vector<CadBooleanBody> transformed_bodies;
    transformed_bodies.reserve(source_bodies.size());
    double transformed_volume = 0.0;
    try {
        auto move_solid = [&](const TopoDS_Solid& source) {
            const TopoDS_Shape moved = BRepBuilderAPI_Transform(
                source, relative, true).Shape();
            if (moved.IsNull() || moved.ShapeType() != TopAbs_SOLID
                || !BRepCheck_Analyzer(moved, true).IsValid()) {
                throw Standard_Failure("invalid transformed definition solid");
            }
            return TopoDS::Solid(moved);
        };
        for (const DecomposedSolidPiece* source : source_pieces) {
            const TopoDS_Solid solid = move_solid(source->solid);
            GProp_GProps props;
            BRepGProp::VolumeProperties(solid, props);
            transformed_volume += std::abs(props.Mass());
            transformed.push_back(solid);
        }
        for (const CadBooleanBody* source : source_bodies) {
            CadBooleanBody body;
            body.source_solid_id = target_solid_id;
            body.materialized_solid = BRepBuilderAPI_Transform(
                source->materialized_solid, relative, true).Shape();
            if (body.materialized_solid.IsNull()
                || !BRepCheck_Analyzer(body.materialized_solid, true).IsValid())
                throw Standard_Failure("invalid transformed Boolean body");
            for (const TopoDS_Solid& positive : source->positive_cells)
                body.positive_cells.push_back(move_solid(positive));
            for (const TopoDS_Solid& negative : source->negative_cells)
                body.negative_cells.push_back(move_solid(negative));
            GProp_GProps props;
            BRepGProp::VolumeProperties(body.materialized_solid, props);
            transformed_volume += std::abs(props.Mass());
            body.role = source->role;
            body.request_id = source->request_id;
            body.material_id = source->material_id;
            body.density = source->density;
            body.emit_as_cell = source->emit_as_cell;
            transformed_bodies.push_back(std::move(body));
        }
    } catch (const Standard_Failure&) {
        return false;
    }

    GProp_GProps target_props;
    BRepGProp::VolumeProperties(target_shape, target_props);
    const double target_volume = std::abs(target_props.Mass());
    const double relative_delta = std::abs(transformed_volume - target_volume)
        / std::max(target_volume, kEps);
    if (!decomposition_volume_guard(model, target_solid_id, relative_delta,
                                    options)) {
        return false;
    }
    if (!remove_solid(model, target_solid_id)) return false;
    for (TopoDS_Solid& solid : transformed) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        DecomposedSolidPiece piece;
        piece.id = next_decomposed_piece_id(model);
        piece.label = piece.id + "/definition_cache_from_" + source_solid_id;
        piece.source_solid_id = target_solid_id;
        piece.solid = std::move(solid);
        piece.volume = std::abs(props.Mass());
        model.decomposed_pieces.push_back(std::move(piece));
    }
    for (CadBooleanBody& body : transformed_bodies) {
        body.id = "cad_boolean:"
            + std::to_string(model.cad_boolean_bodies.size() + 1);
        body.label = body.id + "/definition_cache_from_" + source_solid_id;
        model.cad_boolean_bodies.push_back(std::move(body));
    }
    std::ostringstream message;
    message << "Reused " << source_pieces.size()
            << " certified decomposition piece(s) and "
            << source_bodies.size() << " Boolean body/bodies from "
            << source_solid_id
            << " for transformed occurrence " << target_solid_id
            << "; relative_delta=" << relative_delta << ".";
    model.diagnostics.push_back({DiagnosticSeverity::info,
        "definition_conversion_cache_hit", message.str(), target_solid_id});
    return true;
}

bool decompose_solid_shell_difference(CadModel& model,
                                      const std::string& solid_id_str,
                                      const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)
        || target.is_piece) {
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "shell_difference_target_not_found",
            "Shell-difference conversion requires an imported solid:N target.",
            solid_id_str});
        return false;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(target.solid, props);
    const double volume = std::abs(props.Mass());
    const SolidTopology topology =
        analyze_solid_topology(target.solid, volume, options);
    if (topology.cavity_shell_count == 0) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "shell_difference_not_applicable",
            "The solid has no enclosed cavity shells.", solid_id_str});
        return false;
    }

    const TopoDS_Shell outer = BRepClass3d::OuterShell(target.solid);
    CadBooleanBody body;
    body.id = "cad_boolean:"
        + std::to_string(model.cad_boolean_bodies.size() + 1);
    body.label = body.id + "/from_" + solid_id_str;
    body.source_solid_id = solid_id_str;
    body.materialized_solid = target.solid;
    std::size_t shell_index = 0;
    for (TopExp_Explorer it(target.solid, TopAbs_SHELL);
         it.More(); it.Next()) {
        ++shell_index;
        const TopoDS_Shell shell = TopoDS::Shell(it.Current());
        TopoDS_Solid shell_solid = BRepBuilderAPI_MakeSolid(shell).Solid();
        BRepLib::OrientClosedSolid(shell_solid);
        GProp_GProps shell_props;
        BRepGProp::VolumeProperties(shell_solid, shell_props);
        gp_Pnt interior;
        ConversionReport scratch;
        GeometryCertificate certificate;
        const bool convex =
            find_interior_point(shell_solid, shell_props,
                                options.linear_tolerance, &interior)
            && validate_convex_halfspace_cell(
                shell_solid, interior, target.report_index, options, scratch,
                false, false, false, &model.face_replacements, &certificate);
        if (!convex) {
            std::ostringstream msg;
            msg << "Shell " << shell_index
                << " is not a convex analytic cell ("
                << certificate.failure_code
                << "); shell difference was not applied.";
            model.diagnostics.push_back({DiagnosticSeverity::warning,
                "shell_difference_nonconvex_shell", msg.str(), solid_id_str});
            return false;
        }
        if (!outer.IsNull() && shell.IsSame(outer)) {
            body.positive_cells.push_back(shell_solid);
        } else {
            body.negative_cells.push_back(shell_solid);
        }
    }
    if (body.positive_cells.empty() || body.negative_cells.empty()) return false;
    remove_solid(model, solid_id_str);
    model.cad_boolean_bodies.push_back(std::move(body));
    model.diagnostics.push_back({DiagnosticSeverity::info,
        "shell_difference_applied",
        "Converted the outer shell and enclosed cavity shells to an exact "
        "Boolean difference.", solid_id_str});
    return true;
}

bool decompose_solid_pipe_annulus(CadModel& model,
                                  const std::string& solid_id_str,
                                  const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)
        || target.is_piece) {
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "pipe_annulus_target_not_found",
            "Pipe recognition requires an imported solid:N target.",
            solid_id_str});
        return false;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(target.solid, props);
    const double volume = std::abs(props.Mass());
    const SolidTopology topology =
        analyze_solid_topology(target.solid, volume, options);
    const PipeAnnulusTopology& pipe = topology.pipe_annulus;
    if (!pipe.candidate || pipe.ports.size() != 2) {
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "pipe_annulus_not_applicable",
            "The solid is not a certified straight full-annulus pipe.",
            solid_id_str});
        return false;
    }

    const gp_Pnt base(pipe.ports.front().center.x,
                      pipe.ports.front().center.y,
                      pipe.ports.front().center.z);
    const gp_Dir axis(pipe.axis_direction.x, pipe.axis_direction.y,
                      pipe.axis_direction.z);
    try {
        const gp_Ax2 placement(base, axis);
        const TopoDS_Solid outer = pipe.conical
            ? BRepPrimAPI_MakeCone(placement, pipe.outer_radius,
                                   pipe.top_outer_radius,
                                   pipe.axial_length).Solid()
            : BRepPrimAPI_MakeCylinder(
                  placement, pipe.outer_radius, pipe.axial_length).Solid();
        const TopoDS_Solid inner = pipe.conical
            ? BRepPrimAPI_MakeCone(placement, pipe.inner_radius,
                                   pipe.top_inner_radius,
                                   pipe.axial_length).Solid()
            : BRepPrimAPI_MakeCylinder(
                  placement, pipe.inner_radius, pipe.axial_length).Solid();
        BRepAlgoAPI_Cut cut;
        detail::build_binary_boolean(cut, outer, inner);
        if (!cut.IsDone() || cut.HasErrors() || cut.Shape().IsNull()
            || !BRepCheck_Analyzer(cut.Shape()).IsValid()) {
            model.diagnostics.push_back({DiagnosticSeverity::warning,
                "pipe_annulus_reconstruction_failed",
                "OCCT could not materialize a valid outer-minus-inner pipe.",
                solid_id_str});
            return false;
        }
        TopExp_Explorer solids(cut.Shape(), TopAbs_SOLID);
        if (!solids.More()) return false;
        const TopoDS_Solid reconstructed = TopoDS::Solid(solids.Current());
        solids.Next();
        if (solids.More()) return false;

        std::string measurement_error;
        const double numerical_tolerance = std::max(
            1.0e-12, volume * options.decomposition_relative_volume_tolerance);
        const ApproximationMetrics metrics = measure_shape_approximation(
            target.solid, reconstructed, volume, numerical_tolerance,
            &measurement_error);
        if (!metrics.symmetric_difference_certified
            || metrics.changed_volume_fraction
                   > options.decomposition_relative_volume_tolerance) {
            std::ostringstream message;
            message << "Recognized pipe reconstruction did not pass the exact "
                       "symmetric-difference certificate";
            if (!measurement_error.empty()) message << ": " << measurement_error;
            message << "; changed_fraction=" << std::setprecision(17)
                    << metrics.changed_volume_fraction;
            model.diagnostics.push_back({DiagnosticSeverity::warning,
                "pipe_annulus_certificate_failed", message.str(), solid_id_str});
            return false;
        }

        CadBooleanBody body;
        body.id = "cad_boolean:"
            + std::to_string(model.cad_boolean_bodies.size() + 1);
        body.label = body.id + "/pipe_from_" + solid_id_str;
        body.source_solid_id = solid_id_str;
        body.materialized_solid = target.solid;
        body.positive_cells.push_back(outer);
        body.negative_cells.push_back(inner);
        remove_solid(model, solid_id_str);
        model.cad_boolean_bodies.push_back(std::move(body));
        std::ostringstream message;
        message << "Recognized a straight "
                << (pipe.conical ? "conical" : "cylindrical")
                << " pipe and emitted one exact annulus as outer primitive "
                   "minus inner primitive; changed_fraction="
                << std::setprecision(17) << metrics.changed_volume_fraction;
        model.diagnostics.push_back({DiagnosticSeverity::info,
            "pipe_annulus_difference_applied", message.str(), solid_id_str});
        return true;
    } catch (const Standard_Failure& failure) {
        const char* what = failure.what();
        model.diagnostics.push_back({DiagnosticSeverity::warning,
            "pipe_annulus_reconstruction_failed",
            what ? what : "OCCT pipe reconstruction failed.", solid_id_str});
        return false;
    }
}

bool decompose_solid_by_face(CadModel& model,
                             const std::string& solid_id_str,
                             const std::string& face_id_str,
                             const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "decomposition_target_not_found",
            "Requested decomposition target ID was not found.",
            solid_id_str
        });
        return false;
    }

    std::size_t face_index = 0;
    TopoDS_Face face;
    if (target.is_piece) {
        std::string face_piece_id;
        if (!parse_piece_face_id(face_id_str, &face_piece_id, &face_index)) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "invalid_face_id",
                "Piece face ID must look like piece:N/face:M.",
                face_id_str
            });
            return false;
        }
        if (face_piece_id != solid_id_str) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "face_solid_mismatch",
                "Directed decomposition face must belong to the target piece.",
                face_id_str
            });
            return false;
        }
        if (!find_face_in_solid(target.solid, face_index, &face)) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "face_not_found",
                "Requested face ID was not found in the target piece.",
                face_id_str
            });
            return false;
        }
    } else {
        std::size_t face_solid_index = 0;
        if (!parse_face_id(face_id_str, &face_solid_index, &face_index)) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "invalid_face_id",
                "Face ID must look like face:N.M with positive N and M.",
                face_id_str
            });
            return false;
        }
        if (face_solid_index != target.report_index) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "face_solid_mismatch",
                "Directed decomposition face must belong to the target solid.",
                face_id_str
            });
            return false;
        }
        if (!find_face_in_root(model.root, face_solid_index, face_index, &face)) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "face_not_found",
                "Requested face ID was not found in the loaded model.",
                face_id_str
            });
            return false;
        }
    }

    ConversionReport decomposition_report;
    std::vector<TopoDS_Solid> pieces =
        split_solid_by_face(target.solid, face, options, decomposition_report,
                            target.report_index, face_index);
    for (const Diagnostic& diagnostic : decomposition_report.diagnostics) {
        model.diagnostics.push_back(diagnostic);
    }
    if (pieces.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "face_decomposition_failed",
            "Directed face decomposition did not produce usable pieces.",
            solid_id_str
        });
        return false;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(target.solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);
        pieces_volume += std::abs(piece_props.Mass());
    }

    const double absolute_delta = std::abs(pieces_volume - original_volume);
    double relative_delta = 0.0;
    if (original_volume > kEps) {
        relative_delta = absolute_delta / original_volume;
    }

    if (!decomposition_volume_guard(model, solid_id_str,
                                    relative_delta, options)) {
        return false;
    }

    std::ostringstream msg;
    msg << "Directed face decomposition using " << face_id_str
        << " produced " << pieces.size()
        << " pieces. original_volume=" << std::setprecision(10) << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= 1.0e-6 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
        "face_decomposition",
        msg.str(),
        solid_id_str
    });

    if (target.is_piece) {
        target.piece->active = false;
    } else {
        remove_solid(model, solid_id_str);
    }
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);

        DecomposedSolidPiece decomposed;
        decomposed.id = next_decomposed_piece_id(model);
        decomposed.label = decomposed.id + "/from_" + solid_id_str;
        decomposed.source_solid_id = decomposition_source_id(target, solid_id_str);
        decomposed.solid = piece;
        decomposed.volume = std::abs(piece_props.Mass());
        decomposed.active = true;
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

bool decompose_solid_by_plane(CadModel& model,
                              const std::string& solid_id_str,
                              const Vec3& origin,
                              const Vec3& normal,
                              const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "decomposition_target_not_found",
            "Requested decomposition target ID was not found.",
            solid_id_str
        });
        return false;
    }

    const double nx = normal.x, ny = normal.y, nz = normal.z;
    const double n_mag = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (n_mag <= kEps) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_plane_normal",
            "Decomposition plane normal must be non-zero.",
            solid_id_str
        });
        return false;
    }

    const gp_Pnt origin_pnt(origin.x, origin.y, origin.z);
    const gp_Dir normal_dir(nx, ny, nz);
    const gp_Pln plane(origin_pnt, normal_dir);

    ConversionReport decomposition_report;
    std::vector<TopoDS_Solid> pieces =
        split_solid_by_plane(target.solid, plane, options, decomposition_report,
                             target.report_index, solid_id_str);
    for (const Diagnostic& diagnostic : decomposition_report.diagnostics) {
        model.diagnostics.push_back(diagnostic);
    }
    if (pieces.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "plane_decomposition_failed",
            "Directed plane decomposition did not produce usable pieces.",
            solid_id_str
        });
        return false;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(target.solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);
        pieces_volume += std::abs(piece_props.Mass());
    }

    const double absolute_delta = std::abs(pieces_volume - original_volume);
    double relative_delta = 0.0;
    if (original_volume > kEps) {
        relative_delta = absolute_delta / original_volume;
    }

    if (!decomposition_volume_guard(model, solid_id_str,
                                    relative_delta, options)) {
        return false;
    }

    std::ostringstream msg;
    msg << "Directed plane decomposition using "
        << "origin=(" << std::setprecision(10)
        << origin.x << "," << origin.y << "," << origin.z << ") "
        << "normal=(" << normal.x << "," << normal.y << "," << normal.z << ") "
        << "produced " << pieces.size()
        << " pieces. original_volume=" << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= 1.0e-6 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
        "plane_decomposition",
        msg.str(),
        solid_id_str
    });

    if (target.is_piece) {
        target.piece->active = false;
    } else {
        remove_solid(model, solid_id_str);
    }
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);

        DecomposedSolidPiece decomposed;
        decomposed.id = next_decomposed_piece_id(model);
        decomposed.label = decomposed.id + "/from_" + solid_id_str;
        decomposed.source_solid_id = decomposition_source_id(target, solid_id_str);
        decomposed.solid = piece;
        decomposed.volume = std::abs(piece_props.Mass());
        decomposed.active = true;
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

bool decompose_solid_by_planes(CadModel& model,
                               const std::string& solid_id_str,
                               const std::vector<PlaneSpec>& planes,
                               const ConversionOptions& options) {
    using namespace csgify::detail;
    DecompositionTarget target;
    if (!find_decomposition_target(model, solid_id_str, &target)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "decomposition_target_not_found",
            "Requested decomposition target ID was not found.",
            solid_id_str
        });
        return false;
    }

    if (planes.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "no_planes_supplied",
            "Multi-plane decomposition requires at least one plane.",
            solid_id_str
        });
        return false;
    }

    std::vector<gp_Pln> occ_planes;
    occ_planes.reserve(planes.size());
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const PlaneSpec& p = planes[i];
        const double n_mag = std::sqrt(p.normal.x * p.normal.x
                                       + p.normal.y * p.normal.y
                                       + p.normal.z * p.normal.z);
        if (n_mag <= kEps) {
            model.diagnostics.push_back({
                DiagnosticSeverity::error,
                "invalid_plane_normal",
                "Multi-plane decomposition: at least one plane has a "
                "near-zero normal.",
                solid_id_str
            });
            return false;
        }
        const gp_Pnt origin_pnt(p.origin.x, p.origin.y, p.origin.z);
        const gp_Dir normal_dir(p.normal.x, p.normal.y, p.normal.z);
        occ_planes.emplace_back(origin_pnt, normal_dir);
    }

    ConversionReport decomposition_report;
    std::vector<TopoDS_Solid> pieces =
        split_solid_by_planes(target.solid, occ_planes, options,
                              decomposition_report,
                              target.report_index, solid_id_str);
    for (const Diagnostic& diagnostic : decomposition_report.diagnostics) {
        model.diagnostics.push_back(diagnostic);
    }
    if (pieces.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "planes_decomposition_failed",
            "Multi-plane decomposition did not produce usable pieces.",
            solid_id_str
        });
        return false;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(target.solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);
        pieces_volume += std::abs(piece_props.Mass());
    }

    const double absolute_delta = std::abs(pieces_volume - original_volume);
    double relative_delta = 0.0;
    if (original_volume > kEps) {
        relative_delta = absolute_delta / original_volume;
    }

    if (!decomposition_volume_guard(model, solid_id_str,
                                    relative_delta, options)) {
        return false;
    }

    std::ostringstream msg;
    msg << "Multi-plane decomposition using " << planes.size()
        << " planes produced " << pieces.size()
        << " pieces. original_volume=" << std::setprecision(10) << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= 1.0e-6 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
        "planes_decomposition",
        msg.str(),
        solid_id_str
    });

    if (target.is_piece) {
        target.piece->active = false;
    } else {
        remove_solid(model, solid_id_str);
    }
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);

        DecomposedSolidPiece decomposed;
        decomposed.id = next_decomposed_piece_id(model);
        decomposed.label = decomposed.id + "/from_" + solid_id_str;
        decomposed.source_solid_id = decomposition_source_id(target, solid_id_str);
        decomposed.solid = piece;
        decomposed.volume = std::abs(piece_props.Mass());
        decomposed.active = true;
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

namespace {

constexpr double kProposalVolumeTolerance = 1.0e-6;
constexpr double kProposalSliverVolumeFraction = 1.0e-8;

std::string split_tool_kind_for_surface(GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane: return "face_plane";
    case GeomAbs_Cylinder: return "face_cylinder";
    case GeomAbs_Cone: return "face_cone";
    case GeomAbs_Sphere: return "face_sphere";
    case GeomAbs_Torus: return "torus_axis_plane";
    default: return "unsupported";
    }
}

std::string first_diagnostic_reason(const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::error
            || diagnostic.severity == DiagnosticSeverity::warning) {
            return diagnostic.code + ": " + diagnostic.message;
        }
    }
    return {};
}

void score_decomposition_candidate(DecompositionCandidate& candidate) {
    if (candidate.piece_count < 2) {
        candidate.usable = false;
        candidate.confidence = "rejected";
        if (candidate.rejection_reason.empty()) {
            candidate.rejection_reason = "split produced fewer than two pieces";
        }
        return;
    }
    if (candidate.relative_delta > kProposalVolumeTolerance) {
        candidate.usable = false;
        candidate.confidence = "rejected";
        std::ostringstream msg;
        msg << "relative volume delta " << candidate.relative_delta
            << " exceeds " << kProposalVolumeTolerance;
        candidate.rejection_reason = msg.str();
        return;
    }
    candidate.usable = true;
    const double exact_fraction = candidate.piece_count > 0
        ? static_cast<double>(candidate.exact_piece_count)
            / static_cast<double>(candidate.piece_count)
        : 0.0;
    const double decision_fraction = candidate.piece_count > 0
        ? static_cast<double>(candidate.pieces_requiring_user_decision)
            / static_cast<double>(candidate.piece_count)
        : 1.0;

    double surface_bonus = 0.0;
    if (candidate.tool_kind == "face_plane") {
        surface_bonus = 6.0;
    } else if (candidate.tool_kind == "face_cylinder"
               || candidate.tool_kind == "face_cone") {
        surface_bonus = 4.0;
    } else if (candidate.tool_kind == "face_sphere") {
        surface_bonus = 3.0;
    } else if (candidate.tool_kind == "torus_axis_plane") {
        surface_bonus = 2.0;
    }

    double reason_bonus = 0.0;
    if (candidate.reason == "convexity_failure_face") {
        reason_bonus = 25.0;
    } else if (candidate.reason == "existing_face") {
        reason_bonus = 5.0;
    } else if (candidate.reason == "bbox_axis_fallback") {
        reason_bonus = -10.0;
    }

    candidate.score =
        100.0
        + 40.0 * exact_fraction
        + 20.0 * candidate.volume_balance
        + 80.0 * candidate.progress_ratio
        + surface_bonus
        + reason_bonus
        - 25.0 * decision_fraction
        - 20.0 * static_cast<double>(candidate.sliver_piece_count)
        - 100000.0 * candidate.relative_delta;

    if (candidate.exact_piece_count == candidate.piece_count
        && candidate.pieces_requiring_user_decision == 0
        && candidate.relative_delta <= 1.0e-8
        && candidate.volume_balance >= 0.1
        && candidate.reason != "bbox_axis_fallback") {
        candidate.confidence = "high";
    } else if ((candidate.exact_piece_count > 0
                && candidate.pieces_requiring_decomposition
                       < candidate.piece_count)
               || candidate.progress_ratio >= 0.05) {
        candidate.confidence = "medium";
    } else {
        candidate.confidence = "low";
    }
}

bool has_clear_score_dominance(const std::vector<DecompositionCandidate>& candidates,
                               const DecompositionCandidate& candidate) {
    double second_best = -1.0e100;
    for (const DecompositionCandidate& other : candidates) {
        if (!other.usable || other.id == candidate.id) {
            continue;
        }
        second_best = std::max(second_best, other.score);
    }
    return second_best < -1.0e90 || candidate.score >= second_best + 10.0;
}

} // namespace

DecompositionProposal propose_decomposition_candidates_impl(
    const CadModel& model,
    const std::string& solid_id_str,
    const ConversionOptions& options,
    bool convexity_failure_face_only,
    bool longest_bbox_axis_only,
    bool include_multi_bbox_fallback,
    bool lazy_first_usable,
    std::map<std::string, std::vector<TopoDS_Solid>>* geometry_out = nullptr,
    bool include_parallel_plane_batches = false,
    std::size_t max_parallel_families = 0,
    std::size_t max_planes_per_batch = 0,
    std::size_t max_generated_candidates =
        std::numeric_limits<std::size_t>::max()) {
    using namespace csgify::detail;
    DecompositionProposal proposal;
    proposal.target_id = solid_id_str;

    TopoDS_Solid solid;
    std::size_t report_index = 0;
    bool is_piece = false;
    if (!find_decomposition_target(model, solid_id_str, &solid, &report_index, &is_piece)) {
        proposal.diagnostics.push_back({
            DiagnosticSeverity::error,
            "decomposition_target_not_found",
            "Requested decomposition target ID was not found.",
            solid_id_str
        });
        return proposal;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    if (original_volume <= kEps) {
        proposal.diagnostics.push_back({
            DiagnosticSeverity::error,
            "empty_or_tiny_solid",
            "Cannot propose decomposition for a near-zero-volume solid.",
            solid_id_str
        });
        return proposal;
    }
    const std::size_t parent_violation_count =
        convexity_halfspace_violation_count(solid, report_index, options);

    std::size_t convexity_failure_face_index = 0;
    const bool has_convexity_failure_face =
        find_convexity_failure_face(solid,
                                    report_index,
                                    options,
                                    &convexity_failure_face_index);

    auto finish_candidate = [&](DecompositionCandidate& candidate,
                                const std::vector<TopoDS_Solid>& pieces) {
        if (geometry_out) {
            (*geometry_out)[candidate.id] = pieces;
        }
        candidate.piece_count = pieces.size();
        double min_fraction = 0.0;
        double max_fraction = 0.0;
        std::size_t piece_index = 0;
        for (const TopoDS_Solid& piece : pieces) {
            ++piece_index;
            GProp_GProps piece_props;
            BRepGProp::VolumeProperties(piece, piece_props);
            const double volume = std::abs(piece_props.Mass());
            candidate.pieces_volume += volume;
            const double fraction = volume / original_volume;
            if (piece_index == 1) {
                min_fraction = fraction;
                max_fraction = fraction;
            } else {
                min_fraction = std::min(min_fraction, fraction);
                max_fraction = std::max(max_fraction, fraction);
            }
            if (fraction <= kProposalSliverVolumeFraction) {
                ++candidate.sliver_piece_count;
            }

            DecomposedSolidPiece piece_record;
            piece_record.id = "proposal_piece:" + std::to_string(piece_index);
            piece_record.label = piece_record.id + "/from_" + solid_id_str;
            piece_record.source_solid_id = solid_id_str;
            piece_record.solid = piece;
            piece_record.volume = volume;

            const std::size_t report_solid_index = 200000 + piece_index;
            const AnalyzedDecomposedPiece analyzed =
                analyze_decomposed_piece(piece_record, piece_index,
                                         report_solid_index, options);

            DecompositionCandidatePiece summary;
            summary.index = piece_index;
            summary.volume = volume;
            summary.volume_fraction = fraction;
            summary.bbox = analyzed.bbox;
            summary.face_count = analyzed.face_count;
            summary.exact_csg_supported = analyzed.exact_csg_supported;
            summary.needs_decomposition = analyzed.needs_decomposition;
            summary.needs_spline_replacement = analyzed.needs_spline_replacement;
            summary.needs_surface_replacement = analyzed.needs_surface_replacement;
            summary.needs_user_decision = analyzed.needs_user_decision;
            summary.halfspace_violation_count =
                convexity_halfspace_violation_count(
                    piece, report_solid_index, options);
            candidate.max_piece_halfspace_violation_count = std::max(
                candidate.max_piece_halfspace_violation_count,
                summary.halfspace_violation_count);
            candidate.pieces.push_back(summary);

            if (analyzed.exact_csg_supported) {
                ++candidate.exact_piece_count;
            }
            if (analyzed.needs_decomposition) {
                ++candidate.pieces_requiring_decomposition;
            }
            if (analyzed.needs_spline_replacement) {
                ++candidate.pieces_requiring_spline_replacement;
            }
            if (analyzed.needs_user_decision) {
                ++candidate.pieces_requiring_user_decision;
            }
        }

        candidate.absolute_delta =
            std::abs(candidate.pieces_volume - candidate.original_volume);
        candidate.relative_delta = candidate.absolute_delta / candidate.original_volume;
        candidate.min_piece_volume_fraction = min_fraction;
        candidate.max_piece_volume_fraction = max_fraction;
        if (max_fraction > kEps) {
            candidate.volume_balance = min_fraction / max_fraction;
        }
        candidate.parent_halfspace_violation_count = parent_violation_count;
        if (parent_violation_count > 0 && candidate.piece_count >= 2) {
            candidate.progress_ratio = std::max(
                0.0, 1.0
                    - static_cast<double>(
                          candidate.max_piece_halfspace_violation_count)
                        / static_cast<double>(parent_violation_count));
        }

        score_decomposition_candidate(candidate);
    };

    auto has_usable_in_proposal = [&]() {
        for (const DecompositionCandidate& c : proposal.candidates) {
            if (c.usable) return true;
        }
        return false;
    };
    auto has_high_confidence_in_proposal = [&]() {
        for (const DecompositionCandidate& c : proposal.candidates) {
            if (c.usable && c.confidence == "high") return true;
        }
        return false;
    };

    std::size_t candidate_index = 0;
    auto candidate_budget_available = [&]() {
        return proposal.candidates.size() < max_generated_candidates;
    };

    // GEOUNED-style fast path for repeated planar structure: canonicalize
    // planar face supports into parallel families and hand all useful offsets
    // in one family to one OCCT splitter call. Single face cuts often peel a
    // sliver without making a child convex; the coherent family exposes the
    // complete profile partition at once.
    if (include_parallel_plane_batches && max_parallel_families > 0
        && max_planes_per_batch >= 2) {
        struct PlaneFamily {
            gp_Dir normal;
            std::vector<gp_Pln> planes;
            bool contains_failure = false;
            std::size_t first_face_index = 0;
        };
        std::vector<PlaneFamily> families;
        std::size_t face_index = 0;
        for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
             face_exp.More(); face_exp.Next()) {
            ++face_index;
            const TopoDS_Face face = TopoDS::Face(face_exp.Current());
            BRepAdaptor_Surface adaptor(face, false);
            if (adaptor.GetType() != GeomAbs_Plane) continue;
            gp_Pln plane = adaptor.Plane();
            gp_Dir normal = plane.Axis().Direction();
            const double sign_component = std::abs(normal.X()) > 1.0e-12
                ? normal.X() : std::abs(normal.Y()) > 1.0e-12
                    ? normal.Y() : normal.Z();
            if (sign_component < 0.0) {
                normal.Reverse();
                plane.SetAxis(gp_Ax1(plane.Location(), normal));
            }
            PlaneFamily* family = nullptr;
            for (PlaneFamily& existing : families) {
                if (existing.normal.Dot(normal)
                        >= 1.0 - std::max(options.angular_tolerance, 1.0e-9)) {
                    family = &existing;
                    break;
                }
            }
            if (!family) {
                families.push_back({normal, {}, false, face_index});
                family = &families.back();
            }
            const bool duplicate = std::any_of(
                family->planes.begin(), family->planes.end(),
                [&](const gp_Pln& existing) {
                    return existing.Distance(plane.Location())
                        <= std::max(options.linear_tolerance * 10.0, 1.0e-6);
                });
            if (!duplicate) family->planes.push_back(plane);
            if (has_convexity_failure_face
                && face_index == convexity_failure_face_index) {
                family->contains_failure = true;
            }
        }
        std::stable_sort(families.begin(), families.end(),
            [](const PlaneFamily& left, const PlaneFamily& right) {
                if (left.contains_failure != right.contains_failure)
                    return left.contains_failure;
                if (left.planes.size() != right.planes.size())
                    return left.planes.size() > right.planes.size();
                return left.first_face_index < right.first_face_index;
            });
        std::size_t families_evaluated = 0;
        for (const PlaneFamily& family : families) {
            if (!candidate_budget_available()) break;
            if (families_evaluated >= max_parallel_families) break;
            if (family.planes.size() < 2) continue;
            ++families_evaluated;
            ++candidate_index;
            DecompositionCandidate candidate;
            candidate.id = "candidate:" + std::to_string(candidate_index);
            candidate.target_id = solid_id_str;
            candidate.surface_type = "plane";
            candidate.tool_kind = "parallel_face_planes";
            candidate.reason = "parallel_face_plane_batch";
            candidate.original_volume = original_volume;
            std::vector<gp_Pln> tools;
            const std::size_t plane_count = std::min(
                family.planes.size(), max_planes_per_batch);
            tools.reserve(plane_count);
            candidate.planes.reserve(plane_count);
            for (std::size_t i = 0; i < plane_count; ++i) {
                const gp_Pln& plane = family.planes[i];
                tools.push_back(plane);
                PlaneSpec spec;
                spec.origin = {plane.Location().X(), plane.Location().Y(),
                               plane.Location().Z()};
                const gp_Dir direction = plane.Axis().Direction();
                spec.normal = {direction.X(), direction.Y(), direction.Z()};
                candidate.planes.push_back(spec);
            }
            ConversionReport split_report;
            std::vector<TopoDS_Solid> pieces = split_solid_by_planes(
                solid, tools, options, split_report, report_index,
                candidate.tool_kind);
            candidate.diagnostics = split_report.diagnostics;
            candidate.rejection_reason =
                first_diagnostic_reason(candidate.diagnostics);
            finish_candidate(candidate, pieces);
            proposal.candidates.push_back(std::move(candidate));
            if (lazy_first_usable && has_high_confidence_in_proposal()) break;
        }
    }

    // Helper: evaluate one face as a decomposition candidate. Runs the
    // BRepAlgoAPI_Splitter call, scores the result, appends to the
    // proposal. The split is the expensive step; the per-piece analysis
    // inside finish_candidate is the second most expensive. Both run
    // exactly once per call.
    std::vector<gp_Pln> evaluated_planar_supports;
    auto evaluate_face = [&](std::size_t face_idx, const TopoDS_Face& face) {
        if (!candidate_budget_available()) return;
        BRepAdaptor_Surface adaptor(face, false);
        if (adaptor.GetType() == GeomAbs_Plane) {
            const gp_Pln plane = adaptor.Plane();
            const bool duplicate = std::any_of(
                evaluated_planar_supports.begin(),
                evaluated_planar_supports.end(),
                [&](const gp_Pln& existing) {
                    return std::abs(existing.Axis().Direction().Dot(
                                        plane.Axis().Direction()))
                               >= 1.0 - std::max(options.angular_tolerance,
                                                1.0e-9)
                        && existing.Distance(plane.Location())
                               <= std::max(options.linear_tolerance * 10.0,
                                           1.0e-6);
                });
            if (duplicate) return;
            evaluated_planar_supports.push_back(plane);
        }
        ++candidate_index;

        DecompositionCandidate candidate;
        candidate.id = "candidate:" + std::to_string(candidate_index);
        candidate.target_id = solid_id_str;
        candidate.source_face_id = is_piece
            ? solid_id_str + "/face:" + std::to_string(face_idx)
            : face_id(report_index, face_idx);
        candidate.source_face_label = is_piece
            ? solid_id_str + "/face[" + std::to_string(face_idx) + "]"
            : face_label(report_index, face_idx);
        candidate.surface_type = surface_type_name(adaptor.GetType());
        candidate.tool_kind = split_tool_kind_for_surface(adaptor.GetType());
        candidate.reason = has_convexity_failure_face
            && face_idx == convexity_failure_face_index
            ? "convexity_failure_face"
            : "existing_face";
        candidate.original_volume = original_volume;
        if (adaptor.GetType() == GeomAbs_Plane) {
            const gp_Pln support = adaptor.Plane();
            candidate.plane_valid = true;
            candidate.plane_origin = {
                support.Location().X(), support.Location().Y(),
                support.Location().Z()};
            const gp_Dir direction = support.Axis().Direction();
            candidate.plane_normal = {
                direction.X(), direction.Y(), direction.Z()};
        }

        ConversionReport split_report;
        std::vector<TopoDS_Solid> pieces =
            split_solid_by_face(solid, face, options, split_report,
                                report_index, face_idx);
        candidate.diagnostics = split_report.diagnostics;
        candidate.rejection_reason = first_diagnostic_reason(candidate.diagnostics);
        finish_candidate(candidate, pieces);
        proposal.candidates.push_back(std::move(candidate));
    };

    // High-confidence is the early-stop bar for the lazy path: any
    // weaker candidate (medium / bbox-fallback) might be beaten by a
    // not-yet-evaluated face. Only when *no* face produces high do we
    // fall through to bbox; only when *no* bbox is usable do we give
    // up. This preserves the baseline's "best candidate wins" semantics
    // without paying for exhaustive evaluation on the common case.
    if (lazy_first_usable) {
        // Priority order for the lazy path: convexity-failure face
        // first (it's usually the natural cut), then remaining faces in
        // TopExp order. Stop as soon as a *high-confidence* candidate
        // is found — a merely-usable candidate isn't enough because a
        // later face may produce a strictly better cut. When no high
        // candidate exists we evaluate every face and fall through to
        // the bbox fallback below, preserving the baseline's
        // best-candidate-wins semantics for the hard cases. The
        // exhaustive path keeps face-index order so candidate IDs stay
        // stable for the agent-review workflow.
        if (!has_high_confidence_in_proposal()
            && has_convexity_failure_face) {
            std::size_t fi = 0;
            for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
                 face_exp.More(); face_exp.Next()) {
                if (!candidate_budget_available()) break;
                ++fi;
                if (fi == convexity_failure_face_index) {
                    evaluate_face(fi, TopoDS::Face(face_exp.Current()));
                    break;
                }
            }
        }
        if (!has_high_confidence_in_proposal()
            && !convexity_failure_face_only) {
            std::size_t fi = 0;
            for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
                 face_exp.More(); face_exp.Next()) {
                if (!candidate_budget_available()) break;
                ++fi;
                if (has_convexity_failure_face
                    && fi == convexity_failure_face_index) {
                    continue;  // already tried
                }
                evaluate_face(fi, TopoDS::Face(face_exp.Current()));
                if (has_high_confidence_in_proposal()) {
                    break;
                }
            }
        }
    } else {
        // Exhaustive order: every face, by TopExp index. Candidate IDs
        // are derived from this iteration so they match the documented
        // face IDs (face:N.M ↔ candidate:M for solid:N's M-th face).
        std::size_t fi = 0;
        for (TopExp_Explorer face_exp(solid, TopAbs_FACE);
             face_exp.More(); face_exp.Next()) {
            if (!candidate_budget_available()) break;
            ++fi;
            if (convexity_failure_face_only
                && has_convexity_failure_face
                && fi != convexity_failure_face_index) {
                continue;
            }
            evaluate_face(fi, TopoDS::Face(face_exp.Current()));
        }
    }

    const bool has_usable_face_candidate = has_usable_in_proposal();

    if (!has_usable_face_candidate) {
        const BBox bbox = bbox_from_shape(solid);
        if (bbox.valid) {
            struct AxisFallback {
                const char* axis;
                gp_Pnt point;
                gp_Dir normal;
            };
            // Anchor the cutting planes at the volume centroid rather than
            // the bbox center. For non-convex pieces with off-center mass,
            // the bbox center can land outside the solid and produce
            // zero-piece splits. The centroid stays inside the convex hull
            // and is much more likely to actually bisect the part. The
            // centroid is clamped to the bbox in case OCCT returns a
            // value just outside numeric tolerance.
            const gp_Pnt com = original_props.CentreOfMass();
            const double cx = std::min(std::max(com.X(), bbox.xmin), bbox.xmax);
            const double cy = std::min(std::max(com.Y(), bbox.ymin), bbox.ymax);
            const double cz = std::min(std::max(com.Z(), bbox.zmin), bbox.zmax);
            const AxisFallback fallbacks[] = {
                {"x", gp_Pnt(cx, cy, cz), gp_Dir(1.0, 0.0, 0.0)},
                {"y", gp_Pnt(cx, cy, cz), gp_Dir(0.0, 1.0, 0.0)},
                {"z", gp_Pnt(cx, cy, cz), gp_Dir(0.0, 0.0, 1.0)},
            };
            const char* longest_axis = "x";
            double longest_extent = bbox_dx(bbox);
            if (bbox_dy(bbox) > longest_extent) {
                longest_axis = "y";
                longest_extent = bbox_dy(bbox);
            }
            if (bbox_dz(bbox) > longest_extent) {
                longest_axis = "z";
            }
            // Priority order: longest bbox axis first, then the others.
            // Lazy mode stops as soon as one axis split is usable, which
            // turns the worst case from 3 splits to 1 on parts where any
            // axis-aligned cut works.
            std::vector<AxisFallback> ordered_fallbacks;
            ordered_fallbacks.reserve(3);
            for (const AxisFallback& fallback : fallbacks) {
                if (std::string(fallback.axis) == longest_axis) {
                    ordered_fallbacks.push_back(fallback);
                }
            }
            for (const AxisFallback& fallback : fallbacks) {
                if (std::string(fallback.axis) != longest_axis) {
                    ordered_fallbacks.push_back(fallback);
                }
            }

            // The bbox section is already gated by "no usable face
            // candidate" — so it only fires on hard pieces, and only 3
            // splits are involved. We evaluate all 3 (no lazy stop) and
            // let the final sort pick the best by score. The cost is
            // bounded by 3 splitter calls; the coverage gain on parts
            // where the longest axis isn't the right cut is worth it.
            std::size_t bbox_attempts = 0;
            for (const AxisFallback& fallback : ordered_fallbacks) {
                if (!candidate_budget_available()) break;
                if (longest_bbox_axis_only && bbox_attempts >= 1) {
                    break;
                }
                ++bbox_attempts;
                ++candidate_index;
                DecompositionCandidate candidate;
                candidate.id = "candidate:" + std::to_string(candidate_index);
                candidate.target_id = solid_id_str;
                candidate.surface_type = "plane";
                candidate.tool_kind = std::string("bbox_plane_") + fallback.axis;
                candidate.reason = "bbox_axis_fallback";
                candidate.original_volume = original_volume;
                candidate.plane_valid = true;
                candidate.plane_origin = {fallback.point.X(),
                                          fallback.point.Y(),
                                          fallback.point.Z()};
                candidate.plane_normal = {fallback.normal.X(),
                                          fallback.normal.Y(),
                                          fallback.normal.Z()};

                ConversionReport split_report;
                const gp_Pln plane(fallback.point, fallback.normal);
                std::vector<TopoDS_Solid> pieces =
                    split_solid_by_plane(solid, plane, options, split_report,
                                         report_index, candidate.tool_kind);
                candidate.diagnostics = split_report.diagnostics;
                candidate.rejection_reason =
                    first_diagnostic_reason(candidate.diagnostics);
                finish_candidate(candidate, pieces);
                proposal.candidates.push_back(std::move(candidate));
            }

            // Multi-tool variant: try all three axis-aligned planes at
            // once. For parts with multiple parallel concavities (rails,
            // slotted brackets, E-shaped frames) one cut at a time is
            // not enough; handing the whole set to BRepAlgoAPI_Splitter
            // in a single call often produces 4+ usable pieces where
            // any single plane would have failed to bisect.
            if (include_multi_bbox_fallback && candidate_budget_available()) {
                ++candidate_index;
                DecompositionCandidate candidate;
                candidate.id = "candidate:" + std::to_string(candidate_index);
                candidate.target_id = solid_id_str;
                candidate.surface_type = "plane";
                candidate.tool_kind = "bbox_planes_xyz";
                candidate.reason = "bbox_axis_fallback";
                candidate.original_volume = original_volume;
                candidate.planes.reserve(3);
                std::vector<gp_Pln> occ_planes;
                occ_planes.reserve(3);
                for (const AxisFallback& fallback : fallbacks) {
                    PlaneSpec spec;
                    spec.origin = {fallback.point.X(), fallback.point.Y(),
                                   fallback.point.Z()};
                    spec.normal = {fallback.normal.X(), fallback.normal.Y(),
                                   fallback.normal.Z()};
                    candidate.planes.push_back(spec);
                    occ_planes.emplace_back(fallback.point, fallback.normal);
                }

                ConversionReport split_report;
                std::vector<TopoDS_Solid> pieces =
                    split_solid_by_planes(solid, occ_planes, options,
                                          split_report, report_index,
                                          candidate.tool_kind);
                candidate.diagnostics = split_report.diagnostics;
                candidate.rejection_reason =
                    first_diagnostic_reason(candidate.diagnostics);
                finish_candidate(candidate, pieces);
                proposal.candidates.push_back(std::move(candidate));
            }
        }
    }

    std::stable_sort(proposal.candidates.begin(), proposal.candidates.end(),
                     [](const DecompositionCandidate& a,
                        const DecompositionCandidate& b) {
        if (a.usable != b.usable) {
            return a.usable && !b.usable;
        }
        return a.score > b.score;
    });

    for (const DecompositionCandidate& candidate : proposal.candidates) {
        if (candidate.usable) {
            proposal.ok = true;
            proposal.best_candidate_id = candidate.id;
            if (candidate.confidence == "high"
                && !has_clear_score_dominance(proposal.candidates, candidate)) {
                for (DecompositionCandidate& mutable_candidate : proposal.candidates) {
                    if (mutable_candidate.id == candidate.id) {
                        mutable_candidate.confidence = "medium";
                        proposal.best_candidate_id = mutable_candidate.id;
                        break;
                    }
                }
            }
            return proposal;
        }
    }

    proposal.diagnostics.push_back({
        DiagnosticSeverity::warning,
        "decomposition_no_usable_candidate",
        "No face-driven decomposition candidate produced usable pieces.",
        solid_id_str
    });
    return proposal;
}

DecompositionProposal propose_decomposition_candidates(
    const CadModel& model,
    const std::string& solid_id_str,
    const ConversionOptions& options) {
    // Exhaustive: every face, every bbox axis, the multi-tool variant.
    // Used by `decompose-suggest` where the caller (human or agent) wants
    // to see all options.
    return propose_decomposition_candidates_impl(model,
                                                solid_id_str,
                                                options,
                                                /*convexity_failure_face_only=*/false,
                                                /*longest_bbox_axis_only=*/false,
                                                /*include_multi_bbox_fallback=*/true,
                                                /*lazy_first_usable=*/false,
                                                nullptr, false, 0, 0,
                                                std::numeric_limits<std::size_t>::max());
}

DecompositionProposal propose_bsp_decomposition_candidates(
    const CadModel& model,
    const std::string& solid_id_str,
    const ConversionOptions& options) {
    // BSP path: same candidate ladder as the exhaustive proposer, but
    // lazy — stop as soon as a usable candidate is found. Priority order
    // is convexity-failure face, then remaining faces, then bbox axes
    // (longest first). The multi-tool variant is excluded; it was the
    // runaway-prone candidate when auto-selected inside the BSP loop.
    return propose_decomposition_candidates_impl(model,
                                                solid_id_str,
                                                options,
                                                /*convexity_failure_face_only=*/false,
                                                /*longest_bbox_axis_only=*/false,
                                                /*include_multi_bbox_fallback=*/false,
                                                /*lazy_first_usable=*/true,
                                                nullptr, false, 0, 0,
                                                std::numeric_limits<std::size_t>::max());
}

namespace detail {

LocalBspProposal propose_bsp_decomposition_candidates_local(
    const CadModel& model,
    const std::string& solid_id_str,
    const ConversionOptions& options,
    bool include_parallel_plane_batches,
    std::size_t max_parallel_families,
    std::size_t max_planes_per_batch,
    std::size_t max_generated_candidates) {
    LocalBspProposal local;
    local.proposal = propose_decomposition_candidates_impl(
        model, solid_id_str, options,
        /*convexity_failure_face_only=*/false,
        /*longest_bbox_axis_only=*/false,
        /*include_multi_bbox_fallback=*/false,
        /*lazy_first_usable=*/true,
        &local.candidate_children,
        include_parallel_plane_batches,
        max_parallel_families,
        max_planes_per_batch,
        max_generated_candidates);
    return local;
}

bool commit_local_decomposition_partition(
    CadModel& model,
    const std::string& target_id,
    const std::vector<TopoDS_Solid>& children,
    std::vector<std::string>* committed_ids) {
    if (children.empty()) return false;
    DecompositionTarget target;
    if (!find_decomposition_target(model, target_id, &target)) return false;
    const std::string source_id = decomposition_source_id(target, target_id);
    if (target.is_piece) target.piece->active = false;
    else if (!remove_solid(model, target_id)) return false;
    if (committed_ids) {
        committed_ids->clear();
        committed_ids->reserve(children.size());
    }
    for (const TopoDS_Solid& child : children) {
        if (child.IsNull()) return false;
        GProp_GProps properties;
        BRepGProp::VolumeProperties(child, properties);
        DecomposedSolidPiece piece;
        piece.id = next_decomposed_piece_id(model);
        piece.label = piece.id + "/from_" + target_id;
        piece.source_solid_id = source_id;
        piece.solid = child;
        piece.volume = std::abs(properties.Mass());
        piece.active = true;
        if (committed_ids) committed_ids->push_back(piece.id);
        model.decomposed_pieces.push_back(std::move(piece));
    }
    return true;
}

} // namespace detail

bool decomposition_target_exact_csg_supported(const CadModel& model,
                                             const std::string& solid_id_str,
                                             const ConversionOptions& options,
                                             bool* found_out) {
    using namespace csgify::detail;

    if (found_out) {
        *found_out = false;
    }

    TopoDS_Solid solid;
    std::size_t report_index = 0;
    bool is_piece = false;
    if (!find_decomposition_target(model, solid_id_str, &solid, &report_index, &is_piece)) {
        return false;
    }

    if (found_out) {
        *found_out = true;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);

    DecomposedSolidPiece piece_record;
    piece_record.id = solid_id_str;
    piece_record.label = solid_id_str;
    piece_record.source_solid_id = is_piece ? solid_id_str : std::string();
    piece_record.solid = solid;
    piece_record.volume = std::abs(props.Mass());
    piece_record.active = true;

    std::size_t piece_index = 0;
    if (is_piece) {
        parse_piece_id(solid_id_str, &piece_index);
    } else {
        piece_index = report_index;
    }

    const AnalyzedDecomposedPiece analyzed =
        analyze_decomposed_piece(piece_record, piece_index, report_index, options);
    return analyzed.exact_csg_supported;
}

std::string add_synthetic_box(CadModel& model,
                              const BBox& bbox,
                              int material_id,
                              double density,
                              const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/box" : label;
    body.kind = SyntheticBodyKind::box;
    body.bbox = bbox;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_sphere(CadModel& model,
                                 Vec3 center,
                                 double radius,
                                 int material_id,
                                 double density,
                                 const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/sphere" : label;
    body.kind = SyntheticBodyKind::sphere;
    body.center = center;
    body.radius = radius;
    body.bbox.valid = true;
    body.bbox.xmin = center.x - radius;
    body.bbox.ymin = center.y - radius;
    body.bbox.zmin = center.z - radius;
    body.bbox.xmax = center.x + radius;
    body.bbox.ymax = center.y + radius;
    body.bbox.zmax = center.z + radius;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_cylinder(CadModel& model,
                                   Vec3 base,
                                   Vec3 height,
                                   double radius,
                                   int material_id,
                                   double density,
                                   const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/cylinder" : label;
    body.kind = SyntheticBodyKind::cylinder;
    body.base = base;
    body.height = height;
    body.radius = radius;
    body.bbox.valid = true;
    body.bbox.xmin = std::min(base.x, base.x + height.x) - radius;
    body.bbox.ymin = std::min(base.y, base.y + height.y) - radius;
    body.bbox.zmin = std::min(base.z, base.z + height.z) - radius;
    body.bbox.xmax = std::max(base.x, base.x + height.x) + radius;
    body.bbox.ymax = std::max(base.y, base.y + height.y) + radius;
    body.bbox.zmax = std::max(base.z, base.z + height.z) + radius;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_cone(CadModel& model,
                               Vec3 base,
                               Vec3 height,
                               double base_radius,
                               double top_radius,
                               int material_id,
                               double density,
                               const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/cone" : label;
    body.kind = SyntheticBodyKind::cone;
    body.base = base;
    body.height = height;
    body.radius = base_radius;
    body.top_radius = top_radius;
    const Vec3 top = add(base, height);
    const double r = std::max(base_radius, top_radius);
    body.bbox.valid = true;
    body.bbox.xmin = std::min(base.x, top.x) - r;
    body.bbox.ymin = std::min(base.y, top.y) - r;
    body.bbox.zmin = std::min(base.z, top.z) - r;
    body.bbox.xmax = std::max(base.x, top.x) + r;
    body.bbox.ymax = std::max(base.y, top.y) + r;
    body.bbox.zmax = std::max(base.z, top.z) + r;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_union(CadModel& model,
                                const std::string& left_id,
                                const std::string& right_id,
                                int material_id,
                                double density,
                                const std::string& label) {
    using namespace csgify::detail;
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/union" : label;
    body.kind = SyntheticBodyKind::union_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_intersection(CadModel& model,
                                       const std::string& left_id,
                                       const std::string& right_id,
                                       int material_id,
                                       double density,
                                       const std::string& label) {
    using namespace csgify::detail;
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/intersection" : label;
    body.kind = SyntheticBodyKind::intersection_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_difference(CadModel& model,
                                     const std::string& left_id,
                                     const std::string& right_id,
                                     int material_id,
                                     double density,
                                     const std::string& label) {
    using namespace csgify::detail;
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/difference" : label;
    body.kind = SyntheticBodyKind::difference_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

ReplacementMetrics replacement_metrics_for_operation(const AnalyzedSolid& solid,
                                                     ReplacementOperation operation) {
    switch (operation) {
    case ReplacementOperation::bbox:
        return solid.bbox_replacement;
    case ReplacementOperation::bbox_cylinder:
        return solid.bbox_cylinder_replacement;
    case ReplacementOperation::fitted_cylinder:
        return solid.fitted_cylinder_replacement.metrics;
    case ReplacementOperation::fitted_sphere:
        return solid.fitted_sphere_replacement.metrics;
    case ReplacementOperation::fitted_cone:
        return solid.fitted_cone_replacement.metrics;
    }
    return {};
}

ReplacementCheck check_replacement_limits(const CadModel& model,
                                          const std::string& solid_id,
                                          ReplacementOperation operation,
                                          const ReplacementLimits& limits,
                                          const ConversionOptions& options) {
    ReplacementCheck check;
    if (!limits.has_absolute_delta && !limits.has_relative_delta) {
        check.ok = true;
        return check;
    }

    const AnalysisReport report = analyze_model(model, options);
    const AnalyzedSolid* analyzed_solid = nullptr;
    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.id == solid_id) {
            analyzed_solid = &solid;
            break;
        }
    }

    if (!analyzed_solid) {
        check.diagnostic = {
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            solid_id
        };
        return check;
    }

    check.metrics = replacement_metrics_for_operation(*analyzed_solid, operation);
    if (!check.metrics.available) {
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_metrics_unavailable",
            "No replacement metrics are available for the requested operation.",
            solid_id
        };
        return check;
    }

    if (limits.has_absolute_delta
        && check.metrics.absolute_delta > limits.max_absolute_delta) {
        std::ostringstream msg;
        msg << "Replacement absolute volume delta exceeds limit. absolute_delta="
            << std::setprecision(10) << check.metrics.absolute_delta
            << " max_absolute_delta=" << limits.max_absolute_delta;
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_abs_delta_exceeded",
            msg.str(),
            solid_id
        };
        return check;
    }

    if (limits.has_relative_delta
        && check.metrics.relative_delta > limits.max_relative_delta) {
        std::ostringstream msg;
        msg << "Replacement relative volume delta exceeds limit. relative_delta="
            << std::setprecision(10) << check.metrics.relative_delta
            << " max_relative_delta=" << limits.max_relative_delta;
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_rel_delta_exceeded",
            msg.str(),
            solid_id
        };
        return check;
    }

    check.ok = true;
    return check;
}

bool replace_solid_with_bbox(CadModel& model,
                             const std::string& id,
                             const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_solid_bbox",
            "Could not build a valid bounding box replacement for the requested solid.",
            id
        });
        return false;
    }

    remove_solid(model, id);
    add_synthetic_box(model, bbox, options.default_material_id, options.default_density,
                      next_synthetic_id(model) + "/bbox_from_" + id);
    return true;
}

bool replace_solid_with_bbox_cylinder(CadModel& model,
                                      const std::string& id,
                                      const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_solid_bbox",
            "Could not build a valid bounding box cylinder replacement for the requested solid.",
            id
        });
        return false;
    }

    const double dx = bbox_dx(bbox);
    const double dy = bbox_dy(bbox);
    const double dz = bbox_dz(bbox);
    const double cx = 0.5 * (bbox.xmin + bbox.xmax);
    const double cy = 0.5 * (bbox.ymin + bbox.ymax);
    const double cz = 0.5 * (bbox.zmin + bbox.zmax);

    Vec3 base;
    Vec3 height;
    double radius = 0.0;

    if (dx >= dy && dx >= dz) {
        base = {bbox.xmin, cy, cz};
        height = {dx, 0.0, 0.0};
        radius = 0.5 * std::max(dy, dz);
    } else if (dy >= dx && dy >= dz) {
        base = {cx, bbox.ymin, cz};
        height = {0.0, dy, 0.0};
        radius = 0.5 * std::max(dx, dz);
    } else {
        base = {cx, cy, bbox.zmin};
        height = {0.0, 0.0, dz};
        radius = 0.5 * std::max(dx, dy);
    }

    if (vec_len(height) <= options.linear_tolerance || radius <= options.linear_tolerance) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_bbox_cylinder",
            "Bounding box is too small or degenerate for a cylinder replacement.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const double replacement_volume =
        3.14159265358979323846264338327950288 * radius * radius * vec_len(height);
    std::ostringstream msg;
    msg << "Replacing solid with conservative bbox cylinder. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << replacement_volume;
    if (original_volume > kEps) {
        msg << " relative_delta="
            << std::abs(replacement_volume - original_volume) / original_volume;
    }
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "bbox_cylinder_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cylinder(model, base, height, radius,
                           options.default_material_id, options.default_density,
                           next_synthetic_id(model) + "/bbox_cylinder_from_" + id);
    return true;
}

bool replace_solid_with_fitted_cylinder(CadModel& model,
                                        const std::string& id,
                                        const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedCylinderCandidate candidate =
        fitted_cylinder_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_cylinder_unavailable",
            "No usable cylindrical face was found for fitted cylinder replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted cylinder. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_cylinder_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cylinder(model, candidate.base, candidate.height, candidate.radius,
                           options.default_material_id, options.default_density,
                           next_synthetic_id(model) + "/fitted_cylinder_from_" + id);
    return true;
}

bool replace_solid_with_fitted_sphere(CadModel& model,
                                      const std::string& id,
                                      const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedSphereCandidate candidate =
        fitted_sphere_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_sphere_unavailable",
            "No usable spherical face was found for fitted sphere replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted sphere. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_sphere_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_sphere(model, candidate.center, candidate.radius,
                         options.default_material_id, options.default_density,
                         next_synthetic_id(model) + "/fitted_sphere_from_" + id);
    return true;
}

bool replace_solid_with_fitted_cone(CadModel& model,
                                    const std::string& id,
                                    const ConversionOptions& options) {
    using namespace csgify::detail;
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedConeCandidate candidate =
        fitted_cone_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_cone_unavailable",
            "No usable conical face was found for fitted cone replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted cone. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_cone_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cone(model, candidate.base, candidate.height,
                       candidate.base_radius, candidate.top_radius,
                       options.default_material_id, options.default_density,
                       next_synthetic_id(model) + "/fitted_cone_from_" + id);
    return true;
}

bool apply_replacement_operation(CadModel& model,
                                 const std::string& solid_id,
                                 ReplacementOperation operation,
                                 const ConversionOptions& options) {
    switch (operation) {
    case ReplacementOperation::bbox:
        return replace_solid_with_bbox(model, solid_id, options);
    case ReplacementOperation::bbox_cylinder:
        return replace_solid_with_bbox_cylinder(model, solid_id, options);
    case ReplacementOperation::fitted_cylinder:
        return replace_solid_with_fitted_cylinder(model, solid_id, options);
    case ReplacementOperation::fitted_sphere:
        return replace_solid_with_fitted_sphere(model, solid_id, options);
    case ReplacementOperation::fitted_cone:
        return replace_solid_with_fitted_cone(model, solid_id, options);
    }
    return false;
}

bool apply_checked_replacement_operation(CadModel& model,
                                         const std::string& solid_id,
                                         ReplacementOperation operation,
                                         const ReplacementLimits& limits,
                                         ReplacementCheck* check_out,
                                         const ConversionOptions& options) {
    ReplacementCheck check =
        check_replacement_limits(model, solid_id, operation, limits, options);
    if (check_out) {
        *check_out = check;
    }
    if (!check.ok) {
        if (!check.diagnostic.code.empty()) {
            model.diagnostics.push_back(check.diagnostic);
        }
        return false;
    }
    return apply_replacement_operation(model, solid_id, operation, options);
}

} // namespace csgify
