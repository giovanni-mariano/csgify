#include "operation_registry.hpp"

namespace csgify {

const std::vector<OperationDescriptor>& operation_registry() {
    static const std::vector<OperationDescriptor> registry = {
        {"load_step", "document", OperationTargetKind::document, false, false, 1, "", "", false, ReplacementOperation::bbox},
        {"define_material", "material", OperationTargetKind::document, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"stage_contained_medium_request", "contained_medium", OperationTargetKind::document, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"apply_contained_medium", "contained_medium", OperationTargetKind::document, true, false, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::apply_contained_medium,
         R"({"medium_request":{"type":"object","required":true}})", true, true, true,
         "resolve_ports_material_or_source"},
        {"emit_mcnp", "output", OperationTargetKind::document, false, false, 1, "", "", false, ReplacementOperation::bbox},
        {"export_step", "output", OperationTargetKind::document, false, false, 1, "", "", false, ReplacementOperation::bbox},
        {"undo_last_mutation", "revision", OperationTargetKind::document, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"auto_fit_splines", "face", OperationTargetKind::document, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_to_fixed_point", "decomposition", OperationTargetKind::document, true, false, 1, "--fixed-point", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::decompose_to_fixed_point,
         R"({})", true, true, true, "review_residual_or_run_bounded_shatter"},
        {"fill_hole", "solid", OperationTargetKind::document, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"remove_solid", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::remove_solid,
         R"({"target":{"kind":"solid","required":true}})", true, true, true,
         "retain_component_or_confirm_removal"},
        {"remove_residual_with_approximation", "solid", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::remove_residual_with_approximation,
         R"({"target":{"kind":"piece","required":true}})", true, true, true,
         "retain_residual_or_adjust_human_budget"},
        {"resolve_overlap_with_approximation", "overlap_ownership", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::resolve_overlap_with_approximation,
         R"({"target":{"kind":"solid","required":true},"candidate":{"kind":"owner_solid","required":true}})", true, true, true,
         "choose_owner_or_repair_source"},
        {"decompose_planar", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_prismatic", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_revolution", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"reuse_definition_conversion", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_shell_difference", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_pipe_annulus", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_by_face", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_by_plane", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"decompose_by_planes", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"shatter", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox},
        {"apply_decomposition_candidate", "solid", OperationTargetKind::solid, true, false, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::apply_decomposition_candidate,
         R"({"target":{"kind":"solid","required":true},"candidate":{"kind":"decomposition_candidate","required":true}})", true, true, true,
         "choose_alternative_or_repair_source"},
        {"apply_surface_replacement_candidate", "face", OperationTargetKind::face, true, true, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::apply_surface_replacement_candidate,
         R"({"target":{"kind":"face","required":true},"candidate":{"kind":"surface_candidate","required":true}})", true, true, true,
         "choose_alternative_or_repair_source"},
        {"apply_budgeted_surface_replacement_candidate", "face", OperationTargetKind::face, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"replace_face_with_fit", "face", OperationTargetKind::face, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"apply_replacement_operation", "solid", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"apply_checked_replacement_operation", "solid", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"apply_budgeted_replacement_operation", "solid", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox},
        {"shatter_with_approximation", "solid", OperationTargetKind::solid, true, true, 1, "", "", false, ReplacementOperation::bbox,
         AutonomyOperationKind::shatter_with_approximation,
         R"({"target":{"kind":"solid_or_piece","required":true},"parameters":{"max_depth":{"type":"integer","minimum":1,"maximum":64,"default":8}}})",
         true, true, true, "adjust_human_budget_or_repair_source"},
        {"replace_solid_with_bbox", "solid", OperationTargetKind::solid, true, true, 1, "--replace-solid-bbox", "bbox", true, ReplacementOperation::bbox},
        {"replace_solid_with_bbox_cylinder", "solid", OperationTargetKind::solid, true, true, 1, "--replace-solid-bbox-cylinder", "bbox_cylinder", true, ReplacementOperation::bbox_cylinder},
        {"replace_solid_with_fitted_cylinder", "solid", OperationTargetKind::solid, true, true, 1, "--replace-solid-fitted-cylinder", "fitted_cylinder", true, ReplacementOperation::fitted_cylinder},
        {"replace_solid_with_fitted_sphere", "solid", OperationTargetKind::solid, true, true, 1, "--replace-solid-fitted-sphere", "fitted_sphere", true, ReplacementOperation::fitted_sphere},
        {"replace_solid_with_fitted_cone", "solid", OperationTargetKind::solid, true, true, 1, "--replace-solid-fitted-cone", "fitted_cone", true, ReplacementOperation::fitted_cone},
    };
    return registry;
}

const OperationDescriptor* find_operation(const std::string& name) {
    for (const OperationDescriptor& descriptor : operation_registry()) {
        if (name == descriptor.name) return &descriptor;
    }
    return nullptr;
}

const OperationDescriptor* find_replacement_by_cli_flag(const std::string& flag) {
    for (const OperationDescriptor& descriptor : operation_registry()) {
        if (descriptor.has_replacement_operation && flag == descriptor.cli_flag)
            return &descriptor;
    }
    return nullptr;
}

const OperationDescriptor* find_replacement_by_lua_value(const std::string& value) {
    for (const OperationDescriptor& descriptor : operation_registry()) {
        if (descriptor.has_replacement_operation && value == descriptor.lua_value)
            return &descriptor;
    }
    return nullptr;
}

const OperationDescriptor* find_replacement(ReplacementOperation operation) {
    for (const OperationDescriptor& descriptor : operation_registry()) {
        if (descriptor.has_replacement_operation
            && descriptor.replacement_operation == operation)
            return &descriptor;
    }
    return nullptr;
}

} // namespace csgify
