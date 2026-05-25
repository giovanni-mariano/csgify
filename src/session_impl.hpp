#pragma once

// Internal: Session and CadDocument struct definitions.
// Include only from session.cpp, session_view.cpp, and viewer.cpp.
// Not installed as part of the public API.

#include "cad2csg/session.hpp"
#include "decision_log.hpp"
#include "engine_model.hpp"

namespace cad2csg {

struct CadDocument {
    DocumentId id;
    std::string source_path;
    ConversionOptions options;
    CadModel model;
    ConversionReport report;
    PerformanceMetrics performance;
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

} // namespace cad2csg
