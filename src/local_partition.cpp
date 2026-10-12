#include "local_partition.hpp"
#include "occt_boolean.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Compound.hxx>

namespace csgify::detail {
namespace {

double volume_of(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0.0;
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}

bool bbox_within(const Bnd_Box& child, const Bnd_Box& parent, double pad) {
    if (child.IsVoid() || parent.IsVoid()) return false;
    double cx0, cy0, cz0, cx1, cy1, cz1;
    double px0, py0, pz0, px1, py1, pz1;
    child.Get(cx0, cy0, cz0, cx1, cy1, cz1);
    parent.Get(px0, py0, pz0, px1, py1, pz1);
    return cx0 >= px0 - pad && cy0 >= py0 - pad && cz0 >= pz0 - pad
        && cx1 <= px1 + pad && cy1 <= py1 + pad && cz1 <= pz1 + pad;
}

template <class Operation>
bool build_boolean(Operation* operation,
                   const TopoDS_Shape& argument,
                   const TopoDS_Shape& tool,
                   double* volume) {
    try {
        build_binary_boolean(*operation, argument, tool);
        if (!operation->IsDone() || operation->Shape().IsNull()) return false;
        *volume = volume_of(operation->Shape());
        return std::isfinite(*volume);
    } catch (...) {
        return false;
    }
}

} // namespace

LocalPartitionCertificate certify_local_partition(
    const TopoDS_Solid& parent,
    const std::vector<TopoDS_Solid>& children,
    const ConversionOptions& options,
    std::size_t maximum_boolean_calls,
    PerformanceMetrics* performance) {
    const auto started = std::chrono::steady_clock::now();
    LocalPartitionCertificate result;
    auto finish = [&]() {
        if (performance) {
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            performance->local_certificate_ms += elapsed;
            performance->local_partition_boolean_calls +=
                result.exact_boolean_calls;
            performance->peak_local_pieces = std::max(
                performance->peak_local_pieces, children.size());
            if (result.exact) ++performance->local_trials_certified;
            else ++performance->local_trials_rejected;
        }
        return result;
    };
    if (performance) ++performance->local_trials_generated;
    if (parent.IsNull() || parent.ShapeType() != TopAbs_SOLID
        || children.empty()) {
        result.failure_code = "invalid_partition_input";
        return finish();
    }
    result.parent_volume = volume_of(parent);
    if (!std::isfinite(result.parent_volume) || result.parent_volume <= 0.0
        || !BRepCheck_Analyzer(parent).IsValid()) {
        result.failure_code = "invalid_partition_parent";
        return finish();
    }
    const double relative_limit =
        std::max(0.0, options.decomposition_relative_volume_tolerance);
    const double volume_limit = std::max(
        result.parent_volume * relative_limit, 1.0e-12);
    const double bbox_pad = std::max(options.linear_tolerance, 1.0e-9);
    Bnd_Box parent_bbox;
    BRepBndLib::Add(parent, parent_bbox);
    std::vector<Bnd_Box> child_bboxes(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        const TopoDS_Solid& child = children[i];
        if (child.IsNull() || child.ShapeType() != TopAbs_SOLID
            || !BRepCheck_Analyzer(child).IsValid()) {
            result.failure_code = "invalid_partition_child";
            return finish();
        }
        const double volume = volume_of(child);
        if (!std::isfinite(volume) || volume <= 0.0) {
            result.failure_code = "nonpositive_partition_child";
            return finish();
        }
        result.child_volume_sum += volume;
        BRepBndLib::Add(child, child_bboxes[i]);
        if (!bbox_within(child_bboxes[i], parent_bbox, bbox_pad)) {
            result.failure_code = "partition_child_bbox_outside_parent";
            return finish();
        }
    }
    result.valid_children = true;
    result.relative_volume_delta =
        std::abs(result.child_volume_sum - result.parent_volume)
        / result.parent_volume;
    result.volume_within_tolerance =
        std::abs(result.child_volume_sum - result.parent_volume) <= volume_limit;
    if (!result.volume_within_tolerance) {
        result.failure_code = "partition_volume_drift";
        return finish();
    }
    auto consume_boolean = [&]() {
        if (maximum_boolean_calls > 0
            && result.exact_boolean_calls >= maximum_boolean_calls) {
            result.failure_code = "local_boolean_budget";
            return false;
        }
        ++result.exact_boolean_calls;
        return true;
    };

    // Use the child compound as their geometric union for proof purposes.
    // One difference in each direction establishes union containment and
    // coverage independently of child count; non-negative outside material
    // cannot cancel between children.
    TopoDS_Compound child_compound;
    BRep_Builder builder;
    builder.MakeCompound(child_compound);
    for (const TopoDS_Solid& child : children) builder.Add(child_compound, child);
    if (!consume_boolean()) return finish();
    BRepAlgoAPI_Cut outside;
    if (!build_boolean(&outside, child_compound, parent,
                       &result.outside_parent_volume)) {
        result.failure_code = "partition_containment_unknown";
        return finish();
    }
    if (result.outside_parent_volume > volume_limit) {
        result.failure_code = "partition_outside_parent";
        return finish();
    }
    result.contained_in_parent = true;

    if (!consume_boolean()) return finish();
    BRepAlgoAPI_Cut uncovered;
    if (!build_boolean(&uncovered, parent, child_compound,
                       &result.uncovered_parent_volume)) {
        result.failure_code = "partition_coverage_unknown";
        return finish();
    }
    if (result.uncovered_parent_volume > volume_limit) {
        result.failure_code = "partition_incomplete_coverage";
        return finish();
    }
    result.covers_parent = true;

    // Pairwise sibling Commons are unnecessary once containment and coverage
    // are exact. Let U be the union of children. The two Boolean checks above
    // establish U == parent within tolerance. The volume precheck establishes
    // sum(volume(children)) == volume(parent). Their difference is precisely
    // the integrated excess multiplicity caused by sibling overlap. Bound
    // that consequence directly, avoiding O(children^2) expensive Commons for
    // large splitter partitions whose legal interfaces all touch.
    result.sibling_overlap_volume = std::max(
        0.0, result.child_volume_sum - result.parent_volume)
        + result.uncovered_parent_volume + result.outside_parent_volume;
    const double derived_overlap_limit = 3.0 * volume_limit;
    if (result.sibling_overlap_volume > derived_overlap_limit) {
        result.failure_code = "partition_sibling_overlap";
        return finish();
    }
    result.siblings_disjoint = true;
    result.exact = result.valid_children && result.contained_in_parent
        && result.covers_parent && result.siblings_disjoint
        && result.volume_within_tolerance;
    if (!result.exact) result.failure_code = "partition_certificate_failed";
    return finish();
}

} // namespace csgify::detail
