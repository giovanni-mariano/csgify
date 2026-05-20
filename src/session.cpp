// Session/workspace API. This is the future user/agent boundary: callers
// work with documents, IDs, commands, and reports. OCCT-backed CadModel state
// stays behind the Session object.

#include "cad2csg/session.hpp"

#include "decision_log.hpp"
#include "engine_model.hpp"
#include "step_export.hpp"

#include <cmath>
#include <memory>
#include <sstream>
#include <vector>

namespace cad2csg {
namespace {

struct CadDocument {
    DocumentId id;
    std::string source_path;
    ConversionOptions options;
    CadModel model;
    ConversionReport report;
};

std::string make_document_id(std::size_t index) {
    std::ostringstream out;
    out << "doc:" << index;
    return out.str();
}

std::string scoped_shape_id(const DocumentId& document_id, const std::string& local_id) {
    if (local_id.empty()) {
        return {};
    }
    if (local_id.compare(0, 4, "doc:") == 0 && local_id.find('/') != std::string::npos) {
        return local_id;
    }
    return document_id + "/" + local_id;
}

bool parse_scoped_shape_id(const std::string& value,
                           DocumentId* document_id,
                           std::string* local_shape_id) {
    if (value.compare(0, 4, "doc:") != 0) {
        return false;
    }
    const std::size_t slash = value.find('/');
    if (slash == std::string::npos || slash == 4 || slash + 1 >= value.size()) {
        return false;
    }
    *document_id = value.substr(0, slash);
    *local_shape_id = value.substr(slash + 1);
    return true;
}

Diagnostic make_error(const std::string& code,
                      const std::string& message,
                      const std::string& shape_id = {}) {
    Diagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::error;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostic.shape_id = shape_id;
    diagnostic.shape_label = shape_id;
    return diagnostic;
}

CommandResult error_result(const std::string& code,
                           const std::string& message,
                           const std::string& shape_id = {}) {
    CommandResult result;
    result.report.diagnostics.push_back(make_error(code, message, shape_id));
    return result;
}

void scope_diagnostic(const CadDocument& document, Diagnostic& diagnostic) {
    diagnostic.document_id = document.id;
    if (diagnostic.local_shape_id.empty()) {
        diagnostic.local_shape_id = diagnostic.shape_id;
    }
    diagnostic.shape_id = scoped_shape_id(document.id, diagnostic.shape_id);
}

ConversionReport scoped_report(const CadDocument& document,
                               const ConversionReport& local_report) {
    ConversionReport report = local_report;
    report.document_id = document.id;
    report.source_path = document.source_path;

    for (Diagnostic& diagnostic : report.diagnostics) {
        scope_diagnostic(document, diagnostic);
    }

    for (AnalyzedSolid& solid : report.solids) {
        solid.id = scoped_shape_id(document.id, solid.id);
        for (std::string& face_id : solid.face_ids) {
            face_id = scoped_shape_id(document.id, face_id);
        }
    }

    for (AnalyzedFace& face : report.faces) {
        face.id = scoped_shape_id(document.id, face.id);
        face.solid_id = scoped_shape_id(document.id, face.solid_id);
    }

    for (AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        piece.id = scoped_shape_id(document.id, piece.id);
        piece.source_solid_id = scoped_shape_id(document.id, piece.source_solid_id);
    }

    for (AssemblyNode& node : report.assembly) {
        node.id = scoped_shape_id(document.id, node.id);
        node.parent_id = scoped_shape_id(document.id, node.parent_id);
        node.shape_id = scoped_shape_id(document.id, node.shape_id);
    }

    return report;
}

DecompositionProposal scoped_proposal(const CadDocument& document,
                                      const DecompositionProposal& local_proposal) {
    DecompositionProposal proposal = local_proposal;
    proposal.target_id = scoped_shape_id(document.id, proposal.target_id);
    for (Diagnostic& diagnostic : proposal.diagnostics) {
        scope_diagnostic(document, diagnostic);
    }
    for (DecompositionCandidate& candidate : proposal.candidates) {
        candidate.target_id = scoped_shape_id(document.id, candidate.target_id);
        candidate.source_face_id =
            scoped_shape_id(document.id, candidate.source_face_id);
        for (Diagnostic& diagnostic : candidate.diagnostics) {
            scope_diagnostic(document, diagnostic);
        }
    }
    return proposal;
}
} // namespace

struct Session {
    explicit Session(const ConversionOptions& options_in) : options(options_in) {}

    ConversionOptions options;
    ConversionReport last_report_value;
    DecompositionProposal last_decomposition_proposal_value;
    std::vector<CadDocument> documents;
    std::unique_ptr<DecisionLog> log;
};

void SessionDeleter::operator()(Session* session) const {
    delete session;
}

SessionPtr create_session(const ConversionOptions& options) {
    return SessionPtr(new Session(options));
}

void attach_decision_log(Session& session,
                         const std::string& log_path,
                         const std::string& step_path_hint) {
    session.log.reset(new DecisionLog(log_path, step_path_hint));
}

namespace {

// Resolve metadata for a local shape id like "solid:N". Used to stamp
// decision records with a stable identity for replay (component_entry
// plus geometric fingerprint).
const ShapeMetadata* metadata_for_local_solid_id(const CadDocument* document,
                                                 const std::string& local_shape_id) {
    if (!document || local_shape_id.empty()) {
        return nullptr;
    }
    for (const ShapeMetadata& m : document->model.solid_metadata) {
        if (m.shape_id == local_shape_id) {
            return &m;
        }
    }
    return nullptr;
}

void log_solid_op(Session& session,
                  const CadDocument* document,
                  const std::string& local_shape_id,
                  const std::string& scoped_shape_id_value,
                  const std::string& op,
                  const std::vector<DecisionParam>& params,
                  bool ok) {
    if (!session.log) return;
    DecisionTarget target;
    target.kind = "solid";
    target.shape_id = scoped_shape_id_value;
    if (const ShapeMetadata* m = metadata_for_local_solid_id(document, local_shape_id)) {
        target.component_entry = m->component_entry;
        target.fingerprint = m->fingerprint;
    }
    session.log->record(target, op, params, ok ? "applied" : "failed");
}

void log_document_op(Session& session,
                     const std::string& document_id,
                     const std::string& op,
                     const std::vector<DecisionParam>& params,
                     bool ok) {
    if (!session.log) return;
    DecisionTarget target;
    target.kind = "document";
    target.shape_id = document_id;
    session.log->record(target, op, params, ok ? "applied" : "failed");
}

} // namespace

namespace {

CommandResult session_error(Session& session,
                            const std::string& code,
                            const std::string& message,
                            const std::string& shape_id = {}) {
    CommandResult result = error_result(code, message, shape_id);
    session.last_report_value = result.report;
    return result;
}

} // namespace

DocumentId load_step(Session& session,
                     const std::string& step_path,
                     const ConversionOptions* options_override) {
    const ConversionOptions options = options_override ? *options_override : session.options;
    CadDocument document;
    document.id = make_document_id(session.documents.size() + 1);
    document.source_path = step_path;
    document.options = options;
    document.model = load_step_model(step_path, options);
    document.report.diagnostics = document.model.diagnostics;
    document.report = scoped_report(document, document.report);
    session.last_report_value = document.report;
    session.documents.push_back(document);
    const DocumentId new_id = session.documents.back().id;
    if (session.log) {
        log_document_op(session, new_id, "load_step",
                        {param_string("step_path", step_path)},
                        document.model.loaded());
    }
    return new_id;
}

CommandResult analyze(Session& session,
                      const DocumentId& document_id,
                      ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        const ConversionReport local_report = analyze_model(document.model, document.options);
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local_report)
            : local_report;
        session.last_report_value = document.report;
        return {document.report.ok(), document.report};
    }
    return session_error(session,
                         "document_not_found",
                         "Requested document ID was not found.",
                         document_id);
}

namespace {

struct ShapeTarget {
    CadDocument* document = nullptr;
    std::string local_shape_id;
    std::string error_code;
    std::string error_message;
};

ShapeTarget find_shape_target(Session& session, const std::string& shape_id) {
    DocumentId document_id;
    std::string local_shape_id;
    if (parse_scoped_shape_id(shape_id, &document_id, &local_shape_id)) {
        for (CadDocument& document : session.documents) {
            if (document.id == document_id) {
                return {&document, local_shape_id};
            }
        }
        return {nullptr, {}, "document_not_found",
                "Document prefix in shape ID did not match a loaded document."};
    }

    if (session.documents.size() == 1) {
        return {&session.documents.front(), shape_id};
    }
    if (session.documents.empty()) {
        return {nullptr, {}, "no_documents_loaded",
                "No documents are loaded in this session."};
    }
    return {nullptr, {}, "ambiguous_shape_id",
            "Unscoped shape ID is ambiguous because the session has multiple documents."};
}

CommandResult shape_target_error(Session& session,
                                 const ShapeTarget& target,
                                 const std::string& shape_id) {
    return session_error(session,
                         target.error_code.empty() ? "shape_target_not_found" : target.error_code,
                         target.error_message.empty()
                             ? "Shape ID did not resolve to a loaded document."
                             : target.error_message,
                         shape_id);
}

CommandResult finish_document_mutation(Session& session,
                                       CadDocument& document,
                                       bool ok,
                                       ReportIdMode id_mode = ReportIdMode::scoped) {
    const ConversionReport local_report = analyze_model(document.model, document.options);
    document.report = id_mode == ReportIdMode::scoped
        ? scoped_report(document, local_report)
        : local_report;
    session.last_report_value = document.report;
    return {ok && document.report.ok(), document.report};
}

CommandResult mutate_solid(Session& session,
                           const std::string& shape_id,
                           bool (*operation)(CadModel&, const std::string&, const ConversionOptions&),
                           ReportIdMode id_mode,
                           const std::string& op_name) {
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        log_solid_op(session, nullptr, {}, shape_id, op_name, {}, false);
        return shape_target_error(session, target, shape_id);
    }
    const bool ok = operation(target.document->model,
                              target.local_shape_id,
                              target.document->options);
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 op_name, {}, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

} // namespace

CommandResult remove_solid(Session& session,
                           const std::string& shape_id,
                           ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        log_solid_op(session, nullptr, {}, shape_id, "remove_solid", {}, false);
        return shape_target_error(session, target, shape_id);
    }
    const bool ok = remove_solid(target.document->model, target.local_shape_id);
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "remove_solid", {}, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

CommandResult replace_solid_with_bbox(Session& session,
                                      const std::string& shape_id,
                                      ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, replace_solid_with_bbox, id_mode,
                        "replace_solid_with_bbox");
}

CommandResult replace_solid_with_bbox_cylinder(Session& session,
                                               const std::string& shape_id,
                                               ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, replace_solid_with_bbox_cylinder, id_mode,
                        "replace_solid_with_bbox_cylinder");
}

CommandResult replace_solid_with_fitted_cylinder(Session& session,
                                                 const std::string& shape_id,
                                                 ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, replace_solid_with_fitted_cylinder, id_mode,
                        "replace_solid_with_fitted_cylinder");
}

CommandResult replace_solid_with_fitted_sphere(Session& session,
                                               const std::string& shape_id,
                                               ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, replace_solid_with_fitted_sphere, id_mode,
                        "replace_solid_with_fitted_sphere");
}

CommandResult replace_solid_with_fitted_cone(Session& session,
                                             const std::string& shape_id,
                                             ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, replace_solid_with_fitted_cone, id_mode,
                        "replace_solid_with_fitted_cone");
}

CommandResult apply_replacement_operation(Session& session,
                                          const std::string& shape_id,
                                          ReplacementOperation operation,
                                          ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        log_solid_op(session, nullptr, {}, shape_id, "apply_replacement_operation",
                     {param_int("operation", static_cast<long long>(operation))}, false);
        return shape_target_error(session, target, shape_id);
    }
    const bool ok = apply_replacement_operation(target.document->model,
                                                target.local_shape_id,
                                                operation,
                                                target.document->options);
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "apply_replacement_operation",
                 {param_int("operation", static_cast<long long>(operation))}, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

ReplacementCommandResult apply_checked_replacement_operation(
    Session& session,
    const std::string& shape_id,
    ReplacementOperation operation,
    const ReplacementLimits& limits,
    ReportIdMode id_mode) {
    ReplacementCommandResult result;
    ShapeTarget target = find_shape_target(session, shape_id);
    const std::vector<DecisionParam> params = {
        param_int("operation", static_cast<long long>(operation)),
        param_bool("has_max_abs_delta", limits.has_absolute_delta),
        param_double("max_abs_delta", limits.max_absolute_delta),
        param_bool("has_max_rel_delta", limits.has_relative_delta),
        param_double("max_rel_delta", limits.max_relative_delta),
    };
    if (!target.document) {
        log_solid_op(session, nullptr, {}, shape_id,
                     "apply_checked_replacement_operation", params, false);
        result.command = shape_target_error(session, target, shape_id);
        return result;
    }
    const bool ok = apply_checked_replacement_operation(target.document->model,
                                                        target.local_shape_id,
                                                        operation,
                                                        limits,
                                                        &result.check,
                                                        target.document->options);
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "apply_checked_replacement_operation", params, ok);
    result.command = finish_document_mutation(session, *target.document, ok, id_mode);
    return result;
}

CommandResult replace_face_with_fit(Session& session,
                                    const std::string& shape_id,
                                    SplineFitKind kind,
                                    bool has_max_deviation,
                                    double max_deviation,
                                    ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    const std::vector<DecisionParam> params = {
        param_int("kind", static_cast<long long>(kind)),
        param_bool("has_max_deviation", has_max_deviation),
        param_double("max_deviation", max_deviation),
    };
    if (!target.document) {
        if (session.log) {
            DecisionTarget log_target;
            log_target.kind = "face";
            log_target.shape_id = shape_id;
            session.log->record(log_target, "replace_face_with_fit", params, "failed");
        }
        return shape_target_error(session, target, shape_id);
    }
    const bool ok = replace_face_with_fit(target.document->model,
                                          target.local_shape_id,
                                          kind,
                                          has_max_deviation,
                                          max_deviation,
                                          nullptr,
                                          target.document->options);
    if (session.log) {
        DecisionTarget log_target;
        log_target.kind = "face";
        log_target.shape_id = shape_id;
        session.log->record(log_target, "replace_face_with_fit", params,
                            ok ? "applied" : "failed");
    }
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

CommandResult auto_fit_splines(Session& session,
                               const DocumentId& document_id,
                               bool has_max_deviation,
                               double max_deviation,
                               ReportIdMode id_mode) {
    return auto_fit_splines_with_summary(session,
                                         document_id,
                                         has_max_deviation,
                                         max_deviation,
                                         id_mode).command;
}

AutoFitCommandResult auto_fit_splines_with_summary(
    Session& session,
    const DocumentId& document_id,
    bool has_max_deviation,
    double max_deviation,
    ReportIdMode id_mode) {
    AutoFitCommandResult result;
    const std::vector<DecisionParam> params = {
        param_bool("has_max_deviation", has_max_deviation),
        param_double("max_deviation", max_deviation),
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        result.summary = auto_fit_splines(document.model,
                                          has_max_deviation,
                                          max_deviation,
                                          document.options);
        log_document_op(session, document_id, "auto_fit_splines", params, true);
        result.command = finish_document_mutation(session, document, true, id_mode);
        return result;
    }
    log_document_op(session, document_id, "auto_fit_splines", params, false);
    result.command = session_error(session,
                                   "document_not_found",
                                   "Requested document ID was not found.",
                                   document_id);
    return result;
}

CommandResult decompose_planar(Session& session,
                               const std::string& shape_id,
                               ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, decompose_solid_by_planar_faces, id_mode,
                        "decompose_planar");
}

CommandResult decompose_by_face(Session& session,
                                const std::string& solid_shape_id,
                                const std::string& face_shape_id,
                                ReportIdMode id_mode) {
    ShapeTarget solid_target = find_shape_target(session, solid_shape_id);
    const std::vector<DecisionParam> params = {
        param_string("face_id", face_shape_id),
    };
    if (!solid_target.document) {
        log_solid_op(session, nullptr, {}, solid_shape_id,
                     "decompose_by_face", params, false);
        return shape_target_error(session, solid_target, solid_shape_id);
    }

    ShapeTarget face_target = find_shape_target(session, face_shape_id);
    if (!face_target.document) {
        log_solid_op(session, solid_target.document, solid_target.local_shape_id,
                     solid_shape_id, "decompose_by_face", params, false);
        return shape_target_error(session, face_target, face_shape_id);
    }
    if (face_target.document != solid_target.document) {
        log_solid_op(session, solid_target.document, solid_target.local_shape_id,
                     solid_shape_id, "decompose_by_face", params, false);
        return session_error(session,
                             "cross_document_face_id",
                             "Directed decomposition face must belong to the same document as the solid.",
                             face_shape_id);
    }

    const bool ok = decompose_solid_by_face(solid_target.document->model,
                                            solid_target.local_shape_id,
                                            face_target.local_shape_id,
                                            solid_target.document->options);
    log_solid_op(session, solid_target.document, solid_target.local_shape_id,
                 solid_shape_id, "decompose_by_face", params, ok);
    return finish_document_mutation(session, *solid_target.document, ok, id_mode);
}

DecompositionProposal propose_decomposition(Session& session,
                                            const std::string& solid_shape_id,
                                            ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, solid_shape_id);
    if (!target.document) {
        DecompositionProposal proposal;
        proposal.target_id = solid_shape_id;
        proposal.diagnostics.push_back(make_error(
            target.error_code.empty() ? "shape_target_not_found" : target.error_code,
            target.error_message.empty()
                ? "Shape ID did not resolve to a loaded document."
                : target.error_message,
            solid_shape_id));
        session.last_decomposition_proposal_value = proposal;
        return proposal;
    }

    DecompositionProposal local =
        propose_decomposition_candidates(target.document->model,
                                         target.local_shape_id,
                                         target.document->options);
    DecompositionProposal proposal = id_mode == ReportIdMode::scoped
        ? scoped_proposal(*target.document, local)
        : local;
    session.last_decomposition_proposal_value = proposal;
    return proposal;
}

namespace {

bool target_exact_after_analysis(const CadDocument& document,
                                 const std::string& local_shape_id,
                                 bool* found_out) {
    const ConversionReport report = analyze_model(document.model, document.options);
    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.id == local_shape_id) {
            *found_out = true;
            return solid.exact_csg_supported;
        }
    }
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        if (piece.id == local_shape_id) {
            *found_out = true;
            return piece.exact_csg_supported;
        }
    }
    *found_out = false;
    return false;
}

bool candidate_allowed_by_policy(const DecompositionCandidate& candidate,
                                 const DecompositionPolicy& policy,
                                 std::string* reason_out) {
    if (!candidate.usable) {
        *reason_out = "candidate is not usable";
        return false;
    }
    if (candidate.relative_delta > policy.max_relative_delta) {
        std::ostringstream msg;
        msg << "candidate relative_delta=" << candidate.relative_delta
            << " exceeds policy max_relative_delta="
            << policy.max_relative_delta;
        *reason_out = msg.str();
        return false;
    }
    if (candidate.min_piece_volume_fraction < policy.min_piece_volume_fraction) {
        std::ostringstream msg;
        msg << "candidate min_piece_volume_fraction="
            << candidate.min_piece_volume_fraction
            << " is below policy min_piece_volume_fraction="
            << policy.min_piece_volume_fraction;
        *reason_out = msg.str();
        return false;
    }
    if (candidate.reason == "bbox_axis_fallback"
        && !policy.allow_bbox_fallback_auto) {
        *reason_out = "bbox fallback candidate requires user or agent approval";
        return false;
    }
    if (candidate.confidence == "high") {
        if (candidate.reason == "bbox_axis_fallback") {
            return policy.allow_bbox_fallback_auto;
        }
        if (!policy.allow_high_confidence_face_auto) {
            *reason_out = "policy disables high-confidence face auto-decomposition";
            return false;
        }
        return true;
    }
    if (candidate.confidence == "medium"
        && policy.allow_medium_confidence_auto) {
        return true;
    }
    *reason_out = "candidate confidence is " + candidate.confidence
        + " and policy does not allow automatic application";
    return false;
}

const DecompositionCandidate* first_policy_allowed_candidate(
    const DecompositionProposal& proposal,
    const DecompositionPolicy& policy,
    std::string* reason_out) {
    *reason_out = "no usable candidate was available";
    for (const DecompositionCandidate& candidate : proposal.candidates) {
        std::string reason;
        if (candidate_allowed_by_policy(candidate, policy, &reason)) {
            return &candidate;
        }
        if (candidate.usable) {
            *reason_out = reason;
        }
    }
    return nullptr;
}

} // namespace

CommandResult apply_decomposition_candidate(Session& session,
                                            const std::string& shape_id,
                                            const std::string& candidate_id,
                                            ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        return shape_target_error(session, target, shape_id);
    }

    DecompositionProposal proposal =
        propose_decomposition_candidates(target.document->model,
                                         target.local_shape_id,
                                         target.document->options);
    session.last_decomposition_proposal_value = id_mode == ReportIdMode::scoped
        ? scoped_proposal(*target.document, proposal)
        : proposal;

    const DecompositionCandidate* selected = nullptr;
    for (const DecompositionCandidate& candidate : proposal.candidates) {
        if (candidate.id == candidate_id) {
            selected = &candidate;
            break;
        }
    }
    if (!selected) {
        return session_error(session,
                             "decomposition_candidate_not_found",
                             "Requested decomposition candidate ID was not found in the current proposal.",
                             shape_id);
    }
    if (!selected->usable) {
        return session_error(session,
                             "decomposition_candidate_not_usable",
                             "Requested decomposition candidate was rejected by proposal scoring.",
                             shape_id);
    }

    const bool ok = decompose_solid_by_face(target.document->model,
                                            target.local_shape_id,
                                            selected->source_face_id,
                                            target.document->options);
    log_solid_op(session,
                 target.document,
                 target.local_shape_id,
                 shape_id,
                 "apply_decomposition_candidate",
                 {param_string("candidate_id", candidate_id),
                  param_string("face_id", selected->source_face_id),
                  param_double("score", selected->score),
                  param_string("confidence", selected->confidence)},
                 ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

CommandResult auto_decompose_bsp(Session& session,
                                 const std::string& shape_id,
                                 int max_depth,
                                 const DecompositionPolicy& policy,
                                 ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        return shape_target_error(session, target, shape_id);
    }
    if (max_depth < 0) {
        return session_error(session,
                             "invalid_max_depth",
                             "BSP auto-decomposition max_depth must be non-negative.",
                             shape_id);
    }

    struct QueueItem {
        std::string local_id;
        int depth = 0;
    };

    std::vector<QueueItem> queue;
    queue.push_back({target.local_shape_id, 0});
    std::size_t splits_applied = 0;
    std::size_t targets_seen = 0;
    bool stopped = false;

    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const QueueItem item = queue[cursor];
        ++targets_seen;

        bool found = false;
        if (target_exact_after_analysis(*target.document, item.local_id, &found)) {
            continue;
        }
        if (!found) {
            Diagnostic diagnostic = make_error(
                "auto_decompose_target_not_found",
                "BSP auto-decomposition target disappeared before it was processed.",
                item.local_id);
            target.document->model.diagnostics.push_back(diagnostic);
            stopped = true;
            break;
        }

        if (item.depth >= max_depth) {
            DecompositionProposal proposal =
                propose_decomposition_candidates(target.document->model,
                                                 item.local_id,
                                                 target.document->options);
            session.last_decomposition_proposal_value = id_mode == ReportIdMode::scoped
                ? scoped_proposal(*target.document, proposal)
                : proposal;
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::warning;
            diagnostic.code = "auto_decompose_bsp_max_depth";
            diagnostic.message =
                "BSP auto-decomposition stopped because max_depth was reached.";
            diagnostic.shape_id = item.local_id;
            diagnostic.shape_label = item.local_id;
            target.document->model.diagnostics.push_back(diagnostic);
            stopped = true;
            break;
        }

        DecompositionProposal proposal =
            propose_decomposition_candidates(target.document->model,
                                             item.local_id,
                                             target.document->options);
        session.last_decomposition_proposal_value = id_mode == ReportIdMode::scoped
            ? scoped_proposal(*target.document, proposal)
            : proposal;
        std::string policy_stop_reason;
        const DecompositionCandidate* best =
            first_policy_allowed_candidate(proposal, policy, &policy_stop_reason);
        if (!best) {
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::warning;
            diagnostic.code = "auto_decompose_bsp_needs_decision";
            diagnostic.message = "BSP auto-decomposition stopped: "
                + policy_stop_reason + ".";
            diagnostic.shape_id = item.local_id;
            diagnostic.shape_label = item.local_id;
            target.document->model.diagnostics.push_back(diagnostic);
            stopped = true;
            break;
        }

        const std::size_t before = target.document->model.decomposed_pieces.size();
        const bool ok = decompose_solid_by_face(target.document->model,
                                                item.local_id,
                                                best->source_face_id,
                                                target.document->options);
        log_solid_op(session,
                     target.document,
                     item.local_id,
                     scoped_shape_id(target.document->id, item.local_id),
                     "decompose_by_face",
                     {param_string("face_id",
                                   scoped_shape_id(target.document->id,
                                                   best->source_face_id))},
                     ok);
        if (!ok) {
            stopped = true;
            break;
        }
        ++splits_applied;

        for (std::size_t i = before;
             i < target.document->model.decomposed_pieces.size(); ++i) {
            const std::size_t produced_index = i - before;
            const DecomposedSolidPiece& piece =
                target.document->model.decomposed_pieces[i];
            if (!piece.active) {
                continue;
            }
            bool exact_from_candidate = false;
            if (produced_index < best->pieces.size()) {
                exact_from_candidate =
                    best->pieces[produced_index].exact_csg_supported;
            }
            if (!exact_from_candidate) {
                queue.push_back({piece.id, item.depth + 1});
            }
        }
    }

    Diagnostic summary;
    summary.severity = stopped ? DiagnosticSeverity::warning : DiagnosticSeverity::info;
    summary.code = stopped ? "auto_decompose_bsp_stopped" : "auto_decompose_bsp_complete";
    std::ostringstream msg;
    msg << "BSP auto-decomposition processed " << targets_seen
        << " targets and applied " << splits_applied << " high-confidence splits.";
    summary.message = msg.str();
    summary.shape_id = target.local_shape_id;
    summary.shape_label = target.local_shape_id;
    target.document->model.diagnostics.push_back(summary);

    return finish_document_mutation(session,
                                    *target.document,
                                    !stopped,
                                    id_mode);
}

CommandResult emit_document(Session& session,
                            const DocumentId& document_id,
                            const std::string& output_path,
                            ReportIdMode id_mode) {
    const std::vector<DecisionParam> params = {
        param_string("output_path", output_path),
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        ConversionResult conversion = convert_model(document.model, document.options);
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, conversion.report)
            : conversion.report;
        if (document.report.ok()) {
            const int export_status = export_mcnp(conversion.system.get(), output_path);
            if (export_status != 0) {
                Diagnostic diagnostic = make_error(
                    "export_failed",
                    "Alea MCNP export failed.",
                    document_id);
                diagnostic.document_id = document.id;
                document.report.diagnostics.push_back(diagnostic);
            }
        }
        log_document_op(session, document_id, "emit_mcnp", params,
                        document.report.ok());
        session.last_report_value = document.report;
        return {document.report.ok(), document.report};
    }
    log_document_op(session, document_id, "emit_mcnp", params, false);
    return session_error(session,
                         "document_not_found",
                         "Requested document ID was not found.",
                         document_id);
}

CommandResult export_document_step(Session& session,
                                   const DocumentId& document_id,
                                   const std::string& output_path,
                                   ReportIdMode id_mode) {
    const std::vector<DecisionParam> params = {
        param_string("output_path", output_path),
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        const int rc = export_step(document.model, output_path);
        const bool ok = rc == 0;
        if (!ok) {
            Diagnostic diagnostic = make_error(
                "step_export_failed",
                "STEP export of as-simplified geometry failed.",
                document_id);
            diagnostic.document_id = document.id;
            document.report.diagnostics.push_back(diagnostic);
        }
        log_document_op(session, document_id, "export_step", params, ok);
        ConversionReport local;
        local.diagnostics = document.report.diagnostics;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local)
            : local;
        session.last_report_value = document.report;
        return {ok, document.report};
    }
    log_document_op(session, document_id, "export_step", params, false);
    return session_error(session,
                         "document_not_found",
                         "Requested document ID was not found.",
                         document_id);
}

VolumeComparison compare_document_volumes(Session& session,
                                          const DocumentId& original_document_id,
                                          const DocumentId& candidate_document_id,
                                          double absolute_tolerance,
                                          double relative_tolerance,
                                          ReportIdMode id_mode) {
    VolumeComparison comparison;
    comparison.absolute_tolerance = absolute_tolerance;
    comparison.relative_tolerance = relative_tolerance;
    comparison.original = analyze(session, original_document_id, id_mode).report;
    comparison.candidate = analyze(session, candidate_document_id, id_mode).report;
    comparison.absolute_delta =
        std::abs(comparison.candidate.total_volume - comparison.original.total_volume);
    if (std::abs(comparison.original.total_volume) > 1.0e-12) {
        comparison.relative_delta = comparison.absolute_delta
            / std::abs(comparison.original.total_volume);
    } else {
        comparison.relative_delta = comparison.absolute_delta > 1.0e-12 ? 1.0 : 0.0;
    }

    const bool abs_ok = absolute_tolerance <= 0.0
        || comparison.absolute_delta <= absolute_tolerance;
    const bool rel_ok = relative_tolerance <= 0.0
        || comparison.relative_delta <= relative_tolerance;
    comparison.within_tolerance = abs_ok && rel_ok;
    return comparison;
}

const ConversionReport& last_report(const Session& session) {
    return session.last_report_value;
}

const DecompositionProposal& last_decomposition_proposal(const Session& session) {
    return session.last_decomposition_proposal_value;
}

std::size_t document_count(const Session& session) {
    return session.documents.size();
}

} // namespace cad2csg
