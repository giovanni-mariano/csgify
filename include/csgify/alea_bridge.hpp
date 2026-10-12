#pragma once

#include "csgify/engine.hpp"
#include "csgify/project_store.hpp"

#include <alea.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace csgify {

struct ConversionReport;
struct ConversionOptions;

inline constexpr const char* kAleaHandoffSchema = "csgify.alea-handoff";
inline constexpr std::size_t kAleaHandoffSchemaMajor = 1;
inline constexpr std::size_t kAleaHandoffSchemaMinor = 0;

// Capabilities are negotiated before a project job spends time constructing
// geometry. They describe only public libalea APIs used by the bridge; csgify
// never reaches into libalea's internal system representation.
struct AleaBridgeCapabilities {
    std::string provider = "libalea";
    std::string provider_version;
    std::size_t schema_major = kAleaHandoffSchemaMajor;
    std::size_t schema_minor = kAleaHandoffSchemaMinor;
    bool boolean_cells = false;
    bool cell_comments = false;
    bool universes = false;
    bool fills = false;
    bool transforms = false;
    bool materials = false;
    bool void_generation = false;
    bool complete_coverage = false;
    bool structured_diagnostics = false;
};

struct AleaBridgeRequirements {
    std::size_t schema_major = kAleaHandoffSchemaMajor;
    std::size_t minimum_schema_minor = 0;
    bool boolean_cells = true;
    bool cell_comments = true;
    bool universes = true;
    bool fills = false;
    bool transforms = false;
    bool materials = true;
    bool void_generation = false;
    bool complete_coverage = false;
    bool structured_diagnostics = true;
};

struct AleaBridgeNegotiation {
    bool compatible = false;
    std::vector<std::string> missing_capabilities;
    std::vector<Diagnostic> diagnostics;
};

struct AleaChunkReference {
    std::string digest_algorithm;
    std::string digest;
    std::string kind;
    std::string relative_path;
    std::uint64_t size = 0;
};

struct AleaCellRecord {
    std::size_t storage_index = 0;
    int cell_id = 0;
    int material_id = 0;
    double density = 0.0;
    int universe_id = 0;
    int fill_universe = -1;
    int fill_transform = 0;
    std::uint64_t root_node = 0;
    std::string expression_digest;
    std::vector<std::string> source_entity_ids;
};

struct AleaSurfaceRecord {
    std::size_t storage_index = 0;
    int surface_id = 0;
    int primitive_type = 0;
};

struct AleaUniverseRecord {
    std::size_t storage_index = 0;
    int universe_id = 0;
    std::size_t cell_count = 0;
    BBox bbox;
};

struct AleaSidecarAnnotation {
    std::string key;
    std::string value;
    std::string origin;
    std::string review_state;
    bool user_verified = false;
};

// This is deliberately resolved UTF-8 text rather than document-local StringId
// values. It remains meaningful after the Alea system and source report have
// been persisted or transported independently.
struct AleaSidecarEntity {
    std::string entity_id;
    std::string kind;
    std::string definition_id;
    std::string instance_id;
    std::string material_id;
    std::vector<int> alea_cell_ids;
    std::string stable_name;
    std::string display_name;
    std::string description;
    std::string engineering_role;
    std::string subsystem;
    std::string source_cad_label;
    std::string source_component_path;
    std::vector<std::string> tags;
    std::vector<AleaSidecarAnnotation> annotations;
    InstanceTransform transform;
    bool has_transform = false;
};

struct AleaHandoffManifest {
    std::string schema = kAleaHandoffSchema;
    std::size_t schema_major = kAleaHandoffSchemaMajor;
    std::size_t schema_minor = kAleaHandoffSchemaMinor;
    std::string producer = "csgify";
    std::string producer_version;
    std::string libalea_version;
    std::string source_digest;
    std::string policy_digest;
    std::string contract_digest;
    AleaBridgeCapabilities capabilities;
    std::vector<AleaCellRecord> cells;
    std::vector<AleaSurfaceRecord> surfaces;
    std::vector<AleaUniverseRecord> universes;
    std::vector<AleaSidecarEntity> sidecar;
    std::vector<AleaChunkReference> chunks;
    std::vector<Diagnostic> diagnostics;
};

AleaBridgeCapabilities query_alea_bridge_capabilities();

AleaBridgeNegotiation negotiate_alea_bridge(
    const AleaBridgeCapabilities& capabilities,
    const AleaBridgeRequirements& requirements = {});

// Register csgify's row-major affine 3x4 transform as a named libalea
// auxiliary-to-main transform. Only rigid/reflection transforms accepted by
// libalea are permitted.
bool register_alea_instance_transform(alea_system_t* system,
                                      int transform_id,
                                      const InstanceTransform& transform,
                                      std::string* error = nullptr);

// Create a void container cell that fills one existing universe through a
// registered instance transform. `container_root` belongs to `system` and
// bounds the physical occurrence in `parent_universe`.
int add_alea_fill_instance(alea_system_t* system,
                           int cell_id,
                           alea_node_id_t container_root,
                           int parent_universe,
                           int fill_universe,
                           int transform_id,
                           const InstanceTransform& transform,
                           const std::string& source_entity_id,
                           std::string* error = nullptr);

std::string alea_bridge_policy_digest(const ConversionOptions& options);

AleaHandoffManifest make_alea_handoff_manifest(
    const alea_system_t* system,
    const ConversionReport& report,
    std::string source_digest = {},
    std::string policy_digest = {});

bool validate_alea_handoff_manifest(const AleaHandoffManifest& manifest,
                                    std::string* error = nullptr);

std::string serialize_alea_handoff_manifest(const AleaHandoffManifest& manifest);

bool add_alea_handoff_chunk(AleaHandoffManifest& manifest,
                            const ArtifactRecord& artifact,
                            std::string* error = nullptr);

bool publish_alea_handoff_manifest(const ArtifactStore& store,
                                   AleaHandoffManifest manifest,
                                   ArtifactRecord* artifact,
                                   std::string* error = nullptr);

} // namespace csgify
