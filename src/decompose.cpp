#include "decompose.hpp"

#include "analytic_surface.hpp"
#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
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

TopoDS_Shape make_supported_cutter(const TopoDS_Face& face,
                                   double half_size,
                                   ConversionReport& report,
                                   std::size_t solid_index,
                                   std::size_t face_index) {
    BRepAdaptor_Surface surface(face, false);
    const GeomAbs_SurfaceType type = surface.GetType();
    switch (type) {
    case GeomAbs_Plane:
        return BRepBuilderAPI_MakeFace(surface.Plane(), -half_size, half_size,
                                       -half_size, half_size).Shape();
    case GeomAbs_Cylinder:
        return BRepBuilderAPI_MakeFace(surface.Cylinder(), 0.0, 2.0 * kPi,
                                       -half_size, half_size).Shape();
    case GeomAbs_Cone:
        return BRepBuilderAPI_MakeFace(surface.Cone(), 0.0, 2.0 * kPi,
                                       -half_size, half_size).Shape();
    case GeomAbs_Sphere:
        return BRepBuilderAPI_MakeFace(surface.Sphere(), 0.0, 2.0 * kPi,
                                       -kPi / 2.0, kPi / 2.0).Shape();
    case GeomAbs_Torus: {
        const gp_Torus torus = surface.Torus();
        const gp_Dir axis = torus.Axis().Direction();
        const gp_Dir normal = perpendicular_direction(axis);
        const gp_Pln plane(torus.Location(), normal);
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

std::vector<TopoDS_Solid> split_solid_by_face(const TopoDS_Solid& solid,
                                              const TopoDS_Face& face,
                                              const ConversionOptions& options,
                                              ConversionReport& report,
                                              std::size_t solid_index,
                                              std::size_t face_index) {
    ++report.decomposition_attempts;

    const double span = solid_box_span(solid);
    const double half_size = std::max(1.0, span * 2.0);

    NCollection_List<TopoDS_Shape> tools;
    TopoDS_Shape cutter =
        make_supported_cutter(face, half_size, report, solid_index, face_index);
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

    NCollection_List<TopoDS_Shape> tools;
    TopoDS_Shape cutter =
        BRepBuilderAPI_MakeFace(plane, -half_size, half_size,
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

    NCollection_List<TopoDS_Shape> tools;
    for (const gp_Pln& plane : planes) {
        TopoDS_Shape cutter =
            BRepBuilderAPI_MakeFace(plane, -half_size, half_size,
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
