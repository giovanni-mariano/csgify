#pragma once

#include "csgify/material.hpp"

#include <array>
#include <string>

namespace csgify {

using ComponentDefinitionId = std::string;
using ComponentInstanceId = std::string;

struct InstanceTransform {
    // Affine 3x4 matrix in row-major order. The default is identity.
    std::array<double, 12> values{{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
    }};
};

struct ComponentDefinition {
    ComponentDefinitionId id;
    // Stable XCAF prototype identity. A later content digest augments this
    // field; reuse never relies on the legacy geometric soft fingerprint.
    StringId source_definition_key = kInvalidStringId;
    std::string geometry_digest;
    bool geometry_digest_verified = false;
    MetadataId metadata_id = kInvalidMetadataId;
};

struct ComponentInstance {
    ComponentInstanceId id;
    ComponentDefinitionId definition_id;
    std::string parent_assembly_id;
    std::string source_shape_id;
    InstanceTransform transform;
    MetadataId metadata_id = kInvalidMetadataId;
    MaterialId material_override;
    bool active = true;
};

} // namespace csgify
