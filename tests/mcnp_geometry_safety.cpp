#include "csgify/session.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_step(const std::string& path,
                const std::vector<TopoDS_Shape>& shapes) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& shape : shapes) builder.Add(compound, shape);
    STEPControl_Writer writer;
    assert(writer.Transfer(compound, STEPControl_AsIs) == IFSelect_RetDone);
    assert(writer.Write(path.c_str()) == IFSelect_RetDone);
}

bool exists(const std::string& path) {
    std::ifstream input(path);
    return input.good();
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    std::filesystem::create_directories(root);
    const TopoDS_Shape domain = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TopoDS_Shape left = BRepPrimAPI_MakeBox(5.0, 10.0, 10.0).Shape();
    const TopoDS_Shape right = BRepPrimAPI_MakeBox(
        gp_Pnt(5.0, 0.0, 0.0), 5.0, 10.0, 10.0).Shape();

    const std::string covered_step = root + "/covered.step";
    const std::string covered_mcnp = root + "/covered.i";
    write_step(covered_step, {domain, left, right});
    std::remove(covered_mcnp.c_str());
    csgify::ConversionOptions options;
    options.mcnp_domain_id = "solid:1";
    options.require_domain_coverage = true;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId doc = csgify::load_step(*session, covered_step);
    const csgify::CommandResult covered = csgify::emit_document(
        *session, doc, covered_mcnp, csgify::ReportIdMode::local);
    assert(covered.ok);
    assert(exists(covered_mcnp));
    assert(covered.report.mcnp_geometry_safety.validated);
    assert(covered.report.mcnp_geometry_safety.safe_to_emit);
    assert(covered.report.mcnp_geometry_safety.coverage_status
           == csgify::CoverageStatus::certified);

    const std::string gap_step = root + "/gap.step";
    const std::string gap_mcnp = root + "/gap.i";
    write_step(gap_step, {domain, left});
    std::remove(gap_mcnp.c_str());
    session = csgify::create_session(options);
    const csgify::DocumentId gap_doc = csgify::load_step(*session, gap_step);
    const csgify::CommandResult gap = csgify::emit_document(
        *session, gap_doc, gap_mcnp, csgify::ReportIdMode::local);
    assert(!gap.ok);
    assert(!exists(gap_mcnp));
    assert(gap.report.mcnp_geometry_safety.coverage_status
           == csgify::CoverageStatus::gap);
    assert(gap.report.mcnp_geometry_safety.uncovered_volume > 499.0);
    assert(gap.report.mcnp_geometry_safety.failure_code == "coverage_gap");

    csgify::ConversionOptions missing;
    missing.require_domain_coverage = true;
    session = csgify::create_session(missing);
    const csgify::DocumentId missing_doc =
        csgify::load_step(*session, covered_step);
    const std::string missing_mcnp = root + "/missing.i";
    std::remove(missing_mcnp.c_str());
    const csgify::CommandResult missing_result = csgify::emit_document(
        *session, missing_doc, missing_mcnp, csgify::ReportIdMode::local);
    assert(!missing_result.ok);
    assert(!exists(missing_mcnp));
    assert(missing_result.report.mcnp_geometry_safety.coverage_status
           == csgify::CoverageStatus::not_configured);
    assert(missing_result.report.mcnp_geometry_safety.failure_code
           == "coverage_domain_not_configured");
    return 0;
}
