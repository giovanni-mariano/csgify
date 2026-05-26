#include "decompose.hpp"

#include "analytic_surface.hpp"
#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>

#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTools.hxx>
#include <BOPAlgo_Splitter.hxx>
#include <Bnd_Box.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_List.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Vec.hxx>
#include <gp_Torus.hxx>

namespace cad2csg::detail {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

gp_Dir perpendicular_direction(const gp_Dir& axis) {
    const gp_Dir x_axis(1.0, 0.0, 0.0);
    const gp_Dir y_axis(0.0, 1.0, 0.0);
    if (std::abs(axis.Dot(x_axis)) < 0.9) {
        return gp_Dir(axis.XYZ().Crossed(x_axis.XYZ()));
    }
    return gp_Dir(axis.XYZ().Crossed(y_axis.XYZ()));
}

// Re-anchor a plane at the projection of `anchor` onto it, keeping the same
// geometric plane. A bounded cutter face built from this plane is centered
// near `anchor` rather than at the plane's intrinsic Location -- essential at
// large model coordinates, where the intrinsic origin can sit far outside the
// target solid so a +/- half_size square never reaches it (the cut silently
// produces no split).
gp_Pnt solid_box_center(const TopoDS_Solid& solid) {
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (box.IsVoid()) {
        return gp_Pnt(0.0, 0.0, 0.0);
    }
    double xmin = 0.0, ymin = 0.0, zmin = 0.0;
    double xmax = 0.0, ymax = 0.0, zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    return gp_Pnt(0.5 * (xmin + xmax), 0.5 * (ymin + ymax), 0.5 * (zmin + zmax));
}

gp_Pln plane_anchored_at(const gp_Pln& plane, const gp_Pnt& anchor) {
    const gp_Dir normal = plane.Axis().Direction();
    const double d = gp_Vec(plane.Location(), anchor).Dot(gp_Vec(normal));
    const gp_Pnt origin = anchor.Translated(gp_Vec(normal).Multiplied(-d));
    return gp_Pln(origin, normal);
}

// Axial coordinate of `anchor` measured from a revolved surface's Location
// along its axis -- used to center the V (axial) parameter range of a
// cylinder/cone cutter on the target solid.
double axial_coordinate(const gp_Pnt& location, const gp_Dir& axis,
                        const gp_Pnt& anchor) {
    return gp_Vec(location, anchor).Dot(gp_Vec(axis));
}

TopoDS_Shape make_supported_cutter(const TopoDS_Face& face,
                                   const gp_Pnt& anchor,
                                   double half_size,
                                   ConversionReport& report,
                                   std::size_t solid_index,
                                   std::size_t face_index) {
    BRepAdaptor_Surface surface(face, false);
    const GeomAbs_SurfaceType type = surface.GetType();
    switch (type) {
    case GeomAbs_Plane:
        return BRepBuilderAPI_MakeFace(plane_anchored_at(surface.Plane(), anchor),
                                       -half_size, half_size,
                                       -half_size, half_size).Shape();
    case GeomAbs_Cylinder: {
        const gp_Cylinder cylinder = surface.Cylinder();
        // Cylinder V parameter is the axial distance from Location, so center
        // the V range on the anchor's axial coordinate.
        const double v = axial_coordinate(cylinder.Location(),
                                          cylinder.Axis().Direction(), anchor);
        return BRepBuilderAPI_MakeFace(cylinder, 0.0, 2.0 * kPi,
                                       v - half_size, v + half_size).Shape();
    }
    case GeomAbs_Cone: {
        const gp_Cone cone = surface.Cone();
        // Cone axial component of V is v*cos(semiAngle), so the V that reaches
        // axial coordinate t is t/cos(semiAngle). Center the range there.
        const double cos_a = std::max(1.0e-3, std::abs(std::cos(cone.SemiAngle())));
        const double t = axial_coordinate(cone.Location(),
                                          cone.Axis().Direction(), anchor);
        const double v = t / cos_a;
        const double dv = half_size / cos_a;
        return BRepBuilderAPI_MakeFace(cone, 0.0, 2.0 * kPi,
                                       v - dv, v + dv).Shape();
    }
    case GeomAbs_Sphere:
        // Closed surface -- position is intrinsic, no anchoring needed.
        return BRepBuilderAPI_MakeFace(surface.Sphere(), 0.0, 2.0 * kPi,
                                       -kPi / 2.0, kPi / 2.0).Shape();
    case GeomAbs_Torus: {
        const gp_Torus torus = surface.Torus();
        const gp_Dir axis = torus.Axis().Direction();
        const gp_Dir normal = perpendicular_direction(axis);
        const gp_Pln plane = plane_anchored_at(gp_Pln(torus.Location(), normal), anchor);
        return BRepBuilderAPI_MakeFace(plane, -half_size, half_size,
                                       -half_size, half_size).Shape();
    }
    default:
        add_diag(report, DiagnosticSeverity::warning, "decomposition_face_tool_unsupported",
                 "Selected face surface cannot yet be used as a decomposition cutter.",
                 face_label(solid_index, face_index));
        return {};
    }
}

std::vector<TopoDS_Solid> run_splitter(const TopoDS_Solid& solid,
                                       const NCollection_List<TopoDS_Shape>& tools,
                                       const ConversionOptions& options,
                                       ConversionReport& report,
                                       std::size_t solid_index,
                                       const char* failed_code,
                                       const char* no_split_code,
                                       const char* no_split_message) {
    std::vector<TopoDS_Solid> pieces;
    if (tools.IsEmpty()) {
        return pieces;
    }

    NCollection_List<TopoDS_Shape> arguments;
    arguments.Append(solid);

    BOPAlgo_Splitter splitter;
    splitter.SetArguments(arguments);
    splitter.SetTools(tools);
    splitter.SetFuzzyValue(options.linear_tolerance);
    splitter.Perform();
    if (splitter.HasErrors()) {
        add_diag(report, DiagnosticSeverity::warning, failed_code,
                 "OCCT splitter failed while trying decomposition.",
                 solid_label(solid_index));
        return pieces;
    }

    const TopoDS_Shape result = splitter.Shape();
    for (TopExp_Explorer exp(result, TopAbs_SOLID); exp.More(); exp.Next()) {
        pieces.push_back(TopoDS::Solid(exp.Current()));
    }

    if (pieces.size() <= 1) {
        pieces.clear();
        add_diag(report, DiagnosticSeverity::warning, no_split_code,
                 no_split_message,
                 solid_label(solid_index));
    }
    return pieces;
}

// A coarse, rounded identity for a face's underlying surface, used only to
// avoid handing OCCT many coincident cutters for the same surface (e.g. the
// several planar faces that lie in one plane, or the matching halves of a
// split cylinder). Correctness does not depend on it -- redundant tools are
// harmless, just slower and noisier. Empty string means "do not dedup"
// (unsupported / spline faces, which make_supported_cutter handles anyway).
std::string surface_signature(const TopoDS_Face& face) {
    BRepAdaptor_Surface surface(face, false);
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    auto put_pnt = [&](const gp_Pnt& p) { out << p.X() << ',' << p.Y() << ',' << p.Z() << ';'; };
    auto put_dir = [&](const gp_Dir& d) {
        // Direction and its opposite denote the same surface; canonicalize.
        double x = d.X(), y = d.Y(), z = d.Z();
        if (x < -1e-9 || (std::abs(x) <= 1e-9 && (y < -1e-9 || (std::abs(y) <= 1e-9 && z < 0.0)))) {
            x = -x; y = -y; z = -z;
        }
        out << x << ',' << y << ',' << z << ';';
    };
    switch (surface.GetType()) {
    case GeomAbs_Plane: {
        const gp_Pln pln = surface.Plane();
        out << "P;";
        put_dir(pln.Axis().Direction());
        // Signed distance from origin along the (canonicalized) normal.
        const gp_Pnt loc = pln.Location();
        out << (loc.XYZ().Dot(pln.Axis().Direction().XYZ())) << ';';
        return out.str();
    }
    case GeomAbs_Cylinder: {
        const gp_Cylinder cyl = surface.Cylinder();
        out << "C;";
        put_pnt(cyl.Position().Location());
        put_dir(cyl.Axis().Direction());
        out << cyl.Radius() << ';';
        return out.str();
    }
    case GeomAbs_Cone: {
        const gp_Cone cone = surface.Cone();
        out << "K;";
        put_pnt(cone.Apex());
        put_dir(cone.Axis().Direction());
        out << cone.SemiAngle() << ';';
        return out.str();
    }
    case GeomAbs_Sphere: {
        const gp_Sphere sph = surface.Sphere();
        out << "S;";
        put_pnt(sph.Location());
        out << sph.Radius() << ';';
        return out.str();
    }
    default:
        // Tori (axis-plane cut depends on the torus, keep all) and anything
        // else: do not deduplicate.
        return {};
    }
}

// For a *partial* cylinder/cone face (an arc that neither closes the full
// circle nor is negligibly small), synthesize the bounding plane through the
// two angular-extreme generators -- the "chord plane" containing the axis for
// a cylinder, or the plane through the two extreme rulings and the apex for a
// cone. This is the separating surface a swept arc needs when no planar CAD
// face already bounds it (e.g. a fillet between two curved faces). The plane
// is derived entirely from the face's own geometry, not invented. Appends a
// bounded cutter to `tools` when one applies.
void add_closure_planes_for_face(const TopoDS_Face& face,
                                 double half_size,
                                 NCollection_List<TopoDS_Shape>& tools) {
    BRepAdaptor_Surface surface(face, false);
    const GeomAbs_SurfaceType type = surface.GetType();
    if (type != GeomAbs_Cylinder && type != GeomAbs_Cone) {
        return;
    }
    Standard_Real u_min = 0.0, u_max = 0.0, v_min = 0.0, v_max = 0.0;
    BRepTools::UVBounds(face, u_min, u_max, v_min, v_max);
    const double angular_span = std::abs(u_max - u_min);
    // Full revolutions are handled by the quadric cut; near-zero arcs are
    // degenerate. Only partial arcs need a closure plane.
    if (angular_span <= 0.05 || angular_span >= 2.0 * kPi - 0.05) {
        return;
    }
    const double v_mid = 0.5 * (v_min + v_max);
    const gp_Pnt p1 = surface.Value(u_min, v_mid);
    const gp_Pnt p2 = surface.Value(u_max, v_mid);
    if (p1.Distance(p2) < 1.0e-7) {
        return;
    }

    gp_Vec normal;
    if (type == GeomAbs_Cylinder) {
        const gp_Dir axis = surface.Cylinder().Axis().Direction();
        normal = gp_Vec(p1, p2).Crossed(gp_Vec(axis));
    } else {
        const gp_Pnt apex = surface.Cone().Apex();
        normal = gp_Vec(p1, p2).Crossed(gp_Vec(p1, apex));
    }
    if (normal.Magnitude() < 1.0e-9) {
        return;
    }
    const gp_Pln plane(p1, gp_Dir(normal));
    const TopoDS_Shape cutter =
        BRepBuilderAPI_MakeFace(plane, -half_size, half_size,
                                -half_size, half_size).Shape();
    if (!cutter.IsNull()) {
        tools.Append(cutter);
    }
}

} // namespace

double solid_box_span(const TopoDS_Solid& solid) {
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (box.IsVoid()) {
        return 1.0;
    }
    double xmin = 0.0, ymin = 0.0, zmin = 0.0;
    double xmax = 0.0, ymax = 0.0, zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::vector<TopoDS_Solid> split_solid_by_planar_faces(const TopoDS_Solid& solid,
                                                      const ConversionOptions& options,
                                                      ConversionReport& report,
                                                      std::size_t solid_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);
    const gp_Pnt anchor = solid_box_center(solid);

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
            BRepBuilderAPI_MakeFace(plane_anchored_at(surface.plane, anchor),
                                    -half_size, half_size,
                                    -half_size, half_size).Shape();
        if (!cutting_face.IsNull()) {
            tools.Append(cutting_face);
        }
    }

    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_planar_tools",
                 "No planar cutting tools were available for this non-convex solid.",
                 solid_label(solid_index));
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "decomposition_splitter_failed",
                        "decomposition_no_split",
                        "Planar decomposition did not produce multiple solid pieces.");
}

std::vector<TopoDS_Solid> split_solid_by_all_surfaces(const TopoDS_Solid& solid,
                                                      const ConversionOptions& options,
                                                      ConversionReport& report,
                                                      std::size_t solid_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);
    const gp_Pnt anchor = solid_box_center(solid);

    NCollection_List<TopoDS_Shape> tools;
    std::set<std::string> seen_surfaces;
    std::size_t face_index = 0;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        ++face_index;
        const TopoDS_Face face = TopoDS::Face(face_exp.Current());
        const std::string signature = surface_signature(face);
        if (!signature.empty() && !seen_surfaces.insert(signature).second) {
            continue;  // already have a cutter for this surface
        }
        const TopoDS_Shape cutter =
            make_supported_cutter(face, anchor, half_size, report, solid_index, face_index);
        if (!cutter.IsNull()) {
            tools.Append(cutter);
        }
    }

    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "shatter_no_tools",
                 "No usable cutting surfaces were available for subdivision.",
                 solid_label(solid_index));
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "shatter_splitter_failed",
                        "shatter_no_split",
                        "Surface-driven subdivision did not produce multiple solid pieces.");
}

std::vector<TopoDS_Solid> split_solid_by_closure_planes(const TopoDS_Solid& solid,
                                                        const ConversionOptions& options,
                                                        ConversionReport& report,
                                                        std::size_t solid_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);

    NCollection_List<TopoDS_Shape> tools;
    for (TopExp_Explorer face_exp(solid, TopAbs_FACE); face_exp.More(); face_exp.Next()) {
        add_closure_planes_for_face(TopoDS::Face(face_exp.Current()), half_size, tools);
    }

    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "shatter_no_closure_planes",
                 "No partial curved faces yielded a closure plane.",
                 solid_label(solid_index));
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "shatter_closure_splitter_failed",
                        "shatter_closure_no_split",
                        "Closure-plane subdivision did not produce multiple solid pieces.");
}

std::vector<TopoDS_Solid> split_solid_by_face(const TopoDS_Solid& solid,
                                              const TopoDS_Face& face,
                                              const ConversionOptions& options,
                                              ConversionReport& report,
                                              std::size_t solid_index,
                                              std::size_t face_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);
    const gp_Pnt anchor = solid_box_center(solid);

    NCollection_List<TopoDS_Shape> tools;
    TopoDS_Shape cutter =
        make_supported_cutter(face, anchor, half_size, report, solid_index, face_index);
    if (!cutter.IsNull()) {
        tools.Append(cutter);
    }
    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_face_tool",
                 "Selected face did not produce a usable cutting tool.",
                 face_label(solid_index, face_index));
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "decomposition_by_face_splitter_failed",
                        "decomposition_by_face_no_split",
                        "Directed face decomposition did not produce multiple solid pieces.");
}

std::vector<TopoDS_Solid> split_solid_by_plane(const TopoDS_Solid& solid,
                                               const gp_Pln& plane,
                                               const ConversionOptions& options,
                                               ConversionReport& report,
                                               std::size_t solid_index,
                                               const std::string& label) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);
    const gp_Pnt anchor = solid_box_center(solid);

    NCollection_List<TopoDS_Shape> tools;
    TopoDS_Shape cutter =
        BRepBuilderAPI_MakeFace(plane_anchored_at(plane, anchor),
                                -half_size, half_size,
                                -half_size, half_size).Shape();
    if (!cutter.IsNull()) {
        tools.Append(cutter);
    }
    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_plane_tool",
                 "Fallback plane did not produce a usable cutting tool.",
                 label.empty() ? solid_label(solid_index) : label);
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "decomposition_by_plane_splitter_failed",
                        "decomposition_by_plane_no_split",
                        "Fallback plane decomposition did not produce multiple solid pieces.");
}

std::vector<TopoDS_Solid> split_solid_by_planes(const TopoDS_Solid& solid,
                                                const std::vector<gp_Pln>& planes,
                                                const ConversionOptions& options,
                                                ConversionReport& report,
                                                std::size_t solid_index,
                                                const std::string& label) {
    ++report.decomposition_attempts;

    if (planes.empty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_planes_tool",
                 "Multi-plane decomposition was called with no planes.",
                 label.empty() ? solid_label(solid_index) : label);
        return {};
    }

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);
    const gp_Pnt anchor = solid_box_center(solid);

    NCollection_List<TopoDS_Shape> tools;
    for (const gp_Pln& plane : planes) {
        TopoDS_Shape cutter =
            BRepBuilderAPI_MakeFace(plane_anchored_at(plane, anchor),
                                    -half_size, half_size,
                                    -half_size, half_size).Shape();
        if (!cutter.IsNull()) {
            tools.Append(cutter);
        }
    }
    if (tools.IsEmpty()) {
        add_diag(report, DiagnosticSeverity::warning, "decomposition_no_planes_tool",
                 "No usable plane tools were built for multi-plane decomposition.",
                 label.empty() ? solid_label(solid_index) : label);
        return {};
    }

    return run_splitter(solid, tools, options, report, solid_index,
                        "decomposition_by_planes_splitter_failed",
                        "decomposition_by_planes_no_split",
                        "Multi-plane decomposition did not produce multiple solid pieces.");
}

} // namespace cad2csg::detail
