#pragma once

#include "csgify/metadata.hpp"

#include <string>

namespace csgify {

using MaterialId = std::string;

enum class MaterialPhase {
    solid,
    liquid,
    gas,
    vacuum,
    unknown,
};

struct MaterialDefinition {
    MaterialId id;
    MetadataId metadata_id = kInvalidMetadataId;
    MaterialPhase phase = MaterialPhase::unknown;
    double density = 0.0;
    StringId density_unit = kInvalidStringId;
    StringId composition_reference = kInvalidStringId;
};

const char* material_phase_name(MaterialPhase phase);

} // namespace csgify
