#pragma once

#include "csgify/engine.hpp"

namespace csgify {

struct CadModel;

HoleFillProposal propose_hole_fill_candidates(
    const CadModel& model,
    const HoleFillOptions& hole_options,
    const ConversionOptions& conversion_options = {});

} // namespace csgify
