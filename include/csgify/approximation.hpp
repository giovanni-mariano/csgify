#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace csgify {

struct ApproximationPolicy {
    bool enabled = false;
    double max_global_changed_volume_fraction = 0.0;
    double max_body_changed_volume_fraction = 0.0;
    double max_action_changed_volume_fraction = 0.0;
    double max_global_added_volume_fraction = 0.0;
    double max_global_removed_volume_fraction = 0.0;
    double max_body_added_volume_fraction = 0.0;
    double max_body_removed_volume_fraction = 0.0;
    double max_surface_deviation = 0.0;
    bool allow_surface_replacement = false;
    bool allow_sliver_removal = false;
    bool allow_bbox_replacement = false;
    bool allow_numerical_shatter_loss = false;
    // Permit assigning an overlapping region to one explicitly selected
    // owner and subtracting it from the other body. The removed volume is
    // measured and charged like every other approximation.
    bool allow_overlap_resolution = false;
    bool require_containment_preservation = true;
    bool require_topology_preservation = true;
    std::vector<std::string> protected_target_ids;
    std::vector<int> protected_material_ids;
};

struct ApproximationMetrics {
    double baseline_volume = 0.0;
    double before_volume = 0.0;
    double after_volume = 0.0;
    double signed_volume_delta = 0.0;
    double absolute_net_volume_delta = 0.0;
    double added_volume = 0.0;
    double removed_volume = 0.0;
    double changed_volume = 0.0;
    double changed_volume_fraction = 0.0;
    double added_volume_fraction = 0.0;
    double removed_volume_fraction = 0.0;
    double max_surface_deviation = 0.0;
    double rms_surface_deviation = 0.0;
    bool symmetric_difference_certified = false;
    bool containment_preserved = false;
    bool topology_preserved = false;
};

struct ApproximationLedgerEntry {
    std::string source_solid_id;
    double baseline_volume = 0.0;
    double cumulative_changed_volume = 0.0;
    double cumulative_added_volume = 0.0;
    double cumulative_removed_volume = 0.0;
    std::size_t accepted_actions = 0;
};

struct ApproximationLedger {
    double document_baseline_volume = 0.0;
    std::vector<ApproximationLedgerEntry> bodies;
    double cumulative_changed_volume = 0.0;
    double cumulative_added_volume = 0.0;
    double cumulative_removed_volume = 0.0;
};

struct ApproximationBudgetCheck {
    bool ok = false;
    std::string code;
    std::string message;
    double global_changed_fraction_after = 0.0;
    double body_changed_fraction_after = 0.0;
    double global_added_fraction_after = 0.0;
    double global_removed_fraction_after = 0.0;
    double body_added_fraction_after = 0.0;
    double body_removed_fraction_after = 0.0;
};

// User-facing CLI percentages require an explicit '%' suffix. The parsed
// value is a normalized fraction: "1%" -> 0.01.
bool parse_approximation_percent(const std::string& text,
                                 double* fraction_out,
                                 std::string* error_out = nullptr);

ApproximationBudgetCheck check_approximation_budget(
    const ApproximationPolicy& policy,
    const ApproximationLedger& ledger,
    const std::string& source_solid_id,
    const ApproximationMetrics& metrics);

bool commit_approximation_metrics(ApproximationLedger& ledger,
                                  const ApproximationPolicy& policy,
                                  const std::string& source_solid_id,
                                  const ApproximationMetrics& metrics,
                                  ApproximationBudgetCheck* check_out = nullptr);

} // namespace csgify
