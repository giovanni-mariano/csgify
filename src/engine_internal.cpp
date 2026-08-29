#include "engine_internal.hpp"

#include "analytic_surface.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <algorithm>
#include <array>

#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Vec.hxx>

namespace csgify::detail {
namespace {

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

bool looks_like_shape_id(const std::string& value) {
    return value.compare(0, 6, "solid:") == 0
        || value.compare(0, 5, "face:") == 0
        || value.compare(0, 6, "piece:") == 0
        || value.compare(0, 10, "synthetic:") == 0;
}

} // namespace

void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label) {
    Diagnostic diagnostic{severity, code, message, label};
    if (looks_like_shape_id(label)) {
        diagnostic.shape_id = label;
    } else {
        diagnostic.shape_id = shape_id_from_label(label);
    }
    report.diagnostics.push_back(diagnostic);
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
    // 1. Centre of mass -- the common case for convex / near-convex solids.
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
    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;

    // 2. Inset from each face centroid along the inward normal. This is far
    //    more reliable than a coarse grid for thin wedges and awkward
    //    BOPAlgo-produced fragments: a face centroid is guaranteed on the
    //    boundary, and a short step inward lands in the interior even when
    //    the centre of mass falls outside a non-convex piece. We step by
    //    fractions of the *smallest* nonzero bbox extent (the dimension that
    //    governs how far we can go before exiting a thin piece), largest step
    //    first so the returned point is as deep inside as possible -- and
    //    therefore unambiguous for halfspace-sense selection.
    std::array<double, 3> extents = {dx, dy, dz};
    double min_ext = 0.0;
    for (double e : extents) {
        if (e > kEps && (min_ext == 0.0 || e < min_ext)) {
            min_ext = e;
        }
    }
    const double max_ext = std::max({dx, dy, dz, 1.0});
    const double base = min_ext > kEps ? min_ext : max_ext;
    const std::array<double, 5> fracs = {0.5, 0.25, 0.1, 0.05, 0.02};

    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());
        BRepGProp_Face face_prop(face);
        double u1 = 0.0, u2 = 0.0, v1 = 0.0, v2 = 0.0;
        face_prop.Bounds(u1, u2, v1, v2);
        if (u2 < u1 || v2 < v1) {
            continue;
        }
        gp_Pnt face_point;
        gp_Vec normal;
        face_prop.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), face_point, normal);
        if (normal.Magnitude() <= kEps) {
            continue;
        }
        normal.Normalize();
        for (double frac : fracs) {
            const double step = base * frac;
            // Try both senses; one points into the solid.
            for (int sign : {-1, 1}) {
                const gp_Pnt candidate =
                    face_point.Translated(normal.Multiplied(sign * step));
                if (point_is_inside_or_on(solid, candidate, tolerance)) {
                    *point_out = candidate;
                    return true;
                }
            }
        }
    }

    // 3. Bbox grid fallback, coarse then finer, for shapes the face inset
    //    missed (e.g. a face whose centroid lies outside the solid).
    for (int steps : {5, 9}) {
        for (int ix = 1; ix < steps; ++ix) {
            const double x = xmin + dx * ix / steps;
            for (int iy = 1; iy < steps; ++iy) {
                const double y = ymin + dy * iy / steps;
                for (int iz = 1; iz < steps; ++iz) {
                    const double z = zmin + dz * iz / steps;
                    const gp_Pnt candidate(x, y, z);
                    if (point_is_inside_or_on(solid, candidate, tolerance)) {
                        *point_out = candidate;
                        return true;
                    }
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
        // Use a geometric (distance-normalized) value so the comparison
        // against linear_tolerance is dimensionally correct for every surface
        // kind -- raw value_at() returns length^2 for quadrics/spheres and
        // length^4 for tori, which made the check spuriously fail on
        // large-radius faces and triggered cuts that could not split.
        const double interior_value = surface.signed_distance_at(interior_point);
        const int sense = interior_value <= options.linear_tolerance ? -1 : 1;
        for (const gp_Pnt& vertex : vertices) {
            const double value = surface.signed_distance_at(vertex);
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

bool find_convexity_failure_face(const TopoDS_Solid& solid,
                                 std::size_t solid_index,
                                 const ConversionOptions& options,
                                 std::size_t* face_index_out) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    gp_Pnt interior_point;
    if (!find_interior_point(solid, props, options.linear_tolerance, &interior_point)) {
        return false;
    }

    const std::vector<gp_Pnt> vertices = solid_vertices(solid);
    if (vertices.empty()) {
        return false;
    }

    std::size_t face_index = 0;
    ConversionReport scratch;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        AnalyticSurface surface;
        if (!analytic_surface_from_face(TopoDS::Face(face_exp.Current()),
                                        solid_index,
                                        face_index,
                                        scratch,
                                        false,
                                        &surface)) {
            continue;
        }
        // Use a geometric (distance-normalized) value so the comparison
        // against linear_tolerance is dimensionally correct for every surface
        // kind -- raw value_at() returns length^2 for quadrics/spheres and
        // length^4 for tori, which made the check spuriously fail on
        // large-radius faces and triggered cuts that could not split.
        const double interior_value = surface.signed_distance_at(interior_point);
        const int sense = interior_value <= options.linear_tolerance ? -1 : 1;
        for (const gp_Pnt& vertex : vertices) {
            const double value = surface.signed_distance_at(vertex);
            const bool outside = sense < 0
                ? value > options.linear_tolerance
                : value < -options.linear_tolerance;
            if (outside) {
                *face_index_out = face_index;
                return true;
            }
        }
    }
    return false;
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

} // namespace csgify::detail
