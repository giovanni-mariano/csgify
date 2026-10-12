#include "local_overlap.hpp"

#include <iostream>
#include <string>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

namespace {

int fail(const std::string& message) {
    std::cerr << "local_overlap: " << message << '\n';
    return 1;
}

TopoDS_Solid box(double x, double dx) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, 0, 0), dx, 10, 10).Solid();
}

} // namespace

int main() {
    csgify::CadModel model;
    TopoDS_Compound root;
    BRep_Builder builder;
    builder.MakeCompound(root);
    const TopoDS_Solid parent = box(0, 10);
    const TopoDS_Solid neighbour = box(10, 10);
    builder.Add(root, parent);
    builder.Add(root, neighbour);
    model.root = root;
    csgify::ConversionOptions options;
    csgify::PerformanceMetrics performance;

    const auto safe = csgify::detail::certify_local_neighbour_overlaps(
        model, "solid:1", {box(0, 5), box(5, 5)}, {}, options,
        &performance);
    if (!safe.safe || safe.unknown || safe.new_overlap_count != 0
        || performance.local_exact_overlap_tests == 0) {
        return fail("touching neighbour was not certified safely");
    }

    csgify::PerformanceMetrics contained_performance;
    const auto contained = csgify::detail::certify_local_neighbour_overlaps(
        model, "solid:1", {box(0, 5), box(5, 5)}, {}, options,
        &contained_performance, true);
    if (!contained.safe || !contained.containment_proof_used
        || contained.spatial_candidates != 0
        || contained.exact_tests != 0
        || contained_performance.local_exact_overlap_tests != 0
        || contained_performance.local_overlap_containment_proofs != 1) {
        return fail("certified containment did not eliminate redundant neighbour scans");
    }

    const auto unsafe = csgify::detail::certify_local_neighbour_overlaps(
        model, "solid:1", {box(0, 9), box(9, 2)}, {}, options);
    if (unsafe.safe || unsafe.new_overlap_count != 1
        || unsafe.conflicting_body_id != "solid:2") {
        return fail("new positive-volume neighbour overlap was not rejected");
    }

    csgify::SolidOverlap inherited;
    inherited.solid_a_id = "solid:1";
    inherited.solid_b_id = "solid:2";
    const auto allowed = csgify::detail::certify_local_neighbour_overlaps(
        model, "solid:1", {box(0, 9), box(9, 2)}, {inherited}, options);
    if (!allowed.safe || allowed.inherited_overlap_hits == 0) {
        return fail("pre-existing target-neighbour overlap was not inherited");
    }
    return 0;
}
