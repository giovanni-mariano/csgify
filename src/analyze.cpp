// Analyze stage: walk a CadModel and produce an AnalysisReport with per-solid
// and per-face classification, fitted-primitive candidates, cleanup metrics,
// and diagnostics. No mutation of the model; pure read.

#include "cad2csg/engine.hpp"

#include "analytic_surface.hpp"
#include "engine_internal.hpp"
#include "fitted_candidates.hpp"
#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>

namespace cad2csg {
namespace {

using namespace cad2csg::detail;

const ShapeMetadata* find_solid_metadata(const CadModel& model, const std::string& id) {
    for (const ShapeMetadata& metadata : model.solid_metadata) {
        if (metadata.shape_id == id) {
            return &metadata;
        }
    }
    return nullptr;
}

void inspect_solid(const TopoDS_Solid& solid,
                   std::size_t solid_index,
                   const CadModel& model,
                   const ConversionOptions& options,
                   ConversionReport& report) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double volume = std::abs(props.Mass());
    const std::string label = solid_label(solid_index);
    AnalyzedSolid analyzed_solid;
    analyzed_solid.id = solid_id(solid_index);
    analyzed_solid.label = label;
    analyzed_solid.index = solid_index;
    if (const ShapeMetadata* metadata = find_solid_metadata(model, analyzed_solid.id)) {
        analyzed_solid.name = metadata->name;
        analyzed_solid.hierarchy_path = metadata->hierarchy_path;
    }
    analyzed_solid.volume = volume;
    analyzed_solid.bbox = bbox_from_shape(solid);

    report.total_volume += volume;
    report.solid_volumes.push_back({label, volume});

    if (volume <= options.linear_tolerance) {
        add_diag(report, DiagnosticSeverity::warning, "empty_or_tiny_solid",
                 "Solid has near-zero volume and was skipped.", label);
        analyzed_solid.needs_user_decision = true;
        report.solids.push_back(analyzed_solid);
        return;
    }

    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        ++report.faces_seen;
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());

        AnalyzedFace analyzed_face;
        analyzed_face.id = face_id(solid_index, face_index);
        analyzed_face.label = face_label(solid_index, face_index);
        analyzed_face.solid_id = analyzed_solid.id;
        analyzed_face.index = face_index;
        analyzed_face.bbox = bbox_from_shape(face);

        GProp_GProps face_props;
        BRepGProp::SurfaceProperties(face, face_props);
        analyzed_face.area = std::abs(face_props.Mass());

        BRepAdaptor_Surface adaptor(face, false);
        const GeomAbs_SurfaceType surface_type = adaptor.GetType();
        AnalyticSurface surface;
        const bool exact_supported =
            analytic_surface_from_face(face, solid_index, face_index, report, true, &surface);
        if (exact_supported) {
            ++report.analytic_faces;
        }
        analyzed_face.surface_type = surface_type_name(surface_type);
        analyzed_face.exact_csg_supported = exact_supported;
        analyzed_face.needs_spline_replacement = surface_type_is_spline(surface_type);
        analyzed_face.needs_emitter =
            !exact_supported && surface_type_needs_emitter(surface_type);

        if (analyzed_face.needs_spline_replacement) {
            analyzed_face.spline_fits = fit_spline_face(face);
            // Pick the best fit and append a hint to the freshly-emitted
            // unsupported_spline_face diagnostic for this face.
            const SplineFitCandidates& f = analyzed_face.spline_fits;
            const SplineFaceFit* best = nullptr;
            auto consider = [&](const SplineFaceFit& fit) {
                if (!fit.available) {
                    return;
                }
                if (!best || fit.relative_max_deviation < best->relative_max_deviation) {
                    best = &fit;
                }
            };
            consider(f.plane);
            consider(f.sphere);
            consider(f.cylinder);
            consider(f.cone);
            if (best && !report.diagnostics.empty()) {
                Diagnostic& last = report.diagnostics.back();
                if (last.code == "unsupported_spline_face"
                    && last.shape_label == analyzed_face.label) {
                    std::ostringstream extra;
                    extra << " Best analytic fit: "
                          << spline_fit_kind_name(best->kind)
                          << " (max_deviation=" << best->max_deviation
                          << ", relative=" << best->relative_max_deviation << ").";
                    last.message += extra.str();
                }
            }
        }

        analyzed_solid.face_ids.push_back(analyzed_face.id);
        analyzed_solid.face_count = analyzed_solid.face_ids.size();
        analyzed_solid.needs_spline_replacement =
            analyzed_solid.needs_spline_replacement || analyzed_face.needs_spline_replacement;
        analyzed_solid.needs_user_decision =
            analyzed_solid.needs_user_decision
            || analyzed_face.needs_spline_replacement
            || analyzed_face.needs_emitter
            || !analyzed_face.exact_csg_supported;
        report.faces.push_back(analyzed_face);
    }

    gp_Pnt interior_point;
    if (!find_interior_point(solid, props, options.linear_tolerance, &interior_point)) {
        add_diag(report, DiagnosticSeverity::error, "interior_point_not_found",
                 "Could not find an interior point for selecting face halfspaces.",
                 label);
        return;
    }

    if (options.require_convex_cells
        && !validate_convex_halfspace_cell(solid, interior_point, solid_index,
                                           options, report, false, true, false)) {
        analyzed_solid.needs_decomposition = true;
        analyzed_solid.needs_user_decision = true;
        add_diag(report, DiagnosticSeverity::error,
                 "nonconvex_solid_requires_decomposition",
                 "At least one vertex lies outside a face halfspace selected from the solid interior point; emitting one Alea intersection cell would remove valid volume.",
                 label);
    }

    analyzed_solid.exact_csg_supported =
        !analyzed_solid.needs_decomposition
        && !analyzed_solid.needs_spline_replacement
        && !analyzed_solid.needs_user_decision;
    analyzed_solid.fitted_cylinder_replacement =
        fitted_cylinder_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_sphere_replacement =
        fitted_sphere_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_cone_replacement =
        fitted_cone_candidate(solid, analyzed_solid.volume);
    report.solids.push_back(analyzed_solid);
}

} // namespace

namespace detail {

AnalyzedDecomposedPiece analyze_decomposed_piece(const DecomposedSolidPiece& piece,
                                                 std::size_t index,
                                                 std::size_t report_solid_index,
                                                 const ConversionOptions& options) {
    AnalyzedDecomposedPiece analyzed;
    analyzed.id = piece.id;
    analyzed.label = piece.label;
    analyzed.source_solid_id = piece.source_solid_id;
    analyzed.index = index;
    analyzed.volume = piece.volume;
    analyzed.bbox = bbox_from_shape(piece.solid);

    bool all_faces_supported = true;
    for (TopExp_Explorer face_exp(piece.solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++analyzed.face_count;
        ConversionReport scratch;
        AnalyticSurface surface;
        if (!analytic_surface_from_face(TopoDS::Face(face_exp.Current()),
                                        report_solid_index,
                                        analyzed.face_count,
                                        scratch,
                                        false,
                                        &surface)) {
            all_faces_supported = false;
            BRepAdaptor_Surface adaptor(TopoDS::Face(face_exp.Current()), false);
            if (surface_type_is_spline(adaptor.GetType())) {
                analyzed.needs_spline_replacement = true;
            }
        }
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(piece.solid, props);
    gp_Pnt interior_point;
    if (!find_interior_point(piece.solid, props, options.linear_tolerance, &interior_point)) {
        analyzed.needs_user_decision = true;
        return analyzed;
    }

    ConversionReport scratch;
    const bool convex =
        validate_convex_halfspace_cell(piece.solid, interior_point, report_solid_index,
                                       options, scratch, false, false, false);
    analyzed.needs_decomposition = !convex;
    analyzed.exact_csg_supported =
        all_faces_supported
        && convex
        && !analyzed.needs_spline_replacement;
    analyzed.needs_user_decision =
        !analyzed.exact_csg_supported
        || analyzed.needs_decomposition
        || analyzed.needs_spline_replacement;
    return analyzed;
}

void finalize_cleanup_metrics(ConversionReport& report, const ConversionOptions& options) {
    double model_diag = 0.0;
    for (AnalyzedSolid& solid : report.solids) {
        solid.bbox_diagonal = bbox_diagonal(solid.bbox);
        model_diag = std::max(model_diag, solid.bbox_diagonal);
    }

    for (AnalyzedSolid& solid : report.solids) {
        if (report.total_volume > kEps) {
            solid.volume_fraction = solid.volume / report.total_volume;
        }
        if (model_diag > kEps) {
            solid.bbox_diagonal_fraction = solid.bbox_diagonal / model_diag;
        }

        sorted_bbox_extents(solid.bbox,
                            &solid.bbox_min_extent,
                            &solid.bbox_mid_extent,
                            &solid.bbox_max_extent);
        if (solid.bbox_min_extent > kEps) {
            solid.bbox_aspect_ratio = solid.bbox_max_extent / solid.bbox_min_extent;
        }

        solid.small_volume_candidate =
            solid.volume_fraction > 0.0
            && solid.volume_fraction <= options.small_volume_fraction;
        solid.small_bbox_candidate =
            solid.bbox_diagonal_fraction > 0.0
            && solid.bbox_diagonal_fraction <= options.small_bbox_fraction;
        solid.thin_plate_candidate =
            solid.bbox_max_extent > kEps
            && solid.bbox_min_extent / solid.bbox_max_extent <= options.thin_plate_ratio
            && solid.bbox_mid_extent / solid.bbox_max_extent > options.thin_plate_ratio;
        solid.long_body_candidate =
            solid.bbox_min_extent > kEps
            && solid.bbox_aspect_ratio >= options.long_body_ratio
            && solid.bbox_mid_extent / solid.bbox_max_extent <= 0.25;

        solid.bbox_replacement =
            replacement_metrics(solid.volume, bbox_volume(solid.bbox));
        solid.bbox_cylinder_replacement =
            replacement_metrics(solid.volume, bbox_conservative_cylinder_volume(solid.bbox));
    }
}

} // namespace

AnalysisReport analyze_model(const CadModel& model,
                             const ConversionOptions& options) {
    using namespace cad2csg::detail;
    AnalysisReport report;
    report.diagnostics = model.diagnostics;
    report.assembly = model.assembly;

    if (model.root.IsNull()) {
        return report;
    }

    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) {
            add_diag(report, DiagnosticSeverity::info, "solid_removed",
                     "Solid is suppressed by preprocessing.", solid_label(solid_index));
            continue;
        }
        ++report.solids_seen;
        inspect_solid(TopoDS::Solid(solid_exp.Current()), solid_index, model, options, report);
    }

    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) {
            continue;
        }
        AnalyzedSolid solid;
        solid.id = body.id;
        solid.label = body.label;
        solid.volume = synthetic_volume_estimate(body);
        solid.bbox = body.bbox;
        solid.exact_csg_supported = true;
        report.solids.push_back(solid);
        report.solid_volumes.push_back({body.label, solid.volume});
        report.total_volume += solid.volume;
        ++report.solids_seen;
    }

    std::size_t piece_index = 0;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) {
            continue;
        }
        ++piece_index;
        const std::size_t report_solid_index = 100000 + piece_index;
        AnalyzedDecomposedPiece analyzed_piece =
            analyze_decomposed_piece(piece, piece_index, report_solid_index, options);
        report.decomposed_pieces.push_back(analyzed_piece);
        report.solid_volumes.push_back({piece.label, piece.volume});
        report.total_volume += piece.volume;
        ++report.solids_seen;
        ++report.decomposition_pieces;
        if (analyzed_piece.needs_decomposition) {
            ++report.solids_requiring_decomposition;
        }
    }

    finalize_cleanup_metrics(report, options);

    if (report.solids_seen == 0) {
        add_diag(report, DiagnosticSeverity::error, "no_active_solids",
                 "No active solids remain after import and preprocessing.");
    }

    return report;
}

ConversionReport inspect_step_file(const std::string& step_path,
                                   const ConversionOptions& options) {
    CadModel model = load_step_model(step_path, options);
    return analyze_model(model, options);
}

} // namespace cad2csg
