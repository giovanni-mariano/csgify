#include "topology_analysis.hpp"

#include "engine_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopAbs_State.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <gp_Dir.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Cone.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Vec.hxx>

namespace csgify::detail {
namespace {

struct CanonicalPlane {
    gp_Dir normal;
    double offset = 0.0;
};

struct CylinderFace {
    gp_Pnt axis_origin;
    gp_Dir axis_direction;
    double radius = 0.0;
    bool full_sweep = false;
};

struct ConeFace {
    gp_Pnt axis_origin;
    gp_Dir axis_direction;
    double reference_level = 0.0;
    double reference_radius = 0.0;
    double radial_slope = 0.0;
    bool full_sweep = false;
};

struct AxialSurfaceFace {
    GeomAbs_SurfaceType type = GeomAbs_OtherSurface;
    gp_Pnt axis_origin;
    gp_Dir axis_direction;
    double angular_extent = 0.0;
    bool full_sweep = false;
};

gp_Dir canonical_direction(gp_Dir direction) {
    const bool flip = direction.X() < -1.0e-12
        || (std::abs(direction.X()) <= 1.0e-12
            && direction.Y() < -1.0e-12)
        || (std::abs(direction.X()) <= 1.0e-12
            && std::abs(direction.Y()) <= 1.0e-12
            && direction.Z() < 0.0);
    if (flip) direction.Reverse();
    return direction;
}

gp_Pnt canonical_axis_origin(const gp_Pnt& point, const gp_Dir& direction) {
    const gp_Vec from_global_origin(gp_Pnt(0.0, 0.0, 0.0), point);
    const double along = from_global_origin.Dot(gp_Vec(direction));
    return point.Translated(gp_Vec(direction).Multiplied(-along));
}

bool coaxial(const CylinderFace& a, const CylinderFace& b,
             double linear_tol, double angular_tol) {
    if (std::abs(a.axis_direction.Dot(b.axis_direction))
        < 1.0 - std::max(angular_tol, 1.0e-9)) {
        return false;
    }
    const gp_Vec between(a.axis_origin, b.axis_origin);
    const double distance = between.Crossed(gp_Vec(a.axis_direction)).Magnitude();
    return distance <= std::max(linear_tol * 10.0, 1.0e-6);
}

bool coaxial(const AxialSurfaceFace& a, const AxialSurfaceFace& b,
             double linear_tol, double angular_tol) {
    if (std::abs(a.axis_direction.Dot(b.axis_direction))
        < 1.0 - std::max(angular_tol, 1.0e-9)) {
        return false;
    }
    const gp_Vec between(a.axis_origin, b.axis_origin);
    const double distance = between.Crossed(gp_Vec(a.axis_direction)).Magnitude();
    return distance <= std::max(linear_tol * 10.0, 1.0e-6);
}

CanonicalPlane canonical_plane(const gp_Pln& plane) {
    gp_Dir n = plane.Axis().Direction();
    const bool flip = n.X() < -1.0e-12
        || (std::abs(n.X()) <= 1.0e-12 && n.Y() < -1.0e-12)
        || (std::abs(n.X()) <= 1.0e-12
            && std::abs(n.Y()) <= 1.0e-12 && n.Z() < 0.0);
    if (flip) n.Reverse();
    const gp_Pnt p = plane.Location();
    return {n, p.X() * n.X() + p.Y() * n.Y() + p.Z() * n.Z()};
}

bool same_direction(const gp_Dir& a, const gp_Dir& b, double angular_tol) {
    return std::abs(a.Dot(b)) >= 1.0 - std::max(angular_tol, 1.0e-9);
}

bool same_plane(const CanonicalPlane& a, const CanonicalPlane& b,
                double linear_tol, double angular_tol) {
    return a.normal.Dot(b.normal) >= 1.0 - std::max(angular_tol, 1.0e-9)
        && std::abs(a.offset - b.offset) <= std::max(linear_tol * 10.0, 1.0e-6);
}

bool classifier_inside(BRepClass3d_SolidClassifier& classifier,
                       const gp_Pnt& point, double tolerance) {
    classifier.Perform(point, tolerance);
    return classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON;
}

} // namespace

SolidTopology analyze_solid_topology(const TopoDS_Solid& solid,
                                     double volume,
                                     const ConversionOptions& options) {
    SolidTopology result;

    TopTools_IndexedMapOfShape vertices;
    TopTools_IndexedMapOfShape edges;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(solid, TopAbs_VERTEX, vertices);
    TopExp::MapShapes(solid, TopAbs_EDGE, edges);
    TopExp::MapShapes(solid, TopAbs_FACE, faces);
    result.vertex_count = static_cast<std::size_t>(vertices.Extent());
    result.edge_count = static_cast<std::size_t>(edges.Extent());
    result.face_count = static_cast<std::size_t>(faces.Extent());
    result.raw_euler_characteristic = static_cast<long long>(result.vertex_count)
        - static_cast<long long>(result.edge_count)
        + static_cast<long long>(result.face_count);

    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    std::array<double, 3> bbox_extents{0.0, 0.0, 0.0};
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        bbox_extents = {xmax - xmin, ymax - ymin, zmax - zmin};
        const double bbox_volume = bbox_extents[0] * bbox_extents[1] * bbox_extents[2];
        if (bbox_volume > kEps) result.bbox_fill_ratio = volume / bbox_volume;
    }

    const TopoDS_Shell outer = BRepClass3d::OuterShell(solid);
    std::size_t shell_index = 0;
    for (TopExp_Explorer it(solid, TopAbs_SHELL); it.More(); it.Next()) {
        ++shell_index;
        const TopoDS_Shell shell = TopoDS::Shell(it.Current());
        TopTools_IndexedMapOfShape sv, se, sf;
        TopExp::MapShapes(shell, TopAbs_VERTEX, sv);
        TopExp::MapShapes(shell, TopAbs_EDGE, se);
        TopExp::MapShapes(shell, TopAbs_FACE, sf);
        ShellTopology summary;
        summary.index = shell_index;
        summary.role = !outer.IsNull() && shell.IsSame(outer) ? "outer" : "cavity";
        summary.vertex_count = static_cast<std::size_t>(sv.Extent());
        summary.edge_count = static_cast<std::size_t>(se.Extent());
        summary.face_count = static_cast<std::size_t>(sf.Extent());
        summary.raw_euler_characteristic = static_cast<long long>(summary.vertex_count)
            - static_cast<long long>(summary.edge_count)
            + static_cast<long long>(summary.face_count);
        for (int face_i = 1; face_i <= sf.Extent(); ++face_i) {
            std::size_t wires = 0;
            for (TopExp_Explorer wi(sf(face_i), TopAbs_WIRE);
                 wi.More(); wi.Next()) {
                ++wires;
            }
            if (wires > 1) summary.face_inner_wire_count += wires - 1;
        }
        std::size_t degenerated_edges = 0;
        for (int edge_i = 1; edge_i <= se.Extent(); ++edge_i) {
            if (BRep_Tool::Degenerated(TopoDS::Edge(se(edge_i)))) {
                ++degenerated_edges;
            }
        }
        summary.euler_characteristic = summary.raw_euler_characteristic
            - static_cast<long long>(summary.face_inner_wire_count)
            + static_cast<long long>(degenerated_edges);
        // Fixed-point decomposition does not consume per-shell validity
        // diagnostics. Avoid rebuilding OCCT's validity graph for every
        // occurrence on its lean analysis passes; the exact convexity,
        // partition, and terminal global certificates remain authoritative.
        summary.closed = options.analyze_topology_metrics
            && BRepCheck_Analyzer(shell).IsValid();
        summary.genus_known = summary.closed
            && summary.euler_characteristic <= 2
            && ((2 - summary.euler_characteristic) % 2 == 0);
        if (summary.genus_known) {
            summary.genus = (2 - summary.euler_characteristic) / 2;
        }
        if (summary.closed) ++result.closed_shell_count;
        if (summary.role == "cavity") ++result.cavity_shell_count;
        result.shells.push_back(std::move(summary));
    }
    result.shell_count = result.shells.size();

    std::vector<CanonicalPlane> planes;
    std::vector<gp_Dir> normal_directions;
    std::vector<std::pair<CanonicalPlane, std::size_t>> planar_faces;
    std::vector<CylinderFace> cylinder_faces;
    std::vector<ConeFace> cone_faces;
    std::vector<AxialSurfaceFace> axial_surface_faces;
    std::vector<gp_Pnt> sphere_centers;
    bool all_faces_planar_or_cylindrical = true;
    bool all_faces_planar_or_conical = true;
    bool all_faces_revolution_supported = true;
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face face = TopoDS::Face(faces(i));
        std::size_t wires = 0;
        for (TopExp_Explorer wi(face, TopAbs_WIRE); wi.More(); wi.Next()) ++wires;
        if (wires > 1) result.face_inner_wire_count += wires - 1;
        BRepAdaptor_Surface surface(face, false);
        if (surface.GetType() == GeomAbs_Cylinder) {
            all_faces_planar_or_conical = false;
            const gp_Cylinder cylinder = surface.Cylinder();
            const gp_Dir direction = canonical_direction(
                cylinder.Axis().Direction());
            CylinderFace record;
            record.axis_direction = direction;
            record.axis_origin = canonical_axis_origin(
                cylinder.Axis().Location(), direction);
            record.radius = cylinder.Radius();
            record.full_sweep = std::abs(surface.LastUParameter()
                                          - surface.FirstUParameter())
                >= 2.0 * std::acos(-1.0)
                    - std::max(options.angular_tolerance * 10.0, 1.0e-7);
            cylinder_faces.push_back(record);
            axial_surface_faces.push_back({
                GeomAbs_Cylinder, record.axis_origin, record.axis_direction,
                std::abs(surface.LastUParameter() - surface.FirstUParameter()),
                record.full_sweep});
            continue;
        }
        if (surface.GetType() == GeomAbs_Cone) {
            all_faces_planar_or_cylindrical = false;
            const gp_Cone cone = surface.Cone();
            const gp_Dir source_direction = cone.Axis().Direction();
            const gp_Dir direction = canonical_direction(source_direction);
            const gp_Pnt origin = canonical_axis_origin(
                cone.Axis().Location(), direction);
            const double extent = std::abs(surface.LastUParameter()
                                           - surface.FirstUParameter());
            const double reference_level = gp_Vec(
                origin, cone.Location()).Dot(gp_Vec(direction));
            cone_faces.push_back({
                origin, direction, reference_level, cone.RefRadius(),
                std::tan(cone.SemiAngle())
                    * source_direction.Dot(direction),
                extent >= 2.0 * std::acos(-1.0)
                    - std::max(options.angular_tolerance * 10.0, 1.0e-7)});
            axial_surface_faces.push_back({
                GeomAbs_Cone,
                origin,
                direction, extent,
                extent >= 2.0 * std::acos(-1.0)
                    - std::max(options.angular_tolerance * 10.0, 1.0e-7)});
            continue;
        }
        if (surface.GetType() == GeomAbs_Torus) {
            all_faces_planar_or_cylindrical = false;
            all_faces_planar_or_conical = false;
            const gp_Torus torus = surface.Torus();
            const gp_Dir direction = canonical_direction(
                torus.Axis().Direction());
            const double extent = std::abs(surface.LastUParameter()
                                           - surface.FirstUParameter());
            axial_surface_faces.push_back({
                GeomAbs_Torus,
                canonical_axis_origin(torus.Axis().Location(), direction),
                direction, extent,
                extent >= 2.0 * std::acos(-1.0)
                    - std::max(options.angular_tolerance * 10.0, 1.0e-7)});
            continue;
        }
        if (surface.GetType() == GeomAbs_Sphere) {
            all_faces_planar_or_cylindrical = false;
            all_faces_planar_or_conical = false;
            sphere_centers.push_back(surface.Sphere().Location());
            continue;
        }
        if (surface.GetType() != GeomAbs_Plane) {
            all_faces_planar_or_cylindrical = false;
            all_faces_planar_or_conical = false;
            all_faces_revolution_supported = false;
            continue;
        }
        ++result.planar_face_count;
        const CanonicalPlane plane = canonical_plane(surface.Plane());
        planar_faces.emplace_back(plane, wires);
        if (std::none_of(planes.begin(), planes.end(), [&](const CanonicalPlane& p) {
                return same_plane(p, plane, options.linear_tolerance,
                                  options.angular_tolerance);
            })) {
            planes.push_back(plane);
        }
        if (std::none_of(normal_directions.begin(), normal_directions.end(),
                         [&](const gp_Dir& n) {
                             return same_direction(n, plane.normal,
                                                   options.angular_tolerance);
                         })) {
            normal_directions.push_back(plane.normal);
        }
    }
    result.unique_plane_count = planes.size();
    result.normal_direction_count = normal_directions.size();
    std::size_t degenerated_edge_count = 0;
    for (int i = 1; i <= edges.Extent(); ++i) {
        if (BRep_Tool::Degenerated(TopoDS::Edge(edges(i)))) {
            ++degenerated_edge_count;
        }
    }
    result.euler_characteristic = result.raw_euler_characteristic
        - static_cast<long long>(result.face_inner_wire_count)
        + static_cast<long long>(degenerated_edge_count);

    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge edge = TopoDS::Edge(edges(i));
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() == GeomAbs_Line) ++result.linear_edge_count;
        if (!edge_faces.Contains(edge)) {
            ++result.boundary_edge_count;
            continue;
        }
        const TopTools_ListOfShape& adjacent = edge_faces.FindFromKey(edge);
        const int count = adjacent.Extent();
        if (count == 1) {
            const TopoDS_Face only_face = TopoDS::Face(adjacent.First());
            if (BRep_Tool::Degenerated(edge)
                || BRep_Tool::IsClosed(edge, only_face)) {
                ++result.manifold_edge_count;
                ++result.tangent_or_unknown_edge_count;
            } else {
                ++result.boundary_edge_count;
            }
            continue;
        }
        if (count != 2) {
            ++result.nonmanifold_edge_count;
            continue;
        }
        ++result.manifold_edge_count;
    }

    if (options.analyze_topology_metrics) {
        const double min_extent = std::max(
            kEps, std::min({bbox_extents[0], bbox_extents[1],
                            bbox_extents[2]}));
        const double probe = std::max(options.linear_tolerance * 100.0,
                                      min_extent * 1.0e-5);
        // Loading the solid constructs the classifier's acceleration data.
        // Reuse it for all edge probes; rebuilding it four times per edge
        // dominates inspect time on large polyhedra.
        BRepClass3d_SolidClassifier solid_classifier(solid);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge edge = TopoDS::Edge(edges(i));
            if (!edge_faces.Contains(edge)) continue;
            const TopTools_ListOfShape& adjacent = edge_faces.FindFromKey(edge);
            if (adjacent.Extent() != 2) continue;
            BRepAdaptor_Curve curve(edge);
            TopTools_ListIteratorOfListOfShape fi(adjacent);
            const TopoDS_Face f1 = TopoDS::Face(fi.Value());
            fi.Next();
            const TopoDS_Face f2 = TopoDS::Face(fi.Value());
            BRepAdaptor_Surface s1(f1, false), s2(f2, false);
            if (s1.GetType() != GeomAbs_Plane || s2.GetType() != GeomAbs_Plane) {
                ++result.tangent_or_unknown_edge_count;
                continue;
            }
            const gp_Dir n1 = s1.Plane().Axis().Direction();
            const gp_Dir n2 = s2.Plane().Axis().Direction();
            if (std::abs(n1.Dot(n2)) > 1.0 - 1.0e-8) {
                ++result.tangent_or_unknown_edge_count;
                continue;
            }
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            if (!std::isfinite(first) || !std::isfinite(last)) {
                ++result.tangent_or_unknown_edge_count;
                continue;
            }
            const gp_Pnt p = curve.Value(0.5 * (first + last));
            int inside_quadrants = 0;
            for (int a : {-1, 1}) {
                for (int b : {-1, 1}) {
                    gp_Vec delta(n1);
                    delta.Multiply(static_cast<double>(a));
                    gp_Vec other(n2);
                    other.Multiply(static_cast<double>(b));
                    delta.Add(other);
                    if (delta.Magnitude() <= kEps) continue;
                    delta.Normalize();
                    delta.Multiply(probe);
                    if (classifier_inside(solid_classifier, p.Translated(delta),
                                          options.linear_tolerance)) {
                        ++inside_quadrants;
                    }
                }
            }
            if (inside_quadrants >= 3) ++result.concave_edge_count;
            else if (inside_quadrants == 1) ++result.convex_edge_count;
            else ++result.tangent_or_unknown_edge_count;
        }
    }

    if (result.planar_face_count == result.face_count
        && result.linear_edge_count == result.edge_count && !planes.empty()) {
        struct AxisCandidate {
            gp_Dir axis;
            std::vector<double> levels;
            double span = 0.0;
            std::size_t end_face_count = 0;
            std::size_t profile_holes = 0;
        };
        std::vector<AxisCandidate> candidates;
        const double angular = std::max(options.angular_tolerance, 1.0e-9);
        const double linear = std::max(options.linear_tolerance * 10.0, 1.0e-6);
        for (const gp_Dir& normal : normal_directions) {
            const gp_Dir axis = canonical_direction(normal);
            AxisCandidate candidate{axis};
            bool compatible = true;
            for (const CanonicalPlane& plane : planes) {
                const double dot = plane.normal.Dot(axis);
                const double alignment = std::abs(dot);
                if (alignment >= 1.0 - angular) {
                    candidate.levels.push_back(plane.offset / dot);
                } else if (alignment > angular) {
                    compatible = false;
                    break;
                }
            }
            if (!compatible) continue;
            std::sort(candidate.levels.begin(), candidate.levels.end());
            candidate.levels.erase(std::unique(candidate.levels.begin(),
                                                candidate.levels.end(),
                [&](double a, double b) { return std::abs(a - b) <= linear; }),
                candidate.levels.end());
            if (candidate.levels.size() < 2) continue;
            double low = std::numeric_limits<double>::infinity();
            double high = -std::numeric_limits<double>::infinity();
            for (int vertex_i = 1; vertex_i <= vertices.Extent(); ++vertex_i) {
                const gp_Pnt point = BRep_Tool::Pnt(
                    TopoDS::Vertex(vertices(vertex_i)));
                const double value = gp_Vec(gp_Pnt(0.0, 0.0, 0.0), point)
                    .Dot(gp_Vec(axis));
                low = std::min(low, value);
                high = std::max(high, value);
            }
            candidate.span = high - low;
            for (const auto& [plane, wires] : planar_faces) {
                if (std::abs(plane.normal.Dot(axis)) < 1.0 - angular) continue;
                ++candidate.end_face_count;
                if (wires > 1) {
                    candidate.profile_holes = std::max(candidate.profile_holes,
                                                       wires - 1);
                }
            }
            candidates.push_back(std::move(candidate));
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const AxisCandidate& a, const AxisCandidate& b) {
                const bool a_constant = a.levels.size() == 2;
                const bool b_constant = b.levels.size() == 2;
                if (a_constant != b_constant) return a_constant > b_constant;
                if (a.span != b.span) return a.span > b.span;
                if (a.levels.size() != b.levels.size()) {
                    return a.levels.size() < b.levels.size();
                }
                if (a.axis.X() != b.axis.X()) return a.axis.X() > b.axis.X();
                if (a.axis.Y() != b.axis.Y()) return a.axis.Y() > b.axis.Y();
                return a.axis.Z() > b.axis.Z();
            });
        if (!candidates.empty()) {
            const AxisCandidate& selected = candidates.front();
            result.prismatic_candidate = true;
            result.axial_level_count = selected.levels.size();
            result.axial_end_face_count = selected.end_face_count;
            result.extrusion_profile_hole_count = selected.profile_holes;
            result.constant_cross_section_candidate =
                selected.levels.size() == 2 && selected.end_face_count >= 2;
            result.prism_axis = {selected.axis.X(), selected.axis.Y(),
                                 selected.axis.Z()};
        }
    }

    if (all_faces_planar_or_cylindrical && cylinder_faces.size() >= 2) {
        const CylinderFace& reference = cylinder_faces.front();
        const bool common_axis = std::all_of(
            cylinder_faces.begin() + 1, cylinder_faces.end(),
            [&](const CylinderFace& face) {
                return coaxial(reference, face, options.linear_tolerance,
                               options.angular_tolerance);
            });
        const bool full_sweeps = std::all_of(
            cylinder_faces.begin(), cylinder_faces.end(),
            [](const CylinderFace& face) { return face.full_sweep; });
        std::vector<double> radii;
        for (const CylinderFace& face : cylinder_faces) {
            if (std::none_of(radii.begin(), radii.end(), [&](double radius) {
                    return std::abs(radius - face.radius)
                        <= std::max(options.linear_tolerance * 10.0, 1.0e-6);
                })) {
                radii.push_back(face.radius);
            }
        }
        std::sort(radii.begin(), radii.end());
        std::vector<double> levels;
        if (common_axis) {
            for (const auto& [plane, wires] : planar_faces) {
                if (wires < 2
                    || std::abs(plane.normal.Dot(reference.axis_direction))
                           < 1.0 - std::max(options.angular_tolerance, 1.0e-9)) {
                    continue;
                }
                const gp_Vec offset(reference.axis_origin,
                                    gp_Pnt(plane.normal.X() * plane.offset,
                                           plane.normal.Y() * plane.offset,
                                           plane.normal.Z() * plane.offset));
                levels.push_back(offset.Dot(gp_Vec(reference.axis_direction)));
            }
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end(), [&](double a,
                                                                    double b) {
            return std::abs(a - b)
                <= std::max(options.linear_tolerance * 10.0, 1.0e-6);
        }), levels.end());
        PipeAnnulusTopology& pipe = result.pipe_annulus;
        pipe.cylindrical_face_count = cylinder_faces.size();
        pipe.planar_end_face_count = levels.size();
        pipe.candidate = common_axis && full_sweeps && radii.size() == 2
            && levels.size() == 2 && radii[0] > 0.0
            && radii[1] > radii[0];
        if (pipe.candidate) {
            pipe.axis_origin = {reference.axis_origin.X(),
                                reference.axis_origin.Y(),
                                reference.axis_origin.Z()};
            pipe.axis_direction = {reference.axis_direction.X(),
                                   reference.axis_direction.Y(),
                                   reference.axis_direction.Z()};
            pipe.inner_radius = radii[0];
            pipe.outer_radius = radii[1];
            pipe.top_inner_radius = radii[0];
            pipe.top_outer_radius = radii[1];
            pipe.axial_length = levels[1] - levels[0];
            const gp_Pnt low = reference.axis_origin.Translated(
                gp_Vec(reference.axis_direction).Multiplied(levels[0]));
            const gp_Pnt high = reference.axis_origin.Translated(
                gp_Vec(reference.axis_direction).Multiplied(levels[1]));
            pipe.ports = {
                {"port:1", {low.X(), low.Y(), low.Z()},
                 {-reference.axis_direction.X(),
                  -reference.axis_direction.Y(),
                  -reference.axis_direction.Z()}, radii[0]},
                {"port:2", {high.X(), high.Y(), high.Z()},
                 {reference.axis_direction.X(),
                  reference.axis_direction.Y(),
                  reference.axis_direction.Z()}, radii[0]},
            };
        }
    }

    if (!result.pipe_annulus.candidate && all_faces_planar_or_conical
        && cone_faces.size() == 2) {
        const ConeFace& reference = cone_faces.front();
        const bool common_axis = coaxial(
            AxialSurfaceFace{GeomAbs_Cone, reference.axis_origin,
                             reference.axis_direction, 0.0, true},
            AxialSurfaceFace{GeomAbs_Cone, cone_faces[1].axis_origin,
                             cone_faces[1].axis_direction, 0.0, true},
            options.linear_tolerance, options.angular_tolerance);
        const bool full_sweeps = reference.full_sweep
            && cone_faces[1].full_sweep;
        std::vector<double> levels;
        if (common_axis) {
            for (const auto& [plane, wires] : planar_faces) {
                if (wires < 2
                    || std::abs(plane.normal.Dot(reference.axis_direction))
                           < 1.0 - std::max(options.angular_tolerance, 1.0e-9)) {
                    continue;
                }
                const gp_Pnt plane_point(plane.normal.X() * plane.offset,
                                         plane.normal.Y() * plane.offset,
                                         plane.normal.Z() * plane.offset);
                levels.push_back(gp_Vec(reference.axis_origin, plane_point)
                    .Dot(gp_Vec(reference.axis_direction)));
            }
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end(), [&](double a,
                                                                    double b) {
            return std::abs(a - b)
                <= std::max(options.linear_tolerance * 10.0, 1.0e-6);
        }), levels.end());
        PipeAnnulusTopology& pipe = result.pipe_annulus;
        pipe.conical_face_count = cone_faces.size();
        pipe.planar_end_face_count = levels.size();
        if (common_axis && full_sweeps && levels.size() == 2) {
            auto radius_at = [](const ConeFace& cone, double level) {
                return cone.reference_radius
                    + (level - cone.reference_level) * cone.radial_slope;
            };
            const std::array<double, 2> low{{
                radius_at(cone_faces[0], levels[0]),
                radius_at(cone_faces[1], levels[0])}};
            const std::array<double, 2> high{{
                radius_at(cone_faces[0], levels[1]),
                radius_at(cone_faces[1], levels[1])}};
            const std::size_t inner = low[0] < low[1] ? 0 : 1;
            const std::size_t outer = 1 - inner;
            const double tolerance = std::max(
                options.linear_tolerance * 10.0, 1.0e-6);
            pipe.candidate = low[inner] > 0.0
                && high[inner] > 0.0
                && low[outer] > low[inner] + tolerance
                && high[outer] > high[inner] + tolerance;
            if (pipe.candidate) {
                pipe.conical = true;
                pipe.axis_origin = {reference.axis_origin.X(),
                                    reference.axis_origin.Y(),
                                    reference.axis_origin.Z()};
                pipe.axis_direction = {reference.axis_direction.X(),
                                       reference.axis_direction.Y(),
                                       reference.axis_direction.Z()};
                pipe.inner_radius = low[inner];
                pipe.outer_radius = low[outer];
                pipe.top_inner_radius = high[inner];
                pipe.top_outer_radius = high[outer];
                pipe.axial_length = levels[1] - levels[0];
                const gp_Pnt low_center = reference.axis_origin.Translated(
                    gp_Vec(reference.axis_direction).Multiplied(levels[0]));
                const gp_Pnt high_center = reference.axis_origin.Translated(
                    gp_Vec(reference.axis_direction).Multiplied(levels[1]));
                pipe.ports = {
                    {"port:1", {low_center.X(), low_center.Y(), low_center.Z()},
                     {-reference.axis_direction.X(),
                      -reference.axis_direction.Y(),
                      -reference.axis_direction.Z()}, low[inner]},
                    {"port:2", {high_center.X(), high_center.Y(), high_center.Z()},
                     {reference.axis_direction.X(),
                      reference.axis_direction.Y(),
                      reference.axis_direction.Z()}, high[inner]},
                };
            }
        }
    }

    if (all_faces_revolution_supported && !axial_surface_faces.empty()) {
        const AxialSurfaceFace& reference = axial_surface_faces.front();
        const double angular = std::max(options.angular_tolerance, 1.0e-9);
        const double linear = std::max(options.linear_tolerance * 10.0, 1.0e-6);
        bool compatible = std::all_of(
            axial_surface_faces.begin() + 1, axial_surface_faces.end(),
            [&](const AxialSurfaceFace& face) {
                return coaxial(reference, face, options.linear_tolerance,
                               options.angular_tolerance);
            });
        std::vector<double> axial_levels;
        std::size_t radial_seams = 0;
        for (const auto& [plane, wires] : planar_faces) {
            (void)wires;
            const double alignment =
                std::abs(plane.normal.Dot(reference.axis_direction));
            if (alignment >= 1.0 - angular) {
                const gp_Pnt plane_point(plane.normal.X() * plane.offset,
                                        plane.normal.Y() * plane.offset,
                                        plane.normal.Z() * plane.offset);
                axial_levels.push_back(
                    gp_Vec(reference.axis_origin, plane_point)
                        .Dot(gp_Vec(reference.axis_direction)));
            } else if (alignment <= angular
                       && std::abs(gp_Vec(gp_Pnt(0.0, 0.0, 0.0),
                                          reference.axis_origin)
                                           .Dot(gp_Vec(plane.normal))
                                   - plane.offset) <= linear) {
                ++radial_seams;
            } else {
                compatible = false;
            }
        }
        for (const gp_Pnt& center : sphere_centers) {
            const gp_Vec offset(reference.axis_origin, center);
            if (offset.Crossed(gp_Vec(reference.axis_direction)).Magnitude()
                > linear) {
                compatible = false;
            }
        }
        std::sort(axial_levels.begin(), axial_levels.end());
        axial_levels.erase(std::unique(axial_levels.begin(), axial_levels.end(),
            [&](double a, double b) { return std::abs(a - b) <= linear; }),
            axial_levels.end());
        RevolutionTopology& revolution = result.revolution;
        revolution.candidate = compatible;
        revolution.axis_origin = {reference.axis_origin.X(),
                                  reference.axis_origin.Y(),
                                  reference.axis_origin.Z()};
        revolution.axis_direction = {reference.axis_direction.X(),
                                     reference.axis_direction.Y(),
                                     reference.axis_direction.Z()};
        revolution.axial_level_count = axial_levels.size();
        revolution.radial_seam_face_count = radial_seams;
        for (const AxialSurfaceFace& face : axial_surface_faces) {
            revolution.angular_extent = std::max(revolution.angular_extent,
                                                  face.angular_extent);
            switch (face.type) {
            case GeomAbs_Cylinder: ++revolution.cylindrical_face_count; break;
            case GeomAbs_Cone: ++revolution.conical_face_count; break;
            case GeomAbs_Torus: ++revolution.toroidal_face_count; break;
            default: break;
            }
        }
        revolution.spherical_face_count = sphere_centers.size();
        const bool all_full = std::all_of(
            axial_surface_faces.begin(), axial_surface_faces.end(),
            [](const AxialSurfaceFace& face) { return face.full_sweep; });
        revolution.full_revolution = compatible && all_full
            && radial_seams == 0;
        if (revolution.full_revolution) {
            revolution.angular_extent = 2.0 * std::acos(-1.0);
        }
    }

    if (result.cavity_shell_count > 0) {
        result.recommended_strategy = "shell_difference";
        result.reason_codes = {"enclosed_cavity_shells"};
        result.selected_recognition_kind = "enclosed_shell_difference";
        result.recognition_candidates.push_back({
            "enclosed_shell_difference", "shell_difference", "high", 1.0,
            true, {"enclosed_cavity_shells"}});
    } else if (result.pipe_annulus.candidate) {
        result.recommended_strategy = "pipe_annulus_difference";
        result.reason_codes = {"coaxial_cylindrical_boundaries",
                               "annular_end_faces", "full_cylindrical_sweep"};
        result.selected_recognition_kind = result.pipe_annulus.conical
            ? "straight_conical_pipe_annulus" : "straight_pipe_annulus";
        result.recognition_candidates.push_back({
            result.selected_recognition_kind, "pipe_annulus_difference", "high", 1.0,
            true, result.reason_codes});
    } else if (result.revolution.candidate
               && result.revolution.full_revolution) {
        result.recommended_strategy = "revolution_decomposition";
        result.reason_codes = {"shared_revolution_axis",
                               "analytic_meridian_supports",
                               "full_angular_sweep"};
        result.selected_recognition_kind = "full_axisymmetric_revolution";
        result.recognition_candidates.push_back({
            "full_axisymmetric_revolution", "revolution_decomposition",
            "high", 0.95, true, result.reason_codes});
    } else if (result.prismatic_candidate) {
        result.recommended_strategy = "prismatic_decomposition";
        result.reason_codes = {"all_faces_planar", "all_edges_linear",
                               "support_plane_derived_extrusion_axis",
                               "orthogonal_support_plane_families"};
        if (result.constant_cross_section_candidate) {
            result.reason_codes.push_back("constant_cross_section");
        }
        result.selected_recognition_kind = "linear_prism";
        result.recognition_candidates.push_back({
            "linear_prism", "prismatic_decomposition", "high", 0.9, true,
            result.reason_codes});
    } else {
        result.recommended_strategy = "progressive_bsp";
        result.reason_codes = {"nonconvex_supported_surfaces"};
        result.selected_recognition_kind = "generic_brep";
        result.recognition_candidates.push_back({
            "generic_brep", "progressive_bsp", "low", 0.1, false,
            result.reason_codes});
    }
    const auto has_recognition = [&](const char* kind) {
        return std::any_of(result.recognition_candidates.begin(),
                           result.recognition_candidates.end(),
                           [&](const RecognitionCandidate& candidate) {
                               return candidate.kind == kind;
                           });
    };
    if (result.prismatic_candidate && !has_recognition("linear_prism")) {
        result.recognition_candidates.push_back({
            "linear_prism", "prismatic_decomposition", "high", 0.9, true,
            {"all_faces_planar", "all_edges_linear",
             "support_plane_derived_extrusion_axis",
             "orthogonal_support_plane_families"}});
    }
    if (result.revolution.candidate
        && !has_recognition("full_axisymmetric_revolution")) {
        result.recognition_candidates.push_back({
            result.revolution.full_revolution
                ? "full_axisymmetric_revolution"
                : "partial_axisymmetric_revolution",
            result.revolution.full_revolution
                ? "revolution_decomposition" : "bounded_revolution_review",
            result.revolution.full_revolution ? "high" : "medium",
            result.revolution.full_revolution ? 0.95 : 0.7,
            result.revolution.full_revolution,
            {"shared_revolution_axis", "analytic_meridian_supports",
             result.revolution.full_revolution
                 ? "full_angular_sweep" : "partial_angular_sweep"}});
    }
    if (!has_recognition("generic_brep")) {
        result.recognition_candidates.push_back({
            "generic_brep", "progressive_bsp", "low", 0.1, false,
            {"generic_fallback"}});
    }
    std::stable_sort(result.recognition_candidates.begin(),
                     result.recognition_candidates.end(),
                     [](const RecognitionCandidate& a,
                        const RecognitionCandidate& b) {
                         return a.score > b.score;
                     });
    if (result.bbox_fill_ratio < 0.1) {
        result.reason_codes.push_back("low_bbox_fill_ratio");
    }
    return result;
}

std::vector<gp_Pln> unique_planar_support_planes(
    const TopoDS_Solid& solid,
    const ConversionOptions& options) {
    std::vector<CanonicalPlane> keys;
    std::vector<gp_Pln> result;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        BRepAdaptor_Surface surface(TopoDS::Face(it.Current()), false);
        if (surface.GetType() != GeomAbs_Plane) continue;
        const gp_Pln plane = surface.Plane();
        const CanonicalPlane key = canonical_plane(plane);
        if (std::none_of(keys.begin(), keys.end(), [&](const CanonicalPlane& p) {
                return same_plane(p, key, options.linear_tolerance,
                                  options.angular_tolerance);
            })) {
            keys.push_back(key);
            result.emplace_back(plane.Location(), key.normal);
        }
    }
    return result;
}

} // namespace csgify::detail
