#pragma once

// Internal: Session and CadDocument struct definitions.
// Include only from session.cpp, session_view.cpp, and viewer.cpp.
// Not installed as part of the public API.

#include "csgify/session.hpp"
#include "decision_log.hpp"
#include "engine_model.hpp"

namespace csgify {

struct DocumentRevisionState {
    CadModel model;
    std::size_t synthetic_body_high_water = 0;
    std::size_t decomposed_piece_high_water = 0;
    std::vector<std::pair<std::string, std::string>> known_overlap_pairs;
};

struct CadDocument {
    DocumentId id;
    std::string source_path;
    ConversionOptions options;
    CadModel model;
    ConversionReport report;
    PerformanceMetrics performance;

    // High-water marks for the mutation-overlap check. After each
    // mutating call, only synthetic bodies / decomposed pieces with
    // index >= the corresponding high-water are tested against the
    // surviving model. The high-water is bumped after each check so
    // we never re-scan the same new geometry twice.
    std::size_t synthetic_body_high_water = 0;
    std::size_t decomposed_piece_high_water = 0;

    // Canonical (lo<->hi) pairs of solid/synthetic/piece ids that are
    // already known to overlap. Pairs detected here are not re-flagged
    // as `mutation_introduced_overlap`; only pairs that were NOT in
    // this set when discovered count as introduced. Populated by the
    // explicit analyze() pass (full pairwise scan) and incrementally
    // by the per-mutation check.
    std::vector<std::pair<std::string, std::string>> known_overlap_pairs;

    // `committed_state` is the rollback boundary. CadModel copies retain
    // OCCT shapes by handle while copying the small mutation/provenance
    // vectors, making revision snapshots substantially cheaper than a STEP
    // round trip.
    DocumentRevisionState committed_state;
    std::vector<DocumentRevisionState> revision_history;
    std::size_t revision = 0;
};

struct Session {
    explicit Session(const ConversionOptions& options_in) : options(options_in) {}

    ConversionOptions options;
    ConversionReport last_report_value;
    DecompositionProposal last_decomposition_proposal_value;
    HoleFillProposal last_hole_fill_proposal_value;
    SurfaceReplacementProposal last_surface_replacement_proposal_value;
    std::vector<CadDocument> documents;
    std::unique_ptr<DecisionLog> log;
};

} // namespace csgify
