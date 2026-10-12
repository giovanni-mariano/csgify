#pragma once

#include "csgify/approximation.hpp"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace csgify::detail {

ApproximationMetrics measure_shape_approximation(
    const TopoDS_Shape& before,
    const TopoDS_Shape& after,
    double baseline_volume = 0.0,
    double numerical_volume_tolerance = 1.0e-12,
    std::string* error_out = nullptr);

// Exact symmetric difference for a disjoint partition. This avoids asking
// OCCT to treat a touching compound as one valid solid during shatter audit.
ApproximationMetrics measure_partition_approximation(
    const TopoDS_Shape& before,
    const std::vector<TopoDS_Shape>& after_parts,
    double baseline_volume = 0.0,
    double numerical_volume_tolerance = 1.0e-12,
    std::string* error_out = nullptr);

} // namespace csgify::detail
