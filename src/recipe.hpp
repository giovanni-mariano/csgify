#pragma once

// Recipe compactor. Reads an append-only decision log and emits a minimal
// Lua script that reproduces the final converted model.
//
// See docs/DECISION_LOG_AND_RECIPE.md. Step 2 of the implementation plan.
//
// The MVP compactor:
// - parses log JSON written by DecisionLog
// - drops failed records, supersedes earlier ops on the same target,
//   skips ops on solids that were later removed
// - emits a Lua script that calls the existing csgify.session() API
//
// Geometric fingerprints, soft-match replay against revised STEP, and
// content-addressed record IDs are deferred to step 3.

#include <string>

namespace csgify {

struct CompactRecipeOptions {
    bool include_provenance_header = true;
};

// Reads the JSON decision log at `log_path`, compacts it, and writes a
// Lua script to `recipe_path`. Returns 0 on success, non-zero on parse
// failure or I/O error (an error message is written to stderr).
int compact_log_to_recipe(const std::string& log_path,
                          const std::string& recipe_path,
                          const CompactRecipeOptions& options = {});

} // namespace csgify
