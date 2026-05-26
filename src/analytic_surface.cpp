#include "analytic_surface.hpp"

#include "engine_internal.hpp"
#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_XYZ.hxx>

namespace cad2csg::detail {
namespace {

double plane_value(const gp_Pln& plane, const gp_Pnt& point) {
    const gp_XYZ n = plane.Axis().Direction().XYZ();
    return n.X() * point.X() + n.Y() * point.Y() + n.Z() * point.Z()
        - n.Dot(plane.Location().XYZ());
}

double sphere_value(const gp_Sphere& sphere, const gp_Pnt& point) {
    const gp_XYZ d = point.XYZ() - sphere.Location().XYZ();
    const double r = sphere.Radius();
    return d.SquareModulus() - r * r;
}

} // namespace

double quadric_value(const QuadricCoefficients& q, const gp_Pnt& p) {
    const double x = p.X();
    const double y = p.Y();
    const double z = p.Z();
    return q.a * x * x + q.b * y * y + q.c * z * z
        + q.d * x * y + q.e * y * z + q.f * x * z
        + q.g * x + q.h * y + q.i * z + q.j;
}

double AnalyticSurface::value_at(const gp_Pnt& point) const {
    switch (kind) {
    case AnalyticSurfaceKind::plane:
        return plane_value(plane, point);
    case AnalyticSurfaceKind::sphere:
        return sphere_value(sphere, point);
    case AnalyticSurfaceKind::quadric:
        return quadric_value(quadric, point);
    case AnalyticSurfaceKind::torus: {
        const gp_XYZ axis = torus.Axis().Direction().XYZ();
        const gp_XYZ d = point.XYZ() - torus.Location().XYZ();
        const double axial = d.Dot(axis);
        const double dist2 = d.SquareModulus();
        const double radial2 = std::max(0.0, dist2 - axial * axial);
        const double major = torus.MajorRadius();
        const double minor = torus.MinorRadius();
        const double a = dist2 + major * major - minor * minor;
        return a * a - 4.0 * major * major * radial2;
    }
    }
    return 0.0;
}

double AnalyticSurface::gradient_norm_at(const gp_Pnt& point) const {
    switch (kind) {
    case AnalyticSurfaceKind::plane:
        // f = n.p - n.p0 with n a unit normal, so |grad f| = 1.
        return 1.0;
    case AnalyticSurfaceKind::sphere: {
        // f = |p - c|^2 - r^2, grad f = 2 (p - c).
        const gp_XYZ d = point.XYZ() - sphere.Location().XYZ();
        return 2.0 * d.Modulus();
    }
    case AnalyticSurfaceKind::quadric: {
        // f = a x^2 + b y^2 + c z^2 + d xy + e yz + f xz + g x + h y + i z + j
        const double x = point.X();
        const double y = point.Y();
        const double z = point.Z();
        const double gx = 2.0 * quadric.a * x + quadric.d * y + quadric.f * z + quadric.g;
        const double gy = 2.0 * quadric.b * y + quadric.d * x + quadric.e * z + quadric.h;
        const double gz = 2.0 * quadric.c * z + quadric.e * y + quadric.f * x + quadric.i;
        return std::sqrt(gx * gx + gy * gy + gz * gz);
    }
    case AnalyticSurfaceKind::torus: {
        // f = A^2 - 4 M^2 R, with A = D + M^2 - m^2, D = |d|^2,
        // R = D - axial^2, d = p - center, axial = d.axis.
        // grad f = 4 [ A d - 2 M^2 (d - axial axis) ].
        const gp_XYZ axis = torus.Axis().Direction().XYZ();
        const gp_XYZ d = point.XYZ() - torus.Location().XYZ();
        const double axial = d.Dot(axis);
        const double major = torus.MajorRadius();
        const double minor = torus.MinorRadius();
        const double big_a = d.SquareModulus() + major * major - minor * minor;
        const gp_XYZ grad =
            (d * big_a - (d - axis * axial) * (2.0 * major * major)) * 4.0;
        return grad.Modulus();
    }
    }
    return 0.0;
}

double AnalyticSurface::signed_distance_at(const gp_Pnt& point) const {
    const double value = value_at(point);
    const double grad = gradient_norm_at(point);
    // Near a critical point (grad -> 0) the first-order estimate breaks down;
    // fall back to the raw value rather than dividing by ~zero.
    constexpr double kMinGrad = 1.0e-12;
    if (grad <= kMinGrad) {
        return value;
    }
    return value / grad;
}

std::string surface_type_name(GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezier";
    case GeomAbs_BSplineSurface: return "bspline";
    case GeomAbs_SurfaceOfRevolution: return "surface_of_revolution";
    case GeomAbs_SurfaceOfExtrusion: return "surface_of_extrusion";
    case GeomAbs_OffsetSurface: return "offset";
    case GeomAbs_OtherSurface: return "other";
    }
    return "unknown";
}

std::string surface_support_kind_name(GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezier";
    case GeomAbs_BSplineSurface: return "bspline";
    case GeomAbs_SurfaceOfRevolution: return "surface_of_revolution";
    case GeomAbs_SurfaceOfExtrusion: return "surface_of_extrusion";
    case GeomAbs_OffsetSurface: return "offset";
    case GeomAbs_OtherSurface: return "unknown";
    }
    return "unknown";
}

bool surface_type_is_spline(GeomAbs_SurfaceType type) {
    return type == GeomAbs_BSplineSurface || type == GeomAbs_BezierSurface;
}

bool surface_type_needs_emitter(GeomAbs_SurfaceType type) {
    return type == GeomAbs_Torus;
}

bool direction_is_axis_aligned(const gp_Dir& direction, double tolerance) {
    return (std::abs(std::abs(direction.X()) - 1.0) <= tolerance
            && std::abs(direction.Y()) <= tolerance
            && std::abs(direction.Z()) <= tolerance)
        || (std::abs(std::abs(direction.Y()) - 1.0) <= tolerance
            && std::abs(direction.X()) <= tolerance
            && std::abs(direction.Z()) <= tolerance)
        || (std::abs(std::abs(direction.Z()) - 1.0) <= tolerance
            && std::abs(direction.X()) <= tolerance
            && std::abs(direction.Y()) <= tolerance);
}

QuadricCoefficients cylinder_quadric(const gp_Cylinder& cylinder) {
    const gp_XYZ axis = cylinder.Axis().Direction().XYZ();
    const gp_XYZ point = cylinder.Location().XYZ();
    const double nx = axis.X();
    const double ny = axis.Y();
    const double nz = axis.Z();

    const double m00 = 1.0 - nx * nx;
    const double m11 = 1.0 - ny * ny;
    const double m22 = 1.0 - nz * nz;
    const double m01 = -nx * ny;
    const double m12 = -ny * nz;
    const double m02 = -nx * nz;

    const double px = m00 * point.X() + m01 * point.Y() + m02 * point.Z();
    const double py = m01 * point.X() + m11 * point.Y() + m12 * point.Z();
    const double pz = m02 * point.X() + m12 * point.Y() + m22 * point.Z();

    QuadricCoefficients q;
    q.a = m00; q.b = m11; q.c = m22;
    q.d = 2.0 * m01; q.e = 2.0 * m12; q.f = 2.0 * m02;
    q.g = -2.0 * px; q.h = -2.0 * py; q.i = -2.0 * pz;
    q.j = point.X() * px + point.Y() * py + point.Z() * pz
        - cylinder.Radius() * cylinder.Radius();
    return q;
}

QuadricCoefficients cone_quadric(Vec3 apex, Vec3 axis, double tan_angle_squared) {
    const double nx = axis.x;
    const double ny = axis.y;
    const double nz = axis.z;
    const double k = tan_angle_squared;

    const double m00 = 1.0 - (1.0 + k) * nx * nx;
    const double m11 = 1.0 - (1.0 + k) * ny * ny;
    const double m22 = 1.0 - (1.0 + k) * nz * nz;
    const double m01 = -(1.0 + k) * nx * ny;
    const double m12 = -(1.0 + k) * ny * nz;
    const double m02 = -(1.0 + k) * nx * nz;

    const double px = m00 * apex.x + m01 * apex.y + m02 * apex.z;
    const double py = m01 * apex.x + m11 * apex.y + m12 * apex.z;
    const double pz = m02 * apex.x + m12 * apex.y + m22 * apex.z;

    QuadricCoefficients q;
    q.a = m00; q.b = m11; q.c = m22;
    q.d = 2.0 * m01; q.e = 2.0 * m12; q.f = 2.0 * m02;
    q.g = -2.0 * px; q.h = -2.0 * py; q.i = -2.0 * pz;
    q.j = apex.x * px + apex.y * py + apex.z * pz;
    return q;
}

QuadricCoefficients cone_quadric(const gp_Cone& cone) {
    const gp_Dir direction = cone.Axis().Direction();
    const double tan_angle = std::tan(cone.SemiAngle());
    const double apex_offset = -cone.RefRadius() / tan_angle;
    const gp_Pnt location = cone.Location();
    const Vec3 axis = {direction.X(), direction.Y(), direction.Z()};
    const Vec3 apex = {
        location.X() + axis.x * apex_offset,
        location.Y() + axis.y * apex_offset,
        location.Z() + axis.z * apex_offset
    };
    return cone_quadric(apex, axis, tan_angle * tan_angle);
}

const FaceReplacement* find_face_replacement(
    const std::vector<FaceReplacement>* replacements,
    std::size_t solid_index,
    std::size_t face_index) {
    if (!replacements) {
        return nullptr;
    }
    const std::string id = face_id(solid_index, face_index);
    for (const FaceReplacement& r : *replacements) {
        if (r.face_id == id) {
            return &r;
        }
    }
    return nullptr;
}

bool analytic_surface_from_replacement(const FaceReplacement& r,
                                       AnalyticSurface* out) {
    switch (r.kind) {
    case SplineFitKind::plane: {
        out->kind = AnalyticSurfaceKind::plane;
        gp_Pnt loc(r.fit.origin.x, r.fit.origin.y, r.fit.origin.z);
        gp_Dir dir(r.fit.direction.x, r.fit.direction.y, r.fit.direction.z);
        out->plane = gp_Pln(loc, dir);
        return true;
    }
    case SplineFitKind::sphere: {
        out->kind = AnalyticSurfaceKind::sphere;
        gp_Ax3 axes(gp_Pnt(r.fit.origin.x, r.fit.origin.y, r.fit.origin.z),
                    gp_Dir(0, 0, 1));
        out->sphere = gp_Sphere(axes, r.fit.radius);
        return true;
    }
    case SplineFitKind::cylinder: {
        gp_Pnt loc(r.fit.origin.x, r.fit.origin.y, r.fit.origin.z);
        gp_Dir dir(r.fit.direction.x, r.fit.direction.y, r.fit.direction.z);
        gp_Ax3 axes(loc, dir);
        gp_Cylinder cyl(axes, r.fit.radius);
        out->kind = AnalyticSurfaceKind::quadric;
        out->quadric = cylinder_quadric(cyl);
        return true;
    }
    case SplineFitKind::cone: {
        const double tan_h = std::tan(r.fit.half_angle);
        out->kind = AnalyticSurfaceKind::quadric;
        out->quadric = cone_quadric(r.fit.origin, r.fit.direction, tan_h * tan_h);
        return true;
    }
    case SplineFitKind::none:
        return false;
    }
    return false;
}

bool analytic_surface_from_face(const TopoDS_Face& face,
                                std::size_t solid_index,
                                std::size_t face_index,
                                ConversionReport& report,
                                bool emit_diagnostics,
                                AnalyticSurface* out,
                                const std::vector<FaceReplacement>* replacements) {
    if (const FaceReplacement* r =
            find_face_replacement(replacements, solid_index, face_index)) {
        if (analytic_surface_from_replacement(*r, out)) {
            if (emit_diagnostics) {
                std::ostringstream msg;
                msg << "Spline face emitted as " << spline_fit_kind_name(r->kind)
                    << " (max_deviation=" << r->fit.max_deviation
                    << ", relative=" << r->fit.relative_max_deviation << ").";
                add_diag(report, DiagnosticSeverity::info,
                         "face_replacement_applied", msg.str(),
                         face_label(solid_index, face_index));
            }
            return true;
        }
    }
    BRepAdaptor_Surface surface(face, false);
    const GeomAbs_SurfaceType type = surface.GetType();
    const std::string label = face_label(solid_index, face_index);

    switch (type) {
    case GeomAbs_Plane:
        out->kind = AnalyticSurfaceKind::plane;
        out->plane = surface.Plane();
        return true;
    case GeomAbs_Sphere:
        out->kind = AnalyticSurfaceKind::sphere;
        out->sphere = surface.Sphere();
        return true;
    case GeomAbs_Cylinder:
        out->kind = AnalyticSurfaceKind::quadric;
        out->quadric = cylinder_quadric(surface.Cylinder());
        return true;
    case GeomAbs_Cone:
        out->kind = AnalyticSurfaceKind::quadric;
        out->quadric = cone_quadric(surface.Cone());
        return true;
    case GeomAbs_Torus: {
        const gp_Torus torus = surface.Torus();
        if (direction_is_axis_aligned(torus.Axis().Direction())) {
            out->kind = AnalyticSurfaceKind::torus;
            out->torus = torus;
            return true;
        }
        if (emit_diagnostics) {
            ++report.analytic_faces;
            add_diag(report, DiagnosticSeverity::error, "unsupported_torus_orientation",
                     "Torus faces are supported only when the torus axis is parallel to X, Y, or Z.",
                     label);
        }
        return false;
    }
    case GeomAbs_BSplineSurface:
    case GeomAbs_BezierSurface:
        if (emit_diagnostics) {
            ++report.spline_faces;
            add_diag(report, DiagnosticSeverity::error, "unsupported_spline_face",
                     "Spline faces need a simplification/replacement decision before exact CSG conversion.",
                     label);
        }
        return false;
    default:
        if (emit_diagnostics) {
            add_diag(report, DiagnosticSeverity::error, "unsupported_surface",
                     "Unsupported OCCT surface type for CSG halfspace conversion.",
                     label);
        }
        return false;
    }
}

} // namespace cad2csg::detail
