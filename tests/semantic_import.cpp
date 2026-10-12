#include "csgify/session.hpp"
#include "csgify/project_store.hpp"

#include <cassert>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    auto first_session = csgify::create_session();
    const csgify::DocumentId first_document =
        csgify::load_step(*first_session, argv[1]);
    const csgify::ConversionReport first =
        csgify::analyze(*first_session, first_document).report;
    auto second_session = csgify::create_session();
    const csgify::DocumentId second_document =
        csgify::load_step(*second_session, argv[1]);
    const csgify::ConversionReport second =
        csgify::analyze(*second_session, second_document).report;

    assert(!first.component_definitions.empty());
    assert(!first.component_instances.empty());
    assert(!first.metadata.empty());
    assert(first.materials.size() == 1);
    assert(first.materials.front().id == "material:1");

    const csgify::ComponentDefinition& definition =
        first.component_definitions.front();
    assert(!definition.id.empty());
    assert(!definition.geometry_digest.empty());
    assert(definition.geometry_digest_verified);
    assert(definition.source_definition_key != csgify::kInvalidStringId);

    const csgify::ComponentInstance& instance = first.component_instances.front();
    assert(instance.definition_id == definition.id);
    assert(instance.metadata_id != csgify::kInvalidMetadataId);
    assert(!first.strings.lookup(
        first.metadata.at(instance.metadata_id - 1).source_component_path).empty());
    assert(first.solids.front().component_instance_id == instance.id);
    assert(first.solids.front().component_definition_id == definition.id);

    assert(second.component_definitions.front().id == definition.id);
    assert(second.component_definitions.front().geometry_digest
           == definition.geometry_digest);
    assert(second.component_instances.front().id == instance.id);

    const std::vector<csgify::SpatialEntry> spatial =
        csgify::spatial_entries_from_report(first);
    assert(spatial.size() == 1);
    assert(spatial.front().entity_id == first.solids.front().id);
    assert(spatial.front().owner_id == instance.id);
    assert(spatial.front().bounds.valid);

    const std::string json = csgify::report_json_string(first);
    assert(json.find("\"semantic_model\"") != std::string::npos);
    assert(json.find(definition.geometry_digest) != std::string::npos);
    return 0;
}
