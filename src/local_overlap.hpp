#pragma once

#include "engine_model.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <TopoDS_Solid.hxx>

namespace csgify::detail {

struct LocalOverlapCertificate {
    bool safe = false;
    bool unknown = false;
    std::size_t spatial_candidates = 0;
    std::size_t exact_tests = 0;
    std::size_t new_overlap_count = 0;
    std::size_t inherited_overlap_hits = 0;
    bool containment_proof_used = false;
    std::string failure_code;
    std::string conflicting_body_id;
    double intersection_volume = 0.0;
};

// Check prospective children against active bodies whose padded bboxes
// intersect them. Existing target-neighbour overlap relationships may be
// inherited by children; every other positive-volume or unknown relationship
// rejects the trial.
LocalOverlapCertificate certify_local_neighbour_overlaps(
    const CadModel& model,
    const std::string& target_id,
    const std::vector<TopoDS_Solid>& children,
    const std::vector<SolidOverlap>& baseline_overlaps,
    const ConversionOptions& options,
    PerformanceMetrics* performance = nullptr,
    bool children_certified_contained = false);

} // namespace csgify::detail
