#pragma once

#include "csgify/engine.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <TopoDS_Solid.hxx>

namespace csgify::detail {

struct LocalPartitionCertificate {
    bool valid_children = false;
    bool contained_in_parent = false;
    bool covers_parent = false;
    bool siblings_disjoint = false;
    bool volume_within_tolerance = false;
    bool exact = false;
    double parent_volume = 0.0;
    double child_volume_sum = 0.0;
    double relative_volume_delta = 0.0;
    double outside_parent_volume = 0.0;
    double uncovered_parent_volume = 0.0;
    double sibling_overlap_volume = 0.0;
    std::size_t exact_boolean_calls = 0;
    std::string failure_code;
};

// Prove that children are a valid, volume-preserving partition of parent.
// A failed OCCT Boolean is unknown and rejects the certificate. The routine
// never mutates parent or children.
LocalPartitionCertificate certify_local_partition(
    const TopoDS_Solid& parent,
    const std::vector<TopoDS_Solid>& children,
    const ConversionOptions& options,
    std::size_t maximum_boolean_calls = 0,
    PerformanceMetrics* performance = nullptr);

} // namespace csgify::detail
