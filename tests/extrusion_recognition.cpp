#include "engine_model.hpp"
#include "csgify/fixed_point.hpp"
#include "csgify/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>
#include <vector>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <unistd.h>

namespace {

int fail(const char* message) {
    std::cerr << "extrusion_recognition: " << message << '\n';
    return EXIT_FAILURE;
}

gp_Pnt point(const gp_Pnt& origin, const gp_Dir& u, const gp_Dir& v,
             double x, double y) {
    gp_Vec offset(u);
    offset.Multiply(x);
    gp_Vec second(v);
    second.Multiply(y);
    offset.Add(second);
    return origin.Translated(offset);
}

TopoDS_Wire polygon(const gp_Pnt& origin, const gp_Dir& u, const gp_Dir& v,
                    const std::vector<std::pair<double, double>>& points) {
    BRepBuilderAPI_MakePolygon builder;
    for (const auto& [x, y] : points) builder.Add(point(origin, u, v, x, y));
    builder.Close();
    return builder.Wire();
}

} // namespace

int main() {
    const gp_Pnt origin(3.0, -5.0, 7.0);
    const gp_Dir axis(1.0, 2.0, 3.0);
    const gp_Dir u = axis.Crossed(gp_Dir(0.0, 0.0, 1.0));
    const gp_Dir v = axis.Crossed(u);
    gp_Vec height(axis);
    height.Multiply(20.0);

    const TopoDS_Wire l_wire = polygon(origin, u, v, {
        {0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}});
    const TopoDS_Face l_face = BRepBuilderAPI_MakeFace(l_wire).Face();
    const TopoDS_Solid l_solid = TopoDS::Solid(
        BRepPrimAPI_MakePrism(l_face, height, false, true).Shape());

    csgify::ConversionOptions options;
    csgify::CadModel l_model;
    l_model.root = l_solid;
    const csgify::ConversionReport l_report =
        csgify::analyze_model(l_model, options);
    if (l_report.solids.size() != 1) return fail("L extrusion is not one solid");
    const csgify::SolidTopology& topology = l_report.solids[0].topology;
    const gp_Vec recognized(topology.prism_axis.x, topology.prism_axis.y,
                            topology.prism_axis.z);
    if (!topology.prismatic_candidate
        || !topology.constant_cross_section_candidate
        || topology.axial_level_count != 2
        || topology.axial_end_face_count < 2
        || std::abs(std::abs(recognized.Dot(gp_Vec(axis))) - 1.0) > 1.0e-8) {
        return fail("rotated extrusion axis/profile was not recognized");
    }
    const std::filesystem::path step_path =
        std::filesystem::temp_directory_path()
        / ("csgify-rotated-extrusion-" + std::to_string(::getpid()) + ".step");
    STEPControl_Writer writer;
    if (writer.Transfer(l_solid, STEPControl_AsIs) != IFSelect_RetDone
        || writer.Write(step_path.string().c_str()) != IFSelect_RetDone) {
        return fail("could not write rotated STEP fixture");
    }
    csgify::CadModel imported = csgify::load_step_model(step_path.string(), options);
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document =
        csgify::load_step(*session, step_path.string());
    std::filesystem::remove(step_path);
    const csgify::ConversionReport imported_report =
        csgify::analyze_model(imported, options);
    if (imported_report.solids.size() != 1
        || !imported_report.solids[0].topology.prismatic_candidate
        || !imported_report.solids[0].topology.constant_cross_section_candidate) {
        return fail("STEP round-trip lost rotated extrusion recognition");
    }
    const csgify::FixedPointDecompositionResult fixed =
        csgify::decompose_to_fixed_point(*session, document);
    if (!fixed.command.ok || !fixed.summary.exact
        || fixed.summary.actions_applied != 1
        || fixed.summary.candidates_evaluated != 0
        || fixed.summary.iterations.size() != 1
        || fixed.summary.iterations.front().strategy != "prismatic") {
        return fail("fixed-point did not short-circuit on exact extrusion");
    }
    if (!csgify::decompose_solid_prismatic(l_model, "solid:1", options)) {
        return fail("rotated L extrusion did not decompose");
    }
    const std::size_t active = static_cast<std::size_t>(std::count_if(
        l_model.decomposed_pieces.begin(), l_model.decomposed_pieces.end(),
        [](const csgify::DecomposedSolidPiece& piece) { return piece.active; }));
    if (active < 2) return fail("L extrusion did not produce a real partition");
    const csgify::ConversionResult converted =
        csgify::convert_model(l_model, options);
    if (!converted.report.ok()
        || std::abs(converted.report.total_volume - 1280.0) > 1.0e-6) {
        std::cerr << "converted ok=" << converted.report.ok()
                  << " volume=" << converted.report.total_volume
                  << " converted=" << converted.report.solids_converted
                  << "/" << converted.report.solids_seen << '\n';
        for (const csgify::Diagnostic& diagnostic : converted.report.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
        return fail("rotated extrusion partition was not exact");
    }

    const TopoDS_Face outer_face = BRepBuilderAPI_MakeFace(polygon(
        origin, u, v, {{0, 0}, {10, 0}, {10, 10}, {0, 10}})).Face();
    const TopoDS_Face inner_face = BRepBuilderAPI_MakeFace(polygon(
        origin, u, v, {{2, 2}, {8, 2}, {8, 8}, {2, 8}})).Face();
    const TopoDS_Shape outer =
        BRepPrimAPI_MakePrism(outer_face, height, false, true).Shape();
    const TopoDS_Shape inner =
        BRepPrimAPI_MakePrism(inner_face, height, false, true).Shape();
    BRepAlgoAPI_Cut hollow_cut(outer, inner);
    hollow_cut.Build();
    if (!hollow_cut.IsDone()) return fail("hollow fixture cut failed");
    csgify::CadModel hollow_model;
    hollow_model.root = hollow_cut.Shape();
    const csgify::ConversionReport hollow_report =
        csgify::analyze_model(hollow_model, options);
    if (hollow_report.solids.size() != 1
        || !hollow_report.solids[0].topology.prismatic_candidate
        || !hollow_report.solids[0].topology.constant_cross_section_candidate
        || hollow_report.solids[0].topology.extrusion_profile_hole_count != 1) {
        return fail("hollow profile topology was not recognized");
    }
    if (!csgify::decompose_solid_prismatic(
            hollow_model, "solid:1", options)) {
        return fail("hollow profile extrusion did not decompose");
    }
    const csgify::ConversionResult hollow_converted =
        csgify::convert_model(hollow_model, options);
    if (!hollow_converted.report.ok()
        || std::abs(hollow_converted.report.total_volume - 1280.0) > 1.0e-6) {
        return fail("hollow profile partition was not exact");
    }

    csgify::CadModel cylinder_model;
    cylinder_model.root = BRepPrimAPI_MakeCylinder(5.0, 20.0).Shape();
    const csgify::ConversionReport cylinder_report =
        csgify::analyze_model(cylinder_model, options);
    if (cylinder_report.solids.empty()
        || cylinder_report.solids[0].topology.prismatic_candidate) {
        return fail("curved cylinder was classified as a planar extrusion");
    }
    return EXIT_SUCCESS;
}
