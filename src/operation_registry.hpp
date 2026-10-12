#pragma once

#include "csgify/engine.hpp"

#include <string>
#include <vector>

namespace csgify {

enum class OperationTargetKind { document, solid, face };

enum class AutonomyOperationKind {
    none,
    apply_contained_medium,
    decompose_to_fixed_point,
    remove_solid,
    remove_residual_with_approximation,
    resolve_overlap_with_approximation,
    apply_decomposition_candidate,
    apply_surface_replacement_candidate,
    shatter_with_approximation,
};

struct OperationDescriptor {
    const char* name;
    const char* category;
    OperationTargetKind target;
    bool mutating;
    bool approximate;
    unsigned version;
    const char* cli_flag;
    const char* lua_value;
    bool has_replacement_operation;
    ReplacementOperation replacement_operation;
    // JSON object schema embedded in planner contexts. Empty means this
    // operation is not part of the untrusted autonomy contract.
    AutonomyOperationKind autonomy_kind = AutonomyOperationKind::none;
    const char* autonomy_argument_schema = "";
    bool autonomy_supported = false;
    bool qt_review_supported = false;
    bool restart_supported = false;
    const char* recovery_action = "not_advertised";
};

const std::vector<OperationDescriptor>& operation_registry();
const OperationDescriptor* find_operation(const std::string& name);
const OperationDescriptor* find_replacement_by_cli_flag(const std::string& flag);
const OperationDescriptor* find_replacement_by_lua_value(const std::string& value);
const OperationDescriptor* find_replacement(ReplacementOperation operation);

} // namespace csgify
