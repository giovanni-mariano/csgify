#include "engine_model.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS.hxx>

namespace {
int fail(const char* message) {
    std::cerr << "shell_difference: " << message << "\n";
    return EXIT_FAILURE;
}
} // namespace

int main() {
    const TopoDS_Shape outer = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TopoDS_Shape cavity =
        BRepPrimAPI_MakeBox(gp_Pnt(2.0, 2.0, 2.0), 6.0, 6.0, 6.0).Shape();
    BRepAlgoAPI_Cut cut(outer, cavity);
    cut.Build();
    if (!cut.IsDone()) return fail("OCCT fixture cut failed");

    csgify::CadModel model;
    model.root = cut.Shape();
    csgify::ConversionOptions options;
    const csgify::ConversionReport before = csgify::analyze_model(model, options);
    if (before.solids.size() != 1
        || before.solids[0].topology.cavity_shell_count != 1
        || before.solids[0].topology.recommended_strategy
               != "shell_difference") {
        if (!before.solids.empty()) {
            std::cerr << "shells=" << before.solids[0].topology.shell_count
                      << " cavities="
                      << before.solids[0].topology.cavity_shell_count
                      << " strategy="
                      << before.solids[0].topology.recommended_strategy << "\n";
        }
        return fail("enclosed cavity topology was not classified");
    }
    if (!csgify::decompose_solid_shell_difference(model, "solid:1", options)) {
        return fail("shell difference operation failed");
    }
    if (model.cad_boolean_bodies.size() != 1
        || model.cad_boolean_bodies[0].positive_cells.size() != 1
        || model.cad_boolean_bodies[0].negative_cells.size() != 1) {
        return fail("shell roles were not preserved");
    }

    csgify::ConversionResult converted = csgify::convert_model(model, options);
    if (!converted.report.ok() || converted.report.solids_converted != 1) {
        return fail("CAD Boolean body did not emit");
    }
    if (std::abs(converted.report.total_volume - 784.0) > 1.0e-7) {
        return fail("Boolean body volume changed");
    }
    return EXIT_SUCCESS;
}
