#include "cad2csg/engine.hpp"

#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BOPAlgo_Splitter.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Surface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <NCollection_List.hxx>
#include <ShapeFix_Shape.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Version.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Vec.hxx>

#include <alea_mcnp.h>

namespace cad2csg {
namespace {

constexpr double kEps = 1.0e-14;

FittedCylinderCandidate fitted_cylinder_candidate(const TopoDS_Solid& solid,
                                                  double original_volume);
FittedSphereCandidate fitted_sphere_candidate(const TopoDS_Solid& solid,
                                              double original_volume);
FittedConeCandidate fitted_cone_candidate(const TopoDS_Solid& solid,
                                          double original_volume);
const char* spline_fit_kind_name(SplineFitKind kind);

enum class AnalyticSurfaceKind {
    plane,
    sphere,
    quadric,
    torus,
};

void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label = {}) {
    report.diagnostics.push_back({severity, code, message, label});
}

std::string solid_label(std::size_t index) {
    std::ostringstream out;
    out << "solid[" << index << "]";
    return out.str();
}

std::string solid_id(std::size_t index) {
    std::ostringstream out;
    out << "solid:" << index;
    return out.str();
}

std::string face_id(std::size_t solid_index, std::size_t face_index) {
    std::ostringstream out;
    out << "face:" << solid_index << "." << face_index;
    return out.str();
}

std::string face_label(std::size_t solid_index, std::size_t face_index) {
    std::ostringstream out;
    out << "solid[" << solid_index << "]/face[" << face_index << "]";
    return out.str();
}

bool parse_solid_id(const std::string& id, std::size_t* index_out) {
    constexpr const char* prefix = "solid:";
    const std::size_t prefix_len = 6;
    if (id.compare(0, prefix_len, prefix) != 0 || id.size() == prefix_len) {
        return false;
    }

    std::size_t value = 0;
    for (std::size_t i = prefix_len; i < id.size(); ++i) {
        const char ch = id[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(ch - '0');
    }

    if (value == 0) {
        return false;
    }
    *index_out = value;
    return true;
}

bool parse_face_id(const std::string& id,
                   std::size_t* solid_index_out,
                   std::size_t* face_index_out) {
    constexpr const char* prefix = "face:";
    const std::size_t prefix_len = 5;
    if (id.compare(0, prefix_len, prefix) != 0) {
        return false;
    }
    std::size_t solid_value = 0;
    std::size_t i = prefix_len;
    for (; i < id.size(); ++i) {
        const char ch = id[i];
        if (ch == '.') {
            break;
        }
        if (ch < '0' || ch > '9') {
            return false;
        }
        solid_value = solid_value * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (solid_value == 0 || i >= id.size() || id[i] != '.') {
        return false;
    }
    ++i;
    if (i >= id.size()) {
        return false;
    }
    std::size_t face_value = 0;
    for (; i < id.size(); ++i) {
        const char ch = id[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        face_value = face_value * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (face_value == 0) {
        return false;
    }
    *solid_index_out = solid_value;
    *face_index_out = face_value;
    return true;
}

bool contains_id(const std::vector<std::string>& ids, const std::string& id) {
    for (const std::string& candidate : ids) {
        if (candidate == id) {
            return true;
        }
    }
    return false;
}

std::string next_synthetic_id(const CadModel& model) {
    return "synthetic:" + std::to_string(model.synthetic_bodies.size() + 1);
}

std::string next_decomposed_piece_id(const CadModel& model) {
    return "piece:" + std::to_string(model.decomposed_pieces.size() + 1);
}

const SyntheticBody* find_synthetic_body(const CadModel& model, const std::string& id) {
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (body.id == id) {
            return &body;
        }
    }
    return nullptr;
}

SyntheticBody* find_synthetic_body_mut(CadModel& model, const std::string& id) {
    for (SyntheticBody& body : model.synthetic_bodies) {
        if (body.id == id) {
            return &body;
        }
    }
    return nullptr;
}

bool is_error(const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::error;
}

BBox bbox_from_shape(const TopoDS_Shape& shape) {
    BBox result;
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        return result;
    }

    box.Get(result.xmin, result.ymin, result.zmin,
            result.xmax, result.ymax, result.zmax);
    result.valid = true;
    return result;
}

double bbox_dx(const BBox& bbox) {
    return bbox.valid ? std::abs(bbox.xmax - bbox.xmin) : 0.0;
}

double bbox_dy(const BBox& bbox) {
    return bbox.valid ? std::abs(bbox.ymax - bbox.ymin) : 0.0;
}

double bbox_dz(const BBox& bbox) {
    return bbox.valid ? std::abs(bbox.zmax - bbox.zmin) : 0.0;
}

double bbox_diagonal(const BBox& bbox) {
    const double dx = bbox_dx(bbox);
    const double dy = bbox_dy(bbox);
    const double dz = bbox_dz(bbox);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void sorted_bbox_extents(const BBox& bbox, double* min_e, double* mid_e, double* max_e) {
    double a = bbox_dx(bbox);
    double b = bbox_dy(bbox);
    double c = bbox_dz(bbox);
    if (a > b) {
        std::swap(a, b);
    }
    if (b > c) {
        std::swap(b, c);
    }
    if (a > b) {
        std::swap(a, b);
    }
    *min_e = a;
    *mid_e = b;
    *max_e = c;
}

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

std::string surface_type_name(GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane:
        return "plane";
    case GeomAbs_Cylinder:
        return "cylinder";
    case GeomAbs_Cone:
        return "cone";
    case GeomAbs_Sphere:
        return "sphere";
    case GeomAbs_Torus:
        return "torus";
    case GeomAbs_BezierSurface:
        return "bezier";
    case GeomAbs_BSplineSurface:
        return "bspline";
    case GeomAbs_SurfaceOfRevolution:
        return "surface_of_revolution";
    case GeomAbs_SurfaceOfExtrusion:
        return "surface_of_extrusion";
    case GeomAbs_OffsetSurface:
        return "offset";
    case GeomAbs_OtherSurface:
        return "other";
    }
    return "unknown";
}

bool surface_type_supported_for_emit(GeomAbs_SurfaceType type) {
    return type == GeomAbs_Plane
        || type == GeomAbs_Sphere
        || type == GeomAbs_Cylinder
        || type == GeomAbs_Cone;
}

bool surface_type_is_spline(GeomAbs_SurfaceType type) {
    return type == GeomAbs_BSplineSurface || type == GeomAbs_BezierSurface;
}

bool surface_type_needs_emitter(GeomAbs_SurfaceType type) {
    return type == GeomAbs_Torus;
}

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

bool direction_is_axis_aligned(const gp_Dir& direction, double tolerance = 1.0e-10) {
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
    q.a = m00;
    q.b = m11;
    q.c = m22;
    q.d = 2.0 * m01;
    q.e = 2.0 * m12;
    q.f = 2.0 * m02;
    q.g = -2.0 * px;
    q.h = -2.0 * py;
    q.i = -2.0 * pz;
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
    q.a = m00;
    q.b = m11;
    q.c = m22;
    q.d = 2.0 * m01;
    q.e = 2.0 * m12;
    q.f = 2.0 * m02;
    q.g = -2.0 * px;
    q.h = -2.0 * py;
    q.i = -2.0 * pz;
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
        // Build a gp_Pln from origin + normal direction. OCCT needs a non-zero
        // direction; the fit guarantees this when available.
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
        // Encode as a general quadric via the existing cylinder_quadric path.
        gp_Pnt loc(r.fit.origin.x, r.fit.origin.y, r.fit.origin.z);
        gp_Dir dir(r.fit.direction.x, r.fit.direction.y, r.fit.direction.z);
        gp_Ax3 axes(loc, dir);
        gp_Cylinder cyl(axes, r.fit.radius);
        out->kind = AnalyticSurfaceKind::quadric;
        out->quadric = cylinder_quadric(cyl);
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
                                const std::vector<FaceReplacement>* replacements = nullptr) {
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

// Spline-surface analytic fits live in src/spline_fit.cpp.


std::vector<gp_Pnt> solid_vertices(const TopoDS_Solid& solid) {
    std::vector<gp_Pnt> points;
    for (TopExp_Explorer exp(solid, TopAbs_VERTEX); exp.More(); exp.Next()) {
        points.push_back(BRep_Tool::Pnt(TopoDS::Vertex(exp.Current())));
    }
    return points;
}

bool point_is_inside_or_on(const TopoDS_Solid& solid,
                           const gp_Pnt& point,
                           double tolerance) {
    BRepClass3d_SolidClassifier classifier(solid);
    classifier.Perform(point, tolerance);
    const TopAbs_State state = classifier.State();
    return state == TopAbs_IN || state == TopAbs_ON;
}

bool find_interior_point(const TopoDS_Solid& solid,
                         const GProp_GProps& props,
                         double tolerance,
                         gp_Pnt* point_out) {
    const gp_Pnt center = props.CentreOfMass();
    if (point_is_inside_or_on(solid, center, tolerance)) {
        *point_out = center;
        return true;
    }

    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (box.IsVoid()) {
        return false;
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    constexpr int steps = 5;
    for (int ix = 1; ix < steps; ++ix) {
        const double x = xmin + (xmax - xmin) * ix / steps;
        for (int iy = 1; iy < steps; ++iy) {
            const double y = ymin + (ymax - ymin) * iy / steps;
            for (int iz = 1; iz < steps; ++iz) {
                const double z = zmin + (zmax - zmin) * iz / steps;
                const gp_Pnt candidate(x, y, z);
                if (point_is_inside_or_on(solid, candidate, tolerance)) {
                    *point_out = candidate;
                    return true;
                }
            }
        }
    }

    return false;
}

bool validate_convex_halfspace_cell(const TopoDS_Solid& solid,
                                    const gp_Pnt& interior_point,
                                    std::size_t solid_index,
                                    const ConversionOptions& options,
                                    ConversionReport& report,
                                    bool emit_error,
                                    bool count_decomposition,
                                    bool count_faces,
                                    const std::vector<FaceReplacement>* replacements = nullptr) {
    const std::vector<gp_Pnt> vertices = solid_vertices(solid);
    if (vertices.empty()) {
        add_diag(report, DiagnosticSeverity::warning, "convexity_vertex_check_skipped",
                 "Solid has no vertices, so vertex-based convexity validation was skipped.",
                 solid_label(solid_index));
        return true;
    }

    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        if (count_faces) {
            ++report.faces_seen;
        }
        AnalyticSurface surface;
        if (!analytic_surface_from_face(TopoDS::Face(face_exp.Current()), solid_index,
                                        face_index, report, false, &surface,
                                        replacements)) {
            continue;
        }
        if (count_faces) {
            ++report.analytic_faces;
        }

        const double interior_value = surface.value_at(interior_point);
        const int sense = interior_value <= options.linear_tolerance ? -1 : 1;

        for (const gp_Pnt& vertex : vertices) {
            const double value = surface.value_at(vertex);
            const bool outside = sense < 0
                ? value > options.linear_tolerance
                : value < -options.linear_tolerance;
            if (outside) {
                if (count_decomposition) {
                    ++report.solids_requiring_decomposition;
                }
                if (emit_error) {
                    add_diag(report, DiagnosticSeverity::error,
                             "nonconvex_solid_requires_decomposition",
                             "At least one vertex lies outside a face halfspace selected from the solid interior point; emitting one Alea intersection cell would remove valid volume.",
                             solid_label(solid_index));
                }
                return false;
            }
        }
    }

    return true;
}

void inspect_solid(const TopoDS_Solid& solid,
                   std::size_t solid_index,
                   const ConversionOptions& options,
                   ConversionReport& report) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double volume = std::abs(props.Mass());
    const std::string label = solid_label(solid_index);
    AnalyzedSolid analyzed_solid;
    analyzed_solid.id = solid_id(solid_index);
    analyzed_solid.label = label;
    analyzed_solid.index = solid_index;
    analyzed_solid.volume = volume;
    analyzed_solid.bbox = bbox_from_shape(solid);

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
        analyzed_face.bbox = bbox_from_shape(face);

        GProp_GProps face_props;
        BRepGProp::SurfaceProperties(face, face_props);
        analyzed_face.area = std::abs(face_props.Mass());

        BRepAdaptor_Surface adaptor(face, false);
        const GeomAbs_SurfaceType surface_type = adaptor.GetType();
        AnalyticSurface surface;
        const bool exact_supported =
            analytic_surface_from_face(face, solid_index, face_index, report, true, &surface);
        if (exact_supported) {
            ++report.analytic_faces;
        }
        analyzed_face.surface_type = surface_type_name(surface_type);
        analyzed_face.exact_csg_supported = exact_supported;
        analyzed_face.needs_spline_replacement = surface_type_is_spline(surface_type);
        analyzed_face.needs_emitter =
            !exact_supported && surface_type_needs_emitter(surface_type);

        if (analyzed_face.needs_spline_replacement) {
            analyzed_face.spline_fits = detail::fit_spline_face(face);
            // Pick the best fit and append a hint to the last diagnostic for
            // this face, if any. We look back at the freshly-emitted
            // unsupported_spline_face diagnostic.
            const SplineFitCandidates& f = analyzed_face.spline_fits;
            const SplineFaceFit* best = nullptr;
            auto consider = [&](const SplineFaceFit& fit) {
                if (!fit.available) {
                    return;
                }
                if (!best || fit.relative_max_deviation < best->relative_max_deviation) {
                    best = &fit;
                }
            };
            consider(f.plane);
            consider(f.sphere);
            consider(f.cylinder);
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
        analyzed_solid.needs_user_decision =
            analyzed_solid.needs_user_decision
            || analyzed_face.needs_spline_replacement
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
                                           options, report, false, true, false)) {
        analyzed_solid.needs_decomposition = true;
        analyzed_solid.needs_user_decision = true;
        add_diag(report, DiagnosticSeverity::error,
                 "nonconvex_solid_requires_decomposition",
                 "At least one vertex lies outside a face halfspace selected from the solid interior point; emitting one Alea intersection cell would remove valid volume.",
                 label);
    }

    analyzed_solid.exact_csg_supported =
        !analyzed_solid.needs_decomposition
        && !analyzed_solid.needs_spline_replacement
        && !analyzed_solid.needs_user_decision;
    analyzed_solid.fitted_cylinder_replacement =
        fitted_cylinder_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_sphere_replacement =
        fitted_sphere_candidate(solid, analyzed_solid.volume);
    analyzed_solid.fitted_cone_replacement =
        fitted_cone_candidate(solid, analyzed_solid.volume);
    report.solids.push_back(analyzed_solid);
}

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
                                       options, scratch, false, false, false);
    analyzed.needs_decomposition = !convex;
    analyzed.exact_csg_supported =
        all_faces_supported
        && convex
        && !analyzed.needs_spline_replacement;
    analyzed.needs_user_decision =
        !analyzed.exact_csg_supported
        || analyzed.needs_decomposition
        || analyzed.needs_spline_replacement;
    return analyzed;
}

double solid_box_span(const TopoDS_Solid& solid) {
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (box.IsVoid()) {
        return 1.0;
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool find_solid_by_id(const CadModel& model,
                      const std::string& id,
                      TopoDS_Solid* solid_out,
                      std::size_t* index_out) {
    std::size_t target_index = 0;
    if (!parse_solid_id(id, &target_index)) {
        return false;
    }

    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        if (solid_index == target_index) {
            *solid_out = TopoDS::Solid(solid_exp.Current());
            *index_out = solid_index;
            return true;
        }
    }

    return false;
}

std::vector<TopoDS_Solid> split_solid_by_planar_faces(const TopoDS_Solid& solid,
                                                      const ConversionOptions& options,
                                                      ConversionReport& report,
                                                      std::size_t solid_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);

    NCollection_List<TopoDS_Shape> arguments;
    arguments.Append(solid);

    NCollection_List<TopoDS_Shape> tools;
    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        AnalyticSurface surface;
        if (!analytic_surface_from_face(TopoDS::Face(face_exp.Current()), solid_index,
                                        face_index, report, false, &surface)) {
            continue;
        }
        if (surface.kind != AnalyticSurfaceKind::plane) {
            continue;
        }

        TopoDS_Shape cutting_face =
            BRepBuilderAPI_MakeFace(surface.plane, -half_size, half_size,
                                    -half_size, half_size).Shape();
        if (!cutting_face.IsNull()) {
            tools.Append(cutting_face);
        }
    }

    std::vector<TopoDS_Solid> pieces;
    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_planar_tools",
                 "No planar cutting tools were available for this non-convex solid.",
                 solid_label(solid_index));
        return pieces;
    }

    BOPAlgo_Splitter splitter;
    splitter.SetArguments(arguments);
    splitter.SetTools(tools);
    splitter.SetFuzzyValue(options.linear_tolerance);
    splitter.Perform();
    if (splitter.HasErrors()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_splitter_failed",
                 "OCCT splitter failed while trying planar decomposition.",
                 solid_label(solid_index));
        return pieces;
    }

    const TopoDS_Shape result = splitter.Shape();
    for (TopExp_Explorer exp(result, TopAbs_SOLID); exp.More(); exp.Next()) {
        pieces.push_back(TopoDS::Solid(exp.Current()));
    }

    if (pieces.size() <= 1) {
        pieces.clear();
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_split",
                 "Planar decomposition did not produce multiple solid pieces.",
                 solid_label(solid_index));
    }

    return pieces;
}

bool add_face_halfspace(alea_system_t* sys,
                        const TopoDS_Face& face,
                        const gp_Pnt& interior_point,
                        std::size_t solid_index,
                        std::size_t face_index,
                        ConversionReport& report,
                        alea_node_id_t* node_out,
                        const std::vector<FaceReplacement>* replacements = nullptr) {
    const std::string label = face_label(solid_index, face_index);
    AnalyticSurface surface;

    int surface_index = -1;
    if (!analytic_surface_from_face(face, solid_index, face_index, report, true,
                                    &surface, replacements)) {
        return false;
    }

    switch (surface.kind) {
    case AnalyticSurfaceKind::plane: {
        const gp_Pln plane = surface.plane;
        const gp_XYZ n = plane.Axis().Direction().XYZ();
        const double d = -n.Dot(plane.Location().XYZ());
        surface_index = alea_plane_surface(sys, 0, n.X(), n.Y(), n.Z(), d);
        break;
    }
    case AnalyticSurfaceKind::sphere: {
        const gp_Sphere sphere = surface.sphere;
        surface_index = alea_sphere_surface(sys, 0,
                                            sphere.Location().X(),
                                            sphere.Location().Y(),
                                            sphere.Location().Z(),
                                            sphere.Radius());
        break;
    }
    case AnalyticSurfaceKind::quadric: {
        const QuadricCoefficients& q = surface.quadric;
        surface_index = alea_quadric_surface(sys, 0, q.a, q.b, q.c, q.d, q.e,
                                             q.f, q.g, q.h, q.i, q.j);
        break;
    }
    case AnalyticSurfaceKind::torus: {
        const gp_Torus torus = surface.torus;
        const gp_Pnt location = torus.Location();
        const gp_Dir direction = torus.Axis().Direction();
        if (std::abs(std::abs(direction.X()) - 1.0) <= 1.0e-10
            && std::abs(direction.Y()) <= 1.0e-10
            && std::abs(direction.Z()) <= 1.0e-10) {
            surface_index = alea_torus_x_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        } else if (std::abs(std::abs(direction.Y()) - 1.0) <= 1.0e-10
                   && std::abs(direction.X()) <= 1.0e-10
                   && std::abs(direction.Z()) <= 1.0e-10) {
            surface_index = alea_torus_y_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        } else if (std::abs(std::abs(direction.Z()) - 1.0) <= 1.0e-10
                   && std::abs(direction.X()) <= 1.0e-10
                   && std::abs(direction.Y()) <= 1.0e-10) {
            surface_index = alea_torus_z_surface(sys, 0,
                                                 location.X(), location.Y(), location.Z(),
                                                 torus.MajorRadius(), torus.MinorRadius());
        }
        break;
    }
    }

    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the surface.",
                 label);
        return false;
    }

    const double value = surface.value_at(interior_point);
    const int sense = value <= kEps ? -1 : 1;
    const alea_node_id_t node = alea_halfspace(sys, surface_index, sense);
    if (node == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid halfspace node.", label);
        return false;
    }

    *node_out = node;
    return true;
}

bool convert_solid(alea_system_t* sys,
                   const TopoDS_Solid& solid,
                   std::size_t solid_index,
                   const ConversionOptions& options,
                   ConversionReport& report,
                   int depth = 0,
                   const std::vector<FaceReplacement>* replacements = nullptr) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double volume = std::abs(props.Mass());
    if (depth == 0) {
        report.total_volume += volume;
        report.solid_volumes.push_back({solid_label(solid_index), volume});
    }

    if (volume <= options.linear_tolerance) {
        add_diag(report, DiagnosticSeverity::warning, "empty_or_tiny_solid",
                 "Solid has near-zero volume and was skipped.", solid_label(solid_index));
        return false;
    }

    gp_Pnt interior_point;
    if (!find_interior_point(solid, props, options.linear_tolerance, &interior_point)) {
        add_diag(report, DiagnosticSeverity::error, "interior_point_not_found",
                 "Could not find an interior point for selecting face halfspaces.",
                 solid_label(solid_index));
        return false;
    }
    std::vector<alea_node_id_t> nodes;
    std::size_t face_index = 0;

    if (options.require_convex_cells
        && !validate_convex_halfspace_cell(solid, interior_point, solid_index,
                                           options, report, false, depth == 0, true,
                                           replacements)) {
        if (options.try_planar_decomposition && depth == 0) {
            const std::vector<TopoDS_Solid> pieces =
                split_solid_by_planar_faces(solid, options, report, solid_index);
            std::size_t converted_before = report.solids_converted;
            for (const TopoDS_Solid& piece : pieces) {
                if (convert_solid(sys, piece, solid_index, options, report, depth + 1,
                                   replacements)) {
                    ++report.decomposition_pieces;
                }
            }
            if (report.solids_converted > converted_before) {
                return true;
            }
        }

        if (depth > 0) {
            return false;
        }

        add_diag(report, DiagnosticSeverity::error,
                 "nonconvex_solid_requires_decomposition",
                 "At least one vertex lies outside a face halfspace selected from the solid interior point; emitting one Alea intersection cell would remove valid volume.",
                 solid_label(solid_index));
        return false;
    }

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        alea_node_id_t node = ALEA_NODE_ID_INVALID;
        if (!add_face_halfspace(sys, TopoDS::Face(face_exp.Current()), interior_point,
                                solid_index, face_index, report, &node,
                                replacements)) {
            return false;
        }
        nodes.push_back(node);
    }

    if (nodes.empty()) {
        add_diag(report, DiagnosticSeverity::error, "solid_without_faces",
                 "Solid contains no faces.", solid_label(solid_index));
        return false;
    }

    const alea_node_id_t root = alea_intersection_n(sys, nodes.data(), nodes.size());
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the face intersection.",
                 solid_label(solid_index));
        return false;
    }

    int material_index = alea_find_material_by_id(sys, options.default_material_id);
    if (material_index < 0) {
        material_index = alea_add_material(sys, options.default_material_id);
    }
    if (material_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_material_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the default material.",
                 solid_label(solid_index));
        return false;
    }

    const int cell_index = alea_add_cell(sys, 0, root, material_index,
                                         options.default_density, 0);
    if (cell_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the converted solid cell.",
                 solid_label(solid_index));
        return false;
    }

    ++report.solids_converted;
    return true;
}

double bbox_volume(const BBox& bbox) {
    if (!bbox.valid) {
        return 0.0;
    }
    return std::abs((bbox.xmax - bbox.xmin)
        * (bbox.ymax - bbox.ymin)
        * (bbox.zmax - bbox.zmin));
}

double bbox_conservative_cylinder_volume(const BBox& bbox) {
    if (!bbox.valid) {
        return 0.0;
    }
    const double dx = bbox_dx(bbox);
    const double dy = bbox_dy(bbox);
    const double dz = bbox_dz(bbox);
    double height = dz;
    double radius = 0.5 * std::max(dx, dy);
    if (dx >= dy && dx >= dz) {
        height = dx;
        radius = 0.5 * std::max(dy, dz);
    } else if (dy >= dx && dy >= dz) {
        height = dy;
        radius = 0.5 * std::max(dx, dz);
    }
    constexpr double pi = 3.14159265358979323846264338327950288;
    return pi * radius * radius * height;
}

ReplacementMetrics replacement_metrics(double original_volume, double replacement_volume) {
    ReplacementMetrics metrics;
    if (replacement_volume <= 0.0) {
        return metrics;
    }
    metrics.available = true;
    metrics.volume = replacement_volume;
    metrics.absolute_delta = std::abs(replacement_volume - original_volume);
    if (std::abs(original_volume) > kEps) {
        metrics.relative_delta = metrics.absolute_delta / std::abs(original_volume);
        metrics.fill_ratio = original_volume / replacement_volume;
    }
    return metrics;
}

std::vector<Vec3> bbox_corners(const BBox& bbox) {
    if (!bbox.valid) {
        return {};
    }
    return {
        {bbox.xmin, bbox.ymin, bbox.zmin},
        {bbox.xmin, bbox.ymin, bbox.zmax},
        {bbox.xmin, bbox.ymax, bbox.zmin},
        {bbox.xmin, bbox.ymax, bbox.zmax},
        {bbox.xmax, bbox.ymin, bbox.zmin},
        {bbox.xmax, bbox.ymin, bbox.zmax},
        {bbox.xmax, bbox.ymax, bbox.zmin},
        {bbox.xmax, bbox.ymax, bbox.zmax},
    };
}

double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 scale(Vec3 v, double s) {
    return {v.x * s, v.y * s, v.z * s};
}

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

    constexpr double pi = 3.14159265358979323846264338327950288;
    const double radius = best_cylinder.Radius();
    const double replacement_volume = pi * radius * radius * length;
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

    constexpr double pi = 3.14159265358979323846264338327950288;
    const gp_Pnt location = best_sphere.Location();
    const double radius = best_sphere.Radius();
    const double replacement_volume = 4.0 * pi * radius * radius * radius / 3.0;
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

    constexpr double pi = 3.14159265358979323846264338327950288;
    const double replacement_volume = pi * length
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

double vec_len(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

bool nearly_zero(double value, double tolerance = 1.0e-10) {
    return std::abs(value) <= tolerance;
}

double synthetic_volume_estimate(const SyntheticBody& body) {
    constexpr double pi = 3.14159265358979323846264338327950288;
    switch (body.kind) {
    case SyntheticBodyKind::box:
        return bbox_volume(body.bbox);
    case SyntheticBodyKind::sphere:
        return 4.0 * pi * body.radius * body.radius * body.radius / 3.0;
    case SyntheticBodyKind::cylinder:
        return pi * body.radius * body.radius * vec_len(body.height);
    case SyntheticBodyKind::cone: {
        const double h = vec_len(body.height);
        return pi * h
            * (body.radius * body.radius
               + body.radius * body.top_radius
               + body.top_radius * body.top_radius) / 3.0;
    }
    case SyntheticBodyKind::union_body:
    case SyntheticBodyKind::intersection_body:
    case SyntheticBodyKind::difference_body:
        return 0.0;
    }
    return 0.0;
}

alea_node_id_t oriented_halfspace(alea_system_t* sys,
                                  int surface_index,
                                  double interior_value,
                                  const std::string& label,
                                  ConversionReport& report) {
    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the synthetic surface.",
                 label);
        return ALEA_NODE_ID_INVALID;
    }

    const int sense = interior_value <= kEps ? -1 : 1;
    const alea_node_id_t node = alea_halfspace(sys, surface_index, sense);
    if (node == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid synthetic halfspace.", label);
    }
    return node;
}

int axis_aligned_cone_surface(alea_system_t* sys,
                              Vec3 apex,
                              Vec3 axis,
                              double tan_angle_squared,
                              double tolerance) {
    if (std::abs(std::abs(axis.x) - 1.0) <= tolerance
        && nearly_zero(axis.y, tolerance)
        && nearly_zero(axis.z, tolerance)) {
        return alea_cone_x_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    if (std::abs(std::abs(axis.y) - 1.0) <= tolerance
        && nearly_zero(axis.x, tolerance)
        && nearly_zero(axis.z, tolerance)) {
        return alea_cone_y_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    if (std::abs(std::abs(axis.z) - 1.0) <= tolerance
        && nearly_zero(axis.x, tolerance)
        && nearly_zero(axis.y, tolerance)) {
        return alea_cone_z_surface(sys, 0, apex.x, apex.y, apex.z,
                                   tan_angle_squared);
    }
    return -1;
}

alea_node_id_t emit_synthetic_cone_node(alea_system_t* sys,
                                        const SyntheticBody& body,
                                        ConversionReport& report) {
    const double length = vec_len(body.height);
    if (length <= kEps || body.radius < 0.0 || body.top_radius < 0.0) {
        add_diag(report, DiagnosticSeverity::error, "invalid_synthetic_cone",
                 "Synthetic cone/frustum has invalid dimensions.", body.label);
        return ALEA_NODE_ID_INVALID;
    }

    if (std::abs(body.radius - body.top_radius) <= kEps) {
        const int surface_index = alea_rcc_surface(sys, 0,
                                                   body.base.x, body.base.y, body.base.z,
                                                   body.height.x, body.height.y, body.height.z,
                                                   body.radius);
        return oriented_halfspace(sys, surface_index, -1.0, body.label, report);
    }

    const Vec3 axis = scale(body.height, 1.0 / length);
    const Vec3 top = add(body.base, body.height);
    const Vec3 interior = add(body.base, scale(body.height, 0.5));
    const double radius_delta = body.top_radius - body.radius;
    const double apex_t = -body.radius * length / radius_delta;
    const Vec3 apex = add(body.base, scale(axis, apex_t));
    const double tan_angle_squared = (radius_delta * radius_delta) / (length * length);
    const QuadricCoefficients side = cone_quadric(apex, axis, tan_angle_squared);

    int side_surface = axis_aligned_cone_surface(sys, apex, axis,
                                                 tan_angle_squared, 1.0e-10);
    if (side_surface < 0) {
        side_surface = alea_quadric_surface(sys, 0,
                                            side.a, side.b, side.c,
                                            side.d, side.e, side.f,
                                            side.g, side.h, side.i, side.j);
    }
    const alea_node_id_t side_node =
        oriented_halfspace(sys, side_surface, quadric_value(side, {interior.x, interior.y, interior.z}),
                           body.label, report);
    if (side_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const double base_d = -dot(axis, body.base);
    const int base_surface = alea_plane_surface(sys, 0, axis.x, axis.y, axis.z, base_d);
    const double base_value = dot(axis, interior) + base_d;
    const alea_node_id_t base_node =
        oriented_halfspace(sys, base_surface, base_value, body.label, report);
    if (base_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const double top_d = -dot(axis, top);
    const int top_surface = alea_plane_surface(sys, 0, axis.x, axis.y, axis.z, top_d);
    const double top_value = dot(axis, interior) + top_d;
    const alea_node_id_t top_node =
        oriented_halfspace(sys, top_surface, top_value, body.label, report);
    if (top_node == ALEA_NODE_ID_INVALID) {
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t side_and_base = alea_intersection(sys, side_node, base_node);
    if (side_and_base == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 "Alea failed to build the synthetic cone side/base intersection.",
                 body.label);
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t root = alea_intersection(sys, side_and_base, top_node);
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_intersection_failed",
                 "Alea failed to build the synthetic cone cap intersection.",
                 body.label);
    }
    return root;
}

alea_node_id_t emit_synthetic_node(alea_system_t* sys,
                                   const CadModel& model,
                                   const SyntheticBody& body,
                                   ConversionReport& report) {
    int surface_index = -1;

    switch (body.kind) {
    case SyntheticBodyKind::box:
        if (!body.bbox.valid) {
            add_diag(report, DiagnosticSeverity::error, "invalid_synthetic_bbox",
                     "Synthetic box has no valid bounding box.", body.label);
            return ALEA_NODE_ID_INVALID;
        }
        surface_index = alea_box_surface(sys, 0,
                                         body.bbox.xmin, body.bbox.xmax,
                                         body.bbox.ymin, body.bbox.ymax,
                                         body.bbox.zmin, body.bbox.zmax);
        break;
    case SyntheticBodyKind::sphere:
        surface_index = alea_sphere_surface(sys, 0,
                                            body.center.x, body.center.y, body.center.z,
                                            body.radius);
        break;
    case SyntheticBodyKind::cylinder:
        surface_index = alea_rcc_surface(sys, 0,
                                         body.base.x, body.base.y, body.base.z,
                                         body.height.x, body.height.y, body.height.z,
                                         body.radius);
        break;
    case SyntheticBodyKind::cone:
        return emit_synthetic_cone_node(sys, body, report);
    case SyntheticBodyKind::union_body:
    case SyntheticBodyKind::intersection_body:
    case SyntheticBodyKind::difference_body: {
        const SyntheticBody* left = find_synthetic_body(model, body.left_id);
        const SyntheticBody* right = find_synthetic_body(model, body.right_id);
        if (!left || !right) {
            add_diag(report, DiagnosticSeverity::error, "synthetic_child_not_found",
                     "Synthetic CSG body references a missing child.", body.label);
            return ALEA_NODE_ID_INVALID;
        }
        const alea_node_id_t left_node = emit_synthetic_node(sys, model, *left, report);
        const alea_node_id_t right_node = emit_synthetic_node(sys, model, *right, report);
        if (left_node == ALEA_NODE_ID_INVALID || right_node == ALEA_NODE_ID_INVALID) {
            return ALEA_NODE_ID_INVALID;
        }
        if (body.kind == SyntheticBodyKind::union_body) {
            return alea_union(sys, left_node, right_node);
        }
        if (body.kind == SyntheticBodyKind::intersection_body) {
            return alea_intersection(sys, left_node, right_node);
        }
        return alea_difference(sys, left_node, right_node);
    }
    }

    if (surface_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_surface_failed",
                 alea_error()[0] ? alea_error() : "Alea rejected the synthetic body.",
                 body.label);
        return ALEA_NODE_ID_INVALID;
    }

    const alea_node_id_t root = alea_halfspace(sys, surface_index, -1);
    if (root == ALEA_NODE_ID_INVALID) {
        add_diag(report, DiagnosticSeverity::error, "alea_halfspace_failed",
                 "Alea did not return a valid synthetic body halfspace.", body.label);
        return ALEA_NODE_ID_INVALID;
    }
    return root;
}

bool emit_synthetic_body(alea_system_t* sys,
                         const CadModel& model,
                         const SyntheticBody& body,
                         ConversionReport& report) {
    const alea_node_id_t root = emit_synthetic_node(sys, model, body, report);
    if (root == ALEA_NODE_ID_INVALID) {
        return false;
    }

    int material_index = alea_find_material_by_id(sys, body.material_id);
    if (material_index < 0) {
        material_index = alea_add_material(sys, body.material_id);
    }
    if (material_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_material_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the synthetic body material.",
                 body.label);
        return false;
    }

    const int cell_index = alea_add_cell(sys, 0, root, material_index, body.density, 0);
    if (cell_index < 0) {
        add_diag(report, DiagnosticSeverity::error, "alea_cell_failed",
                 alea_error()[0] ? alea_error() : "Alea failed to add the synthetic box cell.",
                 body.label);
        return false;
    }

    const double volume = synthetic_volume_estimate(body);
    report.total_volume += volume;
    report.solid_volumes.push_back({body.label, volume});
    ++report.solids_seen;
    ++report.solids_converted;
    add_diag(report, DiagnosticSeverity::info, "synthetic_body_emitted",
             "Synthetic body was emitted as an Alea cell.", body.label);
    return true;
}

TopoDS_Shape read_step_shape(const std::string& step_path, ConversionReport& report) {
    STEPControl_Reader reader;
    const IFSelect_ReturnStatus status = reader.ReadFile(step_path.c_str());
    if (status != IFSelect_RetDone) {
        add_diag(report, DiagnosticSeverity::error, "step_read_failed",
                 "OCCT could not read the STEP file: " + step_path);
        return {};
    }

    const int roots = reader.NbRootsForTransfer();
    if (roots <= 0 || reader.TransferRoots() <= 0) {
        add_diag(report, DiagnosticSeverity::error, "step_transfer_failed",
                 "OCCT did not transfer any STEP roots.");
        return {};
    }

    return reader.OneShape();
}

TopoDS_Shape load_and_prepare_shape(const std::string& step_path,
                                    const ConversionOptions& options,
                                    ConversionReport& report) {
    TopoDS_Shape shape = read_step_shape(step_path, report);
    if (shape.IsNull()) {
        return {};
    }

    if (options.heal_shape) {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->Perform();
        shape = fixer->Shape();
    }

    BRepCheck_Analyzer analyzer(shape);
    if (!analyzer.IsValid()) {
        add_diag(report, DiagnosticSeverity::warning, "invalid_brep",
                 "OCCT reports that the transferred BRep is not fully valid; conversion will continue.");
    }

    return shape;
}

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

const char* spline_fit_kind_name(SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::none:
        return "none";
    case SplineFitKind::plane:
        return "plane";
    case SplineFitKind::sphere:
        return "sphere";
    case SplineFitKind::cylinder:
        return "cylinder";
    }
    return "none";
}

void write_spline_fit_json(std::ostream& out,
                           const SplineFaceFit& fit,
                           const std::string& indent) {
    out << "{\n";
    out << indent << "  \"kind\": \"" << spline_fit_kind_name(fit.kind) << "\",\n";
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
        << fit.radius << "\n";
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

const char* occt_version() {
    return OCC_VERSION_COMPLETE;
}

bool ConversionReport::ok() const {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (is_error(diagnostic)) {
            return false;
        }
    }
    return true;
}

AleaSystem::AleaSystem() : system_(alea_create()) {
    if (!system_) {
        throw std::runtime_error("alea_create failed");
    }
}

AleaSystem::~AleaSystem() {
    alea_destroy(system_);
}

AleaSystem::AleaSystem(AleaSystem&& other) noexcept : system_(other.system_) {
    other.system_ = nullptr;
}

AleaSystem& AleaSystem::operator=(AleaSystem&& other) noexcept {
    if (this != &other) {
        alea_destroy(system_);
        system_ = other.system_;
        other.system_ = nullptr;
    }
    return *this;
}

alea_system_t* AleaSystem::release() {
    alea_system_t* released = system_;
    system_ = nullptr;
    return released;
}

CadModel load_step_model(const std::string& step_path,
                         const ConversionOptions& options) {
    CadModel model;
    model.source_path = step_path;

    ConversionReport load_report;
    model.root = load_and_prepare_shape(step_path, options, load_report);
    model.diagnostics = load_report.diagnostics;
    return model;
}

AnalysisReport analyze_model(const CadModel& model,
                             const ConversionOptions& options) {
    AnalysisReport report;
    report.diagnostics = model.diagnostics;

    if (model.root.IsNull()) {
        return report;
    }

    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) {
            add_diag(report, DiagnosticSeverity::info, "solid_removed",
                     "Solid is suppressed by preprocessing.", solid_label(solid_index));
            continue;
        }
        ++report.solids_seen;
        inspect_solid(TopoDS::Solid(solid_exp.Current()), solid_index, options, report);
    }

    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) {
            continue;
        }
        AnalyzedSolid solid;
        solid.id = body.id;
        solid.label = body.label;
        solid.volume = synthetic_volume_estimate(body);
        solid.bbox = body.bbox;
        solid.exact_csg_supported = true;
        report.solids.push_back(solid);
        report.solid_volumes.push_back({body.label, solid.volume});
        report.total_volume += solid.volume;
        ++report.solids_seen;
    }

    std::size_t piece_index = 0;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        ++piece_index;
        const std::size_t report_solid_index = 100000 + piece_index;
        AnalyzedDecomposedPiece analyzed_piece =
            analyze_decomposed_piece(piece, piece_index, report_solid_index, options);
        report.decomposed_pieces.push_back(analyzed_piece);
        report.solid_volumes.push_back({piece.label, piece.volume});
        report.total_volume += piece.volume;
        ++report.solids_seen;
        ++report.decomposition_pieces;
        if (analyzed_piece.needs_decomposition) {
            ++report.solids_requiring_decomposition;
        }
    }

    finalize_cleanup_metrics(report, options);

    if (report.solids_seen == 0) {
        add_diag(report, DiagnosticSeverity::error, "no_active_solids",
                 "No active solids remain after import and preprocessing.");
    }

    return report;
}

bool remove_solid(CadModel& model, const std::string& id) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    if (!contains_id(model.removed_solid_ids, id)) {
        model.removed_solid_ids.push_back(id);
    }
    return true;
}

namespace {

const SplineFaceFit* select_fit(const SplineFitCandidates& fits, SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::plane:
        return &fits.plane;
    case SplineFitKind::sphere:
        return &fits.sphere;
    case SplineFitKind::cylinder:
        return &fits.cylinder;
    case SplineFitKind::none:
        return nullptr;
    }
    return nullptr;
}

bool find_face_in_root(const TopoDS_Shape& root,
                       std::size_t target_solid_index,
                       std::size_t target_face_index,
                       TopoDS_Face* face_out) {
    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        if (solid_index != target_solid_index) {
            continue;
        }
        std::size_t face_index = 0;
        for (TopExp_Explorer face_exp(solid_exp.Current(), TopAbs_FACE);
             face_exp.More(); face_exp.Next()) {
            ++face_index;
            if (face_index == target_face_index) {
                *face_out = TopoDS::Face(face_exp.Current());
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace

bool replace_face_with_fit(CadModel& model,
                           const std::string& face_id_str,
                           SplineFitKind kind,
                           bool has_max_deviation,
                           double max_deviation,
                           Diagnostic* diagnostic_out,
                           const ConversionOptions& /*options*/) {
    auto emit = [&](DiagnosticSeverity sev, const char* code, const std::string& msg) {
        Diagnostic d{sev, code, msg, face_id_str};
        if (diagnostic_out) {
            *diagnostic_out = d;
        }
        model.diagnostics.push_back(d);
    };

    std::size_t solid_index = 0;
    std::size_t face_index = 0;
    if (!parse_face_id(face_id_str, &solid_index, &face_index)) {
        emit(DiagnosticSeverity::error, "invalid_face_id",
             "Face ID must look like face:N.M with positive N and M.");
        return false;
    }

    TopoDS_Face face;
    if (!find_face_in_root(model.root, solid_index, face_index, &face)) {
        emit(DiagnosticSeverity::error, "face_not_found",
             "Requested face ID was not found in the loaded model.");
        return false;
    }

    BRepAdaptor_Surface adaptor(face, false);
    const GeomAbs_SurfaceType type = adaptor.GetType();
    if (!surface_type_is_spline(type)) {
        emit(DiagnosticSeverity::error, "face_not_spline",
             "Face replacement is only defined for B-spline or Bezier faces.");
        return false;
    }

    const SplineFitCandidates fits = detail::fit_spline_face(face);
    const SplineFaceFit* selected = select_fit(fits, kind);
    if (!selected || !selected->available) {
        emit(DiagnosticSeverity::error, "fit_unavailable",
             "Selected analytic fit is not available for this face.");
        return false;
    }
    if (has_max_deviation && selected->max_deviation > max_deviation) {
        std::ostringstream msg;
        msg << "Fit max_deviation=" << selected->max_deviation
            << " exceeds budget " << max_deviation << ".";
        emit(DiagnosticSeverity::error, "fit_deviation_exceeded", msg.str());
        return false;
    }

    // Remove any prior replacement for the same face, then record the new one.
    for (auto it = model.face_replacements.begin();
         it != model.face_replacements.end();) {
        if (it->face_id == face_id_str) {
            it = model.face_replacements.erase(it);
        } else {
            ++it;
        }
    }
    FaceReplacement record;
    record.face_id = face_id_str;
    record.kind = kind;
    record.fit = *selected;
    record.has_max_deviation = has_max_deviation;
    record.max_deviation = max_deviation;
    model.face_replacements.push_back(record);

    std::ostringstream info;
    info << "Face replaced with " << spline_fit_kind_name(kind)
         << " (max_deviation=" << selected->max_deviation
         << ", relative=" << selected->relative_max_deviation << ").";
    emit(DiagnosticSeverity::info, "face_replacement_recorded", info.str());
    return true;
}

bool decompose_solid_by_planar_faces(CadModel& model,
                                     const std::string& id,
                                     const ConversionOptions& options) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    ConversionReport decomposition_report;
    std::vector<TopoDS_Solid> pieces =
        split_solid_by_planar_faces(solid, options, decomposition_report, index);
    for (const Diagnostic& diagnostic : decomposition_report.diagnostics) {
        model.diagnostics.push_back(diagnostic);
    }
    if (pieces.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "planar_decomposition_failed",
            "Planar decomposition did not produce usable pieces.",
            id
        });
        return false;
    }

    GProp_GProps original_props;
    BRepGProp::VolumeProperties(solid, original_props);
    const double original_volume = std::abs(original_props.Mass());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);
        pieces_volume += std::abs(piece_props.Mass());
    }

    const double absolute_delta = std::abs(pieces_volume - original_volume);
    double relative_delta = 0.0;
    if (original_volume > kEps) {
        relative_delta = absolute_delta / original_volume;
    }

    std::ostringstream msg;
    msg << "Planar decomposition produced " << pieces.size()
        << " pieces. original_volume=" << std::setprecision(10) << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= 1.0e-6 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
        "planar_decomposition",
        msg.str(),
        id
    });

    remove_solid(model, id);
    for (const TopoDS_Solid& piece : pieces) {
        GProp_GProps piece_props;
        BRepGProp::VolumeProperties(piece, piece_props);

        DecomposedSolidPiece decomposed;
        decomposed.id = next_decomposed_piece_id(model);
        decomposed.label = decomposed.id + "/from_" + id;
        decomposed.source_solid_id = id;
        decomposed.solid = piece;
        decomposed.volume = std::abs(piece_props.Mass());
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

std::string add_synthetic_box(CadModel& model,
                              const BBox& bbox,
                              int material_id,
                              double density,
                              const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/box" : label;
    body.kind = SyntheticBodyKind::box;
    body.bbox = bbox;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_sphere(CadModel& model,
                                 Vec3 center,
                                 double radius,
                                 int material_id,
                                 double density,
                                 const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/sphere" : label;
    body.kind = SyntheticBodyKind::sphere;
    body.center = center;
    body.radius = radius;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_cylinder(CadModel& model,
                                   Vec3 base,
                                   Vec3 height,
                                   double radius,
                                   int material_id,
                                   double density,
                                   const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/cylinder" : label;
    body.kind = SyntheticBodyKind::cylinder;
    body.base = base;
    body.height = height;
    body.radius = radius;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_cone(CadModel& model,
                               Vec3 base,
                               Vec3 height,
                               double base_radius,
                               double top_radius,
                               int material_id,
                               double density,
                               const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/cone" : label;
    body.kind = SyntheticBodyKind::cone;
    body.base = base;
    body.height = height;
    body.radius = base_radius;
    body.top_radius = top_radius;
    body.material_id = material_id;
    body.density = density;
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_union(CadModel& model,
                                const std::string& left_id,
                                const std::string& right_id,
                                int material_id,
                                double density,
                                const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/union" : label;
    body.kind = SyntheticBodyKind::union_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_intersection(CadModel& model,
                                       const std::string& left_id,
                                       const std::string& right_id,
                                       int material_id,
                                       double density,
                                       const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/intersection" : label;
    body.kind = SyntheticBodyKind::intersection_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

std::string add_synthetic_difference(CadModel& model,
                                     const std::string& left_id,
                                     const std::string& right_id,
                                     int material_id,
                                     double density,
                                     const std::string& label) {
    SyntheticBody body;
    body.id = next_synthetic_id(model);
    body.label = label.empty() ? body.id + "/difference" : label;
    body.kind = SyntheticBodyKind::difference_body;
    body.left_id = left_id;
    body.right_id = right_id;
    body.material_id = material_id;
    body.density = density;
    if (SyntheticBody* left = find_synthetic_body_mut(model, left_id)) {
        left->emit_as_cell = false;
    }
    if (SyntheticBody* right = find_synthetic_body_mut(model, right_id)) {
        right->emit_as_cell = false;
    }
    model.synthetic_bodies.push_back(body);
    return model.synthetic_bodies.back().id;
}

ReplacementMetrics replacement_metrics_for_operation(const AnalyzedSolid& solid,
                                                     ReplacementOperation operation) {
    switch (operation) {
    case ReplacementOperation::bbox:
        return solid.bbox_replacement;
    case ReplacementOperation::bbox_cylinder:
        return solid.bbox_cylinder_replacement;
    case ReplacementOperation::fitted_cylinder:
        return solid.fitted_cylinder_replacement.metrics;
    case ReplacementOperation::fitted_sphere:
        return solid.fitted_sphere_replacement.metrics;
    case ReplacementOperation::fitted_cone:
        return solid.fitted_cone_replacement.metrics;
    }
    return {};
}

ReplacementCheck check_replacement_limits(const CadModel& model,
                                          const std::string& solid_id,
                                          ReplacementOperation operation,
                                          const ReplacementLimits& limits,
                                          const ConversionOptions& options) {
    ReplacementCheck check;
    if (!limits.has_absolute_delta && !limits.has_relative_delta) {
        check.ok = true;
        return check;
    }

    const AnalysisReport report = analyze_model(model, options);
    const AnalyzedSolid* analyzed_solid = nullptr;
    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.id == solid_id) {
            analyzed_solid = &solid;
            break;
        }
    }

    if (!analyzed_solid) {
        check.diagnostic = {
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            solid_id
        };
        return check;
    }

    check.metrics = replacement_metrics_for_operation(*analyzed_solid, operation);
    if (!check.metrics.available) {
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_metrics_unavailable",
            "No replacement metrics are available for the requested operation.",
            solid_id
        };
        return check;
    }

    if (limits.has_absolute_delta
        && check.metrics.absolute_delta > limits.max_absolute_delta) {
        std::ostringstream msg;
        msg << "Replacement absolute volume delta exceeds limit. absolute_delta="
            << std::setprecision(10) << check.metrics.absolute_delta
            << " max_absolute_delta=" << limits.max_absolute_delta;
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_abs_delta_exceeded",
            msg.str(),
            solid_id
        };
        return check;
    }

    if (limits.has_relative_delta
        && check.metrics.relative_delta > limits.max_relative_delta) {
        std::ostringstream msg;
        msg << "Replacement relative volume delta exceeds limit. relative_delta="
            << std::setprecision(10) << check.metrics.relative_delta
            << " max_relative_delta=" << limits.max_relative_delta;
        check.diagnostic = {
            DiagnosticSeverity::error,
            "replacement_rel_delta_exceeded",
            msg.str(),
            solid_id
        };
        return check;
    }

    check.ok = true;
    return check;
}

bool replace_solid_with_bbox(CadModel& model,
                             const std::string& id,
                             const ConversionOptions& options) {
    (void)options;

    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_solid_bbox",
            "Could not build a valid bounding box replacement for the requested solid.",
            id
        });
        return false;
    }

    remove_solid(model, id);
    add_synthetic_box(model, bbox, options.default_material_id, options.default_density,
                      next_synthetic_id(model) + "/bbox_from_" + id);
    return true;
}

bool replace_solid_with_bbox_cylinder(CadModel& model,
                                      const std::string& id,
                                      const ConversionOptions& options) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    BBox bbox = bbox_from_shape(solid);
    if (!bbox.valid) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_solid_bbox",
            "Could not build a valid bounding box cylinder replacement for the requested solid.",
            id
        });
        return false;
    }

    const double dx = bbox_dx(bbox);
    const double dy = bbox_dy(bbox);
    const double dz = bbox_dz(bbox);
    const double cx = 0.5 * (bbox.xmin + bbox.xmax);
    const double cy = 0.5 * (bbox.ymin + bbox.ymax);
    const double cz = 0.5 * (bbox.zmin + bbox.zmax);

    Vec3 base;
    Vec3 height;
    double radius = 0.0;

    if (dx >= dy && dx >= dz) {
        base = {bbox.xmin, cy, cz};
        height = {dx, 0.0, 0.0};
        radius = 0.5 * std::max(dy, dz);
    } else if (dy >= dx && dy >= dz) {
        base = {cx, bbox.ymin, cz};
        height = {0.0, dy, 0.0};
        radius = 0.5 * std::max(dx, dz);
    } else {
        base = {cx, cy, bbox.zmin};
        height = {0.0, 0.0, dz};
        radius = 0.5 * std::max(dx, dy);
    }

    if (vec_len(height) <= options.linear_tolerance || radius <= options.linear_tolerance) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "invalid_bbox_cylinder",
            "Bounding box is too small or degenerate for a cylinder replacement.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const double replacement_volume =
        3.14159265358979323846264338327950288 * radius * radius * vec_len(height);
    std::ostringstream msg;
    msg << "Replacing solid with conservative bbox cylinder. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << replacement_volume;
    if (original_volume > kEps) {
        msg << " relative_delta="
            << std::abs(replacement_volume - original_volume) / original_volume;
    }
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "bbox_cylinder_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cylinder(model, base, height, radius,
                           options.default_material_id, options.default_density,
                           next_synthetic_id(model) + "/bbox_cylinder_from_" + id);
    return true;
}

bool replace_solid_with_fitted_cylinder(CadModel& model,
                                        const std::string& id,
                                        const ConversionOptions& options) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedCylinderCandidate candidate =
        fitted_cylinder_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_cylinder_unavailable",
            "No usable cylindrical face was found for fitted cylinder replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted cylinder. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_cylinder_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cylinder(model, candidate.base, candidate.height, candidate.radius,
                           options.default_material_id, options.default_density,
                           next_synthetic_id(model) + "/fitted_cylinder_from_" + id);
    return true;
}

bool replace_solid_with_fitted_sphere(CadModel& model,
                                      const std::string& id,
                                      const ConversionOptions& options) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedSphereCandidate candidate =
        fitted_sphere_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_sphere_unavailable",
            "No usable spherical face was found for fitted sphere replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted sphere. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_sphere_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_sphere(model, candidate.center, candidate.radius,
                         options.default_material_id, options.default_density,
                         next_synthetic_id(model) + "/fitted_sphere_from_" + id);
    return true;
}

bool replace_solid_with_fitted_cone(CadModel& model,
                                    const std::string& id,
                                    const ConversionOptions& options) {
    TopoDS_Solid solid;
    std::size_t index = 0;
    if (!find_solid_by_id(model, id, &solid, &index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "solid_not_found",
            "Requested solid ID was not found.",
            id
        });
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double original_volume = std::abs(props.Mass());
    const FittedConeCandidate candidate =
        fitted_cone_candidate(solid, original_volume);
    if (!candidate.metrics.available) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error,
            "fitted_cone_unavailable",
            "No usable conical face was found for fitted cone replacement.",
            id
        });
        return false;
    }

    std::ostringstream msg;
    msg << "Replacing solid with fitted cone. original_volume="
        << std::setprecision(10) << original_volume
        << " replacement_volume=" << candidate.metrics.volume
        << " relative_delta=" << candidate.metrics.relative_delta
        << " confidence=" << candidate.confidence;
    model.diagnostics.push_back({
        DiagnosticSeverity::info,
        "fitted_cone_replacement",
        msg.str(),
        id
    });

    remove_solid(model, id);
    add_synthetic_cone(model, candidate.base, candidate.height,
                       candidate.base_radius, candidate.top_radius,
                       options.default_material_id, options.default_density,
                       next_synthetic_id(model) + "/fitted_cone_from_" + id);
    return true;
}

bool apply_replacement_operation(CadModel& model,
                                 const std::string& solid_id,
                                 ReplacementOperation operation,
                                 const ConversionOptions& options) {
    switch (operation) {
    case ReplacementOperation::bbox:
        return replace_solid_with_bbox(model, solid_id, options);
    case ReplacementOperation::bbox_cylinder:
        return replace_solid_with_bbox_cylinder(model, solid_id, options);
    case ReplacementOperation::fitted_cylinder:
        return replace_solid_with_fitted_cylinder(model, solid_id, options);
    case ReplacementOperation::fitted_sphere:
        return replace_solid_with_fitted_sphere(model, solid_id, options);
    case ReplacementOperation::fitted_cone:
        return replace_solid_with_fitted_cone(model, solid_id, options);
    }
    return false;
}

bool apply_checked_replacement_operation(CadModel& model,
                                         const std::string& solid_id,
                                         ReplacementOperation operation,
                                         const ReplacementLimits& limits,
                                         ReplacementCheck* check_out,
                                         const ConversionOptions& options) {
    ReplacementCheck check =
        check_replacement_limits(model, solid_id, operation, limits, options);
    if (check_out) {
        *check_out = check;
    }
    if (!check.ok) {
        if (!check.diagnostic.code.empty()) {
            model.diagnostics.push_back(check.diagnostic);
        }
        return false;
    }
    return apply_replacement_operation(model, solid_id, operation, options);
}

ConversionReport inspect_step_file(const std::string& step_path,
                                   const ConversionOptions& options) {
    CadModel model = load_step_model(step_path, options);
    return analyze_model(model, options);
}

ConversionResult convert_model(const CadModel& model,
                               const ConversionOptions& options) {
    ConversionResult result;
    result.report.diagnostics = model.diagnostics;
    if (model.root.IsNull()) {
        return result;
    }

    std::size_t solid_index = 0;
    for (TopExp_Explorer solid_exp(model.root, TopAbs_SOLID); solid_exp.More(); solid_exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (contains_id(model.removed_solid_ids, id)) {
            add_diag(result.report, DiagnosticSeverity::info, "solid_removed",
                     "Solid is suppressed by preprocessing.", solid_label(solid_index));
            continue;
        }
        ++result.report.solids_seen;
        convert_solid(result.system.get(), TopoDS::Solid(solid_exp.Current()),
                      solid_index, options, result.report, 0,
                      &model.face_replacements);
    }

    std::size_t piece_index = 0;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        ++piece_index;
        ++result.report.solids_seen;
        ++result.report.decomposition_pieces;
        result.report.decomposed_pieces.push_back(
            analyze_decomposed_piece(piece, piece_index, 100000 + piece_index, options));
        add_diag(result.report, DiagnosticSeverity::info, "decomposed_piece_emitted",
                 "Decomposed solid piece is being converted as an explicit preprocessing result.",
                 piece.label);
        convert_solid(result.system.get(), piece.solid,
                      100000 + piece_index, options, result.report);
    }

    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) {
            continue;
        }
        emit_synthetic_body(result.system.get(), model, body, result.report);
    }

    if (result.report.solids_seen == 0) {
        add_diag(result.report, DiagnosticSeverity::error, "no_active_solids",
                 "No active solids remain after import and preprocessing.");
        return result;
    }

    if (result.report.solids_converted > 0) {
        if (options.split_union_cells) {
            alea_split_union_cells(result.system.get());
        }
        alea_flatten_all_cells(result.system.get(), nullptr);
        if (alea_build_universe_index(result.system.get()) != 0) {
            add_diag(result.report, DiagnosticSeverity::error, "alea_index_failed",
                     alea_error()[0] ? alea_error() : "Alea failed to build the universe index.");
        }
    }

    return result;
}

ConversionResult convert_step_file(const std::string& step_path,
                                   const ConversionOptions& options) {
    CadModel model = load_step_model(step_path, options);
    return convert_model(model, options);
}

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

VolumeComparison compare_step_volumes(const std::string& original_path,
                                      const std::string& candidate_path,
                                      double absolute_tolerance,
                                      double relative_tolerance,
                                      const ConversionOptions& options) {
    VolumeComparison comparison;
    comparison.absolute_tolerance = absolute_tolerance;
    comparison.relative_tolerance = relative_tolerance;
    comparison.original = inspect_step_file(original_path, options);
    comparison.candidate = inspect_step_file(candidate_path, options);
    comparison.absolute_delta =
        std::abs(comparison.candidate.total_volume - comparison.original.total_volume);
    if (std::abs(comparison.original.total_volume) > kEps) {
        comparison.relative_delta = comparison.absolute_delta
            / std::abs(comparison.original.total_volume);
    } else {
        comparison.relative_delta = comparison.absolute_delta > kEps ? 1.0 : 0.0;
    }

    const bool abs_ok = absolute_tolerance <= 0.0
        || comparison.absolute_delta <= absolute_tolerance;
    const bool rel_ok = relative_tolerance <= 0.0
        || comparison.relative_delta <= relative_tolerance;
    comparison.within_tolerance = abs_ok && rel_ok;
    return comparison;
}

int export_mcnp(alea_system_t* system, const std::string& output_path) {
    return mcnp_export_system(system, output_path.c_str());
}

} // namespace cad2csg
