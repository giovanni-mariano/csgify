#pragma once

#include "csgify/engine.hpp"

#include <string>
#include <vector>

#include <TopoDS_Solid.hxx>

namespace csgify::detail {

std::vector<TopoDS_Solid> coalesce_local_partition(
    const std::vector<TopoDS_Solid>& pieces,
    const std::string& source_solid_id,
    const ConversionOptions& options);

} // namespace csgify::detail
