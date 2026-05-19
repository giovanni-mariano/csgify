#pragma once

#include <memory>
#include <string>
#include <vector>

#include <alea.h>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>

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
};

struct SplineFitCandidates {
    bool attempted = false;
    SplineFaceFit plane;
    SplineFaceFit sphere;
    SplineFaceFit cylinder;
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

enum class SyntheticBodyKind {
    box,
    sphere,
    cylinder,
    cone,
    union_body,
    intersection_body,
    difference_body,
};

struct SyntheticBody {
    std::string id;
    std::string label;
    SyntheticBodyKind kind = SyntheticBodyKind::box;
    BBox bbox;
    Vec3 center;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double top_radius = 0.0;
    std::string left_id;
    std::string right_id;
    int material_id = 1;
    double density = 0.0;
    bool emit_as_cell = true;
};

struct DecomposedSolidPiece {
    std::string id;
    std::string label;
    std::string source_solid_id;
    TopoDS_Solid solid;
    double volume = 0.0;
};

struct FaceReplacement {
    std::string face_id;
    SplineFitKind kind = SplineFitKind::none;
    SplineFaceFit fit;
    bool has_max_deviation = false;
    double max_deviation = 0.0;
};

struct CadModel {
    std::string source_path;
    TopoDS_Shape root;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> removed_solid_ids;
    std::vector<SyntheticBody> synthetic_bodies;
    std::vector<DecomposedSolidPiece> decomposed_pieces;
    std::vector<FaceReplacement> face_replacements;

    bool loaded() const { return !root.IsNull(); }
};

using AnalysisReport = ConversionReport;

class AleaSystem {
public:
    AleaSystem();
    ~AleaSystem();

    AleaSystem(const AleaSystem&) = delete;
    AleaSystem& operator=(const AleaSystem&) = delete;

    AleaSystem(AleaSystem&& other) noexcept;
    AleaSystem& operator=(AleaSystem&& other) noexcept;

    alea_system_t* get() const { return system_; }
    alea_system_t* release();

private:
    alea_system_t* system_ = nullptr;
};

struct ConversionResult {
    AleaSystem system;
    ConversionReport report;
};

CadModel load_step_model(const std::string& step_path,
                         const ConversionOptions& options = {});

AnalysisReport analyze_model(const CadModel& model,
                             const ConversionOptions& options = {});

ReplacementMetrics replacement_metrics_for_operation(const AnalyzedSolid& solid,
                                                     ReplacementOperation operation);

ReplacementCheck check_replacement_limits(const CadModel& model,
                                          const std::string& solid_id,
                                          ReplacementOperation operation,
                                          const ReplacementLimits& limits,
                                          const ConversionOptions& options = {});

bool remove_solid(CadModel& model, const std::string& solid_id);

bool replace_face_with_fit(CadModel& model,
                           const std::string& face_id,
                           SplineFitKind kind,
                           bool has_max_deviation,
                           double max_deviation,
                           Diagnostic* diagnostic_out = nullptr,
                           const ConversionOptions& options = {});

bool decompose_solid_by_planar_faces(CadModel& model,
                                     const std::string& solid_id,
                                     const ConversionOptions& options = {});

std::string add_synthetic_box(CadModel& model,
                              const BBox& bbox,
                              int material_id = 1,
                              double density = 0.0,
                              const std::string& label = {});

std::string add_synthetic_sphere(CadModel& model,
                                 Vec3 center,
                                 double radius,
                                 int material_id = 1,
                                 double density = 0.0,
                                 const std::string& label = {});

std::string add_synthetic_cylinder(CadModel& model,
                                   Vec3 base,
                                   Vec3 height,
                                   double radius,
                                   int material_id = 1,
                                   double density = 0.0,
                                   const std::string& label = {});

std::string add_synthetic_cone(CadModel& model,
                               Vec3 base,
                               Vec3 height,
                               double base_radius,
                               double top_radius,
                               int material_id = 1,
                               double density = 0.0,
                               const std::string& label = {});

std::string add_synthetic_union(CadModel& model,
                                const std::string& left_id,
                                const std::string& right_id,
                                int material_id = 1,
                                double density = 0.0,
                                const std::string& label = {});

std::string add_synthetic_intersection(CadModel& model,
                                       const std::string& left_id,
                                       const std::string& right_id,
                                       int material_id = 1,
                                       double density = 0.0,
                                       const std::string& label = {});

std::string add_synthetic_difference(CadModel& model,
                                     const std::string& left_id,
                                     const std::string& right_id,
                                     int material_id = 1,
                                     double density = 0.0,
                                     const std::string& label = {});

bool replace_solid_with_bbox(CadModel& model,
                             const std::string& solid_id,
                             const ConversionOptions& options = {});

bool replace_solid_with_bbox_cylinder(CadModel& model,
                                      const std::string& solid_id,
                                      const ConversionOptions& options = {});

bool replace_solid_with_fitted_cylinder(CadModel& model,
                                        const std::string& solid_id,
                                        const ConversionOptions& options = {});

bool replace_solid_with_fitted_sphere(CadModel& model,
                                      const std::string& solid_id,
                                      const ConversionOptions& options = {});

bool replace_solid_with_fitted_cone(CadModel& model,
                                    const std::string& solid_id,
                                    const ConversionOptions& options = {});

bool apply_replacement_operation(CadModel& model,
                                 const std::string& solid_id,
                                 ReplacementOperation operation,
                                 const ConversionOptions& options = {});

bool apply_checked_replacement_operation(CadModel& model,
                                         const std::string& solid_id,
                                         ReplacementOperation operation,
                                         const ReplacementLimits& limits,
                                         ReplacementCheck* check_out = nullptr,
                                         const ConversionOptions& options = {});

ConversionReport inspect_step_file(const std::string& step_path,
                                   const ConversionOptions& options = {});

ConversionResult convert_model(const CadModel& model,
                               const ConversionOptions& options = {});

ConversionResult convert_step_file(const std::string& step_path,
                                   const ConversionOptions& options = {});

int write_report_json(const ConversionReport& report, const std::string& output_path);

VolumeComparison compare_step_volumes(const std::string& original_path,
                                      const std::string& candidate_path,
                                      double absolute_tolerance,
                                      double relative_tolerance,
                                      const ConversionOptions& options = {});

int export_mcnp(alea_system_t* system, const std::string& output_path);

} // namespace cad2csg
