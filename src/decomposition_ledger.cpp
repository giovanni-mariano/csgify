#include "decomposition_ledger.hpp"

#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>

namespace csgify::detail {
namespace {

void add_contribution(DecompositionProgressState* state,
                      const BodyProgressCertificate& body) {
    ++state->active_piece_count;
    state->total_volume += body.volume;
    if (body.exact_csg_supported) {
        ++state->exact_body_count;
        return;
    }
    ++state->residual_count;
    state->residual_volume += std::abs(body.volume);
    state->unsupported_face_count += body.unsupported_face_count;
    state->halfspace_violation_count += body.halfspace_violation_count;
}

bool remove_contribution(DecompositionProgressState* state,
                         const BodyProgressCertificate& body) {
    if (state->active_piece_count == 0) return false;
    --state->active_piece_count;
    state->total_volume -= body.volume;
    if (body.exact_csg_supported) {
        if (state->exact_body_count == 0) return false;
        --state->exact_body_count;
        return true;
    }
    if (state->residual_count == 0
        || state->unsupported_face_count < body.unsupported_face_count
        || state->halfspace_violation_count < body.halfspace_violation_count) {
        return false;
    }
    --state->residual_count;
    state->residual_volume -= std::abs(body.volume);
    state->unsupported_face_count -= body.unsupported_face_count;
    state->halfspace_violation_count -= body.halfspace_violation_count;
    return true;
}

bool close(double a, double b, double relative, double absolute) {
    if (!std::isfinite(a) || !std::isfinite(b)) return a == b;
    return std::abs(a - b)
        <= std::max(absolute, relative * std::max(std::abs(a), std::abs(b)));
}

BodyProgressCertificate make_certificate(
    const std::string& id,
    const std::string& source_solid_id,
    double volume,
    std::size_t face_count,
    bool exact,
    bool needs_decomposition,
    bool needs_spline,
    bool needs_surface,
    bool needs_user,
    const TopoDS_Solid* shape,
    std::size_t certificate_index,
    const ConversionOptions& options,
    const BodyProgressCertificate* reusable,
    bool* reused_out) {
    BodyProgressCertificate result;
    result.body_id = id;
    result.source_solid_id = source_solid_id.empty() ? id : source_solid_id;
    result.volume = volume;
    result.face_count = face_count;
    result.exact_csg_supported = exact;
    result.needs_decomposition = needs_decomposition;
    result.needs_user_decision = needs_user;
    if (!exact && (needs_spline || needs_surface
                   || (needs_user && !needs_decomposition))) {
        result.unsupported_face_count = std::max<std::size_t>(1, face_count);
    }
    const bool reusable_matches = reusable && reusable->valid
        && reusable->body_id == result.body_id
        && reusable->source_solid_id == result.source_solid_id
        && close(reusable->volume, result.volume, 1.0e-12, 1.0e-12)
        && reusable->face_count == result.face_count
        && reusable->unsupported_face_count == result.unsupported_face_count
        && reusable->exact_csg_supported == result.exact_csg_supported
        && reusable->needs_decomposition == result.needs_decomposition
        && reusable->needs_user_decision == result.needs_user_decision;
    if (reusable_matches) {
        result.halfspace_violation_count =
            reusable->halfspace_violation_count;
        if (reused_out) *reused_out = true;
    } else if (!exact && needs_decomposition && shape && !shape->IsNull()) {
        result.halfspace_violation_count =
            convexity_halfspace_violation_count(
                *shape, certificate_index, options);
    }
    result.valid = std::isfinite(volume) && volume >= 0.0;
    return result;
}

} // namespace

const BodyProgressCertificate* DecompositionProgressLedger::find(
    const std::string& id) const {
    const auto found = bodies_.find(id);
    return found == bodies_.end() ? nullptr : &found->second;
}

bool DecompositionProgressLedger::project_replacement(
    const std::string& parent_id,
    const std::vector<BodyProgressCertificate>& children,
    std::size_t projected_overlap_error_count,
    DecompositionProgressState* state_out) const {
    if (!state_out) return false;
    const auto parent = bodies_.find(parent_id);
    if (parent == bodies_.end()) return false;
    DecompositionProgressState projected = state_;
    if (!remove_contribution(&projected, parent->second)) return false;
    for (const BodyProgressCertificate& child : children) {
        if (!child.valid || child.body_id.empty()
            || child.body_id == parent_id || bodies_.count(child.body_id) != 0) {
            return false;
        }
        add_contribution(&projected, child);
    }
    // Avoid a signed-zero or tiny negative residual caused by subtract/add
    // rounding. Meaningful disagreement is caught by reconciliation.
    if (std::abs(projected.residual_volume) <= 1.0e-15)
        projected.residual_volume = 0.0;
    if (std::abs(projected.total_volume) <= 1.0e-15)
        projected.total_volume = 0.0;
    projected.overlap_error_count = projected_overlap_error_count;
    *state_out = projected;
    return true;
}

bool DecompositionProgressLedger::commit_replacement(
    const std::string& parent_id,
    const std::vector<BodyProgressCertificate>& children,
    std::size_t projected_overlap_error_count) {
    DecompositionProgressState projected;
    if (!project_replacement(parent_id, children,
                             projected_overlap_error_count, &projected)) {
        return false;
    }
    bodies_.erase(parent_id);
    for (const BodyProgressCertificate& child : children) {
        bodies_.emplace(child.body_id, child);
    }
    state_ = projected;
    return true;
}

DecompositionProgressLedger build_decomposition_progress_ledger(
    const CadModel& model,
    const ConversionReport& report,
    const ConversionOptions& options,
    PerformanceMetrics* performance,
    const DecompositionProgressLedger* reusable) {
    DecompositionProgressLedger ledger;
    ledger.state_.overlap_error_count = report.overlaps.size();

    std::unordered_map<std::string, TopoDS_Solid> original_shapes;
    std::size_t original_index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++original_index;
        original_shapes.emplace(solid_id(original_index),
                                TopoDS::Solid(exp.Current()));
    }
    std::unordered_map<std::string, const TopoDS_Solid*> piece_shapes;
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (piece.active) piece_shapes.emplace(piece.id, &piece.solid);
    }

    std::size_t certificate_index = 400000;
    for (const AnalyzedSolid& solid : report.solids) {
        const auto shape = original_shapes.find(solid.id);
        const TopoDS_Solid* solid_shape = shape == original_shapes.end()
            ? nullptr : &shape->second;
        const BodyProgressCertificate* prior = reusable
            ? reusable->find(solid.id) : nullptr;
        bool reused = false;
        BodyProgressCertificate certificate = make_certificate(
            solid.id, solid.id, solid.volume, solid.face_count,
            solid.exact_csg_supported, solid.needs_decomposition,
            solid.needs_spline_replacement, solid.needs_surface_replacement,
            solid.needs_user_decision, solid_shape, ++certificate_index,
            options, prior, &reused);
        if (performance) {
            if (reused) ++performance->body_certificate_cache_hits;
            else ++performance->body_certificate_cache_misses;
        }
        ledger.bodies_.emplace(certificate.body_id, certificate);
        add_contribution(&ledger.state_, certificate);
    }
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        const auto shape = piece_shapes.find(piece.id);
        const TopoDS_Solid* piece_shape = shape == piece_shapes.end()
            ? nullptr : shape->second;
        const BodyProgressCertificate* prior = reusable
            ? reusable->find(piece.id) : nullptr;
        bool reused = false;
        BodyProgressCertificate certificate = make_certificate(
            piece.id, piece.source_solid_id, piece.volume, piece.face_count,
            piece.exact_csg_supported, piece.needs_decomposition,
            piece.needs_spline_replacement, piece.needs_surface_replacement,
            piece.needs_user_decision, piece_shape, ++certificate_index,
            options, prior, &reused);
        if (performance) {
            if (reused) ++performance->body_certificate_cache_hits;
            else ++performance->body_certificate_cache_misses;
        }
        ledger.bodies_.emplace(certificate.body_id, certificate);
        add_contribution(&ledger.state_, certificate);
    }

    // The report is the authority for the complete model volume. It may
    // include exact Boolean/synthetic bodies not presently part of the
    // residual score, so retain that total rather than the contribution sum.
    ledger.state_.total_volume = report.total_volume;
    return ledger;
}

bool progress_states_equivalent(const DecompositionProgressState& left,
                                const DecompositionProgressState& right,
                                double relative_tolerance,
                                double absolute_tolerance) {
    return left.unsupported_face_count == right.unsupported_face_count
        && close(left.residual_volume, right.residual_volume,
                 relative_tolerance, absolute_tolerance)
        && left.halfspace_violation_count == right.halfspace_violation_count
        && left.residual_count == right.residual_count
        && left.active_piece_count == right.active_piece_count
        && left.exact_body_count == right.exact_body_count
        // Incremental splitting can fan one pre-existing parent-neighbour
        // overlap into several child-neighbour report pairs without creating
        // any new overlap volume. The progress ledger tracks whether overlap
        // is blocking; the authoritative terminal report owns the pair count.
        && (left.overlap_error_count == 0)
            == (right.overlap_error_count == 0)
        && close(left.total_volume, right.total_volume,
                 relative_tolerance, absolute_tolerance);
}

} // namespace csgify::detail
