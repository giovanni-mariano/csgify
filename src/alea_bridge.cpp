#include "csgify/alea_bridge.hpp"

#include "fnv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace csgify {
namespace {

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

Diagnostic bridge_diagnostic(DiagnosticSeverity severity,
                             std::string code,
                             std::string message,
                             std::string shape_id = {}) {
    Diagnostic result;
    result.severity = severity;
    result.code = std::move(code);
    result.message = std::move(message);
    result.shape_id = std::move(shape_id);
    return result;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

const char* severity_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::info: return "info";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::error: return "error";
    }
    return "error";
}

std::string digest_text(const std::string& value) {
    return "fnv1a128:" + fnv1a128_hex(fnv1a128(value.data(), value.size()));
}

std::vector<std::string> tagged_entity_ids(const char* comments,
                                           const char* inline_comment) {
    std::vector<std::string> result;
    const auto scan = [&](const char* text) {
        if (!text) return;
        const std::string value(text);
        const std::string prefix = "csgify:id=";
        std::size_t at = 0;
        while ((at = value.find(prefix, at)) != std::string::npos) {
            at += prefix.size();
            std::size_t end = at;
            while (end < value.size()
                   && value[end] != ';'
                   && value[end] != ','
                   && value[end] != '$'
                   && value[end] != '\n'
                   && value[end] != '\r') {
                ++end;
            }
            if (end > at) result.push_back(value.substr(at, end - at));
            at = end;
        }
    };
    scan(comments);
    scan(inline_comment);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

int transport_material_number(const std::string& id) {
    const std::size_t colon = id.find_last_of(':');
    const std::string number = colon == std::string::npos
        ? id : id.substr(colon + 1);
    if (number.empty()) return 0;
    char* end = nullptr;
    const long parsed = std::strtol(number.c_str(), &end, 10);
    return end && *end == '\0' && parsed > 0
        ? static_cast<int>(parsed) : 0;
}

const EntityMetadata* find_metadata(const ConversionReport& report,
                                    MetadataId id) {
    if (id == kInvalidMetadataId) return nullptr;
    const auto found = std::find_if(
        report.metadata.begin(), report.metadata.end(),
        [&](const EntityMetadata& value) { return value.id == id; });
    return found == report.metadata.end() ? nullptr : &*found;
}

void apply_metadata(AleaSidecarEntity* target,
                    const ConversionReport& report,
                    MetadataId id) {
    if (!target) return;
    const EntityMetadata* metadata = find_metadata(report, id);
    if (!metadata) return;
    const auto text = [&](StringId value) -> std::string {
        return report.strings.lookup(value);
    };
    target->stable_name = text(metadata->stable_name);
    target->display_name = text(metadata->display_name);
    target->description = text(metadata->description);
    target->engineering_role = text(metadata->engineering_role);
    target->subsystem = text(metadata->subsystem);
    target->source_cad_label = text(metadata->source_cad_label);
    target->source_component_path = text(metadata->source_component_path);
    for (StringId tag : metadata->tags) target->tags.push_back(text(tag));
    for (const Annotation& annotation : metadata->annotations) {
        target->annotations.push_back({
            text(annotation.key), text(annotation.value),
            annotation_origin_name(annotation.origin),
            metadata_review_state_name(annotation.review_state),
            annotation.user_verified,
        });
    }
}

AleaSidecarEntity* ensure_sidecar(
    std::vector<AleaSidecarEntity>* values,
    std::unordered_map<std::string, std::size_t>* indices,
    const std::string& id,
    const std::string& kind) {
    const auto found = indices->find(id);
    if (found != indices->end()) return &(*values)[found->second];
    AleaSidecarEntity value;
    value.entity_id = id;
    value.kind = kind;
    indices->emplace(id, values->size());
    values->push_back(std::move(value));
    return &values->back();
}

std::string serialize_manifest_impl(const AleaHandoffManifest& manifest) {
    std::ostringstream out;
    out << std::setprecision(17);
    out << "{\n"
        << "  \"schema\": \"" << json_escape(manifest.schema) << "\",\n"
        << "  \"schema_major\": " << manifest.schema_major << ",\n"
        << "  \"schema_minor\": " << manifest.schema_minor << ",\n"
        << "  \"producer\": \"" << json_escape(manifest.producer) << "\",\n"
        << "  \"producer_version\": \""
        << json_escape(manifest.producer_version) << "\",\n"
        << "  \"libalea_version\": \""
        << json_escape(manifest.libalea_version) << "\",\n"
        << "  \"source_digest\": \"" << json_escape(manifest.source_digest)
        << "\",\n"
        << "  \"policy_digest\": \"" << json_escape(manifest.policy_digest)
        << "\",\n"
        << "  \"contract_digest\": \""
        << json_escape(manifest.contract_digest) << "\",\n";
    const AleaBridgeCapabilities& c = manifest.capabilities;
    out << "  \"capabilities\": {\"provider\": \""
        << json_escape(c.provider) << "\", \"provider_version\": \""
        << json_escape(c.provider_version) << "\", \"schema_major\": "
        << c.schema_major << ", \"schema_minor\": " << c.schema_minor
        << ", \"boolean_cells\": " << (c.boolean_cells ? "true" : "false")
        << ", \"cell_comments\": " << (c.cell_comments ? "true" : "false")
        << ", \"universes\": " << (c.universes ? "true" : "false")
        << ", \"fills\": " << (c.fills ? "true" : "false")
        << ", \"transforms\": " << (c.transforms ? "true" : "false")
        << ", \"materials\": " << (c.materials ? "true" : "false")
        << ", \"void_generation\": " << (c.void_generation ? "true" : "false")
        << ", \"complete_coverage\": "
        << (c.complete_coverage ? "true" : "false")
        << ", \"structured_diagnostics\": "
        << (c.structured_diagnostics ? "true" : "false") << "},\n";

    out << "  \"cells\": [";
    for (std::size_t i = 0; i < manifest.cells.size(); ++i) {
        const AleaCellRecord& cell = manifest.cells[i];
        if (i) out << ',';
        out << "\n    {\"storage_index\": " << cell.storage_index
            << ", \"cell_id\": " << cell.cell_id
            << ", \"material_id\": " << cell.material_id
            << ", \"density\": " << cell.density
            << ", \"universe_id\": " << cell.universe_id
            << ", \"fill_universe\": " << cell.fill_universe
            << ", \"fill_transform\": " << cell.fill_transform
            << ", \"root_node\": " << cell.root_node
            << ", \"expression_digest\": \""
            << json_escape(cell.expression_digest) << "\", \"source_entity_ids\": [";
        for (std::size_t j = 0; j < cell.source_entity_ids.size(); ++j) {
            if (j) out << ',';
            out << "\"" << json_escape(cell.source_entity_ids[j]) << "\"";
        }
        out << "]}";
    }
    if (!manifest.cells.empty()) out << '\n';
    out << "  ],\n  \"surfaces\": [";
    for (std::size_t i = 0; i < manifest.surfaces.size(); ++i) {
        if (i) out << ',';
        const AleaSurfaceRecord& surface = manifest.surfaces[i];
        out << "{\"storage_index\": " << surface.storage_index
            << ", \"surface_id\": " << surface.surface_id
            << ", \"primitive_type\": " << surface.primitive_type << "}";
    }
    out << "],\n  \"universes\": [";
    for (std::size_t i = 0; i < manifest.universes.size(); ++i) {
        if (i) out << ',';
        const AleaUniverseRecord& universe = manifest.universes[i];
        out << "{\"storage_index\": " << universe.storage_index
            << ", \"universe_id\": " << universe.universe_id
            << ", \"cell_count\": " << universe.cell_count
            << ", \"bbox\": [" << universe.bbox.xmin << ','
            << universe.bbox.ymin << ',' << universe.bbox.zmin << ','
            << universe.bbox.xmax << ',' << universe.bbox.ymax << ','
            << universe.bbox.zmax << "]}";
    }
    out << "],\n  \"sidecar\": [";
    for (std::size_t i = 0; i < manifest.sidecar.size(); ++i) {
        if (i) out << ',';
        const AleaSidecarEntity& entity = manifest.sidecar[i];
        out << "\n    {\"entity_id\": \"" << json_escape(entity.entity_id)
            << "\", \"kind\": \"" << json_escape(entity.kind)
            << "\", \"definition_id\": \""
            << json_escape(entity.definition_id)
            << "\", \"instance_id\": \"" << json_escape(entity.instance_id)
            << "\", \"material_id\": \"" << json_escape(entity.material_id)
            << "\", \"alea_cell_ids\": [";
        for (std::size_t j = 0; j < entity.alea_cell_ids.size(); ++j) {
            if (j) out << ',';
            out << entity.alea_cell_ids[j];
        }
        out << "], \"stable_name\": \"" << json_escape(entity.stable_name)
            << "\", \"display_name\": \"" << json_escape(entity.display_name)
            << "\", \"description\": \"" << json_escape(entity.description)
            << "\", \"engineering_role\": \""
            << json_escape(entity.engineering_role)
            << "\", \"subsystem\": \"" << json_escape(entity.subsystem)
            << "\", \"source_cad_label\": \""
            << json_escape(entity.source_cad_label)
            << "\", \"source_component_path\": \""
            << json_escape(entity.source_component_path) << "\", \"tags\": [";
        for (std::size_t j = 0; j < entity.tags.size(); ++j) {
            if (j) out << ',';
            out << "\"" << json_escape(entity.tags[j]) << "\"";
        }
        out << "], \"annotations\": [";
        for (std::size_t j = 0; j < entity.annotations.size(); ++j) {
            if (j) out << ',';
            const AleaSidecarAnnotation& annotation = entity.annotations[j];
            out << "{\"key\": \"" << json_escape(annotation.key)
                << "\", \"value\": \"" << json_escape(annotation.value)
                << "\", \"origin\": \"" << json_escape(annotation.origin)
                << "\", \"review_state\": \""
                << json_escape(annotation.review_state)
                << "\", \"user_verified\": "
                << (annotation.user_verified ? "true" : "false") << "}";
        }
        out << "], \"has_transform\": "
            << (entity.has_transform ? "true" : "false")
            << ", \"transform\": [";
        for (std::size_t j = 0; j < entity.transform.values.size(); ++j) {
            if (j) out << ',';
            out << entity.transform.values[j];
        }
        out << "]}";
    }
    if (!manifest.sidecar.empty()) out << '\n';
    out << "  ],\n  \"chunks\": [";
    for (std::size_t i = 0; i < manifest.chunks.size(); ++i) {
        if (i) out << ',';
        const AleaChunkReference& chunk = manifest.chunks[i];
        out << "{\"digest_algorithm\": \""
            << json_escape(chunk.digest_algorithm) << "\", \"digest\": \""
            << json_escape(chunk.digest) << "\", \"kind\": \""
            << json_escape(chunk.kind) << "\", \"relative_path\": \""
            << json_escape(chunk.relative_path) << "\", \"size\": "
            << chunk.size << "}";
    }
    out << "],\n  \"diagnostics\": [";
    for (std::size_t i = 0; i < manifest.diagnostics.size(); ++i) {
        if (i) out << ',';
        const Diagnostic& diagnostic = manifest.diagnostics[i];
        out << "{\"severity\": \"" << severity_name(diagnostic.severity)
            << "\", \"code\": \"" << json_escape(diagnostic.code)
            << "\", \"message\": \"" << json_escape(diagnostic.message)
            << "\", \"shape_id\": \"" << json_escape(diagnostic.shape_id)
            << "\"}";
    }
    out << "]\n}\n";
    return out.str();
}

std::string canonical_digest(const AleaHandoffManifest& manifest) {
    AleaHandoffManifest canonical = manifest;
    canonical.contract_digest.clear();
    return digest_text(serialize_manifest_impl(canonical));
}

} // namespace

AleaBridgeCapabilities query_alea_bridge_capabilities() {
    AleaBridgeCapabilities result;
    result.provider_version = alea_version() ? alea_version() : "unknown";
    result.boolean_cells = true;
    result.cell_comments = true;
    result.universes = true;
    result.fills = true;
    result.transforms = true;
    result.materials = true;
    result.void_generation = true;
    result.complete_coverage = true;
    result.structured_diagnostics = true;
    return result;
}

bool register_alea_instance_transform(alea_system_t* system,
                                      int transform_id,
                                      const InstanceTransform& transform,
                                      std::string* error) {
    if (!system || transform_id <= 0) {
        set_error(error, "Alea transform registration requires a system and positive ID");
        return false;
    }
    // InstanceTransform is row-major [R|T]. libalea/MCNP stores the three
    // auxiliary basis vectors as columns after the displacement.
    const auto& m = transform.values;
    const double values[12] = {
        m[3], m[7], m[11],
        m[0], m[4], m[8],
        m[1], m[5], m[9],
        m[2], m[6], m[10],
    };
    if (alea_add_transform(system, transform_id, values, 12, 0) != 0) {
        set_error(error, alea_error()[0] ? alea_error()
                                         : "libalea rejected the instance transform");
        return false;
    }
    return true;
}

int add_alea_fill_instance(alea_system_t* system,
                           int cell_id,
                           alea_node_id_t container_root,
                           int parent_universe,
                           int fill_universe,
                           int transform_id,
                           const InstanceTransform& transform,
                           const std::string& source_entity_id,
                           std::string* error) {
    if (!system || container_root == ALEA_NODE_ID_INVALID
        || fill_universe < 0 || parent_universe < 0
        || source_entity_id.empty()) {
        set_error(error, "Alea fill instance has invalid identity or geometry");
        return -1;
    }
    if (!register_alea_instance_transform(system, transform_id, transform,
                                          error)) {
        return -1;
    }
    const int cell_index = alea_add_cell(
        system, cell_id, container_root, ALEA_MATERIAL_VOID, 0.0,
        parent_universe);
    if (cell_index < 0
        || alea_set_fill(system, cell_index, fill_universe, transform_id) != 0) {
        if (cell_index >= 0) alea_cell_remove(system, cell_index);
        set_error(error, alea_error()[0] ? alea_error()
                                         : "libalea rejected the fill instance");
        return -1;
    }
    const std::string comment = "csgify:id=" + source_entity_id;
    if (alea_cell_set_inline_comment(system, cell_index, comment.c_str()) != 0) {
        alea_cell_remove(system, cell_index);
        set_error(error, alea_error()[0] ? alea_error()
                                         : "libalea rejected the fill identity tag");
        return -1;
    }
    return cell_index;
}

AleaBridgeNegotiation negotiate_alea_bridge(
    const AleaBridgeCapabilities& capabilities,
    const AleaBridgeRequirements& requirements) {
    AleaBridgeNegotiation result;
    if (capabilities.schema_major != requirements.schema_major) {
        result.missing_capabilities.push_back("schema_major");
    } else if (capabilities.schema_minor < requirements.minimum_schema_minor) {
        result.missing_capabilities.push_back("schema_minor");
    }
    const auto require = [&](bool wanted, bool available, const char* name) {
        if (wanted && !available) result.missing_capabilities.push_back(name);
    };
    require(requirements.boolean_cells, capabilities.boolean_cells,
            "boolean_cells");
    require(requirements.cell_comments, capabilities.cell_comments,
            "cell_comments");
    require(requirements.universes, capabilities.universes, "universes");
    require(requirements.fills, capabilities.fills, "fills");
    require(requirements.transforms, capabilities.transforms, "transforms");
    require(requirements.materials, capabilities.materials, "materials");
    require(requirements.void_generation, capabilities.void_generation,
            "void_generation");
    require(requirements.complete_coverage, capabilities.complete_coverage,
            "complete_coverage");
    require(requirements.structured_diagnostics,
            capabilities.structured_diagnostics, "structured_diagnostics");
    result.compatible = result.missing_capabilities.empty();
    if (result.compatible) {
        result.diagnostics.push_back(bridge_diagnostic(
            DiagnosticSeverity::info, "alea_bridge_compatible",
            "The public libalea API satisfies the requested handoff capabilities."));
    } else {
        std::ostringstream message;
        message << "The public libalea API is missing:";
        for (const std::string& value : result.missing_capabilities) {
            message << ' ' << value;
        }
        result.diagnostics.push_back(bridge_diagnostic(
            DiagnosticSeverity::error, "alea_bridge_capability_missing",
            message.str()));
    }
    return result;
}

std::string alea_bridge_policy_digest(const ConversionOptions& options) {
    std::ostringstream value;
    value << std::setprecision(17)
          << options.linear_tolerance << '|'
          << options.angular_tolerance << '|'
          << options.default_material_id << '|'
          << options.default_density << '|'
          << options.require_convex_cells << '|'
          << options.split_union_cells << '|'
          << options.union_decomposed_pieces << '|'
          << options.allow_overlaps << '|'
          << options.overlap_volume_threshold << '|'
          << options.overlap_volume_fraction << '|'
          << options.mcnp_domain_id << '|'
          << options.require_domain_coverage;
    return digest_text(value.str());
}

AleaHandoffManifest make_alea_handoff_manifest(
    const alea_system_t* system,
    const ConversionReport& report,
    std::string source_digest,
    std::string policy_digest) {
    AleaHandoffManifest result;
    result.producer_version = "0.1.0";
    result.libalea_version = alea_version() ? alea_version() : "unknown";
    result.source_digest = std::move(source_digest);
    result.policy_digest = std::move(policy_digest);
    result.capabilities = query_alea_bridge_capabilities();
    if (!system) {
        result.diagnostics.push_back(bridge_diagnostic(
            DiagnosticSeverity::error, "alea_bridge_null_system",
            "Cannot build a handoff manifest from a null Alea system."));
        result.contract_digest = canonical_digest(result);
        return result;
    }

    std::unordered_map<std::string, std::size_t> sidecar_indices;
    std::unordered_map<int, std::string> material_ids;
    for (const ComponentDefinition& definition : report.component_definitions) {
        AleaSidecarEntity* entity = ensure_sidecar(
            &result.sidecar, &sidecar_indices, definition.id,
            "component_definition");
        entity->definition_id = definition.id;
        apply_metadata(entity, report, definition.metadata_id);
    }
    for (const ComponentInstance& instance : report.component_instances) {
        AleaSidecarEntity* entity = ensure_sidecar(
            &result.sidecar, &sidecar_indices, instance.id,
            "component_instance");
        entity->definition_id = instance.definition_id;
        entity->instance_id = instance.id;
        entity->material_id = instance.material_override;
        entity->transform = instance.transform;
        entity->has_transform = true;
        apply_metadata(entity, report, instance.metadata_id);
    }
    for (const MaterialDefinition& material : report.materials) {
        AleaSidecarEntity* entity = ensure_sidecar(
            &result.sidecar, &sidecar_indices, material.id, "material");
        entity->material_id = material.id;
        apply_metadata(entity, report, material.metadata_id);
        const int number = transport_material_number(material.id);
        if (number > 0) material_ids.emplace(number, material.id);
    }

    std::unordered_map<std::string, const AssemblyNode*> assembly_by_shape;
    for (const AssemblyNode& node : report.assembly) {
        if (!node.shape_id.empty()) assembly_by_shape.emplace(node.shape_id, &node);
    }

    const std::size_t cell_count = alea_cell_count(system);
    result.cells.reserve(cell_count);
    for (std::size_t index = 0; index < cell_count; ++index) {
        alea_cell_info_t info{};
        if (alea_cell_get_info(system, index, &info) != 0) {
            result.diagnostics.push_back(bridge_diagnostic(
                DiagnosticSeverity::error, "alea_bridge_cell_query_failed",
                alea_error()[0] ? alea_error() : "Failed to query an Alea cell."));
            continue;
        }
        AleaCellRecord cell;
        cell.storage_index = index;
        cell.cell_id = info.cell_id;
        cell.material_id = info.material_id;
        cell.density = info.density;
        cell.universe_id = info.universe_id;
        // libalea uses non-positive fill_universe values for "no fill" in
        // traversal. Normalize that implementation sentinel to the bridge
        // schema's explicit -1 so universe 0 material cells are not mistaken
        // for instances.
        cell.fill_universe = info.fill_universe > 0 ? info.fill_universe : -1;
        cell.fill_transform = info.fill_transform;
        cell.root_node = static_cast<std::uint64_t>(info.root);
        cell.source_entity_ids = tagged_entity_ids(info.comments,
                                                   info.inline_comment);
        char* expression = alea_cell_expr(system, index, ":", "&", "#");
        if (expression) {
            cell.expression_digest = digest_text(expression);
            std::free(expression);
        }
        const auto semantic_material = material_ids.find(cell.material_id);
        const std::string material_id = semantic_material == material_ids.end()
            ? "m" + std::to_string(cell.material_id)
            : semantic_material->second;
        if (semantic_material != material_ids.end()) {
            AleaSidecarEntity* material = ensure_sidecar(
                &result.sidecar, &sidecar_indices, material_id, "material");
            material->alea_cell_ids.push_back(cell.cell_id);
        }
        for (const std::string& entity_id : cell.source_entity_ids) {
            AleaSidecarEntity* entity = ensure_sidecar(
                &result.sidecar, &sidecar_indices, entity_id, "geometry");
            entity->alea_cell_ids.push_back(cell.cell_id);
            entity->material_id = material_id;
            const auto assembly = assembly_by_shape.find(entity_id);
            if (assembly != assembly_by_shape.end()) {
                entity->definition_id = assembly->second->definition_id;
                entity->instance_id = assembly->second->instance_id;
                entity->transform = assembly->second->transform;
                entity->has_transform = !assembly->second->instance_id.empty();
                apply_metadata(entity, report, assembly->second->metadata_id);
            }
        }
        result.cells.push_back(std::move(cell));
    }

    const std::size_t surface_count = alea_surface_count(system);
    result.surfaces.reserve(surface_count);
    for (std::size_t index = 0; index < surface_count; ++index) {
        int surface_id = 0;
        alea_primitive_type_t type{};
        if (alea_surface_get(system, index, &surface_id, &type,
                             nullptr, nullptr, nullptr) != 0) {
            result.diagnostics.push_back(bridge_diagnostic(
                DiagnosticSeverity::error, "alea_bridge_surface_query_failed",
                alea_error()[0] ? alea_error() : "Failed to query an Alea surface."));
            continue;
        }
        result.surfaces.push_back(
            {index, surface_id, static_cast<int>(type)});
    }

    const std::size_t universe_count = alea_universe_count(system);
    result.universes.reserve(universe_count);
    for (std::size_t index = 0; index < universe_count; ++index) {
        int universe_id = 0;
        std::size_t universe_cells = 0;
        alea_bbox_t bbox{};
        if (alea_universe_get(system, index, &universe_id, &universe_cells,
                              &bbox) != 0) {
            result.diagnostics.push_back(bridge_diagnostic(
                DiagnosticSeverity::error, "alea_bridge_universe_query_failed",
                alea_error()[0] ? alea_error() : "Failed to query an Alea universe."));
            continue;
        }
        AleaUniverseRecord universe;
        universe.storage_index = index;
        universe.universe_id = universe_id;
        universe.cell_count = universe_cells;
        universe.bbox = {bbox.min_x, bbox.min_y, bbox.min_z,
                         bbox.max_x, bbox.max_y, bbox.max_z, true};
        result.universes.push_back(std::move(universe));
    }

    for (AleaSidecarEntity& entity : result.sidecar) {
        std::sort(entity.alea_cell_ids.begin(), entity.alea_cell_ids.end());
        entity.alea_cell_ids.erase(
            std::unique(entity.alea_cell_ids.begin(), entity.alea_cell_ids.end()),
            entity.alea_cell_ids.end());
    }
    result.diagnostics.push_back(bridge_diagnostic(
        DiagnosticSeverity::info, "alea_handoff_manifest_created",
        "Created a versioned Alea handoff manifest with stable semantic sidecar mappings."));
    result.contract_digest = canonical_digest(result);
    return result;
}

bool validate_alea_handoff_manifest(const AleaHandoffManifest& manifest,
                                    std::string* error) {
    if (manifest.schema != kAleaHandoffSchema) {
        set_error(error, "unsupported Alea handoff schema: " + manifest.schema);
        return false;
    }
    if (manifest.schema_major != kAleaHandoffSchemaMajor
        || manifest.schema_minor > kAleaHandoffSchemaMinor) {
        set_error(error, "unsupported Alea handoff schema version");
        return false;
    }
    if (manifest.libalea_version.empty()) {
        set_error(error, "Alea handoff is missing the provider version");
        return false;
    }
    std::set<int> cell_ids;
    std::set<int> universe_ids;
    for (const AleaUniverseRecord& universe : manifest.universes) {
        if (!universe_ids.insert(universe.universe_id).second) {
            set_error(error, "duplicate Alea universe ID");
            return false;
        }
    }
    for (const AleaCellRecord& cell : manifest.cells) {
        if (!cell_ids.insert(cell.cell_id).second) {
            set_error(error, "duplicate Alea cell ID");
            return false;
        }
        if (cell.expression_digest.empty()) {
            set_error(error, "Alea cell is missing its expression digest");
            return false;
        }
        if (cell.fill_universe >= 0
            && universe_ids.find(cell.fill_universe) == universe_ids.end()) {
            set_error(error, "Alea cell references an undefined fill universe");
            return false;
        }
    }
    std::set<std::string> entity_ids;
    for (const AleaSidecarEntity& entity : manifest.sidecar) {
        if (entity.entity_id.empty()
            || !entity_ids.insert(entity.entity_id).second) {
            set_error(error, "empty or duplicate Alea sidecar entity ID");
            return false;
        }
        for (int cell_id : entity.alea_cell_ids) {
            if (cell_ids.find(cell_id) == cell_ids.end()) {
                set_error(error, "Alea sidecar references an undefined cell");
                return false;
            }
        }
    }
    std::set<std::pair<std::string, std::string>> chunks;
    for (const AleaChunkReference& chunk : manifest.chunks) {
        if (chunk.digest_algorithm.empty() || chunk.digest.empty()
            || chunk.kind.empty() || chunk.relative_path.empty()
            || chunk.size == 0) {
            set_error(error, "Alea handoff contains an incomplete chunk reference");
            return false;
        }
        if (!chunks.emplace(chunk.kind, chunk.digest).second) {
            set_error(error, "Alea handoff contains a duplicate chunk reference");
            return false;
        }
    }
    if (manifest.contract_digest.empty()
        || manifest.contract_digest != canonical_digest(manifest)) {
        set_error(error, "Alea handoff contract digest does not match its content");
        return false;
    }
    return true;
}

std::string serialize_alea_handoff_manifest(const AleaHandoffManifest& manifest) {
    return serialize_manifest_impl(manifest);
}

bool add_alea_handoff_chunk(AleaHandoffManifest& manifest,
                            const ArtifactRecord& artifact,
                            std::string* error) {
    if (artifact.digest_algorithm.empty() || artifact.digest.empty()
        || artifact.kind.empty() || artifact.relative_path.empty()
        || artifact.size == 0) {
        set_error(error, "cannot attach an incomplete artifact record");
        return false;
    }
    const auto duplicate = std::find_if(
        manifest.chunks.begin(), manifest.chunks.end(),
        [&](const AleaChunkReference& chunk) {
            return chunk.kind == artifact.kind && chunk.digest == artifact.digest;
        });
    if (duplicate == manifest.chunks.end()) {
        manifest.chunks.push_back({artifact.digest_algorithm, artifact.digest,
                                   artifact.kind, artifact.relative_path,
                                   artifact.size});
    }
    manifest.contract_digest = canonical_digest(manifest);
    return true;
}

bool publish_alea_handoff_manifest(const ArtifactStore& store,
                                   AleaHandoffManifest manifest,
                                   ArtifactRecord* artifact,
                                   std::string* error) {
    manifest.contract_digest = canonical_digest(manifest);
    if (!validate_alea_handoff_manifest(manifest, error)) return false;
    return store.publish_bytes("alea_handoff_manifest", "json",
                               serialize_alea_handoff_manifest(manifest),
                               artifact, error);
}

} // namespace csgify
