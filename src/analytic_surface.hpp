#pragma once

// OCCT face → CSG analytic-surface translation. Used by analyze (to classify
// faces) and emit (to build halfspaces). The module also owns the override
// path that lets a recorded FaceReplacement substitute the OCCT surface with
// a fitted analytic surface.

#include "cad2csg/engine.hpp"

#include <vector>

#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

namespace cad2csg::detail {

enum class AnalyticSurfaceKind {
    plane,
    sphere,
    quadric,
    torus,
};

struct QuadricCoefficients {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
    double e = 0.0;
    double f = 0.0;
    double g = 0.0;
    double h = 0.0;
    double i = 0.0;
    double j = 0.0;
};

struct AnalyticSurface {
    AnalyticSurfaceKind kind = AnalyticSurfaceKind::plane;
    gp_Pln plane;
    gp_Sphere sphere;
    gp_Torus torus;
    QuadricCoefficients quadric;

    double value_at(const gp_Pnt& point) const;
};

std::string surface_type_name(GeomAbs_SurfaceType type);
bool surface_type_supported_for_emit(GeomAbs_SurfaceType type);
bool surface_type_is_spline(GeomAbs_SurfaceType type);
bool surface_type_needs_emitter(GeomAbs_SurfaceType type);

bool direction_is_axis_aligned(const gp_Dir& direction, double tolerance = 1.0e-10);

QuadricCoefficients cylinder_quadric(const gp_Cylinder& cylinder);
QuadricCoefficients cone_quadric(Vec3 apex, Vec3 axis, double tan_angle_squared);
QuadricCoefficients cone_quadric(const gp_Cone& cone);
double quadric_value(const QuadricCoefficients& q, const gp_Pnt& p);

bool analytic_surface_from_replacement(const FaceReplacement& r,
                                       AnalyticSurface* out);

const FaceReplacement* find_face_replacement(
    const std::vector<FaceReplacement>* replacements,
    std::size_t solid_index,
    std::size_t face_index);

// Classify an OCCT face and produce an AnalyticSurface usable for CSG
// halfspace emission. If `replacements` is provided and contains an entry
// for this face, the recorded fit is used instead of OCCT's surface
// (the analyze stage emits a `face_replacement_applied` info diagnostic).
// Returns false for splines and unsupported types; in that case an error
// diagnostic is appended to `report` when `emit_diagnostics` is true.
bool analytic_surface_from_face(const TopoDS_Face& face,
                                std::size_t solid_index,
                                std::size_t face_index,
                                ConversionReport& report,
                                bool emit_diagnostics,
                                AnalyticSurface* out,
                                const std::vector<FaceReplacement>* replacements = nullptr);

} // namespace cad2csg::detail
