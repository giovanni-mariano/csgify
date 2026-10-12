#pragma once

#include "engine_model.hpp"

#include <map>
#include <string>
#include <vector>

#include <TopoDS_Solid.hxx>

namespace csgify::detail {

// Internal form used by the fixed-point coordinator. Public proposal APIs
// expose only scalar evidence; this form also retains the OCCT children that
// were already created while scoring each candidate, avoiding a second split.
struct LocalBspProposal {
    DecompositionProposal proposal;
    std::map<std::string, std::vector<TopoDS_Solid>> candidate_children;
};

LocalBspProposal propose_bsp_decomposition_candidates_local(
    const CadModel& model,
    const std::string& solid_id,
    const ConversionOptions& options,
    bool include_parallel_plane_batches = true,
    std::size_t max_parallel_families = 8,
    std::size_t max_planes_per_batch = 64,
    std::size_t max_generated_candidates = 128);

// Commit an already-certified partition without invoking an OCCT splitter.
// IDs are allocated only here so rejected local trials do not perturb replay.
bool commit_local_decomposition_partition(
    CadModel& model,
    const std::string& target_id,
    const std::vector<TopoDS_Solid>& children,
    std::vector<std::string>* committed_ids = nullptr);

} // namespace csgify::detail
