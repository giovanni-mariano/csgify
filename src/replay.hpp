#pragma once

// Soft-match replay of a compacted recipe (recipe.json) against an
// arbitrary STEP file. See docs/DECISION_LOG_AND_RECIPE.md step 3 and
// memory/project-priorities.
//
// For each solid op the resolver tries:
//   1. primary match by component_entry  -> applied
//   2. fingerprint match within tolerance -> soft_applied (flagged)
//   3. >1 fingerprint candidate           -> ambiguous (skipped)
//   4. no match                           -> unmatched (skipped)
//
// Document- and face-scoped ops are executed verbatim. Soft-matching
// faces is deferred — face ids are not stable enough to make it useful
// without per-face fingerprints.

#include <string>

namespace csgify {

struct ReplayOptions {
    // Relative tolerance applied to volume, surface area, and bbox
    // diagonal during fingerprint matching.
    double rel_tolerance = 1.0e-3;
    // Centroid distance must satisfy d <= rel_tolerance * bbox_diagonal
    // (so the threshold scales with the part's size).
};

// Replay a recipe.json against `step_override` (or, if empty, the
// step_path recorded in the recipe). Writes the converted MCNP deck to
// `output_path` and a categorization report to `report_path`. Returns 0
// when every op was applied or soft_applied without ambiguity, and the
// emit succeeded; non-zero if any op was unmatched or ambiguous, or on
// I/O / parse / dispatch failure.
int replay_recipe(const std::string& recipe_json_path,
                  const std::string& step_override,
                  const std::string& output_path,
                  const std::string& report_path,
                  const std::string& step_output_path = {},
                  const ReplayOptions& options = {});

} // namespace csgify
