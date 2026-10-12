#include "local_partition.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

namespace {

int fail(const std::string& message) {
    std::cerr << "local_partition: " << message << '\n';
    return 1;
}

TopoDS_Solid box(double x, double y, double z,
                 double dx, double dy, double dz) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Solid();
}

} // namespace

int main() {
    csgify::ConversionOptions options;
    options.decomposition_relative_volume_tolerance = 1.0e-9;
    const TopoDS_Solid parent = box(0, 0, 0, 10, 10, 10);
    const TopoDS_Solid left = box(0, 0, 0, 5, 10, 10);
    const TopoDS_Solid right = box(5, 0, 0, 5, 10, 10);
    csgify::PerformanceMetrics performance;
    const auto exact = csgify::detail::certify_local_partition(
        parent, {left, right}, options, 32, &performance);
    if (!exact.exact || !exact.valid_children || !exact.contained_in_parent
        || !exact.covers_parent || !exact.siblings_disjoint
        || !exact.volume_within_tolerance) {
        return fail("valid two-box partition was rejected: "
                    + exact.failure_code);
    }
    if (performance.local_trials_generated != 1
        || performance.local_trials_certified != 1
        || performance.local_partition_boolean_calls != 2) {
        return fail("certificate telemetry was not recorded");
    }

    const auto missing = csgify::detail::certify_local_partition(
        parent, {left}, options);
    if (missing.exact || missing.failure_code != "partition_volume_drift") {
        return fail("missing half was not rejected by volume guard");
    }
    const auto duplicate = csgify::detail::certify_local_partition(
        parent, {left, left}, options);
    if (duplicate.exact
        || duplicate.failure_code != "partition_incomplete_coverage") {
        return fail("duplicate child was not rejected by the union/volume proof");
    }
    const TopoDS_Solid overhang_left = box(-1, 0, 0, 6, 10, 10);
    const auto overhang = csgify::detail::certify_local_partition(
        parent, {overhang_left, right}, options);
    if (overhang.exact
        || overhang.failure_code != "partition_child_bbox_outside_parent") {
        return fail("overhanging child was not rejected");
    }
    const auto bounded = csgify::detail::certify_local_partition(
        parent, {left, right}, options, 1);
    if (bounded.exact || bounded.failure_code != "local_boolean_budget") {
        return fail("Boolean-call budget was not enforced");
    }
    return 0;
}
