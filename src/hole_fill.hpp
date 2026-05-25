#pragma once

#include "cad2csg/engine.hpp"

namespace cad2csg {

struct CadModel;

HoleFillProposal propose_hole_fill_candidates(
    const CadModel& model,
    const HoleFillOptions& hole_options,
    const ConversionOptions& conversion_options = {});

} // namespace cad2csg
