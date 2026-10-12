#pragma once

#include "csgify/engine.hpp"
#include "csgify/fixed_point.hpp"

#include <memory>
#include <string>
#include <vector>

namespace csgify {

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

struct ApproximationCommandResult {
    CommandResult command;
    ApproximationMetrics metrics;
    ApproximationBudgetCheck budget;
};

struct AutoFitCommandResult {
    CommandResult command;
    AutoFitSummary summary;
};

struct FixedPointDecompositionResult {
    CommandResult command;
    FixedPointDecompositionSummary summary;
};

struct ReplacementCommandResult {
    CommandResult command;
    ReplacementCheck check;
};

struct DecisionAuditContext {
    std::string actor;
    std::string proposal_actor;
    std::string proposal_id;
    std::string rationale;
    std::string policy_rule;
    std::string approval_mode;
};

struct DecisionReviewRecord {
    std::string target_id;
    std::string proposal_id;
    std::string decision;
    std::string review_kind;
    std::string action;
    std::string candidate_id;
    std::size_t base_revision = 0;
    DecisionAuditContext audit;
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
void set_decision_audit_context(Session& session,
                                const DecisionAuditContext& context);
void clear_decision_audit_context(Session& session);
void record_decision_review(Session& session,
                            const std::string& target_id,
                            const std::string& proposal_id,
                            const std::string& decision,
                            const DecisionAuditContext& context,
                            const std::string& review_kind = {},
                            const std::string& action = {},
                            const std::string& candidate_id = {},
                            std::size_t base_revision = 0);
std::vector<DecisionReviewRecord> decision_reviews(const Session& session);
std::vector<DecisionReviewRecord> latest_decision_reviews(
    const Session& session,
    const std::string& decision = {});
std::string decision_review_status(const Session& session,
                                   const std::string& target_id,
                                   const std::string& proposal_id);

DocumentId load_step(Session& session,
                     const std::string& step_path,
                     const ConversionOptions* options_override = nullptr);

CommandResult analyze(Session& session,
                      const DocumentId& document_id,
                      ReportIdMode id_mode = ReportIdMode::scoped,
                      JobProgressCallback progress = {});

// Every successful mutation creates a new immutable document revision.
// Failed mutations and mutations that introduce an overlap are rolled back
// automatically. Undo restores the preceding committed state as a new,
// monotonic revision (there is deliberately no implicit redo stack).
std::size_t document_revision(const Session& session,
                              const DocumentId& document_id);
CommandResult undo_last_mutation(Session& session,
                                 const DocumentId& document_id,
                                 ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult remove_solid(Session& session,
                           const std::string& shape_id,
                           ReportIdMode id_mode = ReportIdMode::scoped);

// Suppress several solids as one atomic document mutation. All targets must
// resolve to the same document. The complete post-mutation analysis and
// geometry certification run once after every target has been staged.
CommandResult remove_solids(
    Session& session,
    const std::vector<std::string>& shape_ids,
    ReportIdMode id_mode = ReportIdMode::scoped,
    JobProgressCallback progress = {});

// Suppress a decomposition residual only when its removed volume fits the
// explicit cumulative approximation budget. Whole imported solids are not
// accepted here: those remain an explicit modeling/removal decision.
ApproximationCommandResult remove_residual_with_approximation(
    Session& session,
    const std::string& shape_id,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode = ReportIdMode::scoped);

// Give the positive-volume intersection to `owner_shape_id` by subtracting
// the owner from `loser_shape_id`. The owner is unchanged; the exact removed
// volume is charged to the loser's source-solid lineage. The operation is
// transactional and the normal hard overlap postcondition still applies.
ApproximationCommandResult resolve_overlap_with_approximation(
    Session& session,
    const std::string& loser_shape_id,
    const std::string& owner_shape_id,
    const ApproximationPolicy& policy,
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

ApproximationCommandResult apply_budgeted_replacement_operation(
    Session& session,
    const std::string& shape_id,
    ReplacementOperation operation,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode = ReportIdMode::scoped);

ApproximationLedger document_approximation_ledger(
    const Session& session,
    const DocumentId& document_id);

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

ApproximationCommandResult apply_budgeted_surface_replacement_candidate(
    Session& session,
    const std::string& face_shape_id,
    const std::string& candidate_id,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult auto_fit_splines(Session& session,
                               const DocumentId& document_id,
                               bool has_max_deviation,
                               double max_deviation,
                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_planar(Session& session,
                               const std::string& shape_id,
                               ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_prismatic(Session& session,
                                  const std::string& shape_id,
                                  ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_revolution(Session& session,
                                   const std::string& shape_id,
                                   ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult reuse_definition_conversion(
    Session& session,
    const std::string& target_shape_id,
    const std::string& source_shape_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

MediumGenerationProposal propose_contained_medium(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    ReportIdMode id_mode = ReportIdMode::scoped);

// Persist a contained-medium intent without creating geometry. This is useful
// for unresolved ports: analyze()/the Qt reviewer can keep presenting the
// blocking decisions until an approved apply replaces the staged request.
CommandResult stage_contained_medium_request(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    ReportIdMode id_mode = ReportIdMode::scoped);

// Resolve a staged request by its stable ID. Review restoration uses this to
// regenerate a fresh contained-medium certificate instead of trusting the
// geometry evidence that was shown before the process restarted.
bool find_contained_medium_request(
    const Session& session,
    const DocumentId& document_id,
    const std::string& request_id,
    MediumGenerationRequest* request);

CommandResult define_material(
    Session& session,
    const DocumentId& document_id,
    const MaterialDefinition& material,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_contained_medium(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    bool user_approved,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_shell_difference(
    Session& session,
    const std::string& shape_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult decompose_pipe_annulus(
    Session& session,
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

// Exhaustive surface-driven subdivision of one solid. Cuts the solid by all
// of its own surfaces, recursing into pieces that still fail convex
// validation up to max_depth. Unlike auto_decompose_bsp there is no
// confidence gate: it either reduces the solid to simple pieces or fails
// honestly with a shatter_residual_nonconvex diagnostic.
CommandResult shatter(Session& session,
                      const std::string& solid_shape_id,
                      int max_depth = 8,
                      bool allow_volume_drift = false,
                      ReportIdMode id_mode = ReportIdMode::scoped);

ApproximationCommandResult shatter_with_approximation(
    Session& session,
    const std::string& solid_shape_id,
    int max_depth,
    const ApproximationPolicy& policy,
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

// `defer_analysis` is retained for source compatibility but validation is
// never deferred: every mutation must cross the same atomic commit boundary.
CommandResult auto_decompose_bsp(Session& session,
                                 const std::string& shape_id,
                                 int max_depth = 4,
                                 const DecompositionPolicy& policy = {},
                                 ReportIdMode id_mode = ReportIdMode::scoped,
                                 bool defer_analysis = false);

// Repeatedly trial fresh decomposition strategies against a copy of the
// current model. A trial is accepted only when it passes ordinary geometry
// postconditions and strictly improves the document-wide progress score.
FixedPointDecompositionResult decompose_to_fixed_point(
    Session& session,
    const DocumentId& document_id,
    const FixedPointDecompositionPolicy& policy = {},
    ReportIdMode id_mode = ReportIdMode::scoped,
    FixedPointProgressCallback progress = {});

// Analyze the current document using the same global metrics and overlap gates
// as the fixed-point coordinator, without mutating it.
DecompositionProgressState decomposition_progress_state(
    Session& session,
    const DocumentId& document_id);

CommandResult emit_document(Session& session,
                            const DocumentId& document_id,
                            const std::string& output_path,
                            ReportIdMode id_mode = ReportIdMode::scoped,
                            JobProgressCallback progress = {});

// Write the as-simplified geometry as STEP. Original surviving solids,
// decomposition pieces, primitives, and recursively materialized Boolean
// synthetic bodies are exported. The operation fails rather than substituting
// placeholder geometry when exact materialization is unavailable.
CommandResult export_document_step(Session& session,
                                   const DocumentId& document_id,
                                   const std::string& output_path,
                                   ReportIdMode id_mode = ReportIdMode::scoped,
                                   JobProgressCallback progress = {});

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

} // namespace csgify
