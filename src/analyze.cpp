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
#include <utility>
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

DecisionMetric metric(const std::string& name, double value, const std::string& unit = {}) {
    DecisionMetric out;
    out.name = name;
    out.value = value;
    out.unit = unit;
    return out;
}

DecisionCandidateView candidate_view(const std::string& id,
                                     const std::string& action,
                                     const std::string& confidence,
                                     const std::string& reason,
                                     std::vector<DecisionMetric> metrics = {}) {
    DecisionCandidateView candidate;
    candidate.id = id;
    candidate.action = action;
    candidate.confidence = confidence;
    candidate.reason = reason;
    candidate.metrics = std::move(metrics);
    return candidate;
}

const SplineFaceFit* best_spline_fit(const SplineFitCandidates& fits) {
    const SplineFaceFit* best = nullptr;
    auto consider = [&](const SplineFaceFit& fit) {
        if (!fit.available) {
            return;
        }
        if (!best || fit.relative_max_deviation < best->relative_max_deviation) {
            best = &fit;
        }
    };
    consider(fits.plane);
    consider(fits.sphere);
    consider(fits.cylinder);
    consider(fits.cone);
    return best;
}

std::string replacement_action_for_fit(SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::plane:
        return "replace_face_with_plane";
    case SplineFitKind::sphere:
        return "replace_face_with_sphere";
    case SplineFitKind::cylinder:
        return "replace_face_with_cylinder";
    case SplineFitKind::cone:
        return "replace_face_with_cone";
    case SplineFitKind::none:
        break;
    }
    return "manual";
}

std::string fit_confidence(const SplineFaceFit& fit) {
    if (!fit.available) {
        return "none";
    }
    if (fit.relative_max_deviation <= 1.0e-8) {
        return "high";
    }
    if (fit.relative_max_deviation <= 1.0e-4) {
        return "medium";
    }
    return "low";
}

std::vector<DecisionCandidateView> spline_fit_candidates_for_face(
    const SplineFitCandidates& fits) {
    std::vector<DecisionCandidateView> out;
    auto add = [&](const SplineFaceFit& fit) {
        if (!fit.available) {
            return;
        }
        const std::string kind = spline_fit_kind_name(fit.kind);
        out.push_back(candidate_view(
            "fit:" + kind,
            replacement_action_for_fit(fit.kind),
            fit_confidence(fit),
            "analytic_fit",
            {metric("max_deviation", fit.max_deviation),
             metric("rms_deviation", fit.rms_deviation),
             metric("relative_max_deviation", fit.relative_max_deviation)}));
    };
    add(fits.plane);
    add(fits.sphere);
    add(fits.cylinder);
    add(fits.cone);
    return out;
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
        const FaceReplacement* replacement =
            find_face_replacement(&model.face_replacements, solid_index, face_index);
        const bool exact_supported =
            analytic_surface_from_face(face,
                                       solid_index,
                                       face_index,
                                       report,
                                       true,
                                       &surface,
                                       &model.face_replacements);
        if (exact_supported) {
            ++report.analytic_faces;
        }
        analyzed_face.surface_type = surface_type_name(surface_type);
        analyzed_face.support_kind = replacement
            ? ("replacement_" + std::string(spline_fit_kind_name(replacement->kind)))
            : surface_support_kind_name(surface_type);
        analyzed_face.exact_csg_supported = exact_supported;
        analyzed_face.needs_spline_replacement =
            surface_type_is_spline(surface_type) && !replacement;
        analyzed_face.needs_emitter =
            !exact_supported && surface_type_needs_emitter(surface_type);
        analyzed_face.needs_surface_replacement =
            analyzed_face.needs_spline_replacement
            || (!exact_supported && !analyzed_face.needs_emitter);

        if (analyzed_face.needs_spline_replacement) {
            analyzed_face.spline_fits = fit_spline_face(face);
            // Pick the best fit and append a hint to the freshly-emitted
            // unsupported_spline_face diagnostic for this face.
            const SplineFaceFit* best = best_spline_fit(analyzed_face.spline_fits);
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
        analyzed_solid.needs_surface_replacement =
            analyzed_solid.needs_surface_replacement
            || analyzed_face.needs_surface_replacement;
        analyzed_solid.needs_user_decision =
            analyzed_solid.needs_user_decision
            || analyzed_face.needs_spline_replacement
            || analyzed_face.needs_surface_replacement
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
        && !analyzed_solid.needs_surface_replacement
        && !analyzed_solid.needs_user_decision;
    analyzed_solid.fitted_cylinder_replacement =
        fitted_cylinder_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_sphere_replacement =
        fitted_sphere_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_cone_replacement =
        fitted_cone_candidate(solid, analyzed_solid.volume);
    report.solids.push_back(analyzed_solid);
}

void append_decision_requests(ConversionReport& report) {
    std::vector<DecisionRequest> requests;

    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.needs_decomposition) {
            DecisionRequest request;
            request.id = "decision:decomposition:" + solid.id;
            request.kind = "decomposition";
            request.target_id = solid.id;
            request.reason = "nonconvex_or_not_single_cell_convertible";
            request.recommended_action = "decompose-suggest";
            request.summary =
                "Solid needs decomposition before it can be emitted as valid CSG.";
            request.metrics = {
                metric("volume", solid.volume),
                metric("volume_fraction", solid.volume_fraction),
                metric("face_count", static_cast<double>(solid.face_count)),
            };
            request.candidates.push_back(candidate_view(
                "action:decompose-suggest",
                "decompose-suggest",
                "review",
                "Generate split candidates and ask the user only if no high-confidence candidate exists."));
            requests.push_back(std::move(request));
        }

        if (solid.small_volume_candidate || solid.small_bbox_candidate
            || solid.thin_plate_candidate || solid.long_body_candidate) {
            DecisionRequest request;
            request.id = "decision:semantic_cleanup:" + solid.id;
            request.kind = "semantic_cleanup";
            request.target_id = solid.id;
            request.reason = "cleanup_candidate";
            request.recommended_action = "remove-suggest";
            request.summary =
                "Solid has cleanup metrics that may indicate removable or simplifiable CAD detail.";
            request.metrics = {
                metric("volume", solid.volume),
                metric("volume_fraction", solid.volume_fraction),
                metric("bbox_diagonal_fraction", solid.bbox_diagonal_fraction),
                metric("bbox_aspect_ratio", solid.bbox_aspect_ratio),
            };
            if (solid.small_volume_candidate) {
                request.candidates.push_back(candidate_view(
                    "action:review_remove_small_volume",
                    "remove_solid",
                    "review",
                    "Small volume relative to the active model."));
            }
            if (solid.small_bbox_candidate) {
                request.candidates.push_back(candidate_view(
                    "action:review_remove_small_bbox",
                    "remove_solid",
                    "review",
                    "Small bounding box relative to the active model."));
            }
            if (solid.thin_plate_candidate) {
                request.candidates.push_back(candidate_view(
                    "action:review_simplify_thin_plate",
                    "replace_solid",
                    "review",
                    "Thin plate-like body may be removable or replaceable depending on neutronics intent."));
            }
            if (solid.long_body_candidate) {
                request.candidates.push_back(candidate_view(
                    "action:review_simplify_long_body",
                    "replace_solid",
                    "review",
                    "Long thin body may be a rail, rod, wire, or support requiring human semantics."));
            }
            requests.push_back(std::move(request));
        }

        auto add_replacement_review =
            [&](const std::string& id,
                const std::string& action,
                const ReplacementMetrics& metrics) {
                if (!metrics.available) {
                    return;
                }
                if (!solid.exact_csg_supported) {
                    return;
                }
                constexpr double kReviewFloor = 1.0e-4;
                if (metrics.relative_delta > kReviewFloor
                    && metrics.relative_delta <= 0.05) {
                    DecisionRequest request;
                    request.id = "decision:replacement:" + solid.id + ":" + id;
                    request.kind = "replacement";
                    request.target_id = solid.id;
                    request.reason = "low_delta_simplified_body_candidate";
                    request.recommended_action = action;
                    request.summary =
                        "A simplified body has small volume delta and may be acceptable after visual review.";
                    request.metrics = {
                        metric("original_volume", solid.volume),
                        metric("replacement_volume", metrics.volume),
                        metric("absolute_delta", metrics.absolute_delta),
                        metric("relative_delta", metrics.relative_delta),
                        metric("fill_ratio", metrics.fill_ratio),
                    };
                    request.candidates.push_back(candidate_view(
                        "action:" + action,
                        action,
                        metrics.relative_delta <= 0.01 ? "medium" : "low",
                        "Volume-preserving simplified replacement candidate.",
                        request.metrics));
                    requests.push_back(std::move(request));
                }
            };
        add_replacement_review("bbox", "replace_solid_bbox", solid.bbox_replacement);
        add_replacement_review("bbox_cylinder", "replace_solid_bbox_cylinder",
                               solid.bbox_cylinder_replacement);
        add_replacement_review("fitted_cylinder", "replace_solid_fitted_cylinder",
                               solid.fitted_cylinder_replacement.metrics);
        add_replacement_review("fitted_sphere", "replace_solid_fitted_sphere",
                               solid.fitted_sphere_replacement.metrics);
        add_replacement_review("fitted_cone", "replace_solid_fitted_cone",
                               solid.fitted_cone_replacement.metrics);
    }

    for (const AnalyzedFace& face : report.faces) {
        if (face.needs_spline_replacement) {
            DecisionRequest request;
            request.id = "decision:spline_replacement:" + face.id;
            request.kind = "spline_replacement";
            request.target_id = face.id;
            request.reason = "unsupported_spline_face";
            const SplineFaceFit* best = best_spline_fit(face.spline_fits);
            if (best) {
                request.recommended_action = replacement_action_for_fit(best->kind);
                request.summary = "Spline face has analytic fit candidates; visual review is needed unless the deviation budget is project-approved.";
                request.metrics = {
                    metric("area", face.area),
                    metric("best_max_deviation", best->max_deviation),
                    metric("best_rms_deviation", best->rms_deviation),
                    metric("best_relative_max_deviation", best->relative_max_deviation),
                };
            } else {
                request.recommended_action = "manual";
                request.summary =
                    "Spline face has no analytic fit candidate within the current fitter capabilities.";
                request.metrics = {metric("area", face.area)};
            }
            request.candidates = spline_fit_candidates_for_face(face.spline_fits);
            if (request.candidates.empty()) {
                request.candidates.push_back(candidate_view(
                    "action:manual", "manual", "review",
                    "No analytic fit is available."));
            }
            requests.push_back(std::move(request));
        } else if (face.needs_emitter || !face.exact_csg_supported) {
            DecisionRequest request;
            request.id = "decision:unsupported_surface:" + face.id;
            request.kind = "unsupported_surface";
            request.target_id = face.id;
            request.reason = "unsupported_analytic_face";
            request.recommended_action = "manual";
            request.summary =
                "Face is not currently supported by the emitter and needs an explicit treatment.";
            request.metrics = {metric("area", face.area)};
            request.candidates.push_back(candidate_view(
                "action:add_emitter_or_replace",
                "add_emitter_or_replace_surface",
                "review",
                "Implement emitter support, replace the face, or decompose the parent solid."));
            requests.push_back(std::move(request));
        }
    }

    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        if (piece.needs_decomposition || piece.needs_spline_replacement
            || piece.needs_surface_replacement
            || piece.needs_user_decision) {
            DecisionRequest request;
            request.id = "decision:piece_repair:" + piece.id;
            request.kind = "piece_repair";
            request.target_id = piece.id;
            request.reason = "decomposed_piece_not_convertible";
            request.recommended_action =
                piece.needs_decomposition ? "decompose-suggest" : "manual";
            request.summary =
                "A decomposed piece still is not directly convertible to CSG.";
            request.metrics = {
                metric("volume", piece.volume),
                metric("face_count", static_cast<double>(piece.face_count)),
            };
            request.candidates.push_back(candidate_view(
                "action:repair_piece",
                request.recommended_action,
                "review",
                "Continue decomposition, replace unsupported surfaces, or inspect manually."));
            requests.push_back(std::move(request));
        }
    }

    std::sort(requests.begin(), requests.end(),
              [](const DecisionRequest& a, const DecisionRequest& b) {
                  if (a.target_id != b.target_id) {
                      return a.target_id < b.target_id;
                  }
                  if (a.kind != b.kind) {
                      return a.kind < b.kind;
                  }
                  return a.id < b.id;
              });
    report.decision_requests = std::move(requests);
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
            if (!surface_type_needs_emitter(adaptor.GetType())) {
                analyzed.needs_surface_replacement = true;
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
        && !analyzed.needs_spline_replacement
        && !analyzed.needs_surface_replacement;
    analyzed.needs_user_decision =
        !analyzed.exact_csg_supported
        || analyzed.needs_decomposition
        || analyzed.needs_spline_replacement
        || analyzed.needs_surface_replacement;
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
    append_decision_requests(report);

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
