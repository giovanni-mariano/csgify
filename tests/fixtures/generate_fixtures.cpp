// Build canned STEP fixtures for the cad2csg contract tests.
//
// Run once after a clean OCCT install; the produced .step files are committed
// alongside the source, so this binary is a developer tool, not a test
// dependency.

#include <cstdlib>
#include <iostream>
#include <string>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeShell.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <STEPControl_Writer.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

namespace {

int write_step(const TopoDS_Shape& shape, const std::string& path) {
    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
        std::cerr << "transfer failed: " << path << '\n';
        return 1;
    }
    if (writer.Write(path.c_str()) != IFSelect_RetDone) {
        std::cerr << "write failed: " << path << '\n';
        return 1;
    }
    return 0;
}

TopoDS_Shape make_l_shape() {
    TopoDS_Shape outer = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    TopoDS_Shape notch = BRepPrimAPI_MakeBox(gp_Pnt(5.0, 5.0, -1.0), 6.0, 6.0, 12.0).Shape();
    return BRepAlgoAPI_Cut(outer, notch).Shape();
}

// A cylinder solid whose every face has been converted to a B-spline surface
// via OCCT's NurbsConvert. The geometry is unchanged; the cad2csg spline-fit
// pass should classify the curved face as cylinder with near-zero deviation.
TopoDS_Shape make_nurbs_cylinder() {
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(2.0, 4.0).Shape();
    BRepBuilderAPI_NurbsConvert convert(cyl, true);
    return convert.Shape();
}

// A truncated cone solid with all faces NurbsConvert'ed. The curved face
// should fit cad2csg's cone candidate to near-machine precision.
TopoDS_Shape make_nurbs_cone() {
    TopoDS_Shape cone = BRepPrimAPI_MakeCone(2.0, 1.0, 4.0).Shape();
    BRepBuilderAPI_NurbsConvert convert(cone, true);
    return convert.Shape();
}

// A solid torus whose axis is along (1,1,1)/sqrt(3) — explicitly not aligned
// with any global axis. cad2csg's analyze pass must classify this with
// `unsupported_torus_orientation`. The surface-splitting workflow targets
// exactly this kind of geometry.
TopoDS_Shape make_tilted_torus() {
    const gp_Pnt origin(0.0, 0.0, 0.0);
    const gp_Dir tilted(1.0, 1.0, 1.0);
    const gp_Ax2 axis(origin, tilted);
    const double major_radius = 5.0;
    const double minor_radius = 1.0;
    return BRepPrimAPI_MakeTorus(axis, major_radius, minor_radius).Shape();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: generate_fixtures <output_dir>\n";
        return EXIT_FAILURE;
    }
    const std::string dir = argv[1];

    struct Item {
        std::string name;
        TopoDS_Shape shape;
    };

    Item items[] = {
        {"box.step", BRepPrimAPI_MakeBox(10.0, 5.0, 2.0).Shape()},
        {"sphere.step", BRepPrimAPI_MakeSphere(3.0).Shape()},
        {"cylinder.step", BRepPrimAPI_MakeCylinder(2.0, 6.0).Shape()},
        {"l_shape.step", make_l_shape()},
        {"spline_face.step", make_nurbs_cylinder()},
        {"spline_cone.step", make_nurbs_cone()},
        {"tilted_torus.step", make_tilted_torus()},
    };

    for (const Item& item : items) {
        const std::string path = dir + "/" + item.name;
        if (write_step(item.shape, path) != 0) {
            return EXIT_FAILURE;
        }
        std::cout << "wrote " << path << '\n';
    }
    return EXIT_SUCCESS;
}
