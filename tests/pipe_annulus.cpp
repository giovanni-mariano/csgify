#include "engine_model.hpp"
#include "csgify/project_store.hpp"
#include "csgify/fixed_point.hpp"
#include "csgify/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <gp_Ax2.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <unistd.h>

namespace {
int fail(const char* message) {
    std::cerr << "pipe_annulus: " << message << "\n";
    return EXIT_FAILURE;
}
} // namespace

int main() {
    const gp_Pnt base(4.0, -3.0, 7.0);
    const gp_Dir axis(1.0, 2.0, 3.0);
    const TopoDS_Solid outer =
        BRepPrimAPI_MakeCylinder(gp_Ax2(base, axis), 5.0, 20.0).Solid();
    const TopoDS_Solid inner =
        BRepPrimAPI_MakeCylinder(gp_Ax2(base, axis), 3.0, 20.0).Solid();
    BRepAlgoAPI_Cut cut(outer, inner);
    cut.Build();
    if (!cut.IsDone()) return fail("OCCT fixture cut failed");

    csgify::CadModel model;
    model.root = cut.Shape();
    csgify::ConversionOptions options;
    const csgify::ConversionReport before = csgify::analyze_model(model, options);
    if (before.solids.size() != 1) return fail("fixture did not yield one solid");
    const csgify::SolidTopology& topology = before.solids[0].topology;
    if (!topology.pipe_annulus.candidate
        || topology.selected_recognition_kind != "straight_pipe_annulus"
        || topology.recommended_strategy != "pipe_annulus_difference"
        || topology.pipe_annulus.ports.size() != 2) {
        return fail("straight pipe was not recognized");
    }
    if (std::abs(topology.pipe_annulus.inner_radius - 3.0) > 1.0e-8
        || std::abs(topology.pipe_annulus.outer_radius - 5.0) > 1.0e-8
        || std::abs(topology.pipe_annulus.axial_length - 20.0) > 1.0e-8) {
        return fail("recognized pipe dimensions are wrong");
    }
    const std::string json = csgify::report_json_string(before);
    if (json.find("\"straight_pipe_annulus\"") == std::string::npos
        || json.find("\"ports\"") == std::string::npos) {
        return fail("recognition evidence is missing from JSON");
    }
    const std::vector<csgify::SpatialEntry> spatial =
        csgify::spatial_entries_from_report(before);
    const std::size_t pipe_ports = static_cast<std::size_t>(std::count_if(
        spatial.begin(), spatial.end(), [](const csgify::SpatialEntry& entry) {
            return entry.kind == "pipe_port";
        }));
    if (pipe_ports != 2) return fail("pipe ports were not spatially indexed");

    const std::filesystem::path step_path =
        std::filesystem::temp_directory_path()
        / ("csgify-pipe-annulus-" + std::to_string(::getpid()) + ".step");
    STEPControl_Writer writer;
    if (writer.Transfer(cut.Shape(), STEPControl_AsIs) != IFSelect_RetDone
        || writer.Write(step_path.string().c_str()) != IFSelect_RetDone) {
        return fail("could not write coordinator fixture");
    }
    csgify::SessionPtr session = csgify::create_session();
    const csgify::DocumentId document =
        csgify::load_step(*session, step_path.string());
    std::filesystem::remove(step_path);
    if (document.empty()) return fail("could not import coordinator fixture");
    csgify::FixedPointDecompositionPolicy policy;
    const csgify::FixedPointDecompositionResult fixed =
        csgify::decompose_to_fixed_point(*session, document, policy);
    const bool recognized_after_import = !fixed.command.report.solids.empty()
        && fixed.command.report.solids.front().topology.pipe_annulus.candidate;
    const bool already_exact = fixed.summary.actions_applied == 0
        && fixed.summary.stop_reason
               == csgify::FixedPointStopReason::already_exact;
    const bool routed = fixed.summary.actions_applied == 1
        && fixed.summary.stop_reason == csgify::FixedPointStopReason::exact
        && !fixed.summary.iterations.empty()
        && fixed.summary.iterations.front().strategy
               == "pipe_annulus_difference";
    if (!fixed.command.ok || !fixed.summary.exact || !recognized_after_import
        || (!already_exact && !routed)) {
        std::cerr << "fixed-point: ok=" << fixed.command.ok
                  << " exact=" << fixed.summary.exact
                  << " actions=" << fixed.summary.actions_applied
                  << " stop=" << static_cast<int>(fixed.summary.stop_reason)
                  << " iterations=" << fixed.summary.iterations.size() << "\n";
        for (const auto& iteration : fixed.summary.iterations) {
            std::cerr << "strategy=" << iteration.strategy << "\n";
        }
        for (const auto& diagnostic : fixed.command.report.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << "\n";
        }
        return fail("import/fixed-point did not preserve pipe recognition");
    }

    if (!csgify::decompose_solid_pipe_annulus(model, "solid:1", options)) {
        return fail("pipe annulus operation failed");
    }
    if (model.cad_boolean_bodies.size() != 1
        || model.cad_boolean_bodies[0].positive_cells.size() != 1
        || model.cad_boolean_bodies[0].negative_cells.size() != 1) {
        return fail("pipe Boolean operands were not preserved");
    }
    const csgify::ConversionResult converted = csgify::convert_model(model, options);
    const double expected = std::acos(-1.0) * (25.0 - 9.0) * 20.0;
    if (!converted.report.ok() || converted.report.solids_converted != 1
        || std::abs(converted.report.total_volume - expected) > 1.0e-6) {
        return fail("recognized pipe did not convert exactly");
    }

    csgify::CadModel cylinder_model;
    cylinder_model.root = outer;
    const csgify::ConversionReport cylinder_report =
        csgify::analyze_model(cylinder_model, options);
    if (cylinder_report.solids.empty()
        || cylinder_report.solids[0].topology.pipe_annulus.candidate) {
        return fail("solid cylinder was incorrectly recognized as a pipe");
    }

    const TopoDS_Solid cone_outer = BRepPrimAPI_MakeCone(
        gp_Ax2(base, axis), 8.0, 5.0, 30.0).Solid();
    const TopoDS_Solid cone_inner = BRepPrimAPI_MakeCone(
        gp_Ax2(base, axis), 5.0, 2.0, 30.0).Solid();
    BRepAlgoAPI_Cut cone_cut(cone_outer, cone_inner);
    cone_cut.Build();
    if (!cone_cut.IsDone()) return fail("OCCT conical fixture cut failed");
    csgify::CadModel cone_model;
    cone_model.root = cone_cut.Shape();
    const csgify::ConversionReport cone_report =
        csgify::analyze_model(cone_model, options);
    if (cone_report.solids.size() != 1) {
        return fail("conical fixture did not yield one solid");
    }
    const csgify::PipeAnnulusTopology& cone_pipe =
        cone_report.solids[0].topology.pipe_annulus;
    if (!cone_pipe.candidate || !cone_pipe.conical
        || cone_report.solids[0].topology.selected_recognition_kind
               != "straight_conical_pipe_annulus") {
        return fail("straight conical pipe was not recognized");
    }
    if (std::abs(cone_pipe.inner_radius - 5.0) > 1.0e-7
        || std::abs(cone_pipe.top_inner_radius - 2.0) > 1.0e-7
        || std::abs(cone_pipe.outer_radius - 8.0) > 1.0e-7
        || std::abs(cone_pipe.top_outer_radius - 5.0) > 1.0e-7
        || std::abs(cone_pipe.axial_length - 30.0) > 1.0e-7) {
        std::cerr << "cone radii=" << cone_pipe.inner_radius << ","
                  << cone_pipe.top_inner_radius << ","
                  << cone_pipe.outer_radius << ","
                  << cone_pipe.top_outer_radius << " length="
                  << cone_pipe.axial_length << "\n";
        return fail("recognized conical pipe dimensions are wrong");
    }
    if (!csgify::decompose_solid_pipe_annulus(
            cone_model, "solid:1", options)) {
        return fail("conical pipe annulus operation failed");
    }
    const double frustum_factor = std::acos(-1.0) * 30.0 / 3.0;
    const double cone_expected = frustum_factor
        * ((8.0 * 8.0 + 8.0 * 5.0 + 5.0 * 5.0)
           - (5.0 * 5.0 + 5.0 * 2.0 + 2.0 * 2.0));
    const csgify::ConversionResult cone_converted =
        csgify::convert_model(cone_model, options);
    if (!cone_converted.report.ok()
        || std::abs(cone_converted.report.total_volume - cone_expected)
               > 1.0e-5) {
        return fail("recognized conical pipe did not convert exactly");
    }

    const gp_Dir reversed_axis(-1.0, -2.0, -3.0);
    const gp_Ax2 reversed_placement(base, reversed_axis);
    BRepAlgoAPI_Cut reversed_cut(
        BRepPrimAPI_MakeCone(
            reversed_placement, 8.0, 5.0, 30.0).Shape(),
        BRepPrimAPI_MakeCone(
            reversed_placement, 5.0, 2.0, 30.0).Shape());
    reversed_cut.Build();
    csgify::CadModel reversed_model;
    reversed_model.root = reversed_cut.Shape();
    const csgify::ConversionReport reversed_report =
        csgify::analyze_model(reversed_model, options);
    if (reversed_report.solids.size() != 1
        || !reversed_report.solids[0].topology.pipe_annulus.candidate
        || !csgify::decompose_solid_pipe_annulus(
            reversed_model, "solid:1", options)) {
        return fail("reversed-axis conical pipe was not reconstructed exactly");
    }
    return EXIT_SUCCESS;
}
