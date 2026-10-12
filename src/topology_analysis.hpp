#pragma once

#include "csgify/engine.hpp"

#include <TopoDS_Solid.hxx>
#include <gp_Pln.hxx>

#include <vector>

namespace csgify::detail {

SolidTopology analyze_solid_topology(const TopoDS_Solid& solid,
                                     double volume,
                                     const ConversionOptions& options);

std::vector<gp_Pln> unique_planar_support_planes(
    const TopoDS_Solid& solid,
    const ConversionOptions& options);

} // namespace csgify::detail
