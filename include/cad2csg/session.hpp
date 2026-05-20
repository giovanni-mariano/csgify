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

DecompositionProposal propose_decomposition(
    Session& session,
    const std::string& solid_shape_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_decomposition_candidate(
    Session& session,
    const std::string& shape_id,
    const std::string& candidate_id,
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult auto_decompose_bsp(Session& session,
                                 const std::string& shape_id,
                                 int max_depth = 4,
                                 const DecompositionPolicy& policy = {},
                                 ReportIdMode id_mode = ReportIdMode::scoped);

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

const DecompositionProposal& last_decomposition_proposal(const Session& session);

std::size_t document_count(const Session& session);

} // namespace cad2csg
