// JSON report writer. Pure output of an already-built ConversionReport; no
// dependencies on the rest of the engine's mutation state. Lives in its own
// translation unit so engine.cpp doesn't carry ~700 lines of stream-stitching.

#include "cad2csg/engine.hpp"

#include "spline_fit.hpp"

#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

#include <alea.h>

namespace cad2csg {
namespace {

const char* severity_json_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::info:
        return "info";
    case DiagnosticSeverity::warning:
        return "warning";
    case DiagnosticSeverity::error:
        return "error";
    }
    return "unknown";
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

void write_actions_for_code(std::ostream& out, const std::string& code) {
    if (code == "unsupported_spline_face") {
        out << "\"replace_with_plane\", \"replace_with_cylinder\", "
            << "\"replace_with_sphere\", \"replace_with_cone\", "
            << "\"split_face_piecewise\", "
            << "\"approximate_with_facets\", \"remove_feature\", \"manual\"";
    } else if (code == "unsupported_torus_orientation") {
        // Until the piecewise surface splitter lands, the workable paths
        // are: decompose the bounding solid along the torus axis plane
        // (engine already supports this via decompose-by-face on the torus
        // face), replace the face with a single cylinder or cone surrogate
        // for low-deviation tori, or remove the part entirely.
        out << "\"decompose_along_torus_axis_plane\", "
            << "\"replace_with_cylinder\", \"replace_with_cone\", "
            << "\"split_face_piecewise\", "
            << "\"remove_solid\", \"manual\"";
    } else if (code == "unsupported_analytic_face") {
        out << "\"add_emitter\", \"replace_surface\", "
            << "\"split_face_piecewise\", \"manual\"";
    } else if (code == "nonconvex_solid_requires_decomposition") {
        out << "\"split_by_plane\", \"split_by_existing_face\", "
            << "\"split_by_cylinder\", \"accept_manual_decomposition\", \"manual\"";
    } else if (code == "empty_or_tiny_solid") {
        out << "\"remove_solid\", \"keep\"";
    } else {
        out << "\"manual\"";
    }
}

void write_replacement_metrics_json(std::ostream& out,
                                    const ReplacementMetrics& metrics,
                                    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"available\": " << (metrics.available ? "true" : "false") << ",\n";
    out << indent << "  \"volume\": " << std::setprecision(17) << metrics.volume << ",\n";
    out << indent << "  \"absolute_delta\": " << std::setprecision(17)
        << metrics.absolute_delta << ",\n";
    out << indent << "  \"relative_delta\": " << std::setprecision(17)
        << metrics.relative_delta << ",\n";
    out << indent << "  \"fill_ratio\": " << std::setprecision(17)
        << metrics.fill_ratio << "\n";
    out << indent << "}";
}

void write_vec3_json(std::ostream& out, Vec3 v) {
    out << "[" << std::setprecision(17) << v.x << ", " << v.y << ", " << v.z << "]";
}

void write_spline_fit_json(std::ostream& out,
                           const SplineFaceFit& fit,
                           const std::string& indent) {
    out << "{\n";
    out << indent << "  \"kind\": \"" << detail::spline_fit_kind_name(fit.kind) << "\",\n";
    out << indent << "  \"available\": " << (fit.available ? "true" : "false") << ",\n";
    out << indent << "  \"sample_count\": " << fit.sample_count << ",\n";
    out << indent << "  \"max_deviation\": " << std::setprecision(17)
        << fit.max_deviation << ",\n";
    out << indent << "  \"rms_deviation\": " << std::setprecision(17)
        << fit.rms_deviation << ",\n";
    out << indent << "  \"relative_max_deviation\": " << std::setprecision(17)
        << fit.relative_max_deviation << ",\n";
    out << indent << "  \"origin\": ";
    write_vec3_json(out, fit.origin);
    out << ",\n";
    out << indent << "  \"direction\": ";
    write_vec3_json(out, fit.direction);
    out << ",\n";
    out << indent << "  \"radius\": " << std::setprecision(17)
        << fit.radius << ",\n";
    out << indent << "  \"half_angle\": " << std::setprecision(17)
        << fit.half_angle << "\n";
    out << indent << "}";
}

void write_spline_fits_json(std::ostream& out,
                            const SplineFitCandidates& fits,
                            const std::string& indent) {
    out << "{\n";
    out << indent << "  \"attempted\": " << (fits.attempted ? "true" : "false") << ",\n";
    out << indent << "  \"plane\": ";
    write_spline_fit_json(out, fits.plane, indent + "  ");
    out << ",\n";
    out << indent << "  \"sphere\": ";
    write_spline_fit_json(out, fits.sphere, indent + "  ");
    out << ",\n";
    out << indent << "  \"cylinder\": ";
    write_spline_fit_json(out, fits.cylinder, indent + "  ");
    out << ",\n";
    out << indent << "  \"cone\": ";
    write_spline_fit_json(out, fits.cone, indent + "  ");
    out << "\n";
    out << indent << "}";
}

void write_fitted_cylinder_json(std::ostream& out,
                                const FittedCylinderCandidate& candidate,
                                const std::string& indent) {
    out << "{\n";
    out << indent << "  \"available\": "
        << (candidate.metrics.available ? "true" : "false") << ",\n";
    out << indent << "  \"volume\": " << std::setprecision(17)
        << candidate.metrics.volume << ",\n";
    out << indent << "  \"absolute_delta\": " << std::setprecision(17)
        << candidate.metrics.absolute_delta << ",\n";
    out << indent << "  \"relative_delta\": " << std::setprecision(17)
        << candidate.metrics.relative_delta << ",\n";
    out << indent << "  \"fill_ratio\": " << std::setprecision(17)
        << candidate.metrics.fill_ratio << ",\n";
    out << indent << "  \"radius\": " << std::setprecision(17)
        << candidate.radius << ",\n";
    out << indent << "  \"length\": " << std::setprecision(17)
        << candidate.length << ",\n";
    out << indent << "  \"axis_origin\": ";
    write_vec3_json(out, candidate.axis_origin);
    out << ",\n";
    out << indent << "  \"axis_direction\": ";
    write_vec3_json(out, candidate.axis_direction);
    out << ",\n";
    out << indent << "  \"base\": ";
    write_vec3_json(out, candidate.base);
    out << ",\n";
    out << indent << "  \"height\": ";
    write_vec3_json(out, candidate.height);
    out << ",\n";
    out << indent << "  \"cylindrical_area\": " << std::setprecision(17)
        << candidate.cylindrical_area << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\"\n";
    out << indent << "}";
}

void write_fitted_sphere_json(std::ostream& out,
                              const FittedSphereCandidate& candidate,
                              const std::string& indent) {
    out << "{\n";
    out << indent << "  \"available\": "
        << (candidate.metrics.available ? "true" : "false") << ",\n";
    out << indent << "  \"volume\": " << std::setprecision(17)
        << candidate.metrics.volume << ",\n";
    out << indent << "  \"absolute_delta\": " << std::setprecision(17)
        << candidate.metrics.absolute_delta << ",\n";
    out << indent << "  \"relative_delta\": " << std::setprecision(17)
        << candidate.metrics.relative_delta << ",\n";
    out << indent << "  \"fill_ratio\": " << std::setprecision(17)
        << candidate.metrics.fill_ratio << ",\n";
    out << indent << "  \"radius\": " << std::setprecision(17)
        << candidate.radius << ",\n";
    out << indent << "  \"center\": ";
    write_vec3_json(out, candidate.center);
    out << ",\n";
    out << indent << "  \"spherical_area\": " << std::setprecision(17)
        << candidate.spherical_area << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\"\n";
    out << indent << "}";
}

void write_fitted_cone_json(std::ostream& out,
                            const FittedConeCandidate& candidate,
                            const std::string& indent) {
    out << "{\n";
    out << indent << "  \"available\": "
        << (candidate.metrics.available ? "true" : "false") << ",\n";
    out << indent << "  \"volume\": " << std::setprecision(17)
        << candidate.metrics.volume << ",\n";
    out << indent << "  \"absolute_delta\": " << std::setprecision(17)
        << candidate.metrics.absolute_delta << ",\n";
    out << indent << "  \"relative_delta\": " << std::setprecision(17)
        << candidate.metrics.relative_delta << ",\n";
    out << indent << "  \"fill_ratio\": " << std::setprecision(17)
        << candidate.metrics.fill_ratio << ",\n";
    out << indent << "  \"base_radius\": " << std::setprecision(17)
        << candidate.base_radius << ",\n";
    out << indent << "  \"top_radius\": " << std::setprecision(17)
        << candidate.top_radius << ",\n";
    out << indent << "  \"length\": " << std::setprecision(17)
        << candidate.length << ",\n";
    out << indent << "  \"axis_origin\": ";
    write_vec3_json(out, candidate.axis_origin);
    out << ",\n";
    out << indent << "  \"axis_direction\": ";
    write_vec3_json(out, candidate.axis_direction);
    out << ",\n";
    out << indent << "  \"base\": ";
    write_vec3_json(out, candidate.base);
    out << ",\n";
    out << indent << "  \"height\": ";
    write_vec3_json(out, candidate.height);
    out << ",\n";
    out << indent << "  \"conical_area\": " << std::setprecision(17)
        << candidate.conical_area << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\"\n";
    out << indent << "}";
}

void write_bbox_json(std::ostream& out, const BBox& bbox, const std::string& indent) {
    if (!bbox.valid) {
        out << "null";
        return;
    }
    out << "{\n";
    out << indent << "  \"min\": [" << std::setprecision(17)
        << bbox.xmin << ", " << bbox.ymin << ", " << bbox.zmin << "],\n";
    out << indent << "  \"max\": [" << std::setprecision(17)
        << bbox.xmax << ", " << bbox.ymax << ", " << bbox.zmax << "]\n";
    out << indent << "}";
}

void write_decision_metric_json(std::ostream& out,
                                const DecisionMetric& metric,
                                const std::string& indent) {
    out << "{\n";
    out << indent << "  \"name\": \"" << json_escape(metric.name) << "\",\n";
    out << indent << "  \"value\": " << std::setprecision(17)
        << metric.value << ",\n";
    out << indent << "  \"unit\": \"" << json_escape(metric.unit) << "\"\n";
    out << indent << "}";
}

void write_decision_metrics_json(std::ostream& out,
                                 const std::vector<DecisionMetric>& metrics,
                                 const std::string& indent) {
    out << "[\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        out << indent << "  ";
        write_decision_metric_json(out, metrics[i], indent + "  ");
        if (i + 1 < metrics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "]";
}

void write_decision_candidate_view_json(
    std::ostream& out,
    const DecisionCandidateView& candidate,
    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(candidate.id) << "\",\n";
    out << indent << "  \"action\": \"" << json_escape(candidate.action) << "\",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\",\n";
    out << indent << "  \"reason\": \"" << json_escape(candidate.reason) << "\",\n";
    out << indent << "  \"metrics\": ";
    write_decision_metrics_json(out, candidate.metrics, indent + "  ");
    out << "\n";
    out << indent << "}";
}

void write_decision_request_json(std::ostream& out,
                                 const DecisionRequest& request,
                                 const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(request.id) << "\",\n";
    out << indent << "  \"kind\": \"" << json_escape(request.kind) << "\",\n";
    out << indent << "  \"target_id\": \""
        << json_escape(request.target_id) << "\",\n";
    out << indent << "  \"reason\": \"" << json_escape(request.reason) << "\",\n";
    out << indent << "  \"recommended_action\": \""
        << json_escape(request.recommended_action) << "\",\n";
    out << indent << "  \"summary\": \"" << json_escape(request.summary) << "\",\n";
    out << indent << "  \"metrics\": ";
    write_decision_metrics_json(out, request.metrics, indent + "  ");
    out << ",\n";
    out << indent << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < request.candidates.size(); ++i) {
        out << indent << "    ";
        write_decision_candidate_view_json(out, request.candidates[i],
                                           indent + "    ");
        if (i + 1 < request.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "  ]\n";
    out << indent << "}";
}

std::string diagnostic_shape_id(const Diagnostic& diagnostic) {
    if (!diagnostic.shape_id.empty()) {
        return diagnostic.shape_id;
    }
    const std::string& label = diagnostic.shape_label;
    if (label.compare(0, 6, "solid:") == 0
        || label.compare(0, 5, "face:") == 0
        || label.compare(0, 6, "piece:") == 0
        || label.compare(0, 10, "synthetic:") == 0) {
        return label;
    }
    std::size_t solid_pos = label.find("solid[");
    if (solid_pos == std::string::npos) {
        return {};
    }
    solid_pos += 6;
    const std::size_t solid_end = label.find(']', solid_pos);
    if (solid_end == std::string::npos) {
        return {};
    }
    const std::string solid_number = label.substr(solid_pos, solid_end - solid_pos);

    const std::size_t face_pos = label.find("/face[", solid_end);
    if (face_pos == std::string::npos) {
        return "solid:" + solid_number;
    }
    const std::size_t face_number_pos = face_pos + 6;
    const std::size_t face_end = label.find(']', face_number_pos);
    if (face_end == std::string::npos) {
        return "solid:" + solid_number;
    }
    return "face:" + solid_number + "."
        + label.substr(face_number_pos, face_end - face_number_pos);
}

void write_report_json_stream(std::ostream& out, const ConversionReport& report) {
    out << "{\n";
    out << "  \"ok\": " << (report.ok() ? "true" : "false") << ",\n";
    out << "  \"occt_version\": \"" << json_escape(occt_version()) << "\",\n";
    out << "  \"alea_version\": \"" << json_escape(alea_version()) << "\",\n";
    if (!report.document_id.empty() || !report.source_path.empty()) {
        out << "  \"document\": {\n";
        out << "    \"id\": \"" << json_escape(report.document_id) << "\",\n";
        out << "    \"source_path\": \"" << json_escape(report.source_path) << "\"\n";
        out << "  },\n";
    }
    out << "  \"summary\": {\n";
    out << "    \"solids_seen\": " << report.solids_seen << ",\n";
    out << "    \"solids_converted\": " << report.solids_converted << ",\n";
    out << "    \"solids_requiring_decomposition\": "
        << report.solids_requiring_decomposition << ",\n";
    out << "    \"decomposition_attempts\": " << report.decomposition_attempts << ",\n";
    out << "    \"decomposition_pieces\": " << report.decomposition_pieces << ",\n";
    out << "    \"faces_seen\": " << report.faces_seen << ",\n";
    out << "    \"analytic_faces\": " << report.analytic_faces << ",\n";
    out << "    \"spline_faces\": " << report.spline_faces << ",\n";
    out << "    \"total_volume\": " << std::setprecision(17) << report.total_volume << "\n";
    out << "  },\n";
    out << "  \"performance\": {\n";
    out << "    \"load_step_ms\": " << std::setprecision(17)
        << report.performance.load_step_ms << ",\n";
    out << "    \"analyze_ms\": " << std::setprecision(17)
        << report.performance.analyze_ms << ",\n";
    out << "    \"propose_ms\": " << std::setprecision(17)
        << report.performance.propose_ms << ",\n";
    out << "    \"split_ms\": " << std::setprecision(17)
        << report.performance.split_ms << ",\n";
    out << "    \"emit_ms\": " << std::setprecision(17)
        << report.performance.emit_ms << ",\n";
    out << "    \"export_step_ms\": " << std::setprecision(17)
        << report.performance.export_step_ms << ",\n";
    out << "    \"candidates_evaluated\": "
        << report.performance.candidates_evaluated << ",\n";
    out << "    \"splitter_calls\": "
        << report.performance.splitter_calls << ",\n";
    out << "    \"pieces_produced\": "
        << report.performance.pieces_produced << "\n";
    out << "  },\n";
    out << "  \"solid_volumes\": [\n";
    for (std::size_t i = 0; i < report.solid_volumes.size(); ++i) {
        const SolidVolume& solid = report.solid_volumes[i];
        out << "    {\n";
        out << "      \"shape_label\": \"" << json_escape(solid.shape_label) << "\",\n";
        out << "      \"volume\": " << std::setprecision(17) << solid.volume << "\n";
        out << "    }";
        if (i + 1 < report.solid_volumes.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"solids\": [\n";
    for (std::size_t i = 0; i < report.solids.size(); ++i) {
        const AnalyzedSolid& solid = report.solids[i];
        out << "    {\n";
        out << "      \"id\": \"" << json_escape(solid.id) << "\",\n";
        out << "      \"label\": \"" << json_escape(solid.label) << "\",\n";
        out << "      \"name\": \"" << json_escape(solid.name) << "\",\n";
        out << "      \"hierarchy_path\": \"" << json_escape(solid.hierarchy_path) << "\",\n";
        out << "      \"index\": " << solid.index << ",\n";
        out << "      \"volume\": " << std::setprecision(17) << solid.volume << ",\n";
        out << "      \"volume_fraction\": " << std::setprecision(17)
            << solid.volume_fraction << ",\n";
        out << "      \"bbox\": ";
        write_bbox_json(out, solid.bbox, "      ");
        out << ",\n";
        out << "      \"bbox_diagonal\": " << std::setprecision(17)
            << solid.bbox_diagonal << ",\n";
        out << "      \"bbox_diagonal_fraction\": " << std::setprecision(17)
            << solid.bbox_diagonal_fraction << ",\n";
        out << "      \"bbox_extents\": {\n";
        out << "        \"min\": " << std::setprecision(17) << solid.bbox_min_extent << ",\n";
        out << "        \"mid\": " << std::setprecision(17) << solid.bbox_mid_extent << ",\n";
        out << "        \"max\": " << std::setprecision(17) << solid.bbox_max_extent << "\n";
        out << "      },\n";
        out << "      \"bbox_aspect_ratio\": " << std::setprecision(17)
            << solid.bbox_aspect_ratio << ",\n";
        out << "      \"cleanup_candidates\": {\n";
        out << "        \"small_volume\": "
            << (solid.small_volume_candidate ? "true" : "false") << ",\n";
        out << "        \"small_bbox\": "
            << (solid.small_bbox_candidate ? "true" : "false") << ",\n";
        out << "        \"thin_plate\": "
            << (solid.thin_plate_candidate ? "true" : "false") << ",\n";
        out << "        \"long_body\": "
            << (solid.long_body_candidate ? "true" : "false") << "\n";
        out << "      },\n";
        out << "      \"replacement_candidates\": {\n";
        out << "        \"bbox\": ";
        write_replacement_metrics_json(out, solid.bbox_replacement, "        ");
        out << ",\n";
        out << "        \"bbox_cylinder\": ";
        write_replacement_metrics_json(out, solid.bbox_cylinder_replacement, "        ");
        out << ",\n";
        out << "        \"fitted_cylinder\": ";
        write_fitted_cylinder_json(out, solid.fitted_cylinder_replacement, "        ");
        out << ",\n";
        out << "        \"fitted_sphere\": ";
        write_fitted_sphere_json(out, solid.fitted_sphere_replacement, "        ");
        out << ",\n";
        out << "        \"fitted_cone\": ";
        write_fitted_cone_json(out, solid.fitted_cone_replacement, "        ");
        out << "\n";
        out << "      },\n";
        out << "      \"face_count\": " << solid.face_count << ",\n";
        out << "      \"exact_csg_supported\": "
            << (solid.exact_csg_supported ? "true" : "false") << ",\n";
        out << "      \"needs_decomposition\": "
            << (solid.needs_decomposition ? "true" : "false") << ",\n";
        out << "      \"needs_spline_replacement\": "
            << (solid.needs_spline_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_surface_replacement\": "
            << (solid.needs_surface_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_user_decision\": "
            << (solid.needs_user_decision ? "true" : "false") << ",\n";
        out << "      \"faces\": [";
        for (std::size_t j = 0; j < solid.face_ids.size(); ++j) {
            out << "\"" << json_escape(solid.face_ids[j]) << "\"";
            if (j + 1 < solid.face_ids.size()) {
                out << ", ";
            }
        }
        out << "]\n";
        out << "    }";
        if (i + 1 < report.solids.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"faces\": [\n";
    for (std::size_t i = 0; i < report.faces.size(); ++i) {
        const AnalyzedFace& face = report.faces[i];
        out << "    {\n";
        out << "      \"id\": \"" << json_escape(face.id) << "\",\n";
        out << "      \"label\": \"" << json_escape(face.label) << "\",\n";
        out << "      \"name\": \"" << json_escape(face.name) << "\",\n";
        out << "      \"hierarchy_path\": \"" << json_escape(face.hierarchy_path) << "\",\n";
        out << "      \"solid_id\": \"" << json_escape(face.solid_id) << "\",\n";
        out << "      \"index\": " << face.index << ",\n";
        out << "      \"surface_type\": \"" << json_escape(face.surface_type) << "\",\n";
        out << "      \"support_kind\": \"" << json_escape(face.support_kind) << "\",\n";
        out << "      \"area\": " << std::setprecision(17) << face.area << ",\n";
        out << "      \"bbox\": ";
        write_bbox_json(out, face.bbox, "      ");
        out << ",\n";
        out << "      \"exact_csg_supported\": "
            << (face.exact_csg_supported ? "true" : "false") << ",\n";
        out << "      \"needs_spline_replacement\": "
            << (face.needs_spline_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_surface_replacement\": "
            << (face.needs_surface_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_emitter\": "
            << (face.needs_emitter ? "true" : "false") << ",\n";
        out << "      \"spline_fits\": ";
        write_spline_fits_json(out, face.spline_fits, "      ");
        out << "\n";
        out << "    }";
        if (i + 1 < report.faces.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"decomposed_pieces\": [\n";
    for (std::size_t i = 0; i < report.decomposed_pieces.size(); ++i) {
        const AnalyzedDecomposedPiece& piece = report.decomposed_pieces[i];
        out << "    {\n";
        out << "      \"id\": \"" << json_escape(piece.id) << "\",\n";
        out << "      \"label\": \"" << json_escape(piece.label) << "\",\n";
        out << "      \"source_solid_id\": \"" << json_escape(piece.source_solid_id) << "\",\n";
        out << "      \"index\": " << piece.index << ",\n";
        out << "      \"volume\": " << std::setprecision(17) << piece.volume << ",\n";
        out << "      \"bbox\": ";
        write_bbox_json(out, piece.bbox, "      ");
        out << ",\n";
        out << "      \"face_count\": " << piece.face_count << ",\n";
        out << "      \"exact_csg_supported\": "
            << (piece.exact_csg_supported ? "true" : "false") << ",\n";
        out << "      \"needs_decomposition\": "
            << (piece.needs_decomposition ? "true" : "false") << ",\n";
        out << "      \"needs_spline_replacement\": "
            << (piece.needs_spline_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_surface_replacement\": "
            << (piece.needs_surface_replacement ? "true" : "false") << ",\n";
        out << "      \"needs_user_decision\": "
            << (piece.needs_user_decision ? "true" : "false") << "\n";
        out << "    }";
        if (i + 1 < report.decomposed_pieces.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"assembly\": [\n";
    for (std::size_t i = 0; i < report.assembly.size(); ++i) {
        const AssemblyNode& node = report.assembly[i];
        out << "    {\n";
        out << "      \"id\": \"" << json_escape(node.id) << "\",\n";
        out << "      \"parent_id\": \"" << json_escape(node.parent_id) << "\",\n";
        out << "      \"name\": \"" << json_escape(node.name) << "\",\n";
        out << "      \"label_entry\": \"" << json_escape(node.label_entry) << "\",\n";
        out << "      \"shape_id\": \"" << json_escape(node.shape_id) << "\",\n";
        out << "      \"kind\": \"" << json_escape(node.kind) << "\",\n";
        out << "      \"path\": \"" << json_escape(node.path) << "\"\n";
        out << "    }";
        if (i + 1 < report.assembly.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"decision_requests\": [\n";
    for (std::size_t i = 0; i < report.decision_requests.size(); ++i) {
        out << "    ";
        write_decision_request_json(out, report.decision_requests[i], "    ");
        if (i + 1 < report.decision_requests.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
        const Diagnostic& diagnostic = report.diagnostics[i];
        out << "    {\n";
        out << "      \"severity\": \"" << severity_json_name(diagnostic.severity) << "\",\n";
        out << "      \"code\": \"" << json_escape(diagnostic.code) << "\",\n";
        out << "      \"document_id\": \"" << json_escape(diagnostic.document_id) << "\",\n";
        out << "      \"shape_id\": \"" << json_escape(diagnostic_shape_id(diagnostic)) << "\",\n";
        out << "      \"local_shape_id\": \"" << json_escape(diagnostic.local_shape_id) << "\",\n";
        out << "      \"shape_label\": \"" << json_escape(diagnostic.shape_label) << "\",\n";
        out << "      \"message\": \"" << json_escape(diagnostic.message) << "\",\n";
        out << "      \"actions\": [";
        write_actions_for_code(out, diagnostic.code);
        out << "]\n";
        out << "    }";
        if (i + 1 < report.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace

int write_report_json(const ConversionReport& report, const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }
    write_report_json_stream(out, report);

    return out ? 0 : -1;
}

std::string report_json_string(const ConversionReport& report) {
    std::ostringstream out;
    write_report_json_stream(out, report);
    return out.str();
}

namespace {

void write_decomposition_candidate_piece_json(
    std::ostream& out,
    const DecompositionCandidatePiece& piece,
    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"index\": " << piece.index << ",\n";
    out << indent << "  \"volume\": " << std::setprecision(17)
        << piece.volume << ",\n";
    out << indent << "  \"volume_fraction\": " << std::setprecision(17)
        << piece.volume_fraction << ",\n";
    out << indent << "  \"bbox\": ";
    write_bbox_json(out, piece.bbox, indent + "  ");
    out << ",\n";
    out << indent << "  \"face_count\": " << piece.face_count << ",\n";
    out << indent << "  \"exact_csg_supported\": "
        << (piece.exact_csg_supported ? "true" : "false") << ",\n";
    out << indent << "  \"needs_decomposition\": "
        << (piece.needs_decomposition ? "true" : "false") << ",\n";
    out << indent << "  \"needs_spline_replacement\": "
        << (piece.needs_spline_replacement ? "true" : "false") << ",\n";
    out << indent << "  \"needs_surface_replacement\": "
        << (piece.needs_surface_replacement ? "true" : "false") << ",\n";
    out << indent << "  \"needs_user_decision\": "
        << (piece.needs_user_decision ? "true" : "false") << "\n";
    out << indent << "}";
}

void write_diagnostic_json(std::ostream& out,
                           const Diagnostic& diagnostic,
                           const std::string& indent) {
    out << "{\n";
    out << indent << "  \"severity\": \""
        << severity_json_name(diagnostic.severity) << "\",\n";
    out << indent << "  \"code\": \"" << json_escape(diagnostic.code) << "\",\n";
    out << indent << "  \"document_id\": \""
        << json_escape(diagnostic.document_id) << "\",\n";
    out << indent << "  \"shape_id\": \""
        << json_escape(diagnostic_shape_id(diagnostic)) << "\",\n";
    out << indent << "  \"local_shape_id\": \""
        << json_escape(diagnostic.local_shape_id) << "\",\n";
    out << indent << "  \"shape_label\": \""
        << json_escape(diagnostic.shape_label) << "\",\n";
    out << indent << "  \"message\": \""
        << json_escape(diagnostic.message) << "\"\n";
    out << indent << "}";
}

void write_decomposition_candidate_json(
    std::ostream& out,
    const DecompositionCandidate& candidate,
    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(candidate.id) << "\",\n";
    out << indent << "  \"target_id\": \""
        << json_escape(candidate.target_id) << "\",\n";
    out << indent << "  \"source_face_id\": \""
        << json_escape(candidate.source_face_id) << "\",\n";
    out << indent << "  \"source_face_label\": \""
        << json_escape(candidate.source_face_label) << "\",\n";
    out << indent << "  \"surface_type\": \""
        << json_escape(candidate.surface_type) << "\",\n";
    out << indent << "  \"tool_kind\": \""
        << json_escape(candidate.tool_kind) << "\",\n";
    out << indent << "  \"reason\": \""
        << json_escape(candidate.reason) << "\",\n";
    out << indent << "  \"plane_valid\": "
        << (candidate.plane_valid ? "true" : "false") << ",\n";
    out << indent << "  \"plane_origin\": ";
    write_vec3_json(out, candidate.plane_origin);
    out << ",\n";
    out << indent << "  \"plane_normal\": ";
    write_vec3_json(out, candidate.plane_normal);
    out << ",\n";
    out << indent << "  \"planes\": [\n";
    for (std::size_t i = 0; i < candidate.planes.size(); ++i) {
        const PlaneSpec& p = candidate.planes[i];
        out << indent << "    {\"origin\": ";
        write_vec3_json(out, p.origin);
        out << ", \"normal\": ";
        write_vec3_json(out, p.normal);
        out << "}";
        if (i + 1 < candidate.planes.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "  ],\n";
    out << indent << "  \"usable\": "
        << (candidate.usable ? "true" : "false") << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\",\n";
    out << indent << "  \"score\": " << std::setprecision(17)
        << candidate.score << ",\n";
    out << indent << "  \"rejection_reason\": \""
        << json_escape(candidate.rejection_reason) << "\",\n";
    out << indent << "  \"piece_count\": " << candidate.piece_count << ",\n";
    out << indent << "  \"exact_piece_count\": "
        << candidate.exact_piece_count << ",\n";
    out << indent << "  \"pieces_requiring_decomposition\": "
        << candidate.pieces_requiring_decomposition << ",\n";
    out << indent << "  \"pieces_requiring_spline_replacement\": "
        << candidate.pieces_requiring_spline_replacement << ",\n";
    out << indent << "  \"pieces_requiring_user_decision\": "
        << candidate.pieces_requiring_user_decision << ",\n";
    out << indent << "  \"sliver_piece_count\": "
        << candidate.sliver_piece_count << ",\n";
    out << indent << "  \"original_volume\": " << std::setprecision(17)
        << candidate.original_volume << ",\n";
    out << indent << "  \"pieces_volume\": " << std::setprecision(17)
        << candidate.pieces_volume << ",\n";
    out << indent << "  \"absolute_delta\": " << std::setprecision(17)
        << candidate.absolute_delta << ",\n";
    out << indent << "  \"relative_delta\": " << std::setprecision(17)
        << candidate.relative_delta << ",\n";
    out << indent << "  \"min_piece_volume_fraction\": "
        << std::setprecision(17) << candidate.min_piece_volume_fraction << ",\n";
    out << indent << "  \"max_piece_volume_fraction\": "
        << std::setprecision(17) << candidate.max_piece_volume_fraction << ",\n";
    out << indent << "  \"volume_balance\": " << std::setprecision(17)
        << candidate.volume_balance << ",\n";
    out << indent << "  \"pieces\": [\n";
    for (std::size_t i = 0; i < candidate.pieces.size(); ++i) {
        out << indent << "    ";
        write_decomposition_candidate_piece_json(out, candidate.pieces[i],
                                                 indent + "    ");
        if (i + 1 < candidate.pieces.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "  ],\n";
    out << indent << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < candidate.diagnostics.size(); ++i) {
        out << indent << "    ";
        write_diagnostic_json(out, candidate.diagnostics[i], indent + "    ");
        if (i + 1 < candidate.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "  ]\n";
    out << indent << "}";
}

void write_decomposition_proposal_json_stream(
    std::ostream& out,
    const DecompositionProposal& proposal) {
    out << "{\n";
    out << "  \"ok\": " << (proposal.ok ? "true" : "false") << ",\n";
    out << "  \"target_id\": \"" << json_escape(proposal.target_id) << "\",\n";
    out << "  \"best_candidate_id\": \""
        << json_escape(proposal.best_candidate_id) << "\",\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        out << "    ";
        write_decomposition_candidate_json(out, proposal.candidates[i], "    ");
        if (i + 1 < proposal.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        out << "    ";
        write_diagnostic_json(out, proposal.diagnostics[i], "    ");
        if (i + 1 < proposal.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void write_string_array_json(std::ostream& out,
                             const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << "\"" << json_escape(values[i]) << "\"";
        if (i + 1 < values.size()) {
            out << ", ";
        }
    }
    out << "]";
}

void write_removal_candidate_json(std::ostream& out,
                                  const RemovalCandidate& candidate,
                                  const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(candidate.id) << "\",\n";
    out << indent << "  \"solid_id\": \""
        << json_escape(candidate.solid_id) << "\",\n";
    out << indent << "  \"label\": \"" << json_escape(candidate.label) << "\",\n";
    out << indent << "  \"name\": \"" << json_escape(candidate.name) << "\",\n";
    out << indent << "  \"hierarchy_path\": \""
        << json_escape(candidate.hierarchy_path) << "\",\n";
    out << indent << "  \"volume\": " << std::setprecision(17)
        << candidate.volume << ",\n";
    out << indent << "  \"volume_fraction\": " << std::setprecision(17)
        << candidate.volume_fraction << ",\n";
    out << indent << "  \"bbox\": ";
    write_bbox_json(out, candidate.bbox, indent + "  ");
    out << ",\n";
    out << indent << "  \"bbox_diagonal_fraction\": " << std::setprecision(17)
        << candidate.bbox_diagonal_fraction << ",\n";
    out << indent << "  \"reason_codes\": ";
    write_string_array_json(out, candidate.reason_codes);
    out << ",\n";
    out << indent << "  \"blockers\": ";
    write_string_array_json(out, candidate.blockers);
    out << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\",\n";
    out << indent << "  \"score\": " << std::setprecision(17)
        << candidate.score << ",\n";
    out << indent << "  \"suggested_action\": \""
        << json_escape(candidate.suggested_action) << "\"\n";
    out << indent << "}";
}

void write_removal_proposal_json_stream(std::ostream& out,
                                        const RemovalProposal& proposal) {
    out << "{\n";
    out << "  \"ok\": " << (proposal.ok ? "true" : "false") << ",\n";
    out << "  \"total_volume\": " << std::setprecision(17)
        << proposal.total_volume << ",\n";
    out << "  \"high_confidence_count\": "
        << proposal.high_confidence_count << ",\n";
    out << "  \"high_confidence_volume\": " << std::setprecision(17)
        << proposal.high_confidence_volume << ",\n";
    out << "  \"high_confidence_volume_fraction\": " << std::setprecision(17)
        << proposal.high_confidence_volume_fraction << ",\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        out << "    ";
        write_removal_candidate_json(out, proposal.candidates[i], "    ");
        if (i + 1 < proposal.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        out << "    ";
        write_diagnostic_json(out, proposal.diagnostics[i], "    ");
        if (i + 1 < proposal.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

const char* hole_fill_kind_json_name(HoleFillKind kind) {
    switch (kind) {
    case HoleFillKind::cylindrical_through:
        return "cylindrical_through";
    case HoleFillKind::cylindrical_blind:
        return "cylindrical_blind";
    case HoleFillKind::coaxial_stack:
        return "coaxial_stack";
    case HoleFillKind::planar_wire_patch:
        return "planar_wire_patch";
    case HoleFillKind::unknown:
        return "unknown";
    }
    return "unknown";
}

void write_hole_fill_candidate_json(std::ostream& out,
                                    const HoleFillCandidate& candidate,
                                    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(candidate.id) << "\",\n";
    out << indent << "  \"host_solid_id\": \""
        << json_escape(candidate.host_solid_id) << "\",\n";
    out << indent << "  \"seed_removed_solid_ids\": ";
    write_string_array_json(out, candidate.seed_removed_solid_ids);
    out << ",\n";
    out << indent << "  \"face_ids\": ";
    write_string_array_json(out, candidate.face_ids);
    out << ",\n";
    out << indent << "  \"kind\": \""
        << hole_fill_kind_json_name(candidate.kind) << "\",\n";
    out << indent << "  \"axis_origin\": ";
    write_vec3_json(out, candidate.axis_origin);
    out << ",\n";
    out << indent << "  \"axis_direction\": ";
    write_vec3_json(out, candidate.axis_direction);
    out << ",\n";
    out << indent << "  \"base\": ";
    write_vec3_json(out, candidate.base);
    out << ",\n";
    out << indent << "  \"height\": ";
    write_vec3_json(out, candidate.height);
    out << ",\n";
    out << indent << "  \"radius\": " << std::setprecision(17)
        << candidate.radius << ",\n";
    out << indent << "  \"top_radius\": " << std::setprecision(17)
        << candidate.top_radius << ",\n";
    out << indent << "  \"length\": " << std::setprecision(17)
        << candidate.length << ",\n";
    out << indent << "  \"plug_volume\": " << std::setprecision(17)
        << candidate.plug_volume << ",\n";
    out << indent << "  \"host_volume\": " << std::setprecision(17)
        << candidate.host_volume << ",\n";
    out << indent << "  \"plug_volume_fraction_of_host\": "
        << std::setprecision(17)
        << candidate.plug_volume_fraction_of_host << ",\n";
    out << indent << "  \"estimated_host_overlap_fraction\": "
        << std::setprecision(17)
        << candidate.estimated_host_overlap_fraction << ",\n";
    out << indent << "  \"estimated_other_overlap_fraction\": "
        << std::setprecision(17)
        << candidate.estimated_other_overlap_fraction << ",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\",\n";
    out << indent << "  \"score\": " << std::setprecision(17)
        << candidate.score << ",\n";
    out << indent << "  \"reason_codes\": ";
    write_string_array_json(out, candidate.reason_codes);
    out << ",\n";
    out << indent << "  \"blockers\": ";
    write_string_array_json(out, candidate.blockers);
    out << ",\n";
    out << indent << "  \"suggested_action\": \""
        << json_escape(candidate.suggested_action) << "\"\n";
    out << indent << "}";
}

void write_hole_fill_proposal_json_stream(std::ostream& out,
                                          const HoleFillProposal& proposal) {
    out << "{\n";
    out << "  \"ok\": " << (proposal.ok ? "true" : "false") << ",\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        out << "    ";
        write_hole_fill_candidate_json(out, proposal.candidates[i], "    ");
        if (i + 1 < proposal.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"decision_requests\": [\n";
    for (std::size_t i = 0; i < proposal.decision_requests.size(); ++i) {
        out << "    ";
        write_decision_request_json(out, proposal.decision_requests[i], "    ");
        if (i + 1 < proposal.decision_requests.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        out << "    ";
        write_diagnostic_json(out, proposal.diagnostics[i], "    ");
        if (i + 1 < proposal.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void write_surface_replacement_candidate_json(
    std::ostream& out,
    const SurfaceReplacementCandidate& candidate,
    const std::string& indent) {
    out << "{\n";
    out << indent << "  \"id\": \"" << json_escape(candidate.id) << "\",\n";
    out << indent << "  \"face_id\": \"" << json_escape(candidate.face_id) << "\",\n";
    out << indent << "  \"owner_target_id\": \""
        << json_escape(candidate.owner_target_id) << "\",\n";
    out << indent << "  \"kind\": \"" << json_escape(candidate.kind) << "\",\n";
    out << indent << "  \"action\": \"" << json_escape(candidate.action) << "\",\n";
    out << indent << "  \"confidence\": \""
        << json_escape(candidate.confidence) << "\",\n";
    out << indent << "  \"reason\": \"" << json_escape(candidate.reason) << "\",\n";
    out << indent << "  \"exact\": " << (candidate.exact ? "true" : "false") << ",\n";
    out << indent << "  \"usable\": " << (candidate.usable ? "true" : "false") << ",\n";
    out << indent << "  \"score\": " << std::setprecision(17)
        << candidate.score << ",\n";
    out << indent << "  \"max_deviation\": " << std::setprecision(17)
        << candidate.max_deviation << ",\n";
    out << indent << "  \"rms_deviation\": " << std::setprecision(17)
        << candidate.rms_deviation << ",\n";
    out << indent << "  \"relative_max_deviation\": " << std::setprecision(17)
        << candidate.relative_max_deviation << ",\n";
    out << indent << "  \"owner_relative_volume_delta\": " << std::setprecision(17)
        << candidate.owner_relative_volume_delta << ",\n";
    out << indent << "  \"segment_count\": " << candidate.segment_count << ",\n";
    out << indent << "  \"origin\": ";
    write_vec3_json(out, candidate.origin);
    out << ",\n";
    out << indent << "  \"direction\": ";
    write_vec3_json(out, candidate.direction);
    out << ",\n";
    out << indent << "  \"radius\": " << std::setprecision(17)
        << candidate.radius << ",\n";
    out << indent << "  \"half_angle\": " << std::setprecision(17)
        << candidate.half_angle << ",\n";
    out << indent << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < candidate.diagnostics.size(); ++i) {
        out << indent << "    ";
        write_diagnostic_json(out, candidate.diagnostics[i], indent + "    ");
        if (i + 1 < candidate.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << indent << "  ]\n";
    out << indent << "}";
}

void write_surface_replacement_proposal_json_stream(
    std::ostream& out,
    const SurfaceReplacementProposal& proposal) {
    out << "{\n";
    out << "  \"ok\": " << (proposal.ok ? "true" : "false") << ",\n";
    out << "  \"face_id\": \"" << json_escape(proposal.face_id) << "\",\n";
    out << "  \"owner_target_id\": \""
        << json_escape(proposal.owner_target_id) << "\",\n";
    out << "  \"support_kind\": \""
        << json_escape(proposal.support_kind) << "\",\n";
    out << "  \"best_candidate_id\": \""
        << json_escape(proposal.best_candidate_id) << "\",\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < proposal.candidates.size(); ++i) {
        out << "    ";
        write_surface_replacement_candidate_json(out, proposal.candidates[i], "    ");
        if (i + 1 < proposal.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"diagnostics\": [\n";
    for (std::size_t i = 0; i < proposal.diagnostics.size(); ++i) {
        out << "    ";
        write_diagnostic_json(out, proposal.diagnostics[i], "    ");
        if (i + 1 < proposal.diagnostics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace

int write_decomposition_proposal_json(const DecompositionProposal& proposal,
                                      const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }
    write_decomposition_proposal_json_stream(out, proposal);
    return out ? 0 : -1;
}

std::string decomposition_proposal_json_string(const DecompositionProposal& proposal) {
    std::ostringstream out;
    write_decomposition_proposal_json_stream(out, proposal);
    return out.str();
}

int write_removal_proposal_json(const RemovalProposal& proposal,
                                const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }
    write_removal_proposal_json_stream(out, proposal);
    return out ? 0 : -1;
}

std::string removal_proposal_json_string(const RemovalProposal& proposal) {
    std::ostringstream out;
    write_removal_proposal_json_stream(out, proposal);
    return out.str();
}

int write_hole_fill_proposal_json(const HoleFillProposal& proposal,
                                  const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }
    write_hole_fill_proposal_json_stream(out, proposal);
    return out ? 0 : -1;
}

std::string hole_fill_proposal_json_string(const HoleFillProposal& proposal) {
    std::ostringstream out;
    write_hole_fill_proposal_json_stream(out, proposal);
    return out.str();
}

int write_surface_replacement_proposal_json(
    const SurfaceReplacementProposal& proposal,
    const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }
    write_surface_replacement_proposal_json_stream(out, proposal);
    return out ? 0 : -1;
}

std::string surface_replacement_proposal_json_string(
    const SurfaceReplacementProposal& proposal) {
    std::ostringstream out;
    write_surface_replacement_proposal_json_stream(out, proposal);
    return out.str();
}

} // namespace cad2csg
