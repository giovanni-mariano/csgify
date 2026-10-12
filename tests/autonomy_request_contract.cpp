#include "csgify/autonomy.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int fail(const std::string& message) {
    std::cerr << "autonomy_request_contract: " << message << '\n';
    return EXIT_FAILURE;
}

bool rejects(const std::string& json) {
    try {
        (void)csgify::parse_autonomy_request_json(json);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

} // namespace

int main() {
    const std::string valid = R"({
      "schema":"csgify.autonomy-actions",
      "schema_version":2,
      "request_id":"request:contract-valid",
      "source_digest":"source-digest",
      "expected_revision":7,
      "actor":"llm:contract-test",
      "actions":[{
        "id":"proposal-1",
        "op":"shatter_with_approximation",
        "target":"solid:1",
        "parameters":{"max_depth":12},
        "rationale":"bounded fallback"
      }]
    })";
    const csgify::AutonomyRequest request =
        csgify::parse_autonomy_request_json(valid);
    if (request.actions.size() != 1
        || request.request_id != "request:contract-valid"
        || request.source_digest != "source-digest"
        || request.expected_revision != 7
        || request.actions[0].parameters.shatter_max_depth != 12)
        return fail("valid request did not preserve normalized arguments");
    const std::string digest = csgify::autonomy_request_digest(request);
    if (digest.empty()) return fail("valid request has no identity digest");
    csgify::AutonomyRequest changed = request;
    ++changed.expected_revision;
    if (csgify::autonomy_request_digest(changed) == digest)
        return fail("request digest did not bind expected_revision");

    const std::vector<std::string> malformed = {
        R"({"schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":1,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":-1,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":1e30,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[null]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"extra":true,"actions":[{"id":"a","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","id":"b","op":"remove_solid","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"same","op":"remove_solid","target":"solid:1"},{"id":"same","op":"remove_solid","target":"solid:2"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1","unexpected":true}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"apply_decomposition_candidate","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1","candidate":"solid:2"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"decompose_to_fixed_point","target":"solid:1"}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"remove_solid","target":"solid:1","parameters":{}}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"shatter_with_approximation","target":"solid:1","parameters":{"unknown":2}}]})",
        R"({"schema":"csgify.autonomy-actions","schema_version":2,"request_id":"r","source_digest":"s","expected_revision":0,"actions":[{"id":"a","op":"apply_contained_medium","medium_request":{"id":"m","unknown":true}}]})"
    };
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        if (!rejects(malformed[i]))
            return fail("malformed request accepted at case "
                        + std::to_string(i + 1));
    }

    csgify::AutonomyProjectPolicy policy;
    policy.project_id = "policy-roundtrip";
    policy.intended_analysis = "shielding \"prototype\"";
    policy.preparation.heal_shape = false;
    policy.preparation.unit_override = "cm";
    policy.preparation.unit_scale_to_mm_override = 10.0;
    policy.preparation.normalize_to_unit = "mm";
    policy.authority.mode = csgify::AutonomyMode::automatic;
    policy.authority.allow_removal = true;
    csgify::MaterialDefinition material;
    material.id = "water";
    material.phase = csgify::MaterialPhase::liquid;
    material.density = 0.998;
    policy.authority.contained_medium_materials.push_back(material);
    policy.authority.approximation.enabled = true;
    policy.authority.approximation.max_global_changed_volume_fraction = 0.01;
    policy.authority.approximation.max_body_changed_volume_fraction = 0.02;
    policy.authority.approximation.max_action_changed_volume_fraction = 0.003;
    policy.authority.approximation.max_surface_deviation = 0.04;
    policy.authority.approximation.allow_overlap_resolution = true;
    policy.authority.approximation.protected_target_ids = {"solid:1", "face:2.3"};
    policy.authority.approximation.protected_material_ids = {7, 11};
    const std::filesystem::path policy_path =
        std::filesystem::temp_directory_path()
        / ("csgify-policy-roundtrip-" + std::to_string(::getpid()) + ".json");
    std::error_code remove_error;
    std::filesystem::remove(policy_path, remove_error);
    if (csgify::write_autonomy_project_policy_json(
            policy_path.string(), policy) != 0)
        return fail("complete project policy writer failed");
    const csgify::AutonomyProjectPolicy restored =
        csgify::read_autonomy_project_policy_json(policy_path.string());
    if (restored.project_id != policy.project_id
        || restored.intended_analysis != policy.intended_analysis
        || restored.preparation.heal_shape
        || restored.preparation.unit_override != "cm"
        || restored.preparation.unit_scale_to_mm_override != 10.0
        || restored.preparation.normalize_to_unit != "mm"
        || restored.authority.mode != csgify::AutonomyMode::automatic
        || !restored.authority.allow_removal
        || restored.authority.contained_medium_materials.size() != 1
        || restored.authority.contained_medium_materials[0].id != "water"
        || !restored.authority.approximation.enabled
        || restored.authority.approximation.max_global_changed_volume_fraction != 0.01
        || !restored.authority.approximation.allow_overlap_resolution
        || restored.authority.approximation.protected_target_ids
            != policy.authority.approximation.protected_target_ids
        || restored.authority.approximation.protected_material_ids
            != policy.authority.approximation.protected_material_ids)
        return fail("complete project policy did not round-trip");
    csgify::AutonomyProjectPolicy invalid_policy = policy;
    invalid_policy.intended_analysis.clear();
    if (csgify::write_autonomy_project_policy_json(
            policy_path.string(), invalid_policy) == 0)
        return fail("invalid project policy was written");
    if (csgify::read_autonomy_project_policy_json(policy_path.string())
            .intended_analysis != policy.intended_analysis)
        return fail("invalid write replaced the previous project policy");
    std::filesystem::remove(policy_path, remove_error);
    return EXIT_SUCCESS;
}
