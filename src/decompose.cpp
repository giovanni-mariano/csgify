#include "decompose.hpp"

#include "analytic_surface.hpp"
#include "engine_internal.hpp"

#include <algorithm>
#include <cstddef>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BOPAlgo_Splitter.hxx>
#include <Bnd_Box.hxx>
#include <NCollection_List.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace cad2csg::detail {

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

} // namespace cad2csg::detail
