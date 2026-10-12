#pragma once

#include "csgify/assembly.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace csgify {

enum class MediumFillMode { full, level, bounded_segment };

enum class MediumPortDisposition {
    connect_to_port,
    cap_here,
    open_to_domain,
    interface_to_external_model,
    unresolved,
};

enum class GeometricSeedKind { point, face };

enum class MediumValveState { not_applicable, open, closed, unresolved };

enum class MediumJunctionKind {
    straight,
    reducer,
    elbow,
    tee,
    manifold,
    unknown,
};

struct GeometricSeed {
    GeometricSeedKind kind = GeometricSeedKind::point;
    std::array<double, 3> point{{0.0, 0.0, 0.0}};
    std::string entity_id;
};

struct MediumPortDecision {
    std::string port_id;
    MediumPortDisposition disposition = MediumPortDisposition::unresolved;
    std::string peer_port_id;
    MetadataId metadata_id = kInvalidMetadataId;
    MediumValveState valve_state = MediumValveState::not_applicable;
};

struct MediumGenerationRequest {
    std::string id;
    MetadataId metadata_id = kInvalidMetadataId;
    std::vector<ComponentInstanceId> host_component_ids;
    std::vector<GeometricSeed> seed_points_or_faces;
    MaterialId material_id;
    // Required for partial liquid fills when the complementary gas volume is
    // to be emitted as a material cell. If absent, the gas complement is
    // certified and reported but is not emitted as material.
    MaterialId complement_material_id;
    MediumFillMode fill_mode = MediumFillMode::full;
    std::array<double, 3> gravity_direction{{0.0, 0.0, -1.0}};
    std::optional<double> fill_level;
    std::optional<double> target_volume_fraction;
    std::vector<MediumPortDecision> ports;
    std::vector<ComponentInstanceId> internal_components_to_subtract;
    bool allow_disconnected_pockets = false;
    std::size_t max_worker_count = 1;
    bool require_exact_geometry = true;
};

struct MediumNetworkPort {
    std::string id;
    ComponentInstanceId host_component_id;
    std::string local_port_id;
    std::array<double, 3> center{{0.0, 0.0, 0.0}};
    std::array<double, 3> normal{{0.0, 0.0, 1.0}};
    double radius = 0.0;
    MediumPortDisposition disposition = MediumPortDisposition::unresolved;
    std::string peer_port_id;
    MediumValveState valve_state = MediumValveState::not_applicable;
};

struct MediumNetworkSegment {
    std::string id;
    ComponentInstanceId host_component_id;
    std::string source_shape_id;
    MediumJunctionKind kind = MediumJunctionKind::unknown;
    bool conical = false;
    std::array<double, 3> base{{0.0, 0.0, 0.0}};
    std::array<double, 3> height{{0.0, 0.0, 0.0}};
    double radius = 0.0;
    double top_radius = 0.0;
    std::vector<std::string> port_ids;
};

struct MediumNetworkTopology {
    std::vector<MediumNetworkSegment> segments;
    std::vector<MediumNetworkPort> ports;
    std::size_t connection_count = 0;
    std::size_t connected_component_count = 0;
    std::size_t reducer_count = 0;
    std::size_t elbow_count = 0;
    std::size_t tee_count = 0;
    std::size_t manifold_count = 0;
};

struct MediumRequestValidation {
    bool valid = false;
    std::vector<std::string> error_codes;
};

struct DerivedRegionCertificate {
    bool solid_valid = false;
    bool closed = false;
    bool leak_test_passed = false;
    bool overlap_test_passed = false;
    std::size_t unresolved_port_count = 0;
    double generated_volume = 0.0;
    double material_overlap_volume = 0.0;
    std::size_t network_segment_count = 0;
    std::size_t network_connection_count = 0;
    std::size_t network_connected_component_count = 0;
    bool obstacle_subtraction_passed = false;
    bool phase_coverage_passed = false;
    bool protected_interface_passed = false;
    double liquid_volume = 0.0;
    double gas_volume = 0.0;
    double phase_coverage_delta = 0.0;
    double phase_overlap_volume = 0.0;
    std::size_t liquid_pocket_count = 0;
    std::size_t gas_pocket_count = 0;
    std::size_t worker_count_used = 0;
};

struct DerivedRegionRecord {
    std::string region_id;
    std::string request_id;
    StringId operation = kInvalidStringId;
    std::vector<ComponentInstanceId> host_components;
    MaterialId material_id;
    std::vector<std::string> connected_ports;
    std::vector<std::string> capped_ports;
    std::vector<ComponentInstanceId> subtracted_components;
    DerivedRegionCertificate certificate;
    bool user_approved = false;
    MetadataId metadata_id = kInvalidMetadataId;
};

MediumRequestValidation validate_medium_generation_request(
    const MediumGenerationRequest& request,
    const std::vector<MaterialDefinition>& materials);

const char* medium_fill_mode_name(MediumFillMode mode);
const char* medium_port_disposition_name(MediumPortDisposition disposition);
const char* geometric_seed_kind_name(GeometricSeedKind kind);
const char* medium_valve_state_name(MediumValveState state);
const char* medium_junction_kind_name(MediumJunctionKind kind);

} // namespace csgify
