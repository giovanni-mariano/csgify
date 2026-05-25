#pragma once

// Engine-internal helpers shared across analyze / cleanup / emit stages.
// Not part of the public API. All under cad2csg::detail.

#include "engine_model.hpp"

#include <string>
#include <vector>

#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>

namespace cad2csg::detail {

constexpr double kEps = 1.0e-14;

// Diagnostics.
void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label = {});

bool is_error(const Diagnostic& diagnostic);

// Stable ID and label formatting.
std::string solid_label(std::size_t index);
std::string solid_id(std::size_t index);
std::string face_id(std::size_t solid_index, std::size_t face_index);
std::string face_label(std::size_t solid_index, std::size_t face_index);

bool parse_solid_id(const std::string& id, std::size_t* index_out);
bool parse_face_id(const std::string& id,
                   std::size_t* solid_index_out,
                   std::size_t* face_index_out);

// BBox geometry.
BBox bbox_from_shape(const TopoDS_Shape& shape);
double bbox_dx(const BBox& bbox);
double bbox_dy(const BBox& bbox);
double bbox_dz(const BBox& bbox);
double bbox_diagonal(const BBox& bbox);
void sorted_bbox_extents(const BBox& bbox, double* min_e, double* mid_e, double* max_e);
double bbox_volume(const BBox& bbox);
double bbox_conservative_cylinder_volume(const BBox& bbox);
std::vector<Vec3> bbox_corners(const BBox& bbox);

// Solid-volume metrics.
ReplacementMetrics replacement_metrics(double original_volume,
                                       double replacement_volume);

// Interior-point search and convexity validation.
std::vector<gp_Pnt> solid_vertices(const TopoDS_Solid& solid);
bool point_is_inside_or_on(const TopoDS_Solid& solid,
                           const gp_Pnt& point,
                           double tolerance);
bool find_interior_point(const TopoDS_Solid& solid,
                         const GProp_GProps& props,
                         double tolerance,
                         gp_Pnt* point_out);
bool validate_convex_halfspace_cell(const TopoDS_Solid& solid,
                                    const gp_Pnt& interior_point,
                                    std::size_t solid_index,
                                    const ConversionOptions& options,
                                    ConversionReport& report,
                                    bool emit_error,
                                    bool count_decomposition,
                                    bool count_faces,
                                    const std::vector<FaceReplacement>* replacements = nullptr);
bool find_convexity_failure_face(const TopoDS_Solid& solid,
                                 std::size_t solid_index,
                                 const ConversionOptions& options,
                                 std::size_t* face_index_out);

// Small cross-cutting utilities.
double vec_len(Vec3 v);
double dot(Vec3 a, Vec3 b);
Vec3 sub(Vec3 a, Vec3 b);
Vec3 add(Vec3 a, Vec3 b);
Vec3 scale(Vec3 v, double s);
bool nearly_zero(double value, double tolerance = 1.0e-10);
bool contains_id(const std::vector<std::string>& ids, const std::string& id);
double synthetic_volume_estimate(const SyntheticBody& body);
const SyntheticBody* find_synthetic_body(const CadModel& model, const std::string& id);
SyntheticBody* find_synthetic_body_mut(CadModel& model, const std::string& id);

// Analyze a single decomposed piece (lives in src/analyze.cpp). Exposed so
// the emit path in engine.cpp can populate decomposed-piece entries in the
// conversion report.
AnalyzedDecomposedPiece analyze_decomposed_piece(const DecomposedSolidPiece& piece,
                                                 std::size_t index,
                                                 std::size_t report_solid_index,
                                                 const ConversionOptions& options);

} // namespace cad2csg::detail
