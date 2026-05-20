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
            << "\"approximate_with_facets\", \"remove_feature\", \"manual\"";
    } else if (code == "unsupported_analytic_face") {
        out << "\"add_emitter\", \"replace_surface\", \"manual\"";
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

std::string shape_id_from_label(const std::string& label) {
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

} // namespace

int write_report_json(const ConversionReport& report, const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out) {
        return -1;
    }

    out << "{\n";
    out << "  \"ok\": " << (report.ok() ? "true" : "false") << ",\n";
    out << "  \"occt_version\": \"" << json_escape(occt_version()) << "\",\n";
    out << "  \"alea_version\": \"" << json_escape(alea_version()) << "\",\n";
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
        out << "      \"solid_id\": \"" << json_escape(face.solid_id) << "\",\n";
        out << "      \"index\": " << face.index << ",\n";
        out << "      \"surface_type\": \"" << json_escape(face.surface_type) << "\",\n";
        out << "      \"area\": " << std::setprecision(17) << face.area << ",\n";
        out << "      \"bbox\": ";
        write_bbox_json(out, face.bbox, "      ");
        out << ",\n";
        out << "      \"exact_csg_supported\": "
            << (face.exact_csg_supported ? "true" : "false") << ",\n";
        out << "      \"needs_spline_replacement\": "
            << (face.needs_spline_replacement ? "true" : "false") << ",\n";
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
        out << "      \"needs_user_decision\": "
            << (piece.needs_user_decision ? "true" : "false") << "\n";
        out << "    }";
        if (i + 1 < report.decomposed_pieces.size()) {
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
        out << "      \"shape_id\": \"" << json_escape(shape_id_from_label(diagnostic.shape_label)) << "\",\n";
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

    return out ? 0 : -1;
}

} // namespace cad2csg
