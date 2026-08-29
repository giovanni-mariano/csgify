#pragma once

#include "csgify/engine.hpp"

#include <string>
#include <vector>

namespace csgify {

enum class OperationTargetKind { document, solid, face };

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
};

const std::vector<OperationDescriptor>& operation_registry();
const OperationDescriptor* find_operation(const std::string& name);
const OperationDescriptor* find_replacement_by_cli_flag(const std::string& flag);
const OperationDescriptor* find_replacement_by_lua_value(const std::string& value);
const OperationDescriptor* find_replacement(ReplacementOperation operation);

} // namespace csgify
