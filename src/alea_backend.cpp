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

    // Simplify before deciding whether a cell really contains a union.  A
    // decomposition commonly produces complementary branches such as
    //
    //     (A & +cut) | (A & -cut)
    //
    // which is just the single intersection A.  Splitting first destroys
    // that relationship and permanently emits two cells.  Alea's factoring
    // and complement reduction can remove the artificial cut while the
    // branches are still together; only genuine residual unions should be
    // split afterwards.
    alea_simplify_and_prune_cells(system, nullptr);

    if (split_union_cells && alea_split_union_cells(system) < 0) {
        if (error_out) {
            *error_out = alea_error()[0]
                ? alea_error()
                : "Alea failed to split union cells.";
        }
        return false;
    }

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
