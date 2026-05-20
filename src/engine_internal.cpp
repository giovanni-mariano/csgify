#include "engine_internal.hpp"

#include "analytic_surface.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Bnd_Box.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

namespace cad2csg::detail {

void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label) {
    report.diagnostics.push_back({severity, code, message, label});
}

bool is_error(const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::error;
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
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    *min_e = a;
    *mid_e = b;
    *max_e = c;
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
    double xmin = 0.0, ymin = 0.0, zmin = 0.0;
    double xmax = 0.0, ymax = 0.0, zmax = 0.0;
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
                                    const std::vector<FaceReplacement>* replacements) {
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

double vec_len(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 sub(Vec3 a, Vec3 b)   { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b)   { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(Vec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }

bool nearly_zero(double value, double tolerance) {
    return std::abs(value) <= tolerance;
}

bool contains_id(const std::vector<std::string>& ids, const std::string& id) {
    for (const std::string& candidate : ids) {
        if (candidate == id) {
            return true;
        }
    }
    return false;
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

} // namespace cad2csg::detail
