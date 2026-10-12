#include "operation_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "operation_registry_contract: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main() {
    std::set<std::string> names;
    std::set<csgify::AutonomyOperationKind> autonomy_kinds;
    std::size_t autonomy_count = 0;
    for (const csgify::OperationDescriptor& operation
         : csgify::operation_registry()) {
        if (!names.insert(operation.name).second)
            return fail("duplicate operation name: " + std::string(operation.name));
        if (operation.autonomy_supported) {
            ++autonomy_count;
            if (!operation.mutating || !operation.restart_supported)
                return fail("autonomy operation lacks mutation/restart support: "
                            + std::string(operation.name));
            if (!operation.autonomy_argument_schema[0])
                return fail("autonomy operation lacks an argument schema: "
                            + std::string(operation.name));
            if (operation.autonomy_kind
                == csgify::AutonomyOperationKind::none)
                return fail("autonomy operation lacks a typed kind: "
                            + std::string(operation.name));
            if (!autonomy_kinds.insert(operation.autonomy_kind).second)
                return fail("duplicate autonomy operation kind: "
                            + std::string(operation.name));
            if (std::string(operation.recovery_action) == "not_advertised")
                return fail("autonomy operation lacks a recovery route: "
                            + std::string(operation.name));
        }
        if (!operation.autonomy_supported
            && operation.autonomy_kind != csgify::AutonomyOperationKind::none)
            return fail("unavailable operation has an autonomy kind: "
                        + std::string(operation.name));
        if (operation.qt_review_supported && !operation.autonomy_supported)
            return fail("Qt autonomy review advertised for unavailable operation: "
                        + std::string(operation.name));
        if (operation.has_replacement_operation) {
            if (csgify::find_replacement(operation.replacement_operation)
                    != &operation)
                return fail("replacement lookup is inconsistent");
        }
    }
    if (autonomy_count != 8)
        return fail("unexpected autonomy operation count");
    return EXIT_SUCCESS;
}
