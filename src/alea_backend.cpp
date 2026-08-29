#include "alea_backend.hpp"

namespace csgify::detail {

bool finalize_alea_system(alea_system_t* system,
                          bool split_union_cells,
                          std::string* error_out) {
    if (!system) {
        if (error_out) {
            *error_out = "Alea system is null.";
        }
        return false;
    }

    if (split_union_cells && alea_split_union_cells(system) < 0) {
        if (error_out) {
            *error_out = alea_error()[0]
                ? alea_error()
                : "Alea failed to split union cells.";
        }
        return false;
    }

    // This is Boolean-expression simplification, not universe flattening.
    // alea_flatten() has different semantics and must not be substituted here.
    alea_simplify_and_prune_cells(system, nullptr);

    if (alea_build_universe_index(system) != 0) {
        if (error_out) {
            *error_out = alea_error()[0]
                ? alea_error()
                : "Alea failed to build the universe index.";
        }
        return false;
    }
    return true;
}

} // namespace csgify::detail
