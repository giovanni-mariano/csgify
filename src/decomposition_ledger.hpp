#pragma once

#include "csgify/fixed_point.hpp"
#include "engine_model.hpp"

#include <map>
#include <string>
#include <vector>

namespace csgify::detail {

// Cached contribution of one active geometric body to the fixed-point score.
// It deliberately contains measured scalar evidence rather than references to
// report objects, allowing projected replacement states to be computed without
// re-running analysis for unrelated bodies.
struct BodyProgressCertificate {
    std::string body_id;
    std::string source_solid_id;
    double volume = 0.0;
    std::size_t face_count = 0;
    std::size_t unsupported_face_count = 0;
    std::size_t halfspace_violation_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_user_decision = false;
    bool valid = false;
};

class DecompositionProgressLedger {
public:
    const DecompositionProgressState& state() const { return state_; }
    const std::map<std::string, BodyProgressCertificate>& bodies() const {
        return bodies_;
    }
    const BodyProgressCertificate* find(const std::string& id) const;

    // Calculate the state that would result from replacing one active parent
    // with locally analyzed children. The ledger itself is not modified.
    bool project_replacement(
        const std::string& parent_id,
        const std::vector<BodyProgressCertificate>& children,
        std::size_t projected_overlap_error_count,
        DecompositionProgressState* state_out) const;

    // Commit an already-certified projection to the ledger. This is an
    // algebraic cache mutation only; CadModel mutation remains the caller's
    // transaction boundary.
    bool commit_replacement(
        const std::string& parent_id,
        const std::vector<BodyProgressCertificate>& children,
        std::size_t projected_overlap_error_count);

private:
    friend DecompositionProgressLedger build_decomposition_progress_ledger(
        const CadModel&, const ConversionReport&, const ConversionOptions&,
        PerformanceMetrics*, const DecompositionProgressLedger*);

    std::map<std::string, BodyProgressCertificate> bodies_;
    DecompositionProgressState state_;
};

DecompositionProgressLedger build_decomposition_progress_ledger(
    const CadModel& model,
    const ConversionReport& report,
    const ConversionOptions& options,
    PerformanceMetrics* performance = nullptr,
    const DecompositionProgressLedger* reusable = nullptr);

bool progress_states_equivalent(const DecompositionProgressState& left,
                                const DecompositionProgressState& right,
                                double relative_tolerance = 1.0e-12,
                                double absolute_tolerance = 1.0e-12);

} // namespace csgify::detail
