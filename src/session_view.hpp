#pragma once

// Internal bridge between the session layer and the AIS viewer.
// Declares SolidViewEntry, SolidViewStatus, and collect_view_entries.
// Not installed as part of the public API.

#include "csgify/session.hpp"

#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

namespace csgify {

enum class SolidViewStatus {
    exact_csg,            // ready for CSG emission — displayed green
    needs_decomposition,  // nonconvex, split required — displayed yellow
    no_interior_point,    // interior_point_not_found — displayed red
    piece,                // decomposed piece — displayed blue
    generated_medium,     // certified contained fluid — displayed cyan
    preview_exact,        // proposed exact CSG piece — displayed bright green
    preview_residual,     // proposed piece still needing work — displayed orange
    preview_sliver,       // proposed near-zero-volume piece — displayed magenta
};

struct SolidViewEntry {
    std::string  shape_id;
    std::string  label;
    std::string  name;            // from STEP metadata, may be empty
    TopoDS_Shape shape;
    SolidViewStatus status = SolidViewStatus::needs_decomposition;
    std::string  source_solid_id; // non-empty for decomposed pieces
    bool overlap_conflict = false; // participant in a certified overlap pair
};

// Collect all displayable solids and pieces from the first document in the
// session. Removed solids are excluded. Status coloring uses the last
// analysis report when available; defaults to needs_decomposition otherwise.
std::vector<SolidViewEntry> collect_view_entries(const Session& session);

// A decomposition preview is generated against a shallow copy of the current
// CadModel. OCCT shapes in the live session are never suppressed or changed.
// Only the selected candidate's resulting shapes are retained by the caller,
// avoiding an all-candidates geometry cache on large models.
struct DecompositionPreview {
    bool ok = false;
    std::string target_id;
    std::string candidate_id;
    std::size_t document_revision = 0;
    std::vector<SolidViewEntry> pieces;
    std::size_t exact_count = 0;
    std::size_t residual_count = 0;
    std::size_t sliver_count = 0;
    double total_volume = 0.0;
    bool summary_matches = false;
    bool certificate_attempted = false;
    bool partition_exact = false;
    bool valid_children = false;
    bool contained_in_parent = false;
    bool covers_parent = false;
    bool siblings_disjoint = false;
    bool volume_within_tolerance = false;
    double relative_volume_delta = 0.0;
    double outside_parent_volume = 0.0;
    double uncovered_parent_volume = 0.0;
    double sibling_overlap_volume = 0.0;
    std::size_t certificate_boolean_calls = 0;
    std::string certificate_failure_code;
    bool overlap_safe = false;
    bool overlap_unknown = false;
    std::size_t overlap_spatial_candidates = 0;
    std::size_t overlap_exact_tests = 0;
    std::size_t overlap_inherited_hits = 0;
    std::string overlap_failure_code;
    std::string overlap_conflicting_body_id;
    double overlap_intersection_volume = 0.0;
    std::string recommended_next_action;
    std::vector<Diagnostic> diagnostics;
};

DecompositionPreview collect_decomposition_preview(
    const Session& session,
    const DecompositionCandidate& candidate,
    JobProgressCallback progress = {});

} // namespace csgify
