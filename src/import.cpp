// STEP import. Reads a file through OCCT's STEP reader, optionally runs
// ShapeFix, and packages the result as a CadModel ready for the analyze
// and emit stages. Pure import — no Alea, no traversal of solids.

#include "cad2csg/engine.hpp"

#include <string>

#include <BRepCheck_Analyzer.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <ShapeFix_Shape.hxx>
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>

namespace cad2csg {
namespace {

void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label = {}) {
    report.diagnostics.push_back({severity, code, message, label});
}

TopoDS_Shape read_step_shape(const std::string& step_path, ConversionReport& report) {
    STEPControl_Reader reader;
    const IFSelect_ReturnStatus status = reader.ReadFile(step_path.c_str());
    if (status != IFSelect_RetDone) {
        add_diag(report, DiagnosticSeverity::error, "step_read_failed",
                 "OCCT could not read the STEP file: " + step_path);
        return {};
    }

    const int roots = reader.NbRootsForTransfer();
    if (roots <= 0 || reader.TransferRoots() <= 0) {
        add_diag(report, DiagnosticSeverity::error, "step_transfer_failed",
                 "OCCT did not transfer any STEP roots.");
        return {};
    }

    return reader.OneShape();
}

TopoDS_Shape load_and_prepare_shape(const std::string& step_path,
                                    const ConversionOptions& options,
                                    ConversionReport& report) {
    TopoDS_Shape shape = read_step_shape(step_path, report);
    if (shape.IsNull()) {
        return {};
    }

    if (options.heal_shape) {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->Perform();
        shape = fixer->Shape();
    }

    BRepCheck_Analyzer analyzer(shape);
    if (!analyzer.IsValid()) {
        add_diag(report, DiagnosticSeverity::warning, "invalid_brep",
                 "OCCT reports that the transferred BRep is not fully valid; conversion will continue.");
    }

    return shape;
}

} // namespace

CadModel load_step_model(const std::string& step_path,
                         const ConversionOptions& options) {
    CadModel model;
    model.source_path = step_path;

    ConversionReport load_report;
    model.root = load_and_prepare_shape(step_path, options, load_report);
    model.diagnostics = load_report.diagnostics;
    return model;
}

} // namespace cad2csg
