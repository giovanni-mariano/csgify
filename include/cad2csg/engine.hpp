#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cad2csg {

const char* occt_version();

enum class DiagnosticSeverity {
    info,
    warning,
    error,
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::info;
    std::string code;
    std::string message;
    std::string shape_label;
    std::string shape_id;
    std::string document_id;
    std::string local_shape_id;
};

struct BBox {
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    bool valid = false;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SolidVolume {
    std::string shape_label;
    double volume = 0.0;
};

enum class SplineFitKind {
    none,
    plane,
    sphere,
    cylinder,
    cone,
};

struct SplineFaceFit {
    SplineFitKind kind = SplineFitKind::none;
    bool available = false;
    std::size_t sample_count = 0;
    double max_deviation = 0.0;
    double rms_deviation = 0.0;
    double relative_max_deviation = 0.0;
    Vec3 origin;
    Vec3 direction;
    double radius = 0.0;
    double half_angle = 0.0; // radians; populated for cone only
};

struct SplineFitCandidates {
    bool attempted = false;
    SplineFaceFit plane;
    SplineFaceFit sphere;
    SplineFaceFit cylinder;
    SplineFaceFit cone;
};

struct AnalyzedFace {
    std::string id;
    std::string label;
    std::string solid_id;
    std::size_t index = 0;
    std::string surface_type;
    double area = 0.0;
    BBox bbox;
    bool exact_csg_supported = false;
    bool needs_spline_replacement = false;
    bool needs_emitter = false;
    SplineFitCandidates spline_fits;
    std::string name;
    std::string hierarchy_path;
};

struct ReplacementMetrics {
    bool available = false;
    double volume = 0.0;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double fill_ratio = 0.0;
};

enum class ReplacementOperation {
    bbox,
    bbox_cylinder,
    fitted_cylinder,
    fitted_sphere,
    fitted_cone,
};

struct ReplacementLimits {
    bool has_absolute_delta = false;
    double max_absolute_delta = 0.0;
    bool has_relative_delta = false;
    double max_relative_delta = 0.0;
};

struct ReplacementCheck {
    bool ok = false;
    ReplacementMetrics metrics;
    Diagnostic diagnostic;
};

struct FittedCylinderCandidate {
    ReplacementMetrics metrics;
    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double length = 0.0;
    double cylindrical_area = 0.0;
    std::string confidence;
};

struct FittedSphereCandidate {
    ReplacementMetrics metrics;
    Vec3 center;
    double radius = 0.0;
    double spherical_area = 0.0;
    std::string confidence;
};

struct FittedConeCandidate {
    ReplacementMetrics metrics;
    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double base_radius = 0.0;
    double top_radius = 0.0;
    double length = 0.0;
    double conical_area = 0.0;
    std::string confidence;
};

struct AnalyzedSolid {
    std::string id;
    std::string label;
    std::size_t index = 0;
    double volume = 0.0;
    double volume_fraction = 0.0;
    BBox bbox;
    double bbox_diagonal = 0.0;
    double bbox_diagonal_fraction = 0.0;
    double bbox_min_extent = 0.0;
    double bbox_mid_extent = 0.0;
    double bbox_max_extent = 0.0;
    double bbox_aspect_ratio = 0.0;
    bool small_volume_candidate = false;
    bool small_bbox_candidate = false;
    bool thin_plate_candidate = false;
    bool long_body_candidate = false;
    ReplacementMetrics bbox_replacement;
    ReplacementMetrics bbox_cylinder_replacement;
    FittedCylinderCandidate fitted_cylinder_replacement;
    FittedSphereCandidate fitted_sphere_replacement;
    FittedConeCandidate fitted_cone_replacement;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_user_decision = false;
    std::vector<std::string> face_ids;
    std::string name;
    std::string hierarchy_path;
};

struct AnalyzedDecomposedPiece {
    std::string id;
    std::string label;
    std::string source_solid_id;
    std::size_t index = 0;
    double volume = 0.0;
    BBox bbox;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_user_decision = false;
};

struct DecompositionCandidatePiece {
    std::size_t index = 0;
    double volume = 0.0;
    double volume_fraction = 0.0;
    BBox bbox;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_user_decision = false;
};

struct DecompositionCandidate {
    std::string id;
    std::string target_id;
    std::string source_face_id;
    std::string source_face_label;
    std::string surface_type;
    std::string tool_kind;
    std::string reason;
    bool usable = false;
    std::string confidence;
    double score = 0.0;
    std::string rejection_reason;
    std::size_t piece_count = 0;
    std::size_t exact_piece_count = 0;
    std::size_t pieces_requiring_decomposition = 0;
    std::size_t pieces_requiring_spline_replacement = 0;
    std::size_t pieces_requiring_user_decision = 0;
    std::size_t sliver_piece_count = 0;
    double original_volume = 0.0;
    double pieces_volume = 0.0;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double min_piece_volume_fraction = 0.0;
    double max_piece_volume_fraction = 0.0;
    double volume_balance = 0.0;
    std::vector<DecompositionCandidatePiece> pieces;
    std::vector<Diagnostic> diagnostics;
};

struct DecompositionProposal {
    bool ok = false;
    std::string target_id;
    std::string best_candidate_id;
    std::vector<DecompositionCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};

struct DecompositionPolicy {
    bool allow_high_confidence_face_auto = true;
    bool allow_bbox_fallback_auto = false;
    bool allow_medium_confidence_auto = false;
    double max_relative_delta = 1.0e-8;
    double min_piece_volume_fraction = 1.0e-8;
};

struct AssemblyNode {
    std::string id;
    std::string parent_id;
    std::string name;
    std::string label_entry;
    std::string shape_id;
    std::string kind;
    std::string path;
};

struct ConversionOptions {
    double linear_tolerance = 1.0e-7;
    double angular_tolerance = 1.0e-8;
    double small_volume_fraction = 1.0e-4;
    double small_bbox_fraction = 1.0e-2;
    double thin_plate_ratio = 1.0e-2;
    double long_body_ratio = 10.0;
    int default_material_id = 1;
    double default_density = 0.0;
    bool heal_shape = true;
    bool require_convex_cells = true;
    bool try_planar_decomposition = true;
    bool split_union_cells = true;
};

struct ConversionReport {
    std::string document_id;
    std::string source_path;
    std::vector<Diagnostic> diagnostics;
    std::size_t solids_seen = 0;
    std::size_t solids_converted = 0;
    std::size_t solids_requiring_decomposition = 0;
    std::size_t decomposition_attempts = 0;
    std::size_t decomposition_pieces = 0;
    std::size_t faces_seen = 0;
    std::size_t analytic_faces = 0;
    std::size_t spline_faces = 0;
    double total_volume = 0.0;
    std::vector<SolidVolume> solid_volumes;
    std::vector<AnalyzedSolid> solids;
    std::vector<AnalyzedFace> faces;
    std::vector<AnalyzedDecomposedPiece> decomposed_pieces;
    std::vector<AssemblyNode> assembly;

    bool ok() const;
};

struct VolumeComparison {
    ConversionReport original;
    ConversionReport candidate;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double absolute_tolerance = 0.0;
    double relative_tolerance = 0.0;
    bool within_tolerance = false;
};

struct AutoFitSummary {
    std::size_t spline_faces_seen = 0;
    std::size_t applied = 0;
    std::size_t skipped_no_fit = 0;
    std::size_t skipped_over_budget = 0;
};

int write_report_json(const ConversionReport& report, const std::string& output_path);
std::string report_json_string(const ConversionReport& report);
int write_decomposition_proposal_json(const DecompositionProposal& proposal,
                                      const std::string& output_path);
std::string decomposition_proposal_json_string(const DecompositionProposal& proposal);

} // namespace cad2csg
