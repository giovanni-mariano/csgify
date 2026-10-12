#include "csgify/alea_bridge.hpp"

#include <alea.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

using System = std::unique_ptr<alea_system_t, decltype(&alea_destroy)>;

int fail(const std::string& message) {
    std::cerr << "alea_bridge: " << message << '\n';
    return EXIT_FAILURE;
}

System make_system() {
    System system(alea_create(), &alea_destroy);
    const int surface = alea_sphere_surface(system.get(), 17, 0, 0, 0, 4);
    const alea_node_id_t root = alea_halfspace(system.get(), surface, -1);
    const int material = alea_add_material(system.get(), 7);
    const int cell = alea_add_cell(system.get(), 41, root, material, -7.8, 0);
    if (surface < 0 || root == ALEA_NODE_ID_INVALID || material < 0 || cell < 0
        || alea_cell_set_inline_comment(
               system.get(), cell,
               "component shell; csgify:id=solid:1") != 0
        || alea_build_universe_index(system.get()) != 0) {
        return System(nullptr, &alea_destroy);
    }
    return system;
}

csgify::ConversionReport make_report() {
    csgify::ConversionReport report;
    csgify::EntityMetadata metadata;
    metadata.id = 1;
    metadata.stable_name = report.strings.intern("shield.panel.1");
    metadata.display_name = report.strings.intern("Shield panel α");
    metadata.description = report.strings.intern("ITER shielding component");
    metadata.engineering_role = report.strings.intern("neutron shielding");
    metadata.subsystem = report.strings.intern("port plug");
    metadata.tags.push_back(report.strings.intern("qualified"));
    csgify::Annotation annotation;
    annotation.key = report.strings.intern("owner");
    annotation.value = report.strings.intern("blanket team");
    annotation.origin = csgify::AnnotationOrigin::user;
    annotation.review_state = csgify::MetadataReviewState::approved;
    annotation.user_verified = true;
    metadata.annotations.push_back(annotation);
    report.metadata.push_back(metadata);

    csgify::ComponentDefinition definition;
    definition.id = "definition:shield";
    definition.geometry_digest = "fnv1a128:definition";
    definition.geometry_digest_verified = true;
    definition.metadata_id = 1;
    report.component_definitions.push_back(definition);

    csgify::ComponentInstance instance;
    instance.id = "instance:shield:1";
    instance.definition_id = definition.id;
    instance.source_shape_id = "solid:1";
    instance.metadata_id = 1;
    instance.transform.values[3] = 125.0;
    report.component_instances.push_back(instance);

    csgify::AssemblyNode node;
    node.id = "asm:1";
    node.shape_id = "solid:1";
    node.definition_id = definition.id;
    node.instance_id = instance.id;
    node.metadata_id = 1;
    node.transform = instance.transform;
    report.assembly.push_back(node);

    csgify::MaterialDefinition material;
    material.id = "material:7";
    material.metadata_id = 1;
    material.phase = csgify::MaterialPhase::solid;
    material.density = 7.8;
    report.materials.push_back(material);
    return report;
}

const csgify::AleaSidecarEntity* find_sidecar(
    const csgify::AleaHandoffManifest& manifest,
    const std::string& id) {
    const auto found = std::find_if(
        manifest.sidecar.begin(), manifest.sidecar.end(),
        [&](const csgify::AleaSidecarEntity& entity) {
            return entity.entity_id == id;
        });
    return found == manifest.sidecar.end() ? nullptr : &*found;
}

} // namespace

int main() {
    const csgify::AleaBridgeCapabilities capabilities =
        csgify::query_alea_bridge_capabilities();
    if (!capabilities.boolean_cells || !capabilities.cell_comments
        || !capabilities.universes || !capabilities.fills
        || !capabilities.materials || !capabilities.void_generation
        || !capabilities.complete_coverage
        || !capabilities.structured_diagnostics) {
        return fail("required public libalea capabilities were not detected");
    }
    if (!capabilities.transforms) {
        return fail("public transform construction capability was not detected");
    }
    if (!csgify::negotiate_alea_bridge(capabilities).compatible) {
        return fail("baseline bridge negotiation failed");
    }
    csgify::AleaBridgeRequirements instance_requirements;
    instance_requirements.transforms = true;
    const auto instance_negotiation = csgify::negotiate_alea_bridge(
        capabilities, instance_requirements);
    if (!instance_negotiation.compatible
        || !instance_negotiation.missing_capabilities.empty()) {
        return fail("instance bridge capability negotiation failed");
    }

    System system = make_system();
    if (!system) return fail("could not construct the reference Alea system");
    const csgify::ConversionReport report = make_report();
    if (alea_cell_set_universe(system.get(), 0, 10) != 0) {
        return fail("could not move the definition cell into universe 10");
    }
    const int container_surface = alea_sphere_surface(
        system.get(), 18, 125, 0, 0, 4);
    const alea_node_id_t container_root = alea_halfspace(
        system.get(), container_surface, -1);
    std::string error;
    const int fill_cell = csgify::add_alea_fill_instance(
        system.get(), 61, container_root, 0, 10, 3,
        report.component_instances.front().transform,
        "instance:shield:1", &error);
    if (fill_cell < 0 || alea_build_universe_index(system.get()) != 0) {
        return fail("could not create a transformed universe fill: " + error);
    }
    csgify::ConversionOptions options;
    csgify::AleaHandoffManifest manifest =
        csgify::make_alea_handoff_manifest(
            system.get(), report, "fnv1a128:source",
            csgify::alea_bridge_policy_digest(options));
    if (!csgify::validate_alea_handoff_manifest(manifest, &error)) {
        return fail("fresh manifest failed validation: " + error);
    }
    if (manifest.cells.size() != 2 || manifest.surfaces.size() != 2
        || manifest.universes.size() != 2
        || manifest.cells.front().source_entity_ids
               != std::vector<std::string>{"solid:1"}) {
        return fail("final Alea inventory or identity tag was not retained");
    }
    const csgify::AleaSidecarEntity* solid = find_sidecar(manifest, "solid:1");
    if (!solid || solid->alea_cell_ids != std::vector<int>{41}
        || solid->definition_id != "definition:shield"
        || solid->instance_id != "instance:shield:1"
        || solid->material_id != "material:7"
        || solid->display_name != "Shield panel α"
        || solid->annotations.size() != 1
        || !solid->annotations.front().user_verified
        || !solid->has_transform || solid->transform.values[3] != 125.0) {
        return fail("resolved semantic sidecar mapping is incomplete");
    }
    const csgify::AleaSidecarEntity* material =
        find_sidecar(manifest, "material:7");
    if (!material || material->alea_cell_ids != std::vector<int>{41}) {
        return fail("semantic material-to-cell mapping is incomplete");
    }
    const csgify::AleaSidecarEntity* occurrence =
        find_sidecar(manifest, "instance:shield:1");
    if (!occurrence || occurrence->alea_cell_ids != std::vector<int>{61}) {
        return fail("transformed fill occurrence was not bound in the sidecar");
    }
    const auto fill = std::find_if(
        manifest.cells.begin(), manifest.cells.end(),
        [](const csgify::AleaCellRecord& cell) { return cell.cell_id == 61; });
    if (fill == manifest.cells.end() || fill->fill_universe != 10
        || fill->fill_transform != 3 || fill->universe_id != 0) {
        return fail("universe/fill/transform mapping was not retained");
    }

    System clone(alea_clone(system.get()), &alea_destroy);
    if (!clone || alea_build_universe_index(clone.get()) != 0) {
        return fail("libalea clone failed");
    }
    const csgify::AleaHandoffManifest cloned =
        csgify::make_alea_handoff_manifest(
            clone.get(), report, "fnv1a128:source",
            csgify::alea_bridge_policy_digest(options));
    if (manifest.contract_digest != cloned.contract_digest) {
        return fail("clone round trip changed the deterministic contract digest");
    }

    csgify::AleaHandoffManifest corrupt = manifest;
    corrupt.cells.front().material_id = 99;
    if (csgify::validate_alea_handoff_manifest(corrupt, nullptr)) {
        return fail("content mutation did not invalidate the contract digest");
    }
    corrupt = manifest;
    corrupt.schema_major += 1;
    if (csgify::validate_alea_handoff_manifest(corrupt, nullptr)) {
        return fail("unsupported schema major version was accepted");
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "csgify-alea-bridge-test";
    std::filesystem::remove_all(root);
    csgify::ArtifactStore store(root);
    if (!store.initialize(&error)) return fail(error);
    csgify::ArtifactRecord geometry;
    const std::string large_geometry(1024 * 1024, 'G');
    if (!store.publish_bytes("alea_geometry_chunk", "bin", large_geometry,
                             &geometry, &error)
        || !csgify::add_alea_handoff_chunk(manifest, geometry, &error)
        || !csgify::validate_alea_handoff_manifest(manifest, &error)) {
        return fail("content-addressed chunk attachment failed: " + error);
    }
    const std::string serialized =
        csgify::serialize_alea_handoff_manifest(manifest);
    if (serialized.size() >= large_geometry.size() / 8
        || serialized.find(geometry.digest) == std::string::npos
        || serialized.find("Shield panel α") == std::string::npos) {
        return fail("manifest copied large geometry or lost sidecar/chunk data");
    }
    csgify::ArtifactRecord published;
    if (!csgify::publish_alea_handoff_manifest(
            store, manifest, &published, &error)
        || !store.validate(published, &error)) {
        return fail("manifest artifact publication failed: " + error);
    }
    std::filesystem::remove_all(root);
    return EXIT_SUCCESS;
}
