// Cleanup / Geometric Repair stage. The mutator boundary on `CadModel`:
// every command here either records a decision (face replacement, removal)
// or attaches a synthetic body / decomposed piece. The original CAD geometry
// is never destroyed in-place; conversion is a final read elsewhere.

#include "cad2csg/engine.hpp"

#include "analytic_surface.hpp"
#include "decompose.hpp"
#include "engine_internal.hpp"
#include "fitted_candidates.hpp"
#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
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

namespace cad2csg {
namespace {

using namespace cad2csg::detail;

std::string next_synthetic_id(const CadModel& model) {
    return "synthetic:" + std::to_string(model.synthetic_bodies.size() + 1);
}

std::string next_decomposed_piece_id(const CadModel& model) {
    return "piece:" + std::to_string(model.decomposed_pieces.size() + 1);
}

bool find_solid_by_id(const CadModel& model,
                      const std::string& id,
                      TopoDS_Solid* solid_out,
                      std::size_t* index_out) {
    std::size_t target_index = 0;
    if (!parse_solid_id(id, &target_index)) {
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

} // namespace

bool remove_solid(CadModel& model, const std::string& id) {
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
                           const ConversionOptions& /*options*/) {
    using namespace cad2csg::detail;
    auto emit = [&](DiagnosticSeverity sev, const char* code, const std::string& msg) {
        Diagnostic d{sev, code, msg, face_id_str};
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
    return true;
}

AutoFitSummary auto_fit_splines(CadModel& model,
                                bool has_max_deviation,
                                double max_deviation,
                                const ConversionOptions& options) {
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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
    using namespace cad2csg::detail;
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

} // namespace cad2csg
