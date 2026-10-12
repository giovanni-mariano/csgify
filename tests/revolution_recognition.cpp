#include "engine_model.hpp"
#include "csgify/fixed_point.hpp"
#include "csgify/session.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <unistd.h>

namespace {

int fail(const char* message) {
    std::cerr << "revolution_recognition: " << message << '\n';
    return EXIT_FAILURE;
}

TopoDS_Solid one_solid(const TopoDS_Shape& shape) {
    TopExp_Explorer it(shape, TopAbs_SOLID);
    return it.More() ? TopoDS::Solid(it.Current()) : TopoDS_Solid();
}

} // namespace

int main() {
    const gp_Pnt base(4.0, -7.0, 3.0);
    const gp_Dir axis(1.0, 2.0, 3.0);
    gp_Vec axial_offset(axis);
    axial_offset.Multiply(10.0);
    const TopoDS_Solid lower =
        BRepPrimAPI_MakeCylinder(gp_Ax2(base, axis), 5.0, 10.0).Solid();
    const TopoDS_Solid upper = BRepPrimAPI_MakeCylinder(
        gp_Ax2(base.Translated(axial_offset), axis), 3.0, 10.0).Solid();
    BRepAlgoAPI_Fuse fuse(lower, upper);
    fuse.Build();
    const TopoDS_Solid stepped = one_solid(fuse.Shape());
    if (!fuse.IsDone() || stepped.IsNull()) return fail("stepped fixture failed");

    csgify::ConversionOptions options;
    csgify::CadModel model;
    model.root = stepped;
    const csgify::ConversionReport report = csgify::analyze_model(model, options);
    if (report.solids.size() != 1) return fail("stepped shaft is not one solid");
    const csgify::RevolutionTopology& topology =
        report.solids[0].topology.revolution;
    if (!topology.candidate || !topology.full_revolution
        || topology.cylindrical_face_count < 2
        || topology.axial_level_count < 3
        || report.solids[0].topology.selected_recognition_kind
               != "full_axisymmetric_revolution") {
        return fail("rotated stepped shaft was not recognized");
    }
    if (!csgify::decompose_solid_revolution(model, "solid:1", options)) {
        return fail("stepped shaft revolution did not decompose");
    }
    const csgify::ConversionResult converted = csgify::convert_model(model, options);
    const double expected = std::acos(-1.0) * 340.0;
    if (!converted.report.ok()
        || std::abs(converted.report.total_volume - expected) > 1.0e-6) {
        return fail("stepped shaft decomposition changed volume");
    }

    const std::filesystem::path step_path =
        std::filesystem::temp_directory_path()
        / ("csgify-stepped-revolution-" + std::to_string(::getpid()) + ".step");
    STEPControl_Writer writer;
    if (writer.Transfer(stepped, STEPControl_AsIs) != IFSelect_RetDone
        || writer.Write(step_path.string().c_str()) != IFSelect_RetDone) {
        return fail("could not write revolution STEP fixture");
    }
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document =
        csgify::load_step(*session, step_path.string());
    std::filesystem::remove(step_path);
    const csgify::FixedPointDecompositionResult fixed =
        csgify::decompose_to_fixed_point(*session, document);
    if (!fixed.command.ok || !fixed.summary.exact
        || fixed.summary.actions_applied != 1
        || fixed.summary.candidates_evaluated != 0
        || fixed.summary.iterations.size() != 1
        || fixed.summary.iterations.front().strategy != "revolution") {
        return fail("fixed-point did not select revolution first");
    }

    csgify::CadModel partial_model;
    partial_model.root = BRepPrimAPI_MakeCylinder(
        gp_Ax2(base, axis), 5.0, 10.0, std::acos(-1.0)).Shape();
    const csgify::ConversionReport partial_report =
        csgify::analyze_model(partial_model, options);
    if (partial_report.solids.empty()
        || !partial_report.solids[0].topology.revolution.candidate
        || partial_report.solids[0].topology.revolution.full_revolution
        || partial_report.solids[0].topology.revolution.radial_seam_face_count < 2) {
        return fail("partial revolution extent/seams were not recognized");
    }

    csgify::CadModel cone_model;
    cone_model.root = BRepPrimAPI_MakeCone(
        gp_Ax2(base, axis), 5.0, 2.0, 10.0).Shape();
    const csgify::ConversionReport cone_report =
        csgify::analyze_model(cone_model, options);
    if (cone_report.solids.empty()
        || !cone_report.solids[0].topology.revolution.full_revolution
        || cone_report.solids[0].topology.revolution.conical_face_count != 1) {
        return fail("conical revolution was not recognized");
    }

    csgify::CadModel torus_model;
    torus_model.root = BRepPrimAPI_MakeTorus(
        gp_Ax2(base, axis), 10.0, 2.0).Shape();
    const csgify::ConversionReport torus_report =
        csgify::analyze_model(torus_model, options);
    if (torus_report.solids.empty()
        || !torus_report.solids[0].topology.revolution.full_revolution
        || torus_report.solids[0].topology.revolution.toroidal_face_count != 1) {
        return fail("toroidal revolution was not recognized");
    }

    gp_Vec sideways(axis.Crossed(gp_Dir(0.0, 0.0, 1.0)));
    sideways.Normalize();
    sideways.Multiply(4.0);
    const TopoDS_Solid offset = BRepPrimAPI_MakeCylinder(
        gp_Ax2(base.Translated(sideways), axis), 5.0, 10.0).Solid();
    BRepAlgoAPI_Fuse noncoaxial_fuse(lower, offset);
    noncoaxial_fuse.Build();
    csgify::CadModel noncoaxial_model;
    noncoaxial_model.root = noncoaxial_fuse.Shape();
    const csgify::ConversionReport noncoaxial_report =
        csgify::analyze_model(noncoaxial_model, options);
    if (noncoaxial_report.solids.size() == 1
        && noncoaxial_report.solids[0].topology.revolution.candidate) {
        return fail("non-coaxial union was classified as a revolution");
    }
    return EXIT_SUCCESS;
}
