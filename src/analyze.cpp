// Analyze stage: walk a CadModel and produce an AnalysisReport with per-solid
// and per-face classification, fitted-primitive candidates, cleanup metrics,
// and diagnostics. No mutation of the model; pure read.

#include "csgify/engine.hpp"

#include "analytic_surface.hpp"
#include "engine_internal.hpp"
#include "topology_analysis.hpp"
#include "fitted_candidates.hpp"
#include "occt_boolean.hpp"
#include "spline_fit.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace csgify {
namespace {

using namespace csgify::detail;

bool rigid_transform(const InstanceTransform& transform) {
    const auto& m = transform.values;
    for (int column_a = 0; column_a < 3; ++column_a) {
        for (int column_b = 0; column_b < 3; ++column_b) {
            double dot = 0.0;
            for (int row = 0; row < 3; ++row) {
                dot += m[4 * row + column_a] * m[4 * row + column_b];
            }
            const double expected = column_a == column_b ? 1.0 : 0.0;
            if (std::abs(dot - expected) > 1.0e-9) return false;
        }
    }
    return true;
}

InstanceTransform shape_location_transform(const TopoDS_Shape& shape) {
    InstanceTransform result;
    const gp_Trsf transform = shape.Location().Transformation();
    for (int row = 1; row <= 3; ++row) {
        for (int column = 1; column <= 4; ++column) {
            result.values[4 * (row - 1) + (column - 1)] =
                transform.Value(row, column);
        }
    }
    return result;
}

Vec3 occurrence_point(const Vec3& point,
                      const InstanceTransform& from,
                      const InstanceTransform& to) {
    // Instance matrices are rigid local-to-document transforms. First apply
    // from^-1 (R^T), then apply `to`.
    const auto& a = from.values;
    const auto& b = to.values;
    const double shifted[3] = {
        point.x - a[3], point.y - a[7], point.z - a[11]
    };
    double local[3]{};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            local[column] += a[4 * row + column] * shifted[row];
        }
    }
    Vec3 result;
    result.x = b[0] * local[0] + b[1] * local[1] + b[2] * local[2] + b[3];
    result.y = b[4] * local[0] + b[5] * local[1] + b[6] * local[2] + b[7];
    result.z = b[8] * local[0] + b[9] * local[1] + b[10] * local[2] + b[11];
    return result;
}

Vec3 occurrence_direction(const Vec3& direction,
                          const InstanceTransform& from,
                          const InstanceTransform& to) {
    InstanceTransform from_without_translation = from;
    InstanceTransform to_without_translation = to;
    from_without_translation.values[3] = 0.0;
    from_without_translation.values[7] = 0.0;
    from_without_translation.values[11] = 0.0;
    to_without_translation.values[3] = 0.0;
    to_without_translation.values[7] = 0.0;
    to_without_translation.values[11] = 0.0;
    Vec3 result = occurrence_point(direction, from_without_translation,
                                   to_without_translation);
    const double norm = std::sqrt(result.x * result.x + result.y * result.y
                                  + result.z * result.z);
    if (norm > 0.0) {
        result.x /= norm;
        result.y /= norm;
        result.z /= norm;
    }
    return result;
}

Vec3 occurrence_vector(const Vec3& vector,
                       const InstanceTransform& from,
                       const InstanceTransform& to) {
    InstanceTransform from_without_translation = from;
    InstanceTransform to_without_translation = to;
    from_without_translation.values[3] = 0.0;
    from_without_translation.values[7] = 0.0;
    from_without_translation.values[11] = 0.0;
    to_without_translation.values[3] = 0.0;
    to_without_translation.values[7] = 0.0;
    to_without_translation.values[11] = 0.0;
    return occurrence_point(vector, from_without_translation,
                            to_without_translation);
}

BBox occurrence_bbox(const BBox& bbox,
                     const InstanceTransform& from,
                     const InstanceTransform& to) {
    if (!bbox.valid) return bbox;
    BBox result;
    for (int xi = 0; xi < 2; ++xi) {
        for (int yi = 0; yi < 2; ++yi) {
            for (int zi = 0; zi < 2; ++zi) {
                const Vec3 point = occurrence_point(
                    {xi ? bbox.xmax : bbox.xmin,
                     yi ? bbox.ymax : bbox.ymin,
                     zi ? bbox.zmax : bbox.zmin}, from, to);
                if (!result.valid) {
                    result.xmin = result.xmax = point.x;
                    result.ymin = result.ymax = point.y;
                    result.zmin = result.zmax = point.z;
                    result.valid = true;
                } else {
                    result.xmin = std::min(result.xmin, point.x);
                    result.ymin = std::min(result.ymin, point.y);
                    result.zmin = std::min(result.zmin, point.z);
                    result.xmax = std::max(result.xmax, point.x);
                    result.ymax = std::max(result.ymax, point.y);
                    result.zmax = std::max(result.zmax, point.z);
                }
            }
        }
    }
    return result;
}

SolidTopology transport_topology(const SolidTopology& topology,
                                 const InstanceTransform& from,
                                 const InstanceTransform& to) {
    SolidTopology result = topology;
    result.prism_axis = occurrence_direction(result.prism_axis, from, to);
    result.pipe_annulus.axis_origin = occurrence_point(
        result.pipe_annulus.axis_origin, from, to);
    result.pipe_annulus.axis_direction = occurrence_direction(
        result.pipe_annulus.axis_direction, from, to);
    for (PipePortTopology& port : result.pipe_annulus.ports) {
        port.center = occurrence_point(port.center, from, to);
        port.normal = occurrence_direction(port.normal, from, to);
    }
    result.revolution.axis_origin = occurrence_point(
        result.revolution.axis_origin, from, to);
    result.revolution.axis_direction = occurrence_direction(
        result.revolution.axis_direction, from, to);
    return result;
}

void transport_spline_fit(SplineFaceFit* fit,
                          const InstanceTransform& from,
                          const InstanceTransform& to) {
    if (!fit || !fit->available) return;
    fit->origin = occurrence_point(fit->origin, from, to);
    fit->direction = occurrence_direction(fit->direction, from, to);
}

void transport_fitted_candidates(AnalyzedSolid* solid,
                                 const InstanceTransform& from,
                                 const InstanceTransform& to) {
    if (!solid) return;
    FittedCylinderCandidate& cylinder = solid->fitted_cylinder_replacement;
    cylinder.axis_origin = occurrence_point(cylinder.axis_origin, from, to);
    cylinder.axis_direction = occurrence_direction(
        cylinder.axis_direction, from, to);
    cylinder.base = occurrence_point(cylinder.base, from, to);
    cylinder.height = occurrence_vector(cylinder.height, from, to);
    FittedSphereCandidate& sphere = solid->fitted_sphere_replacement;
    sphere.center = occurrence_point(sphere.center, from, to);
    FittedConeCandidate& cone = solid->fitted_cone_replacement;
    cone.axis_origin = occurrence_point(cone.axis_origin, from, to);
    cone.axis_direction = occurrence_direction(cone.axis_direction, from, to);
    cone.base = occurrence_point(cone.base, from, to);
    cone.height = occurrence_vector(cone.height, from, to);
}

std::string replace_all(std::string value,
                        const std::string& from,
                        const std::string& to) {
    if (from.empty() || from == to) return value;
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

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
                   ConversionReport& report,
                   const SolidTopology* recognized_topology = nullptr) {
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
    analyzed_solid.topology = recognized_topology
        ? *recognized_topology
        : analyze_solid_topology(solid, volume, options);
    if (analyzed_solid.topology.nonmanifold_edge_count > 0
        || analyzed_solid.topology.boundary_edge_count > 0) {
        std::ostringstream msg;
        msg << "Solid topology has "
            << analyzed_solid.topology.boundary_edge_count
            << " boundary edge(s) and "
            << analyzed_solid.topology.nonmanifold_edge_count
            << " non-manifold edge(s).";
        add_diag(report, DiagnosticSeverity::error,
                 "invalid_solid_topology", msg.str(), label);
    }
    if (analyzed_solid.topology.cavity_shell_count > 0) {
        std::ostringstream msg;
        msg << "Detected " << analyzed_solid.topology.cavity_shell_count
            << " enclosed cavity shell(s); Boolean difference is an exact "
               "candidate representation.";
        add_diag(report, DiagnosticSeverity::info,
                 "enclosed_voids_detected", msg.str(), label);
    }
    if (analyzed_solid.topology.prismatic_candidate
        && (analyzed_solid.topology.concave_edge_count > 0
            || analyzed_solid.topology.face_inner_wire_count > 0
            || analyzed_solid.topology.cavity_shell_count > 0)) {
        std::ostringstream msg;
        msg << "All faces are planar and all edges are linear with "
            << analyzed_solid.topology.normal_direction_count
            << " support-normal direction(s) and "
            << analyzed_solid.topology.axial_level_count
            << " axial level(s) along the support-derived axis ("
            << analyzed_solid.topology.prism_axis.x << ", "
            << analyzed_solid.topology.prism_axis.y << ", "
            << analyzed_solid.topology.prism_axis.z << ")";
        if (analyzed_solid.topology.constant_cross_section_candidate) {
            msg << "; a constant cross-section extrusion was recognized";
        }
        if (analyzed_solid.topology.extrusion_profile_hole_count > 0) {
            msg << " with "
                << analyzed_solid.topology.extrusion_profile_hole_count
                << " end-profile hole wire(s)";
        }
        msg << "; planar-prismatic decomposition is recommended.";
        add_diag(report, DiagnosticSeverity::info,
                 "prismatic_solid_detected", msg.str(), label);
    }
    if (analyzed_solid.topology.revolution.candidate
        && (analyzed_solid.topology.concave_edge_count > 0
            || analyzed_solid.topology.face_inner_wire_count > 0)) {
        const RevolutionTopology& revolution =
            analyzed_solid.topology.revolution;
        std::ostringstream msg;
        msg << "Detected "
            << (revolution.full_revolution ? "a full" : "a partial")
            << " analytic revolution around axis ("
            << revolution.axis_direction.x << ", "
            << revolution.axis_direction.y << ", "
            << revolution.axis_direction.z << ") with "
            << revolution.axial_level_count << " axial topology level(s), "
            << revolution.cylindrical_face_count << " cylindrical, "
            << revolution.conical_face_count << " conical, and "
            << revolution.toroidal_face_count << " toroidal face(s).";
        add_diag(report, DiagnosticSeverity::info,
                 revolution.full_revolution
                     ? "full_revolution_detected"
                     : "partial_revolution_detected",
                 msg.str(), label);
    }

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
        if (options.analyze_face_metrics) {
            analyzed_face.bbox = bbox_from_shape(face);
            GProp_GProps face_props;
            BRepGProp::SurfaceProperties(face, face_props);
            analyzed_face.area = std::abs(face_props.Mass());
        }

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
        analyzed_face.surface_type = replacement
            ? spline_fit_kind_name(replacement->kind)
            : surface_type_name(surface_type);
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

        if (analyzed_face.needs_spline_replacement
            && options.analyze_face_metrics) {
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
                                           options, report, true, true, false,
                                           nullptr, &analyzed_solid.certificate)) {
        if (analyzed_solid.certificate.failure_code
            == "curved_halfspace_certificate_deferred") {
            analyzed_solid.convexity_certificate_deferred = true;
        } else {
            analyzed_solid.needs_decomposition = true;
            analyzed_solid.needs_user_decision = true;
        }
    }

    analyzed_solid.exact_csg_supported =
        !analyzed_solid.convexity_certificate_deferred
        && !analyzed_solid.needs_decomposition
        && !analyzed_solid.needs_spline_replacement
        && !analyzed_solid.needs_surface_replacement
        && !analyzed_solid.needs_user_decision;
    if (options.analyze_fitted_replacements) {
        analyzed_solid.fitted_cylinder_replacement =
            fitted_cylinder_candidate(solid, analyzed_solid.volume);
        analyzed_solid.fitted_sphere_replacement =
            fitted_sphere_candidate(solid, analyzed_solid.volume);
        analyzed_solid.fitted_cone_replacement =
            fitted_cone_candidate(solid, analyzed_solid.volume);
    }
    report.solids.push_back(analyzed_solid);
}

void append_decision_requests(ConversionReport& report) {
    std::vector<DecisionRequest> requests;

    for (const MediumGenerationRequest& medium : report.medium_requests) {
        const bool applied = std::any_of(
            report.derived_regions.begin(), report.derived_regions.end(),
            [&](const DerivedRegionRecord& region) {
                return region.request_id == medium.id;
        });
        if (applied) continue;

        std::size_t unresolved = 0;
        if (medium.host_component_ids.size() != 1
            || medium.material_id.empty() || medium.ports.size() != 2) {
            DecisionRequest request;
            request.id = "decision:contained_medium:" + medium.id + ":request";
            request.kind = "contained_medium_request";
            request.target_id = medium.id;
            request.reason = "incomplete_medium_request";
            request.recommended_action = "review_medium_request";
            request.summary = "Straight-pipe medium v1 requires one host, one configured material, and explicit treatment of both recognized ports.";
            request.metrics = {
                metric("host_count", static_cast<double>(
                    medium.host_component_ids.size())),
                metric("port_count", static_cast<double>(medium.ports.size())),
            };
            request.candidates.push_back(candidate_view(
                "action:complete:" + medium.id,
                "stage_contained_medium_request", "review",
                "Complete or correct the staged physical-medium intent."));
            requests.push_back(std::move(request));
            ++unresolved;
        }
        for (const MediumPortDecision& port : medium.ports) {
            if (port.disposition != MediumPortDisposition::unresolved) continue;
            ++unresolved;
            DecisionRequest request;
            request.id = "decision:contained_medium:" + medium.id + ":" + port.port_id;
            request.kind = "contained_medium_port";
            request.target_id = medium.id;
            request.reason = "unresolved_medium_port";
            request.recommended_action = "review_medium_ports";
            request.summary = "Choose the physical boundary treatment for medium port "
                + port.port_id + ".";
            request.candidates.push_back(candidate_view(
                "action:cap:" + port.port_id, "cap_here", "review",
                "Close the generated material region at this CAD port."));
            request.candidates.push_back(candidate_view(
                "action:external:" + port.port_id,
                "interface_to_external_model", "review",
                "Treat this port as an explicitly modeled external interface."));
            requests.push_back(std::move(request));
        }

        if (unresolved == 0) {
            DecisionRequest request;
            request.id = "decision:contained_medium:" + medium.id + ":apply";
            request.kind = "contained_medium_apply";
            request.target_id = medium.id;
            request.reason = "medium_request_staged_not_applied";
            request.recommended_action = "apply_contained_medium";
            request.summary = "The medium request is fully specified but still requires authorization and a fresh geometry certificate.";
            request.candidates.push_back(candidate_view(
                "action:apply:" + medium.id, "apply_contained_medium", "review",
                "Recompute the lumen and overlap certificate, then commit it as a material region."));
            requests.push_back(std::move(request));
        }
    }

    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.needs_decomposition) {
            DecisionRequest request;
            request.id = "decision:decomposition:" + solid.id;
            request.kind = "decomposition";
            request.target_id = solid.id;
            request.reason = "nonconvex_or_not_single_cell_convertible";
            request.recommended_action = solid.topology.recommended_strategy.empty()
                ? "decompose-suggest" : solid.topology.recommended_strategy;
            request.summary =
                "Solid needs decomposition before it can be emitted as valid CSG.";
            request.metrics = {
                metric("volume", solid.volume),
                metric("volume_fraction", solid.volume_fraction),
                metric("face_count", static_cast<double>(solid.face_count)),
                metric("concave_edge_count",
                       static_cast<double>(solid.topology.concave_edge_count)),
                metric("cavity_shell_count",
                       static_cast<double>(solid.topology.cavity_shell_count)),
                metric("bbox_fill_ratio", solid.topology.bbox_fill_ratio),
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
                                       options, scratch, false, false, false,
                                       nullptr, &analyzed.certificate);
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

// Inflate-with-pad bbox intersection check. Two solids' geometric
// volumes can only overlap if their bboxes overlap (modulo numerical
// noise). Used to skip the O(N^2) BRepAlgoAPI_Common pairs that are
// trivially disjoint -- the dominant case for instanced assemblies
// where most pins/bolts do NOT touch each other.
bool bboxes_intersect(const Bnd_Box& a, const Bnd_Box& b, double pad) {
    if (a.IsVoid() || b.IsVoid()) return false;
    double ax0, ay0, az0, ax1, ay1, az1;
    double bx0, by0, bz0, bx1, by1, bz1;
    a.Get(ax0, ay0, az0, ax1, ay1, az1);
    b.Get(bx0, by0, bz0, bx1, by1, bz1);
    return !(ax1 + pad < bx0 || bx1 + pad < ax0
          || ay1 + pad < by0 || by1 + pad < ay0
          || az1 + pad < bz0 || bz1 + pad < az0);
}

double shape_volume(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}

OverlapIntersectionEvidence intersection_volume(const TopoDS_Shape& a,
                                                const TopoDS_Shape& b) {
    OverlapIntersectionEvidence result;
    try {
        BRepAlgoAPI_Common common;
        build_binary_boolean(common, a, b);
        if (common.IsDone()) {
            result.known = true;
            if (!common.Shape().IsNull()) {
                result.volume = shape_volume(common.Shape());
            }
        }
    } catch (...) {
        result.known = false;
        result.volume = 0.0;
    }
    return result;
}

// Detect interpenetrating solid pairs in the imported model. Runs
// AFTER per-solid analysis so we have the bboxes already; uses them
// as a cheap prescreen before falling back to BRepAlgoAPI_Common for
// pairs whose bboxes actually intersect.
//
// Emits a `solid_overlap` warning diagnostic per detected pair --
// warning severity because MCNP cannot tolerate overlapping cells,
// but the csgify user may have authored the overlap intentionally
// (e.g., as a tolerance feature) and we don't want to refuse to
// emit. Pairs are also recorded in report.overlaps[] for tooling.
//
// Lives in csgify::detail so the session layer can run it once per
// explicit analyze call without paying the O(N^2) cost on every
// internal post-mutation analyze.
void detect_solid_overlaps(CadModel& model,
                            const ConversionOptions& options,
                            ConversionReport& report,
                            const OverlapPairCache* reusable,
                            OverlapPairCache* cache_out) {
    if (!options.check_overlaps) return;
    if (model.root.IsNull()) return;
    if (cache_out) cache_out->clear();

    // Refresh the persistent record. Each full overlap pass recomputes
    // the current pairwise overlap set, so stale overlap entries from
    // prior analyzes or mutations must be removed first. Mutation
    // history belongs in the decision log; model.diagnostics is the
    // live gate consumed by emit_document.
    model.diagnostics.erase(
        std::remove_if(model.diagnostics.begin(),
                       model.diagnostics.end(),
                       [](const Diagnostic& d) {
                           return d.code == "solid_overlap"
                               || d.code == "mutation_introduced_overlap"
                               || d.code == "overlap_unknown";
                       }),
        model.diagnostics.end());
    report.diagnostics.erase(
        std::remove_if(report.diagnostics.begin(),
                       report.diagnostics.end(),
                       [](const Diagnostic& d) {
                           return d.code == "solid_overlap"
                               || d.code == "mutation_introduced_overlap"
                               || d.code == "overlap_unknown";
                       }),
        report.diagnostics.end());

    // Walk solids in TopExp order so `solid:N` indices match the rest
    // of the report. Skip suppressed solids.
    struct Entry {
        std::string id;
        TopoDS_Shape shape;
        Bnd_Box bbox;
        double volume = 0.0;
    };
    struct ReportGeometry {
        BBox bbox;
        double volume = 0.0;
    };
    std::unordered_map<std::string, ReportGeometry> report_geometry;
    for (const AnalyzedSolid& solid : report.solids) {
        report_geometry.emplace(solid.id,
                                ReportGeometry{solid.bbox, solid.volume});
    }
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        report_geometry.emplace(piece.id,
                                ReportGeometry{piece.bbox, piece.volume});
    }
    const auto populate_measures = [&](Entry* entry) {
        const auto measured = report_geometry.find(entry->id);
        if (measured != report_geometry.end() && measured->second.bbox.valid) {
            const BBox& bbox = measured->second.bbox;
            entry->bbox.Update(bbox.xmin, bbox.ymin, bbox.zmin,
                               bbox.xmax, bbox.ymax, bbox.zmax);
            entry->volume = measured->second.volume;
        } else {
            BRepBndLib::Add(entry->shape, entry->bbox);
            entry->volume = shape_volume(entry->shape);
        }
    };
    std::vector<Entry> entries;
    std::size_t solid_index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) continue;
        Entry e;
        e.id = id;
        e.shape = exp.Current();
        populate_measures(&e);
        entries.push_back(std::move(e));
    }
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active || piece.solid.IsNull()) continue;
        Entry e;
        e.id = piece.id;
        e.shape = piece.solid;
        populate_measures(&e);
        entries.push_back(std::move(e));
    }
    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (!body.active || body.materialized_solid.IsNull()) continue;
        Entry e;
        e.id = body.id;
        e.shape = body.materialized_solid;
        BRepBndLib::Add(e.shape, e.bbox);
        e.volume = shape_volume(e.shape);
        entries.push_back(std::move(e));
    }
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) continue;
        TopoDS_Shape shape = build_synthetic_shape(model, body);
        if (shape.IsNull()) continue;
        Entry e;
        e.id = body.id;
        e.shape = shape;
        BRepBndLib::Add(e.shape, e.bbox);
        e.volume = shape_volume(e.shape);
        entries.push_back(std::move(e));
    }

    // Pad the bbox prescreen by linear_tolerance so we don't miss
    // numerically-touching pairs.
    const double pad = std::max(options.linear_tolerance, 1.0e-9);

    // Sweep on X and retain only currently overlapping intervals. This turns
    // sparse assemblies from an all-pairs bbox pass into O(N log N + K), where
    // K is the number of local bbox neighbours. Candidate pairs are sorted
    // back into stable source order before exact OCCT intersections.
    struct SweepEntry {
        std::size_t index = 0;
        double xmin = 0.0;
        double xmax = 0.0;
    };
    std::vector<SweepEntry> sweep;
    sweep.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].bbox.IsVoid()) continue;
        double xmin, ymin, zmin, xmax, ymax, zmax;
        entries[index].bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        sweep.push_back({index, xmin, xmax});
    }
    std::sort(sweep.begin(), sweep.end(), [&](const SweepEntry& a,
                                              const SweepEntry& b) {
        if (a.xmin != b.xmin) return a.xmin < b.xmin;
        return entries[a.index].id < entries[b.index].id;
    });
    std::vector<SweepEntry> active;
    std::vector<std::pair<std::size_t, std::size_t>> candidates;
    for (const SweepEntry& current : sweep) {
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const SweepEntry& prior) {
            return prior.xmax + pad < current.xmin;
        }), active.end());
        for (const SweepEntry& prior : active) {
            if (!bboxes_intersect(entries[prior.index].bbox,
                                  entries[current.index].bbox, pad)) {
                continue;
            }
            candidates.emplace_back(
                std::min(prior.index, current.index),
                std::max(prior.index, current.index));
        }
        active.push_back(current);
    }
    std::sort(candidates.begin(), candidates.end());
    report.performance.spatial_candidate_pairs += candidates.size();

    std::vector<OverlapIntersectionEvidence> intersections(candidates.size());
    std::vector<std::size_t> uncached;
    uncached.reserve(candidates.size());
    for (std::size_t candidate_index = 0;
         candidate_index < candidates.size(); ++candidate_index) {
        const auto& candidate = candidates[candidate_index];
        const std::size_t i = candidate.first;
        const std::size_t j = candidate.second;
        const auto key = entries[i].id < entries[j].id
            ? std::make_pair(entries[i].id, entries[j].id)
            : std::make_pair(entries[j].id, entries[i].id);
        const auto cached = reusable ? reusable->find(key)
                                     : OverlapPairCache::const_iterator{};
        if (reusable && cached != reusable->end() && cached->second.known) {
            intersections[candidate_index] = cached->second;
            ++report.performance.overlap_pair_cache_hits;
        } else {
            uncached.push_back(candidate_index);
        }
    }
    report.performance.exact_overlap_tests += uncached.size();
    report.performance.overlap_pair_cache_misses += uncached.size();

    const std::size_t worker_count = std::min(
        std::max<std::size_t>(1, options.overlap_parallelism),
        uncached.size());
    if (worker_count <= 1) {
        for (const std::size_t candidate_index : uncached) {
            const auto& candidate = candidates[candidate_index];
            intersections[candidate_index] = intersection_volume(
                entries[candidate.first].shape,
                entries[candidate.second].shape);
        }
    } else {
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&]() {
                while (true) {
                    const std::size_t work = next.fetch_add(1);
                    if (work >= uncached.size()) break;
                    const std::size_t candidate_index = uncached[work];
                    const auto& candidate = candidates[candidate_index];
                    intersections[candidate_index] = intersection_volume(
                        entries[candidate.first].shape,
                        entries[candidate.second].shape);
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
    }

    for (std::size_t candidate_index = 0;
         candidate_index < candidates.size(); ++candidate_index) {
            const auto& candidate = candidates[candidate_index];
            const std::size_t i = candidate.first;
            const std::size_t j = candidate.second;
            const auto key = entries[i].id < entries[j].id
                ? std::make_pair(entries[i].id, entries[j].id)
                : std::make_pair(entries[j].id, entries[i].id);
            const OverlapIntersectionEvidence& intersection =
                intersections[candidate_index];
            if (cache_out) (*cache_out)[key] = intersection;
            if (!intersection.known) {
                std::ostringstream msg;
                msg << "Could not evaluate boolean intersection between "
                    << entries[i].id << " and " << entries[j].id
                    << "; overlap state is unknown.";
                Diagnostic d;
                d.severity = options.allow_overlaps
                    ? DiagnosticSeverity::warning
                    : DiagnosticSeverity::error;
                d.code = "overlap_unknown";
                d.message = msg.str();
                d.shape_id = entries[i].id;
                d.shape_label = entries[i].id + "<->" + entries[j].id;
                report.diagnostics.push_back(d);
                model.diagnostics.push_back(d);
                continue;
            }
            const double inter_volume = intersection.volume;
            if (inter_volume <= 0.0) continue;

            const double min_vol = std::min(entries[i].volume, entries[j].volume);
            const double min_frac =
                min_vol > 0.0 ? inter_volume / min_vol : 0.0;

            const bool over_absolute =
                inter_volume > options.overlap_volume_threshold;
            const bool over_fraction =
                min_frac > options.overlap_volume_fraction;
            if (!over_absolute && !over_fraction) continue;

            SolidOverlap overlap;
            overlap.solid_a_id = entries[i].id;
            overlap.solid_b_id = entries[j].id;
            overlap.volume_a = entries[i].volume;
            overlap.volume_b = entries[j].volume;
            overlap.intersection_volume = inter_volume;
            overlap.min_fraction = min_frac;
            report.overlaps.push_back(overlap);

            std::ostringstream msg;
            msg << "Solids " << entries[i].id << " and " << entries[j].id
                << " interpenetrate (intersection_volume=" << inter_volume
                << ", fraction_of_smaller=" << min_frac
                << "). MCNP cells must be non-overlapping; resolve via "
                << "decomposition, removal, or fixing the source CAD.";
            Diagnostic d;
            // Error severity by default so report.ok() == false and
            // emit_document refuses to write a broken MCNP deck.
            // Override via options.allow_overlaps (CLI: --allow-overlaps).
            d.severity = options.allow_overlaps
                ? DiagnosticSeverity::warning
                : DiagnosticSeverity::error;
            d.code = "solid_overlap";
            d.message = msg.str();
            d.shape_id = entries[i].id;
            d.shape_label = entries[i].id + "<->" + entries[j].id;
            report.diagnostics.push_back(d);
            // Persist on the model so subsequent analyze_model passes
            // (e.g. inside emit_document's convert_model) carry the
            // diagnostic forward, keeping report.ok() == false until
            // the overlap is resolved.
            model.diagnostics.push_back(d);
    }
}

namespace {

// Canonical (lo, hi) pair key used by mutation-overlap state.
std::pair<std::string, std::string> canonical_pair(const std::string& a,
                                                    const std::string& b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

bool pair_known(
    const std::vector<std::pair<std::string, std::string>>& known,
    const std::string& a, const std::string& b) {
    const auto key = canonical_pair(a, b);
    for (const auto& k : known) {
        if (k.first == key.first && k.second == key.second) return true;
    }
    return false;
}

// One "ego" entry the mutation check considers -- could be a brand-new
// synthetic body or a decomposed piece. Bundled with its TopoDS so
// we can run BRepAlgoAPI_Common against neighbors without rebuilding.
struct EgoEntry {
    std::string id;
    TopoDS_Shape shape;
    Bnd_Box bbox;
    double volume = 0.0;
    // If the ego came from suppressing a source solid, the source id
    // is recorded so we can skip self-overlaps with the (now-suppressed)
    // source. For ego entries whose source is unrelated this stays empty.
    std::string source_solid_id;
};

}  // namespace

// Post-mutation overlap check. See engine_internal.hpp for the contract.
//
// Strategy: walk the "new" tail of model.synthetic_bodies and
// model.decomposed_pieces (those past the document's high-water marks)
// and pair each new ego against every surviving active TopExp solid +
// every prior synthetic body + every prior decomposed piece. Bbox
// prescreen on every candidate pair keeps the worst case to ego_count
// * N_active, with most pairs rejected by bbox alone.
//
// Among new pieces of the same parent decomposition, also run a
// pairwise check -- BRepAlgoAPI_Splitter is supposed to produce
// disjoint pieces, but degenerate cuts can leave residual overlap
// and we want to catch that explicitly.
void detect_mutation_overlaps(
    CadModel& model,
    const ConversionOptions& options,
    ConversionReport& report,
    std::size_t* synthetic_body_high_water,
    std::size_t* decomposed_piece_high_water,
    std::vector<std::pair<std::string, std::string>>* known_overlap_pairs) {
    if (!options.check_overlaps) return;
    if (model.root.IsNull()) return;
    if (!synthetic_body_high_water || !decomposed_piece_high_water
        || !known_overlap_pairs) return;

    // 1. Build the ego list (new synthetics + new pieces).
    std::vector<EgoEntry> egos;
    for (std::size_t i = *synthetic_body_high_water;
         i < model.synthetic_bodies.size(); ++i) {
        const SyntheticBody& body = model.synthetic_bodies[i];
        if (!body.emit_as_cell) continue;
        TopoDS_Shape shape = build_synthetic_shape(model, body);
        if (shape.IsNull()) continue;
        EgoEntry e;
        e.id = body.id;
        e.shape = shape;
        BRepBndLib::Add(shape, e.bbox);
        e.volume = shape_volume(shape);
        e.source_solid_id = body.source_solid_id;
        egos.push_back(std::move(e));
    }
    for (std::size_t i = *decomposed_piece_high_water;
         i < model.decomposed_pieces.size(); ++i) {
        const DecomposedSolidPiece& piece = model.decomposed_pieces[i];
        if (!piece.active || piece.solid.IsNull()) continue;
        EgoEntry e;
        e.id = piece.id;
        e.shape = piece.solid;
        BRepBndLib::Add(piece.solid, e.bbox);
        e.volume = shape_volume(piece.solid);
        e.source_solid_id = piece.source_solid_id;
        egos.push_back(std::move(e));
    }

    // Bump high-water unconditionally so a noop call still advances
    // past whatever was added (otherwise we'd re-scan every time the
    // BSP loop calls finish_document_mutation).
    *synthetic_body_high_water = model.synthetic_bodies.size();
    *decomposed_piece_high_water = model.decomposed_pieces.size();

    if (egos.empty()) return;

    // 2. Build the "neighbor" list = active TopExp solids + already-
    // existing synthetic bodies + already-existing pieces (i.e., the
    // entries we did NOT just create). All represented uniformly as
    // (id, shape, bbox, volume).
    struct Neighbor {
        std::string id;
        TopoDS_Shape shape;
        Bnd_Box bbox;
        double volume = 0.0;
    };
    std::vector<Neighbor> neighbors;
    std::size_t solid_index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) continue;
        Neighbor n;
        n.id = id;
        n.shape = exp.Current();
        BRepBndLib::Add(n.shape, n.bbox);
        n.volume = shape_volume(n.shape);
        neighbors.push_back(std::move(n));
    }
    // Prior synthetic bodies (before the high-water we just bumped).
    for (std::size_t i = 0; i < model.synthetic_bodies.size(); ++i) {
        if (i >= *synthetic_body_high_water) break;  // safety
        // Skip the egos we just added by id check below; but since the
        // egos were taken from the >= high_water tail, the prior set
        // (i < old_high_water == egos[].source set start) is what we
        // want. egos[] only contains body indices >= old_high_water,
        // so iterating [0, new_high_water) here also includes them.
        // Use id-based exclusion to be safe.
    }
    // Recompute prior synthetic bodies cleanly: those with id NOT in
    // the ego set.
    auto in_ego_set = [&](const std::string& id) {
        for (const EgoEntry& e : egos) if (e.id == id) return true;
        return false;
    };
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) continue;
        if (in_ego_set(body.id)) continue;
        TopoDS_Shape shape = build_synthetic_shape(model, body);
        if (shape.IsNull()) continue;
        Neighbor n;
        n.id = body.id;
        n.shape = shape;
        BRepBndLib::Add(shape, n.bbox);
        n.volume = shape_volume(shape);
        neighbors.push_back(std::move(n));
    }
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active || piece.solid.IsNull()) continue;
        if (in_ego_set(piece.id)) continue;
        Neighbor n;
        n.id = piece.id;
        n.shape = piece.solid;
        BRepBndLib::Add(piece.solid, n.bbox);
        n.volume = shape_volume(piece.solid);
        neighbors.push_back(std::move(n));
    }

    const double pad = std::max(options.linear_tolerance, 1.0e-9);

    auto record_pair = [&](const EgoEntry& ego, const std::string& other_id,
                           double other_volume, double inter_volume) {
        const double min_vol = std::min(ego.volume, other_volume);
        const double min_frac = min_vol > 0.0 ? inter_volume / min_vol : 0.0;
        const bool over_abs = inter_volume > options.overlap_volume_threshold;
        const bool over_frac = min_frac > options.overlap_volume_fraction;
        if (!over_abs && !over_frac) return;

        // De-dupe against known pairs (pre-existing or already flagged).
        if (pair_known(*known_overlap_pairs, ego.id, other_id)) return;
        known_overlap_pairs->push_back(canonical_pair(ego.id, other_id));

        SolidOverlap overlap;
        overlap.solid_a_id = ego.id;
        overlap.solid_b_id = other_id;
        overlap.volume_a = ego.volume;
        overlap.volume_b = other_volume;
        overlap.intersection_volume = inter_volume;
        overlap.min_fraction = min_frac;
        report.overlaps.push_back(overlap);

        std::ostringstream msg;
        msg << "Mutation introduced an overlap between " << ego.id
            << " (newly created) and " << other_id
            << " (intersection_volume=" << inter_volume
            << ", fraction_of_smaller=" << min_frac
            << "). MCNP cells must be non-overlapping; review the most"
               " recent op or tighten its volume guard.";
        Diagnostic d;
        // Error severity by default so report.ok() == false and the
        // downstream emit_document gate refuses the deck. Override
        // via options.allow_overlaps (CLI: --allow-overlaps).
        d.severity = options.allow_overlaps
            ? DiagnosticSeverity::warning
            : DiagnosticSeverity::error;
        d.code = "mutation_introduced_overlap";
        d.message = msg.str();
        d.shape_id = ego.id;
        d.shape_label = ego.id + "<->" + other_id;
        report.diagnostics.push_back(d);
        // Persist on the model so subsequent re-analyze passes
        // (including emit_document's fresh analysis) still surface
        // this. Without this push the diagnostic would only show on
        // the immediate finish_document_mutation report.
        model.diagnostics.push_back(d);
    };

    auto record_unknown = [&](const EgoEntry& ego, const std::string& other_id) {
        if (pair_known(*known_overlap_pairs, ego.id, other_id)) return;
        known_overlap_pairs->push_back(canonical_pair(ego.id, other_id));
        std::ostringstream msg;
        msg << "Mutation requires a boolean overlap check between " << ego.id
            << " and " << other_id
            << ", but the intersection could not be evaluated.";
        Diagnostic d;
        d.severity = options.allow_overlaps
            ? DiagnosticSeverity::warning
            : DiagnosticSeverity::error;
        d.code = "overlap_unknown";
        d.message = msg.str();
        d.shape_id = ego.id;
        d.shape_label = ego.id + "<->" + other_id;
        report.diagnostics.push_back(d);
        model.diagnostics.push_back(d);
    };

    // 3. Pair each ego against each neighbor (with bbox prescreen).
    // The source solid is NOT in `neighbors` (it's been suppressed or
    // is one of the ego pieces), so we don't false-positive on the
    // ego occupying the source's footprint.
    for (const EgoEntry& ego : egos) {
        for (const Neighbor& n : neighbors) {
            // Skip the ego's own source solid -- a synthetic body
            // replacing solid:N "overlaps" solid:N by design, and the
            // source is already suppressed in active membership. The
            // contains_id check above handles the suppression, but a
            // belt-and-braces id compare guards against future bugs.
            if (!ego.source_solid_id.empty()
                && ego.source_solid_id == n.id) continue;
            if (!bboxes_intersect(ego.bbox, n.bbox, pad)) continue;
            const OverlapIntersectionEvidence intersection =
                intersection_volume(ego.shape, n.shape);
            if (!intersection.known) {
                record_unknown(ego, n.id);
                continue;
            }
            if (intersection.volume <= 0.0) continue;
            record_pair(ego, n.id, n.volume, intersection.volume);
        }
    }

    // 4. Sibling check: pieces produced together by a decomposition
    // should be disjoint. Pair each new ego against the other new
    // egos (n^2 but n is small per mutation).
    for (std::size_t i = 0; i < egos.size(); ++i) {
        for (std::size_t j = i + 1; j < egos.size(); ++j) {
            if (!bboxes_intersect(egos[i].bbox, egos[j].bbox, pad)) continue;
            const OverlapIntersectionEvidence intersection =
                intersection_volume(egos[i].shape, egos[j].shape);
            if (!intersection.known) {
                record_unknown(egos[i], egos[j].id);
                continue;
            }
            if (intersection.volume <= 0.0) continue;
            record_pair(egos[i], egos[j].id, egos[j].volume, intersection.volume);
        }
    }
}

} // namespace

AnalysisReport analyze_model(const CadModel& model,
                             const ConversionOptions& options,
                             AnalysisProgressCallback progress,
                             bool* cancelled_out) {
    using namespace csgify::detail;
    AnalysisReport report;
    if (cancelled_out) *cancelled_out = false;
    report.diagnostics = model.diagnostics;
    report.assembly = model.assembly;
    report.strings = model.strings;
    report.metadata = model.metadata;
    report.component_definitions = model.component_definitions;
    report.component_instances = model.component_instances;
    report.materials = model.materials;
    report.medium_requests = model.medium_requests;
    report.derived_regions = model.derived_regions;
    report.length_unit = model.length_unit;
    report.length_unit_to_mm = model.length_unit_to_mm;

    if (model.root.IsNull()) {
        return report;
    }

    std::unordered_map<std::string, const ComponentDefinition*> definitions;
    for (const ComponentDefinition& definition : model.component_definitions) {
        definitions.emplace(definition.id, &definition);
    }
    std::unordered_map<std::string, const ComponentInstance*> instances;
    std::unordered_map<std::string, const ComponentInstance*> instances_by_id;
    for (const ComponentInstance& instance : model.component_instances) {
        if (instance.active) instances_by_id.emplace(instance.id, &instance);
        if (instance.active && !instance.source_shape_id.empty()) {
            instances.emplace(instance.source_shape_id, &instance);
        }
    }
    std::unordered_map<std::string, const AssemblyNode*> nodes_by_entry;
    for (const AssemblyNode& node : model.assembly) {
        if (node.kind == "shape" && !node.label_entry.empty()) {
            nodes_by_entry.emplace(node.label_entry, &node);
        }
    }
    struct CachedAnalysis {
        AnalyzedSolid solid;
        std::vector<AnalyzedFace> faces;
        std::vector<Diagnostic> diagnostics;
        InstanceTransform transform;
    };
    std::unordered_map<std::string, CachedAnalysis> analysis_cache;
    // A component definition may contain multiple solids. XCAF associates all
    // of them with the same occurrence label, while source_shape_id names only
    // the first solid for legacy compatibility. Track each solid's stable
    // local ordinal so every repeated multi-solid definition can reuse the
    // complete analysis, not merely its first body.
    std::unordered_map<std::string, std::size_t> occurrence_solid_ordinals;

    std::size_t solid_total = 0;
    for (TopExp_Explorer count(model.root, TopAbs_SOLID); count.More(); count.Next())
        ++solid_total;
    std::size_t solid_index = 0;
    std::size_t progress_completed = 0;
    bool cancelled = false;
    const auto publish_solid = [&](const std::string& id) {
        ++progress_completed;
        if (!progress) return true;
        try {
            return progress(progress_completed, solid_total, id);
        } catch (...) {
            return false;
        }
    };
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (!options.analysis_target_ids.empty()
            && !contains_id(options.analysis_target_ids, id)) {
            if (!publish_solid(id)) { cancelled = true; break; }
            continue;
        }
        if (contains_id(model.removed_solid_ids, id)) {
            add_diag(report, DiagnosticSeverity::info, "solid_removed",
                     "Solid is suppressed by preprocessing.", solid_label(solid_index));
            if (!publish_solid(id)) { cancelled = true; break; }
            continue;
        }
        ++report.solids_seen;
        const TopoDS_Solid current_solid = TopoDS::Solid(solid_exp.Current());
        const InstanceTransform current_transform =
            shape_location_transform(current_solid);
        const ComponentInstance* instance = nullptr;
        const ComponentDefinition* definition = nullptr;
        const ShapeMetadata* solid_metadata = find_solid_metadata(model, id);
        const auto instance_it = instances.find(id);
        if (instance_it != instances.end()) {
            instance = instance_it->second;
        } else if (solid_metadata && !solid_metadata->component_entry.empty()) {
            const auto node_it = nodes_by_entry.find(
                solid_metadata->component_entry);
            if (node_it != nodes_by_entry.end()) {
                const auto semantic_instance = instances_by_id.find(
                    node_it->second->instance_id);
                if (semantic_instance != instances_by_id.end()) {
                    instance = semantic_instance->second;
                }
            }
        }
        std::size_t definition_solid_ordinal = 0;
        if (instance) {
            definition_solid_ordinal =
                ++occurrence_solid_ordinals[instance->id];
            const auto definition_it = definitions.find(instance->definition_id);
            if (definition_it != definitions.end()
                && definition_it->second->geometry_digest_verified
                && !definition_it->second->geometry_digest.empty()
                && rigid_transform(instance->transform)) {
                definition = definition_it->second;
            }
        }
        // OCCT preserves the underlying TShape when the same definition body
        // is instantiated at different rigid locations. Pointer identity is
        // therefore a stronger in-document proof than XCAF label association
        // (which can resolve to a nested subshape label). Prefer it, while
        // retaining the verified definition/local-ordinal key for documents
        // whose reader materializes distinct but definition-identical TShapes.
        const void* tshape_identity = current_solid.TShape().get();
        const std::string analysis_cache_key = definition
            ? definition->geometry_digest + "|solid:"
                + std::to_string(definition_solid_ordinal)
            : (tshape_identity
                ? "tshape:"
                    + std::to_string(reinterpret_cast<std::uintptr_t>(
                        tshape_identity))
                : std::string{});
        const InstanceTransform analysis_transform = definition && instance
            ? instance->transform : current_transform;

        const bool occurrence_has_face_replacement = std::any_of(
            model.face_replacements.begin(), model.face_replacements.end(),
            [&](const FaceReplacement& replacement) {
                std::size_t replacement_solid = 0;
                std::size_t replacement_face = 0;
                return parse_face_id(replacement.face_id, &replacement_solid,
                                     &replacement_face)
                    && replacement_solid == solid_index;
            });
        const auto cached = !analysis_cache_key.empty()
                && !occurrence_has_face_replacement
            ? analysis_cache.find(analysis_cache_key)
            : analysis_cache.end();
        if (cached != analysis_cache.end()) {
            const CachedAnalysis& source = cached->second;
            AnalyzedSolid solid = source.solid;
            const std::string source_id = solid.id;
            const std::string source_label = solid.label;
            solid.id = id;
            solid.label = solid_label(solid_index);
            solid.index = solid_index;
            solid.name.clear();
            solid.hierarchy_path.clear();
            if (const ShapeMetadata* metadata = find_solid_metadata(model, id)) {
                solid.name = metadata->name;
                solid.hierarchy_path = metadata->hierarchy_path;
            }
            solid.bbox = occurrence_bbox(
                source.solid.bbox, source.transform, analysis_transform);
            solid.topology = transport_topology(
                source.solid.topology, source.transform, analysis_transform);
            transport_fitted_candidates(
                &solid, source.transform, analysis_transform);
            solid.face_ids.clear();

            report.total_volume += solid.volume;
            report.solid_volumes.push_back({solid.label, solid.volume});
            for (const AnalyzedFace& cached_face : source.faces) {
                AnalyzedFace face = cached_face;
                face.id = face_id(solid_index, cached_face.index);
                face.label = face_label(solid_index, cached_face.index);
                face.solid_id = id;
                face.bbox = occurrence_bbox(
                    cached_face.bbox, source.transform, analysis_transform);
                transport_spline_fit(
                    &face.spline_fits.plane, source.transform,
                    analysis_transform);
                transport_spline_fit(
                    &face.spline_fits.sphere, source.transform,
                    analysis_transform);
                transport_spline_fit(
                    &face.spline_fits.cylinder, source.transform,
                    analysis_transform);
                transport_spline_fit(
                    &face.spline_fits.cone, source.transform,
                    analysis_transform);
                solid.face_ids.push_back(face.id);
                ++report.faces_seen;
                if (face.exact_csg_supported) ++report.analytic_faces;
                report.faces.push_back(std::move(face));
            }
            solid.face_count = solid.face_ids.size();
            for (const Diagnostic& cached_diagnostic : source.diagnostics) {
                Diagnostic diagnostic = cached_diagnostic;
                diagnostic.shape_id = replace_all(
                    diagnostic.shape_id, source_id, id);
                diagnostic.local_shape_id = replace_all(
                    diagnostic.local_shape_id, source_id, id);
                diagnostic.shape_label = replace_all(
                    diagnostic.shape_label, source_label, solid.label);
                report.diagnostics.push_back(std::move(diagnostic));
            }
            report.solids.push_back(std::move(solid));
            ++report.performance.definition_recognition_cache_hits;
        } else {
            const std::size_t face_start = report.faces.size();
            const std::size_t diagnostic_start = report.diagnostics.size();
            inspect_solid(current_solid, solid_index,
                          model, options, report);
            if (!analysis_cache_key.empty() && !report.solids.empty()
                && report.solids.back().id == id
                && !occurrence_has_face_replacement) {
                CachedAnalysis analysis;
                analysis.solid = report.solids.back();
                analysis.faces.assign(
                    report.faces.begin()
                        + static_cast<std::ptrdiff_t>(face_start),
                    report.faces.end());
                analysis.diagnostics.assign(
                    report.diagnostics.begin()
                        + static_cast<std::ptrdiff_t>(diagnostic_start),
                    report.diagnostics.end());
                analysis.transform = analysis_transform;
                analysis_cache.emplace(
                    analysis_cache_key,
                    std::move(analysis));
                ++report.performance.definition_recognition_cache_misses;
            }
        }
        if (!report.solids.empty() && report.solids.back().id == id
            && instance) {
            report.solids.back().component_instance_id = instance->id;
            report.solids.back().component_definition_id =
                instance->definition_id;
        }
        if (!publish_solid(id)) { cancelled = true; break; }
    }

    if (cancelled) {
        if (cancelled_out) *cancelled_out = true;
        return report;
    }

    for (const SyntheticBody& body : model.synthetic_bodies) {
        ProvenanceRecord provenance;
        provenance.entity_id = body.id;
        provenance.exact = true;
        if (body.kind == SyntheticBodyKind::union_body) {
            provenance.operation = "synthetic_union";
            provenance.source_ids = {body.left_id, body.right_id};
        } else if (body.kind == SyntheticBodyKind::intersection_body) {
            provenance.operation = "synthetic_intersection";
            provenance.source_ids = {body.left_id, body.right_id};
        } else if (body.kind == SyntheticBodyKind::difference_body) {
            provenance.operation = "synthetic_difference";
            provenance.source_ids = {body.left_id, body.right_id};
        } else if (body.role == "hole_fill") {
            provenance.operation = "fill_hole";
            if (!body.host_solid_id.empty())
                provenance.source_ids.push_back(body.host_solid_id);
            if (!body.source_solid_id.empty())
                provenance.source_ids.push_back(body.source_solid_id);
        } else if (body.role == "contained_medium") {
            provenance.operation = "generate_straight_pipe_medium";
            if (!body.host_solid_id.empty())
                provenance.source_ids.push_back(body.host_solid_id);
        } else {
            const struct PrefixOperation { const char* prefix; const char* op; } mappings[] = {
                {"bbox_from_", "replace_solid_with_bbox"},
                {"bbox_cylinder_from_", "replace_solid_with_bbox_cylinder"},
                {"fitted_cylinder_from_", "replace_solid_with_fitted_cylinder"},
                {"fitted_sphere_from_", "replace_solid_with_fitted_sphere"},
                {"fitted_cone_from_", "replace_solid_with_fitted_cone"},
            };
            provenance.operation = "add_synthetic_primitive";
            for (const PrefixOperation& mapping : mappings) {
                const std::size_t pos = body.label.find(mapping.prefix);
                if (pos != std::string::npos) {
                    provenance.operation = mapping.op;
                    provenance.source_ids.push_back(
                        body.label.substr(pos + std::strlen(mapping.prefix)));
                    provenance.approximation = true;
                    provenance.exact = false;
                    break;
                }
            }
        }
        report.provenance.push_back(std::move(provenance));
        if (!body.emit_as_cell) {
            continue;
        }
        AnalyzedSolid solid;
        solid.id = body.id;
        solid.label = body.label;
        solid.volume = synthetic_volume_estimate(body);
        solid.bbox = body.bbox.valid
            ? body.bbox
            : bbox_from_shape(detail::build_synthetic_shape(model, body));
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
        report.provenance.push_back({piece.id, "decompose",
                                     {piece.source_solid_id}, true, false,
                                     false, 0.0});
        if (analyzed_piece.needs_decomposition) {
            ++report.solids_requiring_decomposition;
        }
    }

    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (!body.active || body.materialized_solid.IsNull()) continue;
        AnalyzedSolid solid;
        solid.id = body.id;
        solid.label = body.label;
        solid.volume = shape_volume(body.materialized_solid);
        solid.bbox = bbox_from_shape(body.materialized_solid);
        solid.exact_csg_supported = true;
        report.solids.push_back(solid);
        report.solid_volumes.push_back({body.label, solid.volume});
        report.total_volume += solid.volume;
        ++report.solids_seen;
        report.provenance.push_back({body.id, "shell_difference",
                                     {body.source_solid_id}, true, false,
                                     false, 0.0});
    }

    for (const std::string& removed_id : model.removed_solid_ids) {
        report.provenance.push_back({removed_id, "remove_solid",
                                     {removed_id}, true, false, false, 0.0});
    }
    for (const FaceReplacement& replacement : model.face_replacements) {
        ProvenanceRecord provenance;
        provenance.entity_id = replacement.face_id;
        provenance.operation = "replace_face_with_fit";
        provenance.source_ids = {replacement.face_id};
        provenance.approximation = replacement.fit.max_deviation > 0.0;
        provenance.exact = !provenance.approximation;
        provenance.has_tolerance_budget = replacement.has_max_deviation;
        provenance.tolerance_budget = replacement.max_deviation;
        report.provenance.push_back(std::move(provenance));
    }

    finalize_cleanup_metrics(report, options);
    append_decision_requests(report);
    // Overlap detection is intentionally NOT run here -- analyze_model
    // is called from every mutating session call (via
    // finish_document_mutation), and the O(N^2) bbox-prescreened
    // boolean intersections add up quickly during BSP recursion. The
    // session-level analyze() entry point invokes detect_solid_overlaps
    // separately so the cost is paid once per explicit user request.

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

} // namespace csgify
