#pragma once

// OCCT-backed native engine model. This is intentionally not installed as the
// external API: users and agents should drive the engine through session.hpp,
// IDs, commands, reports, and files.

#include "cad2csg/engine.hpp"

#include <string>
#include <vector>

#include <alea.h>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>

namespace cad2csg {

// Tolerance-rounded geometric identity used as a soft-match key when an
// XCAF component_entry can no longer be resolved against a revised STEP.
// See docs/DECISION_LOG_AND_RECIPE.md ("Identity Across Revisions").
struct ShapeFingerprint {
    bool valid = false;
    double volume = 0.0;
    double surface_area = 0.0;
    double bbox_diagonal = 0.0;
    int face_count = 0;
    double centroid_x = 0.0;
    double centroid_y = 0.0;
    double centroid_z = 0.0;
};

struct ShapeMetadata {
    std::string shape_id;
    std::string name;
    std::string hierarchy_path;
    std::string component_entry;
    ShapeFingerprint fingerprint;
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
    bool active = true;
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
    std::vector<ShapeMetadata> solid_metadata;
    std::vector<AssemblyNode> assembly;

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

AutoFitSummary auto_fit_splines(CadModel& model,
                                bool has_max_deviation,
                                double max_deviation,
                                const ConversionOptions& options = {});

bool decompose_solid_by_planar_faces(CadModel& model,
                                     const std::string& solid_id,
                                     const ConversionOptions& options = {});

bool decompose_solid_by_face(CadModel& model,
                             const std::string& solid_id,
                             const std::string& face_id,
                             const ConversionOptions& options = {});

DecompositionProposal propose_decomposition_candidates(
    const CadModel& model,
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

VolumeComparison compare_step_volumes(const std::string& original_path,
                                      const std::string& candidate_path,
                                      double absolute_tolerance,
                                      double relative_tolerance,
                                      const ConversionOptions& options = {});

int export_mcnp(alea_system_t* system, const std::string& output_path);

} // namespace cad2csg
