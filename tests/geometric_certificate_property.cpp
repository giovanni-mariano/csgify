#include "engine_internal.hpp"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cstdlib>
#include <iostream>

namespace {

bool certified(const TopoDS_Solid& solid,
               csgify::GeometryCertificate* certificate) {
    csgify::ConversionOptions options;
    csgify::ConversionReport report;
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    gp_Pnt interior;
    if (!csgify::detail::find_interior_point(
            solid, props, options.linear_tolerance, &interior)) {
        return false;
    }
    return csgify::detail::validate_convex_halfspace_cell(
        solid, interior, 1, options, report, false, false, false,
        nullptr, certificate);
}

int fail(const char* message) {
    std::cerr << "geometric_certificate_property: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main() {
    for (int i = 0; i < 32; ++i) {
        const double dx = 0.25 + (i % 7) * 0.73;
        const double dy = 0.15 + (i % 5) * 0.41;
        const double dz = i % 4 == 0 ? 1.0e-3 : 0.2 + (i % 3) * 0.67;
        TopoDS_Shape shape = BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
        gp_Trsf transform;
        transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)),
                              0.071 * i);
        shape = BRepBuilderAPI_Transform(shape, transform, true).Shape();
        csgify::GeometryCertificate certificate;
        if (!certified(TopoDS::Solid(shape), &certificate)) {
            return fail("convex transformed box was not certified");
        }
        if (!certificate.certified
            || certificate.faces_checked != 6
            || certificate.boolean_halfspace_checks != 6
            || certificate.maximum_outside_volume
                   > certificate.allowed_outside_volume) {
            return fail("convex box certificate is incomplete");
        }
    }

    BRepAlgoAPI_Fuse fuse(BRepPrimAPI_MakeBox(10.0, 3.0, 2.0).Shape(),
                          BRepPrimAPI_MakeBox(3.0, 10.0, 2.0).Shape());
    fuse.Build();
    TopExp_Explorer solid_exp(fuse.Shape(), TopAbs_SOLID);
    if (!fuse.IsDone() || !solid_exp.More()) {
        return fail("could not construct adversarial L solid");
    }
    csgify::GeometryCertificate concave_certificate;
    if (certified(TopoDS::Solid(solid_exp.Current()), &concave_certificate)) {
        return fail("concave L solid incorrectly received a certificate");
    }
    if (concave_certificate.failure_code.empty()) {
        return fail("failed certificate did not explain its failure");
    }
    return EXIT_SUCCESS;
}
