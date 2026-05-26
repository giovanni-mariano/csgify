#pragma once

#include "cad2csg/engine.hpp"

#include <memory>
#include <string>

namespace cad2csg {

struct Session;

struct SessionDeleter {
    void operator()(Session* session) const;
};

using SessionPtr = std::unique_ptr<Session, SessionDeleter>;

using DocumentId = std::string;

struct CommandResult {
    bool ok = false;
    ConversionReport report;
};

struct AutoFitCommandResult {
    CommandResult command;
    AutoFitSummary summary;
};

struct ReplacementCommandResult {
    CommandResult command;
    ReplacementCheck check;
};

enum class ReportIdMode {
    local,
    scoped,
};

SessionPtr create_session(const ConversionOptions& options = {});

// Attach an append-only decision log. Every subsequent mutating command
// on this session writes a record. See docs/DECISION_LOG_AND_RECIPE.md.
// The first `step_path_hint` recorded in the manifest is whatever the
// caller passes here; subsequent load_step calls do not rewrite the
// manifest. Throws if the file cannot be opened.
void attach_decision_log(Session& session,
                         const std::string& log_path,
                         const std::string& step_path_hint = {});

DocumentId load_step(Session& session,
                     const std::string& step_path,
                     const ConversionOptions* options_override = nullptr);

CommandResult analyze(Session& session,
                      const DocumentId& document_id,
                      ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult remove_solid(Session& session,
                           const std::string& shape_id,
                           ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_solid_with_bbox(Session& session,
                                      const std::string& shape_id,
                                      ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_solid_with_bbox_cylinder(Session& session,
                                               const std::string& shape_id,
                                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_solid_with_fitted_cylinder(Session& session,
                                                 const std::string& shape_id,
                                                 ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_solid_with_fitted_sphere(Session& session,
                                               const std::string& shape_id,
                                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_solid_with_fitted_cone(Session& session,
                                             const std::string& shape_id,
                                             ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_replacement_operation(Session& session,
                                          const std::string& shape_id,
                                          ReplacementOperation operation,
                                          ReportIdMode id_mode = ReportIdMode::scoped);

ReplacementCommandResult apply_checked_replacement_operation(
    Session& session,
    const std::string& shape_id,
    ReplacementOperation operation,
    const ReplacementLimits& limits,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult replace_face_with_fit(Session& session,
                                    const std::string& shape_id,
                                    SplineFitKind kind,
                                    bool has_max_deviation,
                                    double max_deviation,
                                    ReportIdMode id_mode = ReportIdMode::scoped);

SurfaceReplacementProposal propose_surface_replacement(
    Session& session,
    const std::string& face_shape_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_surface_replacement_candidate(
    Session& session,
    const std::string& face_shape_id,
    const std::string& candidate_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult auto_fit_splines(Session& session,
                               const DocumentId& document_id,
                               bool has_max_deviation,
                               double max_deviation,
                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_planar(Session& session,
                               const std::string& shape_id,
                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_by_face(Session& session,
                                const std::string& solid_shape_id,
                                const std::string& face_shape_id,
                                ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_by_plane(Session& session,
                                 const std::string& solid_shape_id,
                                 const Vec3& origin,
                                 const Vec3& normal,
                                 ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_by_planes(Session& session,
                                  const std::string& solid_shape_id,
                                  const std::vector<PlaneSpec>& planes,
                                  ReportIdMode id_mode = ReportIdMode::scoped);

DecompositionProposal propose_decomposition(
    Session& session,
    const std::string& solid_shape_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

RemovalProposal suggest_removals(Session& session,
                                 const DocumentId& document_id,
                                 ReportIdMode id_mode = ReportIdMode::scoped);

HoleFillProposal propose_hole_fills(Session& session,
                                    const DocumentId& document_id,
                                    const HoleFillOptions& hole_options = {},
                                    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult fill_hole(Session& session,
                        const DocumentId& document_id,
                        const std::string& candidate_id,
                        ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_decomposition_candidate(
    Session& session,
    const std::string& shape_id,
    const std::string& candidate_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

// Batched callers (decompose-apply) that invoke auto_decompose_bsp once
// per target can pass defer_analysis=true to skip the trailing full
// analyze_model() pass. The BSP loop still uses the cheap
// decomposition_target_exact_csg_supported query internally to decide
// when each piece is convex; only the final report-shaped analyze is
// what gets deferred. The caller is then responsible for running one
// `analyze` at the end before emit/export.
CommandResult auto_decompose_bsp(Session& session,
                                 const std::string& shape_id,
                                 int max_depth = 4,
                                 const DecompositionPolicy& policy = {},
                                 ReportIdMode id_mode = ReportIdMode::scoped,
                                 bool defer_analysis = false);

CommandResult emit_document(Session& session,
                            const DocumentId& document_id,
                            const std::string& output_path,
                            ReportIdMode id_mode = ReportIdMode::scoped);

// Write the as-simplified geometry as STEP. Pass 1: original surviving
// solids, decomposition pieces, and primitive synthetic bodies. Boolean
// synthetic nodes are emitted as bbox placeholders for now.
CommandResult export_document_step(Session& session,
                                   const DocumentId& document_id,
                                   const std::string& output_path,
                                   ReportIdMode id_mode = ReportIdMode::scoped);

VolumeComparison compare_document_volumes(Session& session,
                                          const DocumentId& original_document_id,
                                          const DocumentId& candidate_document_id,
                                          double absolute_tolerance,
                                          double relative_tolerance,
                                          ReportIdMode id_mode = ReportIdMode::scoped);

AutoFitCommandResult auto_fit_splines_with_summary(
    Session& session,
    const DocumentId& document_id,
    bool has_max_deviation,
    double max_deviation,
    ReportIdMode id_mode = ReportIdMode::scoped);

const ConversionReport& last_report(const Session& session);

// Source-path lookup for a previously loaded document. Returns empty
// when document_id is unknown. Used by callers (Lua bindings,
// scripts) that need to re-derive the on-disk path the document was
// loaded from -- e.g., to pass through to a tool that operates on
// files rather than session state.
const std::string& document_source_path(const Session& session,
                                         const DocumentId& document_id);

const DecompositionProposal& last_decomposition_proposal(const Session& session);

const HoleFillProposal& last_hole_fill_proposal(const Session& session);

const SurfaceReplacementProposal& last_surface_replacement_proposal(
    const Session& session);

std::size_t document_count(const Session& session);

} // namespace cad2csg
