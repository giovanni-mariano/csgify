#include "fitted_candidates.hpp"

#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>

namespace cad2csg::detail {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

} // namespace

FittedCylinderCandidate fitted_cylinder_candidate(const TopoDS_Solid& solid,
                                                  double original_volume) {
    FittedCylinderCandidate candidate;
    double best_area = 0.0;
    gp_Cylinder best_cylinder;
    bool found = false;
    std::size_t cylinder_faces = 0;

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());
        BRepAdaptor_Surface adaptor(face, false);
        if (adaptor.GetType() != GeomAbs_Cylinder) {
            continue;
        }
        ++cylinder_faces;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        const double area = std::abs(props.Mass());
        if (!found || area > best_area) {
            found = true;
            best_area = area;
            best_cylinder = adaptor.Cylinder();
        }
    }
    if (!found) {
        return candidate;
    }
    const BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        return candidate;
    }

    const gp_Pnt location = best_cylinder.Location();
    const gp_Dir direction = best_cylinder.Axis().Direction();
    const Vec3 origin = {location.X(), location.Y(), location.Z()};
    const Vec3 axis = {direction.X(), direction.Y(), direction.Z()};

    bool have_projection = false;
    double min_t = 0.0;
    double max_t = 0.0;
    for (Vec3 corner : bbox_corners(bbox)) {
        const double t = dot(sub(corner, origin), axis);
        if (!have_projection) {
            min_t = t;
            max_t = t;
            have_projection = true;
        } else {
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }
    }

    const double length = max_t - min_t;
    if (length <= kEps || best_cylinder.Radius() <= kEps) {
        return candidate;
    }
    const double radius = best_cylinder.Radius();
    const double replacement_volume = kPi * radius * radius * length;
    candidate.metrics = replacement_metrics(original_volume, replacement_volume);
    candidate.axis_origin = origin;
    candidate.axis_direction = axis;
    candidate.base = add(origin, scale(axis, min_t));
    candidate.height = scale(axis, length);
    candidate.radius = radius;
    candidate.length = length;
    candidate.cylindrical_area = best_area;
    candidate.confidence = cylinder_faces == 1
        ? "single_cylindrical_face"
        : "dominant_cylindrical_face";
    return candidate;
}

FittedSphereCandidate fitted_sphere_candidate(const TopoDS_Solid& solid,
                                              double original_volume) {
    FittedSphereCandidate candidate;
    double best_area = 0.0;
    gp_Sphere best_sphere;
    bool found = false;
    std::size_t sphere_faces = 0;

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());
        BRepAdaptor_Surface adaptor(face, false);
        if (adaptor.GetType() != GeomAbs_Sphere) {
            continue;
        }
        ++sphere_faces;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        const double area = std::abs(props.Mass());
        if (!found || area > best_area) {
            found = true;
            best_area = area;
            best_sphere = adaptor.Sphere();
        }
    }
    if (!found || best_sphere.Radius() <= kEps) {
        return candidate;
    }
    const gp_Pnt location = best_sphere.Location();
    const double radius = best_sphere.Radius();
    const double replacement_volume = 4.0 * kPi * radius * radius * radius / 3.0;
    candidate.metrics = replacement_metrics(original_volume, replacement_volume);
    candidate.center = {location.X(), location.Y(), location.Z()};
    candidate.radius = radius;
    candidate.spherical_area = best_area;
    candidate.confidence = sphere_faces == 1
        ? "single_spherical_face"
        : "dominant_spherical_face";
    return candidate;
}

FittedConeCandidate fitted_cone_candidate(const TopoDS_Solid& solid,
                                          double original_volume) {
    FittedConeCandidate candidate;
    double best_area = 0.0;
    gp_Cone best_cone;
    bool found = false;
    std::size_t cone_faces = 0;

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());
        BRepAdaptor_Surface adaptor(face, false);
        if (adaptor.GetType() != GeomAbs_Cone) {
            continue;
        }
        ++cone_faces;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        const double area = std::abs(props.Mass());
        if (!found || area > best_area) {
            found = true;
            best_area = area;
            best_cone = adaptor.Cone();
        }
    }
    if (!found) {
        return candidate;
    }
    const BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        return candidate;
    }
    const gp_Pnt location = best_cone.Location();
    const gp_Dir direction = best_cone.Axis().Direction();
    const Vec3 origin = {location.X(), location.Y(), location.Z()};
    const Vec3 axis = {direction.X(), direction.Y(), direction.Z()};

    bool have_projection = false;
    double min_t = 0.0;
    double max_t = 0.0;
    for (Vec3 corner : bbox_corners(bbox)) {
        const double t = dot(sub(corner, origin), axis);
        if (!have_projection) {
            min_t = t;
            max_t = t;
            have_projection = true;
        } else {
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }
    }
    const double length = max_t - min_t;
    if (length <= kEps) {
        return candidate;
    }
    const double slope = std::tan(best_cone.SemiAngle());
    const double base_radius = std::abs(best_cone.RefRadius() + min_t * slope);
    const double top_radius = std::abs(best_cone.RefRadius() + max_t * slope);
    if (base_radius <= kEps && top_radius <= kEps) {
        return candidate;
    }
    const double replacement_volume = kPi * length
        * (base_radius * base_radius
           + base_radius * top_radius
           + top_radius * top_radius) / 3.0;
    candidate.metrics = replacement_metrics(original_volume, replacement_volume);
    candidate.axis_origin = origin;
    candidate.axis_direction = axis;
    candidate.base = add(origin, scale(axis, min_t));
    candidate.height = scale(axis, length);
    candidate.base_radius = base_radius;
    candidate.top_radius = top_radius;
    candidate.length = length;
    candidate.conical_area = best_area;
    candidate.confidence = cone_faces == 1
        ? "single_conical_face"
        : "dominant_conical_face";
    return candidate;
}

} // namespace cad2csg::detail
