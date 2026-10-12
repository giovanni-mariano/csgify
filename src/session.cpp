// Session/workspace API. This is the future user/agent boundary: callers
// work with documents, IDs, commands, and reports. OCCT-backed CadModel state
// stays behind the Session object.

#include "csgify/session.hpp"

#include "approximation_internal.hpp"
#include "cleanup_internal.hpp"
#include "coalesce_internal.hpp"
#include "decision_log.hpp"
#include "decomposition_ledger.hpp"
#include "engine_internal.hpp"
#include "engine_model.hpp"
#include "hole_fill.hpp"
#include "local_partition.hpp"
#include "local_overlap.hpp"
#include "occt_boolean.hpp"
#include "session_impl.hpp"
#include "session_view.hpp"
#include "step_export.hpp"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

namespace csgify {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

// CadDocument is defined in session_impl.hpp

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

std::string scoped_decision_id(const DocumentId& document_id,
                               const std::string& id,
                               const std::string& local_target_id) {
    if (id.empty() || local_target_id.empty()) {
        return id;
    }
    if (id.find(document_id + "/") != std::string::npos) {
        return id;
    }
    std::string out = id;
    const std::string scoped_target =
        scoped_shape_id(document_id, local_target_id);
    std::size_t pos = 0;
    while ((pos = out.find(local_target_id, pos)) != std::string::npos) {
        out.replace(pos, local_target_id.size(), scoped_target);
        pos += scoped_target.size();
    }
    return out;
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
    report.approximation_policy = document.options.approximation;
    report.approximation_ledger = document.approximation_ledger;
    if (!report.mcnp_geometry_safety.domain_id.empty())
        report.mcnp_geometry_safety.domain_id = scoped_shape_id(
            document.id, report.mcnp_geometry_safety.domain_id);
    report.document_revision = document.revision;
    report.document_id = document.id;
    report.source_path = document.source_path;
    report.performance = document.performance;

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

    for (ComponentInstance& instance : report.component_instances) {
        instance.parent_assembly_id =
            scoped_shape_id(document.id, instance.parent_assembly_id);
        instance.source_shape_id = scoped_shape_id(document.id, instance.source_shape_id);
    }

    for (DecisionRequest& request : report.decision_requests) {
        const std::string local_target_id = request.target_id;
        request.target_id = scoped_shape_id(document.id, request.target_id);
        request.id = scoped_decision_id(document.id, request.id, local_target_id);
    }

    for (SolidOverlap& overlap : report.overlaps) {
        overlap.solid_a_id = scoped_shape_id(document.id, overlap.solid_a_id);
        overlap.solid_b_id = scoped_shape_id(document.id, overlap.solid_b_id);
    }

    for (ProvenanceRecord& provenance : report.provenance) {
        provenance.entity_id =
            scoped_shape_id(document.id, provenance.entity_id);
        for (std::string& source_id : provenance.source_ids) {
            source_id = scoped_shape_id(document.id, source_id);
        }
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

RemovalProposal scoped_removal_proposal(const CadDocument& document,
                                        const RemovalProposal& local_proposal) {
    RemovalProposal proposal = local_proposal;
    for (Diagnostic& diagnostic : proposal.diagnostics) {
        scope_diagnostic(document, diagnostic);
    }
    for (RemovalCandidate& candidate : proposal.candidates) {
        candidate.solid_id = scoped_shape_id(document.id, candidate.solid_id);
    }
    return proposal;
}

HoleFillProposal scoped_hole_fill_proposal(const CadDocument& document,
                                           const HoleFillProposal& local_proposal) {
    HoleFillProposal proposal = local_proposal;
    for (Diagnostic& diagnostic : proposal.diagnostics) {
        scope_diagnostic(document, diagnostic);
    }
    for (HoleFillCandidate& candidate : proposal.candidates) {
        candidate.host_solid_id =
            scoped_shape_id(document.id, candidate.host_solid_id);
        for (std::string& id : candidate.seed_removed_solid_ids) {
            id = scoped_shape_id(document.id, id);
        }
        for (std::string& id : candidate.face_ids) {
            id = scoped_shape_id(document.id, id);
        }
    }
    for (DecisionRequest& request : proposal.decision_requests) {
        request.target_id = scoped_shape_id(document.id, request.target_id);
    }
    return proposal;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

bool contains_any_word(const std::string& haystack,
                       const std::vector<const char*>& words) {
    for (const char* word : words) {
        if (haystack.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string duplicate_key(const AnalyzedSolid& solid) {
    auto bucket = [](double value) -> long long {
        return static_cast<long long>(std::llround(value * 1.0e6));
    };
    std::ostringstream out;
    out << bucket(solid.volume) << ":"
        << bucket(solid.bbox.xmax - solid.bbox.xmin) << ":"
        << bucket(solid.bbox.ymax - solid.bbox.ymin) << ":"
        << bucket(solid.bbox.zmax - solid.bbox.zmin) << ":"
        << solid.face_count;
    return out.str();
}

RemovalProposal build_removal_proposal(const ConversionReport& report) {
    RemovalProposal proposal;
    proposal.total_volume = report.total_volume;

    std::map<std::string, std::size_t> duplicate_counts;
    for (const AnalyzedSolid& solid : report.solids) {
        duplicate_counts[duplicate_key(solid)] += 1;
    }

    constexpr double kNearZeroVolumeFraction = 1.0e-12;
    constexpr double kTinyVolumeFraction = 1.0e-8;
    constexpr double kSmallVolumeFraction = 1.0e-4;
    constexpr double kTinyBBoxFraction = 1.0e-5;

    std::size_t candidate_index = 0;
    for (const AnalyzedSolid& solid : report.solids) {
        RemovalCandidate candidate;
        candidate.id = "removal:" + std::to_string(++candidate_index);
        candidate.solid_id = solid.id;
        const auto instance = std::find_if(
            report.component_instances.begin(), report.component_instances.end(),
            [&](const ComponentInstance& value) {
                return value.source_shape_id == solid.id;
            });
        const ComponentInstance* owner = instance == report.component_instances.end()
            ? nullptr : &*instance;
        // Multi-solid XCAF occurrences retain one source_shape_id for legacy
        // compatibility. The remaining bodies share the occurrence path, so
        // resolve them through the assembly node instead of leaving them as
        // unrelated solid-only removal rows.
        if (!owner && !solid.hierarchy_path.empty()) {
            const auto node = std::find_if(report.assembly.begin(),
                report.assembly.end(), [&](const AssemblyNode& value) {
                    return value.kind == "shape"
                        && value.path == solid.hierarchy_path
                        && !value.instance_id.empty();
                });
            if (node != report.assembly.end()) {
                const auto semantic_instance = std::find_if(
                    report.component_instances.begin(),
                    report.component_instances.end(),
                    [&](const ComponentInstance& value) {
                        return value.id == node->instance_id;
                    });
                if (semantic_instance != report.component_instances.end())
                    owner = &*semantic_instance;
            }
        }
        if (owner) {
            candidate.component_instance_id = owner->id;
            candidate.component_definition_id = owner->definition_id;
        }
        candidate.label = solid.label;
        candidate.name = solid.name;
        candidate.hierarchy_path = solid.hierarchy_path;
        candidate.volume = solid.volume;
        candidate.volume_fraction = solid.volume_fraction;
        candidate.bbox = solid.bbox;
        candidate.bbox_diagonal_fraction = solid.bbox_diagonal_fraction;
        candidate.confidence = "low";
        candidate.suggested_action = "manual";

        const std::string text =
            lower_copy(solid.name + " " + solid.label + " " + solid.hierarchy_path);
        const bool hardware_name = contains_any_word(
            text, {"bolt", "screw", "nut", "washer", "pin", "rivet", "fastener"});
        const bool cosmetic_name = contains_any_word(
            text, {"logo", "label", "text", "marking", "cosmetic", "decorative"});
        const bool protected_name = contains_any_word(
            text, {"shield", "absorber", "detector", "source", "fuel",
                   "boron", "borated", "cadmium", "hafnium"});
        const bool repeated = duplicate_counts[duplicate_key(solid)] >= 3;

        if (solid.volume_fraction <= kNearZeroVolumeFraction) {
            candidate.reason_codes.push_back("near_zero_volume");
            candidate.score += 100.0;
        }
        if (solid.volume_fraction <= kTinyVolumeFraction) {
            candidate.reason_codes.push_back("tiny_volume");
            candidate.score += 40.0;
        } else if (solid.volume_fraction <= kSmallVolumeFraction) {
            candidate.reason_codes.push_back("small_volume");
            candidate.score += 15.0;
        }
        if (solid.bbox_diagonal_fraction <= kTinyBBoxFraction) {
            candidate.reason_codes.push_back("tiny_bbox");
            candidate.score += 20.0;
        }
        if (hardware_name) {
            candidate.reason_codes.push_back("recognized_hardware_name");
            candidate.score += 15.0;
        }
        if (cosmetic_name) {
            candidate.reason_codes.push_back("recognized_cosmetic_name");
            candidate.score += 25.0;
        }
        if (repeated) {
            candidate.reason_codes.push_back("repeated_geometry_group");
            candidate.score += 15.0;
        }
        if (solid.face_count == 0) {
            candidate.reason_codes.push_back("invalid_or_empty_geometry");
            candidate.score += 80.0;
        }

        if (protected_name) {
            candidate.blockers.push_back("protected_name_or_material");
            candidate.score -= 100.0;
        }
        if (solid.volume_fraction > 1.0e-3) {
            candidate.blockers.push_back("large_volume_fraction");
            candidate.score -= 30.0;
        }
        if (!hardware_name && !cosmetic_name
            && solid.volume_fraction > kTinyVolumeFraction) {
            candidate.blockers.push_back("unknown_component_semantics");
        }

        if (candidate.reason_codes.empty()) {
            --candidate_index;
            continue;
        }

        if (candidate.blockers.empty()
            && (solid.volume_fraction <= kNearZeroVolumeFraction
                || (hardware_name && repeated
                    && solid.volume_fraction <= kTinyVolumeFraction)
                || (cosmetic_name && solid.volume_fraction <= kSmallVolumeFraction))) {
            candidate.confidence = "high";
            candidate.suggested_action = "remove_solid";
        } else if (!protected_name
                   && (solid.volume_fraction <= kSmallVolumeFraction
                       || hardware_name || cosmetic_name || repeated)) {
            candidate.confidence = "medium";
            candidate.suggested_action = "review_remove_solid";
        }

        if (candidate.confidence == "high") {
            ++proposal.high_confidence_count;
            proposal.high_confidence_volume += candidate.volume;
        }
        proposal.candidates.push_back(std::move(candidate));
    }

    if (proposal.total_volume > 0.0) {
        proposal.high_confidence_volume_fraction =
            proposal.high_confidence_volume / proposal.total_volume;
    }

    std::stable_sort(proposal.candidates.begin(), proposal.candidates.end(),
                     [](const RemovalCandidate& a, const RemovalCandidate& b) {
        auto rank = [](const std::string& confidence) {
            if (confidence == "high") return 3;
            if (confidence == "medium") return 2;
            return 1;
        };
        const int ar = rank(a.confidence);
        const int br = rank(b.confidence);
        if (ar != br) return ar > br;
        return a.score > b.score;
    });

    proposal.ok = !proposal.candidates.empty();
    if (!proposal.ok) {
        proposal.diagnostics.push_back({
            DiagnosticSeverity::info,
            "removal_no_candidates",
            "No removable component candidates were found."
        });
    }
    return proposal;
}
} // namespace

// Session is defined in session_impl.hpp

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

void set_decision_audit_context(Session& session,
                                const DecisionAuditContext& context) {
    if (session.log) session.log->set_audit_context(context);
}

void clear_decision_audit_context(Session& session) {
    if (session.log) session.log->clear_audit_context();
}

void record_decision_review(Session& session,
                            const std::string& target_id,
                            const std::string& proposal_id,
                            const std::string& decision,
                            const DecisionAuditContext& context,
                            const std::string& review_kind,
                            const std::string& action,
                            const std::string& candidate_id,
                            std::size_t base_revision) {
    if (session.log) {
        session.log->set_audit_context(context);
        try {
            session.log->record_review(target_id, proposal_id, decision,
                review_kind, action, candidate_id, base_revision);
        } catch (...) {
            session.log->clear_audit_context();
            throw;
        }
        session.log->clear_audit_context();
    }
    DecisionReviewRecord restored;
    restored.target_id = target_id;
    restored.proposal_id = proposal_id;
    restored.decision = decision;
    restored.review_kind = review_kind;
    restored.action = action;
    restored.candidate_id = candidate_id;
    restored.base_revision = base_revision;
    restored.audit = context;
    session.reviews.push_back(std::move(restored));
}

std::vector<DecisionReviewRecord> decision_reviews(const Session& session) {
    return session.reviews;
}

std::vector<DecisionReviewRecord> latest_decision_reviews(
    const Session& session, const std::string& decision) {
    std::vector<DecisionReviewRecord> latest;
    std::set<std::pair<std::string, std::string>> seen;
    for (auto review = session.reviews.rbegin();
         review != session.reviews.rend(); ++review) {
        const auto identity = std::make_pair(
            review->target_id, review->proposal_id);
        if (!seen.insert(identity).second) continue;
        if (decision.empty() || review->decision == decision)
            latest.push_back(*review);
    }
    std::reverse(latest.begin(), latest.end());
    return latest;
}

std::string decision_review_status(const Session& session,
                                   const std::string& target_id,
                                   const std::string& proposal_id) {
    for (auto review = session.reviews.rbegin();
         review != session.reviews.rend(); ++review) {
        if (review->target_id == target_id
            && review->proposal_id == proposal_id) {
            return review->decision;
        }
    }
    return {};
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

std::string localize_shape_id_for_document(const CadDocument& document,
                                           const std::string& shape_id) {
    DocumentId document_id;
    std::string local_shape_id;
    if (parse_scoped_shape_id(shape_id, &document_id, &local_shape_id)
        && document_id == document.id) {
        return local_shape_id;
    }
    return shape_id;
}

Diagnostic diagnostic(DiagnosticSeverity severity,
                      const std::string& code,
                      const std::string& message,
                      const std::string& shape_id = {}) {
    Diagnostic result;
    result.severity = severity;
    result.code = code;
    result.message = message;
    result.shape_id = shape_id;
    result.shape_label = shape_id;
    return result;
}

CommandResult session_diagnostic(Session& session, const Diagnostic& d) {
    CommandResult result;
    result.report.diagnostics.push_back(d);
    session.last_report_value = result.report;
    return result;
}

struct SessionSolidLookup {
    bool found = false;
    bool active = false;
    TopoDS_Solid solid;
    double volume = 0.0;
};

SessionSolidLookup find_document_solid(const CadDocument& document,
                                       const std::string& local_id) {
    SessionSolidLookup result;
    std::size_t wanted = 0;
    if (!detail::parse_solid_id(local_id, &wanted)) {
        return result;
    }
    std::size_t index = 0;
    for (TopExp_Explorer exp(document.model.root, TopAbs_SOLID);
         exp.More();
         exp.Next()) {
        ++index;
        if (index != wanted) {
            continue;
        }
        result.found = true;
        result.active = !detail::contains_id(document.model.removed_solid_ids,
                                             local_id);
        result.solid = TopoDS::Solid(exp.Current());
        GProp_GProps props;
        BRepGProp::VolumeProperties(result.solid, props);
        result.volume = std::abs(props.Mass());
        return result;
    }
    return result;
}

bool finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

const char* spline_kind_name_public(SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::plane: return "plane";
    case SplineFitKind::sphere: return "sphere";
    case SplineFitKind::cylinder: return "cylinder";
    case SplineFitKind::cone: return "cone";
    case SplineFitKind::none: return "none";
    }
    return "none";
}

std::string replacement_action_for_spline_kind(SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::plane: return "replace_face_with_fit:plane";
    case SplineFitKind::sphere: return "replace_face_with_fit:sphere";
    case SplineFitKind::cylinder: return "replace_face_with_fit:cylinder";
    case SplineFitKind::cone: return "replace_face_with_fit:cone";
    case SplineFitKind::none: break;
    }
    return "manual";
}

std::string fit_confidence_for_proposal(const SplineFaceFit& fit) {
    if (!fit.available) return "none";
    if (fit.relative_max_deviation <= 1.0e-8) return "high";
    if (fit.relative_max_deviation <= 1.0e-4) return "medium";
    return "low";
}

void add_fit_surface_candidate(SurfaceReplacementProposal& proposal,
                               const AnalyzedFace& face,
                               const SplineFaceFit& fit) {
    if (!fit.available) return;
    SurfaceReplacementCandidate candidate;
    candidate.id = "candidate:" + std::to_string(proposal.candidates.size() + 1);
    candidate.face_id = face.id;
    candidate.owner_target_id = face.solid_id;
    candidate.kind = spline_kind_name_public(fit.kind);
    candidate.action = replacement_action_for_spline_kind(fit.kind);
    candidate.confidence = fit_confidence_for_proposal(fit);
    candidate.reason = fit.deviation_certified
        ? "bounded_analytic_fit" : "sampled_analytic_fit";
    candidate.deviation_certified = fit.deviation_certified;
    candidate.exact = fit.deviation_certified
        && fit.relative_max_deviation <= 1.0e-10;
    candidate.usable = true;
    candidate.score = candidate.exact ? 100.0 : 1.0 / (1.0 + fit.relative_max_deviation);
    candidate.max_deviation = fit.max_deviation;
    candidate.rms_deviation = fit.rms_deviation;
    candidate.relative_max_deviation = fit.relative_max_deviation;
    candidate.origin = fit.origin;
    candidate.direction = fit.direction;
    candidate.radius = fit.radius;
    candidate.half_angle = fit.half_angle;
    proposal.candidates.push_back(std::move(candidate));
}

SplineFitKind spline_kind_from_public_name(const std::string& kind) {
    if (kind == "plane") return SplineFitKind::plane;
    if (kind == "sphere") return SplineFitKind::sphere;
    if (kind == "cylinder") return SplineFitKind::cylinder;
    if (kind == "cone") return SplineFitKind::cone;
    return SplineFitKind::none;
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
    const auto start = Clock::now();
    document.model = load_step_model(step_path, options);
    document.committed_state.model = document.model;
    document.performance.load_step_ms += elapsed_ms(start);
    document.report.diagnostics = document.model.diagnostics;
    document.report = scoped_report(document, document.report);
    session.last_report_value = document.report;
    session.documents.push_back(document);
    const DocumentId new_id = session.documents.back().id;
    if (session.log) {
        // Journal geometry-defining preparation unconditionally. The explicit
        // values distinguish a default policy from an older, unbound log and
        // let project reopen fail closed instead of replaying decisions against
        // differently prepared geometry. Less fundamental overlap/output
        // settings remain compact when they equal their defaults.
        std::vector<DecisionParam> load_params{param_string("step_path", step_path)};
        load_params.push_back(param_bool("heal_shape", options.heal_shape));
        load_params.push_back(param_string("unit_override", options.unit_override));
        load_params.push_back(param_double(
            "unit_scale_to_mm_override", options.unit_scale_to_mm_override));
        load_params.push_back(param_string(
            "normalize_to_unit", options.normalize_to_unit));
        const ConversionOptions defaults;
        if (options.allow_overlaps != defaults.allow_overlaps) {
            load_params.push_back(param_bool("allow_overlaps", options.allow_overlaps));
        }
        if (options.check_overlaps != defaults.check_overlaps) {
            load_params.push_back(param_bool("check_overlaps", options.check_overlaps));
        }
        if (options.overlap_volume_threshold != defaults.overlap_volume_threshold) {
            load_params.push_back(
                param_double("overlap_volume_threshold", options.overlap_volume_threshold));
        }
        if (options.overlap_volume_fraction != defaults.overlap_volume_fraction) {
            load_params.push_back(
                param_double("overlap_volume_fraction", options.overlap_volume_fraction));
        }
        if (!options.mcnp_domain_id.empty()) {
            load_params.push_back(
                param_string("mcnp_domain_id", options.mcnp_domain_id));
        }
        if (options.require_domain_coverage) {
            load_params.push_back(
                param_bool("require_domain_coverage", true));
        }
        log_document_op(session, new_id, "load_step", load_params,
                        document.model.loaded());
    }
    return new_id;
}

CommandResult analyze(Session& session,
                      const DocumentId& document_id,
                      ReportIdMode id_mode,
                      JobProgressCallback progress) {
    static std::atomic<std::uint64_t> next_analysis_job{0};
    const auto run_start = Clock::now();
    const auto wall_tick = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string job_id = "analyze:" + std::to_string(wall_tick) + ":"
        + std::to_string(++next_analysis_job);
    std::uint64_t sequence = 0;
    std::size_t base_revision = 0;
    const auto publish = [&](JobPhase phase, const std::string& code,
                             std::size_t completed, std::size_t total,
                             bool total_known, const std::string& target,
                             JobOutcome outcome,
                             const ConversionReport* report = nullptr) {
        if (!progress) return true;
        JobProgressEvent event;
        event.job_id = job_id;
        event.sequence = ++sequence;
        event.operation = JobOperation::analyze;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.completed = completed;
        event.total = total;
        event.total_known = total_known;
        event.target_id = target;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = report
            ? report->document_revision : base_revision;
        if (report) event.diagnostics = report->diagnostics;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        base_revision = document.revision;
        if (!publish(JobPhase::analyzing_solids, "analysis_started",
                     0, 0, false, {}, JobOutcome::none)) {
            CommandResult cancelled = session_error(
                session, "job_cancelled",
                "Analysis was cancelled before inspecting geometry.", document_id);
            publish(JobPhase::finished, "job_cancelled", 0, 0, false, {},
                    JobOutcome::cancelled, &cancelled.report);
            return cancelled;
        }
        const auto start = Clock::now();
        bool cancelled_during_analysis = false;
        std::size_t analyzed_completed = 0;
        std::size_t analyzed_total = 0;
        const AnalysisProgressCallback analysis_progress =
            [&](std::size_t completed, std::size_t total,
                const std::string& target) {
                analyzed_completed = completed;
                analyzed_total = total;
                return publish(JobPhase::analyzing_solids,
                               "analysis_solid_completed", completed, total,
                               true, target, JobOutcome::none);
            };
        ConversionReport local_report = analyze_model(
            document.model, document.options, analysis_progress,
            &cancelled_during_analysis);
        if (cancelled_during_analysis) {
            CommandResult cancelled = session_error(
                session, "job_cancelled",
                "Analysis was cancelled at a solid boundary.", document_id);
            publish(JobPhase::finished, "job_cancelled", analyzed_completed,
                    analyzed_total, true, {}, JobOutcome::cancelled,
                    &cancelled.report);
            return cancelled;
        }
        // Pairwise overlap detection only runs on the explicit
        // analyze() entry, NOT on the per-mutation analyze inside
        // finish_document_mutation. The O(N^2) bbox-prescreened
        // intersection passes add up quickly during BSP recursion.
        // The scoped diagnostics + report.overlaps[] are populated
        // here so the caller sees them after `s:analyze(doc)` or
        // `csgify inspect`.
        if (!publish(JobPhase::checking_overlaps, "analysis_checking_overlaps",
                     analyzed_completed, analyzed_total,
                     analyzed_total > 0, {}, JobOutcome::none)) {
            CommandResult cancelled = session_error(
                session, "job_cancelled",
                "Analysis was cancelled before overlap checking.", document_id);
            publish(JobPhase::finished, "job_cancelled", analyzed_completed,
                    analyzed_total, analyzed_total > 0, {},
                    JobOutcome::cancelled, &cancelled.report);
            return cancelled;
        }
        detail::detect_solid_overlaps(document.model, document.options,
                                      local_report);
        document.performance.spatial_candidate_pairs +=
            local_report.performance.spatial_candidate_pairs;
        document.performance.exact_overlap_tests +=
            local_report.performance.exact_overlap_tests;
        document.performance.definition_recognition_cache_hits +=
            local_report.performance.definition_recognition_cache_hits;
        document.performance.definition_recognition_cache_misses +=
            local_report.performance.definition_recognition_cache_misses;
        // Refresh the known-overlap set so subsequent mutation checks
        // treat only the current full-scan pairs as pre-existing.
        document.known_overlap_pairs.clear();
        for (const SolidOverlap& ov : local_report.overlaps) {
            auto key = ov.solid_a_id < ov.solid_b_id
                ? std::make_pair(ov.solid_a_id, ov.solid_b_id)
                : std::make_pair(ov.solid_b_id, ov.solid_a_id);
            document.known_overlap_pairs.push_back(key);
        }
        document.committed_state.known_overlap_pairs =
            document.known_overlap_pairs;
        document.performance.analyze_ms += elapsed_ms(start);
        local_report.performance = document.performance;
        local_report.approximation_policy = document.options.approximation;
        local_report.approximation_ledger = document.approximation_ledger;
        local_report.document_revision = document.revision;
        // Source provenance is independent of local-vs-scoped shape IDs and
        // is required by project persistence in both report modes.
        local_report.source_path = document.source_path;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local_report)
            : local_report;
        session.last_report_value = document.report;
        // analyze always succeeds at producing a report; result.ok means
        // "analysis ran", not "the model is clean". Callers should check
        // `report.ok()` or per-solid flags to decide what's needed next.
        CommandResult result{true, document.report};
        publish(JobPhase::finished, "analysis_finished", analyzed_completed,
                analyzed_total, analyzed_total > 0, {}, JobOutcome::committed,
                &result.report);
        return result;
    }
    CommandResult missing = session_error(session,
                                          "document_not_found",
                                          "Requested document ID was not found.",
                                          document_id);
    publish(JobPhase::finished, "document_not_found", 0, 0, false, {},
            JobOutcome::failed, &missing.report);
    return missing;
}

std::size_t document_revision(const Session& session,
                              const DocumentId& document_id) {
    for (const CadDocument& document : session.documents) {
        if (document.id == document_id) return document.revision;
    }
    return 0;
}

CommandResult undo_last_mutation(Session& session,
                                 const DocumentId& document_id,
                                 ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        if (document.revision_history.empty()) {
            log_document_op(session, document_id, "undo_last_mutation", {}, false);
            return session_error(session,
                                 "revision_history_empty",
                                 "Document has no committed mutation to undo.",
                                 document_id);
        }

        // The journal is the restart authority. Publish the compensating
        // operation before changing the live model so an append failure
        // cannot leave memory rolled back while replay still applies the
        // preceding mutation.
        const std::size_t restored_revision = document.revision + 1;
        log_document_op(session,
                        document_id,
                        "undo_last_mutation",
                        {param_int("revision",
                                   static_cast<long long>(restored_revision))},
                        true);

        DocumentRevisionState restored =
            std::move(document.revision_history.back());
        document.revision_history.pop_back();
        document.model = restored.model;
        document.approximation_ledger = restored.approximation_ledger;
        document.approximation_ledger_initialized =
            restored.approximation_ledger_initialized;
        document.synthetic_body_high_water =
            restored.synthetic_body_high_water;
        document.decomposed_piece_high_water =
            restored.decomposed_piece_high_water;
        document.known_overlap_pairs = restored.known_overlap_pairs;
        document.committed_state = std::move(restored);
        ++document.revision;

        ConversionReport local_report =
            analyze_model(document.model, document.options);
        document.performance.definition_recognition_cache_hits +=
            local_report.performance.definition_recognition_cache_hits;
        document.performance.definition_recognition_cache_misses +=
            local_report.performance.definition_recognition_cache_misses;
        local_report.document_revision = document.revision;
        local_report.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::info,
            "document_revision_restored",
            "The preceding committed document state was restored as a new revision.",
            document_id));
        local_report.performance = document.performance;
        local_report.approximation_policy = document.options.approximation;
        local_report.approximation_ledger = document.approximation_ledger;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local_report)
            : local_report;
        session.last_report_value = document.report;
        return {true, document.report};
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

void ensure_approximation_ledger(CadDocument& document) {
    if (document.approximation_ledger_initialized) return;
    const ConversionReport report = analyze_model(document.model, document.options);
    ApproximationLedger ledger;
    ledger.document_baseline_volume = report.total_volume;
    auto add_baseline = [&](const std::string& source_id, double volume) {
        for (ApproximationLedgerEntry& entry : ledger.bodies) {
            if (entry.source_solid_id == source_id) {
                entry.baseline_volume += volume;
                return;
            }
        }
        ApproximationLedgerEntry entry;
        entry.source_solid_id = source_id;
        entry.baseline_volume = volume;
        ledger.bodies.push_back(std::move(entry));
    };
    for (const AnalyzedSolid& solid : report.solids)
        add_baseline(solid.id, solid.volume);
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces)
        add_baseline(piece.source_solid_id.empty() ? piece.id
                                                   : piece.source_solid_id,
                     piece.volume);
    document.approximation_ledger = std::move(ledger);
    document.approximation_ledger_initialized = true;
    document.committed_state.approximation_ledger =
        document.approximation_ledger;
    document.committed_state.approximation_ledger_initialized = true;
}

double source_baseline_volume(const ApproximationLedger& ledger,
                              const std::string& source_id,
                              double fallback) {
    for (const ApproximationLedgerEntry& entry : ledger.bodies)
        if (entry.source_solid_id == source_id && entry.baseline_volume > 0.0)
            return entry.baseline_volume;
    return fallback;
}

bool replacement_allowed(ReplacementOperation operation,
                         const ApproximationPolicy& policy) {
    if (operation == ReplacementOperation::bbox
        || operation == ReplacementOperation::bbox_cylinder) {
        return policy.allow_bbox_replacement;
    }
    return policy.allow_surface_replacement;
}

int active_material_id(const CadModel& model,
                       const std::string& id,
                       const ConversionOptions& options) {
    if (const SyntheticBody* body = detail::find_synthetic_body(model, id))
        return body->material_id;
    return options.default_material_id;
}

bool material_is_protected(const ApproximationPolicy& policy,
                           int material_id) {
    return std::find(policy.protected_material_ids.begin(),
                     policy.protected_material_ids.end(), material_id)
        != policy.protected_material_ids.end();
}

double measured_shape_volume(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}

bool exact_cut(const TopoDS_Shape& argument,
               const TopoDS_Shape& tool,
               TopoDS_Shape* result) {
    if (!result || argument.IsNull() || tool.IsNull()) return false;
    try {
        BRepAlgoAPI_Cut cut;
        detail::build_binary_boolean(cut, argument, tool);
        if (!cut.IsDone() || cut.Shape().IsNull()) return false;
        BRepCheck_Analyzer validity(cut.Shape());
        if (!validity.IsValid()) return false;
        *result = cut.Shape();
        return true;
    } catch (...) {
        return false;
    }
}

McnpGeometrySafetyReport validate_mcnp_geometry(
    const CadModel& emission_model,
    const ConversionReport& report,
    const ConversionOptions& options,
    const TopoDS_Shape& domain_shape) {
    McnpGeometrySafetyReport safety;
    safety.validated = true;
    safety.domain_id = options.mcnp_domain_id;
    safety.overlap_count = report.overlaps.size();
    for (const Diagnostic& diagnostic : report.diagnostics)
        if (diagnostic.code == "overlap_unknown")
            ++safety.overlap_unknown_count;

    if (safety.overlap_count > 0) {
        safety.failure_code = "overlap_blocked";
    } else if (safety.overlap_unknown_count > 0) {
        safety.failure_code = "overlap_unknown";
    }

    if (options.mcnp_domain_id.empty()) {
        safety.coverage_status = CoverageStatus::not_configured;
        if (options.require_domain_coverage)
            safety.failure_code = "coverage_domain_not_configured";
        safety.safe_to_emit = safety.failure_code.empty();
        return safety;
    }
    if (domain_shape.IsNull() || !BRepCheck_Analyzer(domain_shape).IsValid()) {
        safety.coverage_status = CoverageStatus::unknown;
        safety.failure_code = "coverage_domain_unknown";
        return safety;
    }
    safety.domain_volume = measured_shape_volume(domain_shape);
    TopoDS_Shape residual = domain_shape;
    std::vector<std::string> ids;
    ids.reserve(report.solids.size() + report.decomposed_pieces.size());
    for (const AnalyzedSolid& solid : report.solids) ids.push_back(solid.id);
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces)
        ids.push_back(piece.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const std::string& id : ids) {
        TopoDS_Shape cell;
        if (!active_shape_for_id(emission_model, id, &cell, nullptr)) continue;
        TopoDS_Shape outside;
        if (!exact_cut(cell, domain_shape, &outside)) {
            safety.coverage_status = CoverageStatus::unknown;
            safety.failure_code = "coverage_boolean_unknown";
            return safety;
        }
        safety.outside_domain_volume += measured_shape_volume(outside);
        TopoDS_Shape next_residual;
        if (!exact_cut(residual, cell, &next_residual)) {
            safety.coverage_status = CoverageStatus::unknown;
            safety.failure_code = "coverage_boolean_unknown";
            return safety;
        }
        residual = next_residual;
    }
    safety.uncovered_volume = measured_shape_volume(residual);
    const double tolerance = std::max(
        options.overlap_volume_threshold,
        std::pow(std::max(options.linear_tolerance, 1.0e-12), 3.0));
    if (safety.uncovered_volume > tolerance
        || safety.outside_domain_volume > tolerance) {
        safety.coverage_status = CoverageStatus::gap;
        safety.failure_code = safety.uncovered_volume > tolerance
            ? "coverage_gap" : "cell_outside_modeled_domain";
        return safety;
    }
    safety.coverage_status = CoverageStatus::certified;
    safety.safe_to_emit = safety.failure_code.empty();
    return safety;
}

CommandResult finish_document_mutation(Session& session,
                                       CadDocument& document,
                                       bool ok,
                                       ReportIdMode id_mode = ReportIdMode::scoped,
                                       std::size_t transaction_log_start =
                                           std::numeric_limits<std::size_t>::max(),
                                       const ConversionReport*
                                           authoritative_report = nullptr,
                                       const std::function<bool(
                                           JobPhase, std::size_t, std::size_t,
                                           const std::string&)>&
                                           progress = {}) {
    // CommandResult.ok means "the requested operation applied" -- not
    // "the model is now globally emit-ready". The post-mutation report
    // is returned alongside so the caller can inspect `report.ok()` or
    // walk diagnostics to decide whether further work is needed. This
    // distinction matters whenever an op succeeds on its target but
    // unrelated parts of the model still carry error-severity
    // diagnostics (e.g., a spline face is replaced but the same solid
    // has other spline faces left, or a solid is removed but a
    // sibling still needs decomposition).
    const auto start = Clock::now();
    bool cancelled = progress
        && !progress(JobPhase::certifying_solids, 0, 0, {});
    ConversionReport local_report;
    if (!cancelled) {
        if (authoritative_report) {
            local_report = *authoritative_report;
        } else {
            bool analysis_cancelled = false;
            const AnalysisProgressCallback analysis_progress = progress
                ? AnalysisProgressCallback(
                    [&](std::size_t completed, std::size_t total,
                        const std::string& target) {
                        return progress(JobPhase::certifying_solids,
                                        completed, total, target);
                    })
                : AnalysisProgressCallback{};
            local_report = analyze_model(document.model, document.options,
                                         analysis_progress,
                                         &analysis_cancelled);
            cancelled = analysis_cancelled;
        }
        if (!cancelled)
            cancelled = progress
                && !progress(JobPhase::checking_overlaps, 0, 0, {});
    }
    // Mutation-overlap check: detect whether the most recent op
    // introduced new overlaps. Only inspects synthetic bodies and
    // decomposed pieces ADDED since the last call (high-water marked
    // on the document), so cost is bounded by the new geometry's
    // count, not the model size. Pre-existing overlaps are not
    // re-reported here -- those come from the explicit analyze pass.
    if (!cancelled && !authoritative_report) {
        CadModel overlap_model = document.model;
        if (!document.options.mcnp_domain_id.empty())
            remove_solid(overlap_model, document.options.mcnp_domain_id);
        detail::detect_mutation_overlaps(overlap_model,
                                          document.options,
                                          local_report,
                                          &document.synthetic_body_high_water,
                                          &document.decomposed_piece_high_water,
                                          &document.known_overlap_pairs);
    } else if (!cancelled) {
        // The fixed-point terminal report already ran a complete global
        // overlap sweep, which strictly subsumes the new-tail mutation check.
        document.synthetic_body_high_water =
            document.model.synthetic_bodies.size();
        document.decomposed_piece_high_water =
            document.model.decomposed_pieces.size();
    }
    const bool introduced_invalid_geometry = std::any_of(
        local_report.diagnostics.begin(), local_report.diagnostics.end(),
        [](const Diagnostic& d) {
            return d.severity == DiagnosticSeverity::error
                && (d.code == "mutation_introduced_overlap"
                    || d.code == "overlap_unknown");
        });
    if (!cancelled && ok && !introduced_invalid_geometry && progress
        && !progress(JobPhase::committing, 0, 0, {})) {
        cancelled = true;
    }
    if (!ok || introduced_invalid_geometry || cancelled) {
        std::vector<Diagnostic> rollback_causes;
        const std::size_t committed_diagnostic_count =
            document.committed_state.model.diagnostics.size();
        for (std::size_t i = committed_diagnostic_count;
             i < document.model.diagnostics.size(); ++i) {
            rollback_causes.push_back(document.model.diagnostics[i]);
        }
        for (const Diagnostic& d : local_report.diagnostics) {
            if ((d.code == "mutation_introduced_overlap"
                 || d.code == "overlap_unknown")
                && std::none_of(rollback_causes.begin(), rollback_causes.end(),
                                [&](const Diagnostic& existing) {
                                    return existing.code == d.code
                                        && existing.shape_id == d.shape_id
                                        && existing.message == d.message;
                                })) {
                rollback_causes.push_back(d);
            }
        }
        document.model = document.committed_state.model;
        document.approximation_ledger =
            document.committed_state.approximation_ledger;
        document.approximation_ledger_initialized =
            document.committed_state.approximation_ledger_initialized;
        document.synthetic_body_high_water =
            document.committed_state.synthetic_body_high_water;
        document.decomposed_piece_high_water =
            document.committed_state.decomposed_piece_high_water;
        document.known_overlap_pairs =
            document.committed_state.known_overlap_pairs;

        local_report = analyze_model(document.model, document.options);
        local_report.diagnostics.insert(local_report.diagnostics.end(),
                                        rollback_causes.begin(),
                                        rollback_causes.end());
        Diagnostic rolled_back;
        rolled_back.severity = introduced_invalid_geometry
            ? DiagnosticSeverity::error : DiagnosticSeverity::warning;
        rolled_back.code = "mutation_rolled_back";
        rolled_back.message = cancelled
            ? "Mutation was cancelled at a safe boundary; the previous committed document revision was restored."
            : introduced_invalid_geometry
            ? "Mutation was rolled back because its geometry failed postcondition validation."
            : "Mutation reported failure; the previous committed document revision was restored.";
        local_report.diagnostics.push_back(std::move(rolled_back));
        if (session.log && session.log->last_sequence() > 0) {
            const std::size_t transaction_log_end =
                session.log->last_sequence();
            const std::size_t first =
                transaction_log_start != std::numeric_limits<std::size_t>::max()
                ? transaction_log_start + 1 : transaction_log_end;
            for (std::size_t seq = first; seq <= transaction_log_end; ++seq) {
                session.log->record_undo(seq);
            }
        }
        ok = false;
    } else {
        document.revision_history.push_back(document.committed_state);
        document.committed_state.model = document.model;
        document.committed_state.approximation_ledger =
            document.approximation_ledger;
        document.committed_state.approximation_ledger_initialized =
            document.approximation_ledger_initialized;
        document.committed_state.synthetic_body_high_water =
            document.synthetic_body_high_water;
        document.committed_state.decomposed_piece_high_water =
            document.decomposed_piece_high_water;
        document.committed_state.known_overlap_pairs =
            document.known_overlap_pairs;
        ++document.revision;
    }
    document.performance.analyze_ms += elapsed_ms(start);
    document.performance.definition_recognition_cache_hits +=
        local_report.performance.definition_recognition_cache_hits;
    document.performance.definition_recognition_cache_misses +=
        local_report.performance.definition_recognition_cache_misses;
    local_report.performance = document.performance;
    local_report.approximation_policy = document.options.approximation;
    local_report.approximation_ledger = document.approximation_ledger;
    local_report.document_revision = document.revision;
    document.report = id_mode == ReportIdMode::scoped
        ? scoped_report(document, local_report)
        : local_report;
    session.last_report_value = document.report;
    return {ok, document.report};
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
    const std::size_t before = target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = operation(target.document->model,
                              target.local_shape_id,
                              target.document->options);
    if (ok && (op_name == "decompose_planar"
               || op_name == "decompose_prismatic")) {
        coalesce_decomposed_pieces(target.document->model,
                                    target.document->options);
    }
    if (op_name == "decompose_planar") {
        target.document->performance.split_ms += elapsed_ms(start);
        target.document->performance.splitter_calls += 1;
        if (target.document->model.decomposed_pieces.size() > before) {
            target.document->performance.pieces_produced +=
                target.document->model.decomposed_pieces.size() - before;
        }
    }
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

CommandResult remove_solids(Session& session,
                            const std::vector<std::string>& shape_ids,
                            ReportIdMode id_mode,
    JobProgressCallback progress) {
    static std::atomic<std::uint64_t> next_job{0};
    const auto wall_tick = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string job_id = "batch-remove:"
        + std::to_string(wall_tick) + ":" + std::to_string(++next_job);
    const auto run_start = Clock::now();
    std::uint64_t sequence = 0;
    std::size_t base_revision = 0;
    bool cancellation_requested = false;
    bool terminal_emitted = false;
    const auto publish = [&](JobPhase phase, const std::string& code,
                             std::size_t completed, std::size_t total,
                             const std::string& target, JobOutcome outcome,
                             const ConversionReport* report = nullptr) {
        if (!progress) return true;
        JobProgressEvent event;
        event.job_id = job_id;
        event.sequence = ++sequence;
        event.operation = JobOperation::batch_remove;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.target_id = target;
        event.completed = completed;
        event.total = total;
        event.total_known = total > 0;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = report
            ? report->document_revision : base_revision;
        if (report) event.diagnostics = report->diagnostics;
        if (phase == JobPhase::finished) terminal_emitted = true;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    const auto fail = [&](CommandResult result, const std::string& code,
                          JobOutcome outcome = JobOutcome::failed) {
        if (!terminal_emitted)
            publish(JobPhase::finished, code, 0, shape_ids.size(), {}, outcome,
                    &result.report);
        return result;
    };
    if (shape_ids.empty()) {
        return fail(session_error(session,
                                  "empty_remove_batch",
                                  "Batch removal requires at least one solid ID."),
                    "empty_remove_batch");
    }

    struct RemovalTarget {
        std::string requested_id;
        std::string local_id;
    };

    CadDocument* document = nullptr;
    std::vector<RemovalTarget> targets;
    targets.reserve(shape_ids.size());
    std::set<std::string> seen_local_ids;

    if (!publish(JobPhase::resolving_targets, "remove_batch_resolving_targets",
                 0, shape_ids.size(), {}, JobOutcome::none)) {
        cancellation_requested = true;
        return fail(session_error(session, "job_cancelled",
                                  "Batch removal was cancelled before mutation."),
                    "job_cancelled", JobOutcome::cancelled);
    }

    // Resolve the complete batch before mutating anything. This also prevents
    // an accidentally mixed-document request from partially applying.
    std::size_t resolved = 0;
    for (const std::string& shape_id : shape_ids) {
        ShapeTarget target = find_shape_target(session, shape_id);
        if (!target.document) {
            return fail(shape_target_error(session, target, shape_id),
                        "remove_batch_target_not_found");
        }
        if (document && document != target.document) {
            return fail(session_error(
                session,
                "remove_batch_multiple_documents",
                "Every batch removal target must belong to the same document.",
                shape_id), "remove_batch_multiple_documents");
        }
        document = target.document;
        base_revision = document->revision;
        if (seen_local_ids.insert(target.local_shape_id).second) {
            targets.push_back({shape_id, target.local_shape_id});
        }
        if (!publish(JobPhase::resolving_targets,
                     "remove_batch_target_resolved", ++resolved,
                     shape_ids.size(), shape_id, JobOutcome::none)) {
            cancellation_requested = true;
            return fail(session_error(session, "job_cancelled",
                                      "Batch removal was cancelled before mutation."),
                        "job_cancelled", JobOutcome::cancelled);
        }
    }

    // Removal only changes suppression metadata/active flags, so staging a
    // model copy is inexpensive relative to analysis and guarantees that an
    // invalid target cannot leave a partially modified working document.
    CadModel staged = document->model;
    std::size_t staged_count = 0;
    if (!publish(JobPhase::staging_mutation, "remove_batch_staging",
                 0, targets.size(), {}, JobOutcome::none)) {
        cancellation_requested = true;
        return fail(session_error(session, "job_cancelled",
                                  "Batch removal was cancelled before mutation."),
                    "job_cancelled", JobOutcome::cancelled);
    }
    for (const RemovalTarget& target : targets) {
        if (!remove_solid(staged, target.local_id)) {
            return fail(session_error(
                session,
                "remove_batch_target_invalid",
                "A batch removal target was not found or was not active.",
                target.requested_id), "remove_batch_target_invalid");
        }
        if (!publish(JobPhase::staging_mutation, "remove_batch_target_staged",
                     ++staged_count, targets.size(), target.requested_id,
                     JobOutcome::none)) {
            cancellation_requested = true;
            return fail(session_error(session, "job_cancelled",
                                      "Batch removal was cancelled before mutation."),
                        "job_cancelled", JobOutcome::cancelled);
        }
    }

    const std::size_t transaction_log_start = session.log
        ? session.log->last_sequence() : 0;
    document->model = std::move(staged);
    for (const RemovalTarget& target : targets) {
        log_solid_op(session,
                     document,
                     target.local_id,
                     target.requested_id,
                     "remove_solid",
                     {},
                     true);
    }

    // One logical mutation, one validation boundary, and one revision.
    const auto mutation_progress = [&](JobPhase phase,
                                       std::size_t completed,
                                       std::size_t total,
                                       const std::string& target) {
        const char* code = phase == JobPhase::certifying_solids
            ? "remove_batch_certifying"
            : phase == JobPhase::checking_overlaps
                ? "remove_batch_checking_overlaps"
                : "remove_batch_committing";
        const bool certification_start = phase == JobPhase::certifying_solids
            && total == 0;
        const bool keep_going = publish(
            phase, code,
            certification_start ? 0 : total > 0 ? completed : targets.size(),
            certification_start ? 0 : total > 0 ? total : targets.size(),
            target, JobOutcome::none);
        if (!keep_going) cancellation_requested = true;
        return keep_going;
    };
    CommandResult result = finish_document_mutation(session,
                                                    *document,
                                                    true,
                                                    id_mode,
                                                    transaction_log_start,
                                                    nullptr,
                                                    mutation_progress);
    const JobOutcome outcome = cancellation_requested
        ? JobOutcome::cancelled
        : result.ok ? JobOutcome::committed : JobOutcome::rolled_back;
    publish(JobPhase::finished,
            cancellation_requested ? "job_cancelled"
                : result.ok ? "remove_batch_committed"
                            : "remove_batch_rolled_back",
            result.ok ? targets.size() : 0, targets.size(), {}, outcome,
            &result.report);
    return result;
}

ApproximationCommandResult remove_residual_with_approximation(
    Session& session,
    const std::string& shape_id,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode) {
    ApproximationCommandResult result;
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        result.command = shape_target_error(session, target, shape_id);
        return result;
    }
    if (!policy.enabled || !policy.allow_sliver_removal) {
        result.budget.code = "approximation_residual_removal_not_allowed";
        result.budget.message =
            "Residual removal requires an explicit sliver-removal policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }
    if (target.local_shape_id.rfind("piece:", 0) != 0) {
        result.budget.code = "approximation_removal_requires_piece";
        result.budget.message =
            "Budgeted removal is limited to decomposition pieces; removing an "
            "imported solid is a separate modeling decision.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }
    for (const std::string& protected_id : policy.protected_target_ids) {
        if (protected_id == target.local_shape_id || protected_id == shape_id) {
            result.budget.code = "approximation_protected_geometry";
            result.budget.message = "The target is protected by user policy.";
            result.command = session_error(session, result.budget.code,
                                           result.budget.message, shape_id);
            return result;
        }
    }
    if (material_is_protected(policy, active_material_id(
            target.document->model, target.local_shape_id,
            target.document->options))) {
        result.budget.code = "approximation_protected_material";
        result.budget.message = "The target material is protected by user policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }

    ensure_approximation_ledger(*target.document);
    TopoDS_Shape before_shape;
    std::string source_id;
    if (!active_shape_for_id(target.document->model, target.local_shape_id,
                             &before_shape, &source_id)) {
        result.command = session_error(session, "approximation_target_unavailable",
            "The residual could not be materialized for measurement.", shape_id);
        return result;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(before_shape, props);
    const double removed = std::abs(props.Mass());
    const double baseline = source_baseline_volume(
        target.document->approximation_ledger, source_id, removed);
    result.metrics.baseline_volume = baseline;
    result.metrics.before_volume = removed;
    result.metrics.after_volume = 0.0;
    result.metrics.signed_volume_delta = -removed;
    result.metrics.absolute_net_volume_delta = removed;
    result.metrics.removed_volume = removed;
    result.metrics.changed_volume = removed;
    if (baseline > 0.0) {
        result.metrics.removed_volume_fraction = removed / baseline;
        result.metrics.changed_volume_fraction = removed / baseline;
    }
    result.metrics.symmetric_difference_certified = std::isfinite(removed);
    result.metrics.containment_preserved = true;
    result.metrics.topology_preserved = false;

    bool ok = result.metrics.symmetric_difference_certified;
    if (ok && policy.require_topology_preservation) {
        result.budget.code = "approximation_topology_changed";
        result.budget.message =
            "Residual removal changes topology; the user must explicitly "
            "allow a topology change.";
        ok = false;
    }
    if (ok) {
        ok = commit_approximation_metrics(
            target.document->approximation_ledger, policy, source_id,
            result.metrics, &result.budget);
    }
    if (ok) ok = remove_solid(target.document->model, target.local_shape_id);
    if (!ok && result.budget.code.empty()) {
        result.budget.code = "approximation_residual_removal_failed";
        result.budget.message = "Residual removal or volume measurement failed.";
    }
    target.document->options.approximation = policy;
    const std::vector<DecisionParam> params = {
        param_string("source_solid_id", source_id),
        param_double("changed_volume", result.metrics.changed_volume),
        param_double("added_volume", 0.0),
        param_double("removed_volume", result.metrics.removed_volume),
        param_double("changed_volume_fraction",
                     result.metrics.changed_volume_fraction),
        param_double("approximation_global_limit",
                     policy.max_global_changed_volume_fraction),
        param_double("approximation_body_limit",
                     policy.max_body_changed_volume_fraction),
        param_double("approximation_action_limit",
                     policy.max_action_changed_volume_fraction),
        param_double("approximation_global_added_limit",
                     policy.max_global_added_volume_fraction),
        param_double("approximation_global_removed_limit",
                     policy.max_global_removed_volume_fraction),
        param_double("approximation_body_added_limit",
                     policy.max_body_added_volume_fraction),
        param_double("approximation_body_removed_limit",
                     policy.max_body_removed_volume_fraction),
        param_bool("allow_sliver_removal", policy.allow_sliver_removal),
        param_bool("require_containment_preservation",
                   policy.require_containment_preservation),
        param_bool("require_topology_preservation",
                   policy.require_topology_preservation),
        param_string("approximation_policy_result", result.budget.code),
    };
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "remove_residual_with_approximation", params, ok);
    result.command = finish_document_mutation(session, *target.document,
                                              ok, id_mode);
    return result;
}

ApproximationCommandResult resolve_overlap_with_approximation(
    Session& session,
    const std::string& loser_shape_id,
    const std::string& owner_shape_id,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode) {
    ApproximationCommandResult result;
    ShapeTarget loser = find_shape_target(session, loser_shape_id);
    ShapeTarget owner = find_shape_target(session, owner_shape_id);
    if (!loser.document) {
        result.command = shape_target_error(session, loser, loser_shape_id);
        return result;
    }
    if (!owner.document) {
        result.command = shape_target_error(session, owner, owner_shape_id);
        return result;
    }
    if (loser.document != owner.document) {
        result.budget.code = "overlap_resolution_cross_document";
        result.budget.message =
            "Overlap ownership can only be resolved inside one document.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }
    if (loser.local_shape_id == owner.local_shape_id) {
        result.budget.code = "overlap_resolution_same_target";
        result.budget.message = "The owner and losing component must differ.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }
    if (!policy.enabled || !policy.allow_overlap_resolution) {
        result.budget.code = "approximation_overlap_resolution_not_allowed";
        result.budget.message =
            "Overlap ownership requires an explicit approximation policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }
    for (const std::string& protected_id : policy.protected_target_ids) {
        if (protected_id == loser.local_shape_id
            || protected_id == loser_shape_id) {
            result.budget.code = "approximation_protected_geometry";
            result.budget.message = "The losing component is protected by user policy.";
            result.command = session_error(session, result.budget.code,
                                           result.budget.message,
                                           loser_shape_id);
            return result;
        }
    }
    if (material_is_protected(
            policy, active_material_id(loser.document->model,
                                       loser.local_shape_id,
                                       loser.document->options))) {
        result.budget.code = "approximation_protected_material";
        result.budget.message =
            "The losing component's material is protected by user policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }

    TopoDS_Shape before_shape;
    TopoDS_Shape owner_shape;
    std::string source_id;
    if (!active_shape_for_id(loser.document->model, loser.local_shape_id,
                             &before_shape, &source_id)
        || !active_shape_for_id(owner.document->model, owner.local_shape_id,
                                &owner_shape, nullptr)) {
        result.budget.code = "overlap_resolution_target_unavailable";
        result.budget.message =
            "One of the overlap ownership components could not be materialized.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }

    TopoDS_Shape after_shape;
    if (!exact_cut(before_shape, owner_shape, &after_shape)
        || after_shape.IsNull()) {
        result.budget.code = "overlap_resolution_boolean_unknown";
        result.budget.message =
            "OCCT could not certify the losing component minus its owner.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, loser_shape_id);
        return result;
    }
    ensure_approximation_ledger(*loser.document);
    result.metrics = detail::measure_shape_approximation(
        before_shape, after_shape,
        source_baseline_volume(loser.document->approximation_ledger,
                               source_id, measured_shape_volume(before_shape)),
        std::max(loser.document->options.overlap_volume_threshold,
                 std::pow(std::max(loser.document->options.linear_tolerance,
                                   1.0e-12), 3.0)));
    bool ok = result.metrics.symmetric_difference_certified;
    if (!ok) {
        result.budget.code = "approximation_measurement_unknown";
        result.budget.message =
            "The overlap ownership volume could not be certified.";
    } else if (result.metrics.removed_volume <= 0.0) {
        ok = false;
        result.budget.code = "overlap_resolution_not_needed";
        result.budget.message =
            "The selected components have no positive-volume intersection.";
    } else if (result.metrics.added_volume > 0.0) {
        ok = false;
        result.budget.code = "overlap_resolution_added_volume";
        result.budget.message =
            "Ownership resolution unexpectedly added volume.";
    } else if (policy.require_containment_preservation
               && !result.metrics.containment_preserved) {
        ok = false;
        result.budget.code = "approximation_containment_changed";
        result.budget.message = "Overlap resolution changed containment.";
    } else if (policy.require_topology_preservation
               && !result.metrics.topology_preserved) {
        ok = false;
        result.budget.code = "approximation_topology_changed";
        result.budget.message =
            "Overlap ownership changes the losing component topology; "
            "explicitly allow that change.";
    }
    if (ok) {
        ok = commit_approximation_metrics(
            loser.document->approximation_ledger, policy, source_id,
            result.metrics, &result.budget);
    }

    std::vector<TopoDS_Solid> replacement_solids;
    if (ok) {
        for (TopExp_Explorer exp(after_shape, TopAbs_SOLID);
             exp.More(); exp.Next()) {
            replacement_solids.push_back(TopoDS::Solid(exp.Current()));
        }
        if (replacement_solids.empty()) {
            ok = false;
            result.budget.code = "overlap_resolution_empty_component";
            result.budget.message =
                "Ownership would remove the complete losing component.";
        }
    }
    if (ok) {
        bool deactivated = false;
        for (DecomposedSolidPiece& piece :
             loser.document->model.decomposed_pieces) {
            if (piece.id == loser.local_shape_id && piece.active) {
                piece.active = false;
                deactivated = true;
                break;
            }
        }
        if (!deactivated)
            deactivated = remove_solid(loser.document->model,
                                       loser.local_shape_id);
        ok = deactivated;
    }
    if (ok) {
        auto unique_piece_id = [&]() {
            std::size_t candidate = loser.document->model.decomposed_pieces.size() + 1;
            for (;;) {
                const std::string id = "piece:" + std::to_string(candidate++);
                const bool used = std::any_of(
                    loser.document->model.decomposed_pieces.begin(),
                    loser.document->model.decomposed_pieces.end(),
                    [&](const DecomposedSolidPiece& p) { return p.id == id; });
                if (!used) return id;
            }
        };
        for (std::size_t i = 0; i < replacement_solids.size(); ++i) {
            DecomposedSolidPiece replacement;
            replacement.id = i == 0 ? loser.local_shape_id : unique_piece_id();
            replacement.label = replacement.id + "/overlap_owned_by_"
                + owner.local_shape_id;
            replacement.source_solid_id = source_id;
            replacement.solid = replacement_solids[i];
            replacement.volume = measured_shape_volume(replacement.solid);
            loser.document->model.decomposed_pieces.push_back(
                std::move(replacement));
        }
        std::ostringstream message;
        message << "Assigned " << result.metrics.removed_volume
                << " model-unit^3 of overlap to " << owner.local_shape_id
                << " and subtracted it from " << loser.local_shape_id << '.';
        loser.document->model.diagnostics.push_back({
            DiagnosticSeverity::info, "overlap_ownership_resolved",
            message.str(), loser.local_shape_id});
    }
    if (!ok && result.budget.code.empty()) {
        result.budget.code = "overlap_resolution_failed";
        result.budget.message = "Overlap ownership could not be applied.";
    }
    loser.document->options.approximation = policy;
    const std::vector<DecisionParam> params = {
        param_string("owner_shape_id", owner.local_shape_id),
        param_string("source_solid_id", source_id),
        param_double("changed_volume", result.metrics.changed_volume),
        param_double("added_volume", result.metrics.added_volume),
        param_double("removed_volume", result.metrics.removed_volume),
        param_double("changed_volume_fraction",
                     result.metrics.changed_volume_fraction),
        param_double("approximation_global_limit",
                     policy.max_global_changed_volume_fraction),
        param_double("approximation_body_limit",
                     policy.max_body_changed_volume_fraction),
        param_double("approximation_action_limit",
                     policy.max_action_changed_volume_fraction),
        param_double("approximation_global_added_limit",
                     policy.max_global_added_volume_fraction),
        param_double("approximation_global_removed_limit",
                     policy.max_global_removed_volume_fraction),
        param_double("approximation_body_added_limit",
                     policy.max_body_added_volume_fraction),
        param_double("approximation_body_removed_limit",
                     policy.max_body_removed_volume_fraction),
        param_bool("allow_overlap_resolution", true),
        param_bool("require_containment_preservation",
                   policy.require_containment_preservation),
        param_bool("require_topology_preservation",
                   policy.require_topology_preservation),
        param_string("approximation_policy_result", result.budget.code),
    };
    log_solid_op(session, loser.document, loser.local_shape_id,
                 loser_shape_id, "resolve_overlap_with_approximation",
                 params, ok);
    result.command = finish_document_mutation(session, *loser.document,
                                              ok, id_mode);
    return result;
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

ApproximationCommandResult apply_budgeted_replacement_operation(
    Session& session,
    const std::string& shape_id,
    ReplacementOperation operation,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode) {
    ApproximationCommandResult result;
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        result.command = shape_target_error(session, target, shape_id);
        return result;
    }
    if (!policy.enabled || !replacement_allowed(operation, policy)) {
        result.budget.code = !policy.enabled
            ? "approximation_disabled" : "approximation_operation_not_allowed";
        result.budget.message = !policy.enabled
            ? "Approximation requires an explicit user policy."
            : "The user policy does not permit this replacement family.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }
    for (const std::string& protected_id : policy.protected_target_ids) {
        if (protected_id == target.local_shape_id || protected_id == shape_id) {
            result.budget.code = "approximation_protected_geometry";
            result.budget.message = "The target is protected by user policy.";
            result.command = session_error(session, result.budget.code,
                                           result.budget.message, shape_id);
            return result;
        }
    }

    if (material_is_protected(policy, active_material_id(
            target.document->model, target.local_shape_id,
            target.document->options))) {
        result.budget.code = "approximation_protected_material";
        result.budget.message = "The target material is protected by user policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }

    ensure_approximation_ledger(*target.document);
    target.document->options.approximation = policy;
    TopoDS_Shape before_shape;
    std::string source_id;
    if (!active_shape_for_id(target.document->model, target.local_shape_id,
                             &before_shape, &source_id)) {
        result.command = session_error(session, "approximation_target_unavailable",
            "The active target could not be materialized for measurement.", shape_id);
        return result;
    }
    const std::size_t synthetic_before =
        target.document->model.synthetic_bodies.size();
    bool ok = apply_replacement_operation(target.document->model,
                                          target.local_shape_id, operation,
                                          target.document->options);
    TopoDS_Shape after_shape;
    std::string after_id;
    if (ok && target.document->model.synthetic_bodies.size() > synthetic_before) {
        SyntheticBody& replacement = target.document->model.synthetic_bodies.back();
        replacement.source_solid_id = source_id;
        after_id = replacement.id;
        ok = active_shape_for_id(target.document->model, after_id,
                                 &after_shape, nullptr);
    } else {
        ok = false;
    }

    if (ok) {
        result.metrics = detail::measure_shape_approximation(
            before_shape, after_shape,
            source_baseline_volume(target.document->approximation_ledger,
                                   source_id, 0.0),
            std::max(1.0e-12,
                     target.document->options.linear_tolerance
                         * target.document->options.linear_tolerance
                         * target.document->options.linear_tolerance));
        if (!result.metrics.symmetric_difference_certified) {
            result.budget.code = "approximation_measurement_unknown";
            result.budget.message =
                "Replacement symmetric difference could not be certified.";
            ok = false;
        } else if (policy.require_containment_preservation
            && !result.metrics.containment_preserved) {
            result.budget.code = "approximation_containment_changed";
            result.budget.message =
                "Replacement adds volume outside the source containment.";
            ok = false;
        } else if (policy.require_topology_preservation
                   && !result.metrics.topology_preserved) {
            result.budget.code = "approximation_topology_changed";
            result.budget.message =
                "Replacement changes the source solid/shell topology.";
            ok = false;
        } else {
            ok = commit_approximation_metrics(
                target.document->approximation_ledger, policy, source_id,
                result.metrics, &result.budget);
        }
    }
    if (!ok && result.budget.code.empty()) {
        result.budget.code = "approximation_measurement_or_operation_failed";
        result.budget.message =
            "Replacement or symmetric-difference measurement failed.";
    }
    if (!ok) {
        target.document->model.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::warning, result.budget.code,
            result.budget.message, target.local_shape_id));
    }
    const std::vector<DecisionParam> params = {
        param_int("operation", static_cast<long long>(operation)),
        param_string("source_solid_id", source_id),
        param_double("changed_volume", result.metrics.changed_volume),
        param_double("added_volume", result.metrics.added_volume),
        param_double("removed_volume", result.metrics.removed_volume),
        param_double("changed_volume_fraction",
                     result.metrics.changed_volume_fraction),
        param_double("approximation_global_limit",
                     policy.max_global_changed_volume_fraction),
        param_double("approximation_body_limit",
                     policy.max_body_changed_volume_fraction),
        param_double("approximation_action_limit",
                     policy.max_action_changed_volume_fraction),
        param_double("approximation_global_added_limit",
                     policy.max_global_added_volume_fraction),
        param_double("approximation_global_removed_limit",
                     policy.max_global_removed_volume_fraction),
        param_double("approximation_body_added_limit",
                     policy.max_body_added_volume_fraction),
        param_double("approximation_body_removed_limit",
                     policy.max_body_removed_volume_fraction),
        param_double("approximation_surface_deviation_limit",
                     policy.max_surface_deviation),
        param_bool("allow_surface_replacement",
                   policy.allow_surface_replacement),
        param_bool("allow_sliver_removal", policy.allow_sliver_removal),
        param_bool("allow_bbox_replacement", policy.allow_bbox_replacement),
        param_bool("allow_numerical_shatter_loss",
                   policy.allow_numerical_shatter_loss),
        param_bool("require_containment_preservation",
                   policy.require_containment_preservation),
        param_bool("require_topology_preservation",
                   policy.require_topology_preservation),
        param_string("approximation_policy_result", result.budget.code),
    };
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "apply_budgeted_replacement_operation", params, ok);
    result.command = finish_document_mutation(session, *target.document,
                                              ok, id_mode);
    return result;
}

ApproximationLedger document_approximation_ledger(
    const Session& session,
    const DocumentId& document_id) {
    for (const CadDocument& document : session.documents)
        if (document.id == document_id) return document.approximation_ledger;
    return {};
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

SurfaceReplacementProposal propose_surface_replacement(
    Session& session,
    const std::string& face_shape_id,
    ReportIdMode id_mode) {
    SurfaceReplacementProposal proposal;
    ShapeTarget target = find_shape_target(session, face_shape_id);
    if (!target.document) {
        proposal.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error,
            target.error_code.empty() ? "shape_target_not_found" : target.error_code,
            target.error_message.empty()
                ? "Face ID did not resolve to a loaded document."
                : target.error_message,
            face_shape_id));
        return proposal;
    }

    const auto start = Clock::now();
    ConversionReport local_report =
        analyze_model(target.document->model, target.document->options);
    target.document->performance.analyze_ms += elapsed_ms(start);
    local_report.performance = target.document->performance;
    target.document->report = id_mode == ReportIdMode::scoped
        ? scoped_report(*target.document, local_report)
        : local_report;
    session.last_report_value = target.document->report;

    const std::string report_face_id = id_mode == ReportIdMode::scoped
        ? scoped_shape_id(target.document->id, target.local_shape_id)
        : target.local_shape_id;

    const AnalyzedFace* face = nullptr;
    for (const AnalyzedFace& candidate_face : target.document->report.faces) {
        if (candidate_face.id == report_face_id) {
            face = &candidate_face;
            break;
        }
    }
    if (!face) {
        proposal.face_id = report_face_id;
        proposal.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error,
            "face_not_found",
            "Requested face ID was not found in the analyzed document.",
            report_face_id));
        return proposal;
    }

    proposal.face_id = face->id;
    proposal.owner_target_id = face->solid_id;
    proposal.support_kind = face->support_kind;

    if (face->needs_spline_replacement) {
        add_fit_surface_candidate(proposal, *face, face->spline_fits.plane);
        add_fit_surface_candidate(proposal, *face, face->spline_fits.sphere);
        add_fit_surface_candidate(proposal, *face, face->spline_fits.cylinder);
        add_fit_surface_candidate(proposal, *face, face->spline_fits.cone);
    } else if (face->needs_emitter) {
        SurfaceReplacementCandidate candidate;
        candidate.id = "candidate:1";
        candidate.face_id = face->id;
        candidate.owner_target_id = face->solid_id;
        candidate.kind = "split_before_replace";
        candidate.action = "decompose_or_add_emitter";
        candidate.confidence = "review";
        candidate.reason = "surface_needs_emitter";
        candidate.usable = true;
        candidate.score = 1.0;
        proposal.candidates.push_back(std::move(candidate));
    } else if (!face->exact_csg_supported || face->needs_surface_replacement) {
        SurfaceReplacementCandidate candidate;
        candidate.id = "candidate:1";
        candidate.face_id = face->id;
        candidate.owner_target_id = face->solid_id;
        candidate.kind = "unsupported";
        candidate.action = "manual";
        candidate.confidence = "review";
        candidate.reason = "unsupported_surface";
        candidate.usable = false;
        proposal.candidates.push_back(std::move(candidate));
    }

    double best_score = -1.0;
    for (const SurfaceReplacementCandidate& candidate : proposal.candidates) {
        if (candidate.usable && candidate.score > best_score) {
            proposal.best_candidate_id = candidate.id;
            best_score = candidate.score;
        }
    }
    proposal.ok = !proposal.candidates.empty();
    if (!proposal.ok) {
        proposal.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::info,
            "surface_replacement_not_needed",
            "Face is already supported by the current CSG/emitter path.",
            face->id));
    }
    session.last_surface_replacement_proposal_value = proposal;
    return proposal;
}

CommandResult apply_surface_replacement_candidate(
    Session& session,
    const std::string& face_shape_id,
    const std::string& candidate_id,
    ReportIdMode id_mode) {
    SurfaceReplacementProposal proposal =
        propose_surface_replacement(session, face_shape_id, id_mode);
    const SurfaceReplacementCandidate* selected = nullptr;
    for (const SurfaceReplacementCandidate& candidate : proposal.candidates) {
        if (candidate.id == candidate_id) {
            selected = &candidate;
            break;
        }
    }
    if (!selected) {
        return session_error(session,
                             "surface_candidate_not_found",
                             "Requested surface replacement candidate was not found.",
                             face_shape_id);
    }
    if (!selected->usable) {
        return session_error(session,
                             "surface_candidate_not_usable",
                             "Requested surface replacement candidate is not directly applicable.",
                             face_shape_id);
    }

    const SplineFitKind kind = spline_kind_from_public_name(selected->kind);
    if (kind == SplineFitKind::none) {
        return session_error(session,
                             "surface_candidate_apply_unsupported",
                             "Only analytic spline-fit candidates can be applied for now.",
                             face_shape_id);
    }

    ShapeTarget target = find_shape_target(session, face_shape_id);
    if (!target.document) {
        if (session.log) {
            DecisionTarget log_target;
            log_target.kind = "face";
            log_target.shape_id = face_shape_id;
            session.log->record(log_target,
                                "apply_surface_replacement_candidate",
                                {param_string("candidate_id", candidate_id)},
                                "failed");
        }
        return shape_target_error(session, target, face_shape_id);
    }

    const double max_deviation_budget =
        selected->max_deviation * (1.0 + 1.0e-9) + 1.0e-12;
    const bool ok = replace_face_with_fit(target.document->model,
                                          target.local_shape_id,
                                          kind,
                                          true,
                                          max_deviation_budget,
                                          nullptr,
                                          target.document->options);
    if (session.log) {
        DecisionTarget log_target;
        log_target.kind = "face";
        log_target.shape_id = face_shape_id;
        session.log->record(log_target,
                            "apply_surface_replacement_candidate",
                            {param_string("candidate_id", candidate_id),
                             param_string("kind", selected->kind),
                             param_double("max_deviation",
                                          selected->max_deviation),
                             param_double("max_deviation_budget",
                                          max_deviation_budget)},
                            ok ? "applied" : "failed");
    }
    // finish_document_mutation now returns result.ok = ok ("op applied")
    // rather than ok && report.ok(); no override needed here.
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

ApproximationCommandResult apply_budgeted_surface_replacement_candidate(
    Session& session,
    const std::string& face_shape_id,
    const std::string& candidate_id,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode) {
    ApproximationCommandResult result;
    if (!policy.enabled || !policy.allow_surface_replacement) {
        result.budget.code = "approximation_surface_not_allowed";
        result.budget.message =
            "Surface approximation requires an explicit user policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, face_shape_id);
        return result;
    }
    SurfaceReplacementProposal proposal = propose_surface_replacement(
        session, face_shape_id, ReportIdMode::local);
    const SurfaceReplacementCandidate* selected = nullptr;
    for (const SurfaceReplacementCandidate& candidate : proposal.candidates)
        if (candidate.id == candidate_id) { selected = &candidate; break; }
    if (!selected || !selected->usable) {
        result.budget.code = selected ? "surface_candidate_not_usable"
                                      : "surface_candidate_not_found";
        result.budget.message = selected
            ? "Requested surface candidate is not usable."
            : "Requested surface candidate was not found.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, face_shape_id);
        return result;
    }
    if (policy.max_surface_deviation <= 0.0
        || selected->max_deviation > policy.max_surface_deviation) {
        result.budget.code = "approximation_surface_deviation_exceeded";
        result.budget.message =
            "Surface candidate exceeds the user deviation budget.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, face_shape_id);
        return result;
    }
    ShapeTarget face_target = find_shape_target(session, face_shape_id);
    ShapeTarget owner_target = find_shape_target(
        session, selected->owner_target_id);
    if (!face_target.document || !owner_target.document
        || face_target.document != owner_target.document) {
        result.command = shape_target_error(session, face_target, face_shape_id);
        return result;
    }
    for (const std::string& protected_id : policy.protected_target_ids) {
        if (protected_id == face_shape_id
            || protected_id == face_target.local_shape_id
            || protected_id == selected->owner_target_id
            || protected_id == owner_target.local_shape_id) {
            result.budget.code = "approximation_protected_geometry";
            result.budget.message = "The face or its owner is protected.";
            result.command = session_error(session, result.budget.code,
                                           result.budget.message, face_shape_id);
            return result;
        }
    }
    if (material_is_protected(policy, active_material_id(
            face_target.document->model, owner_target.local_shape_id,
            face_target.document->options))) {
        result.budget.code = "approximation_protected_material";
        result.budget.message =
            "The face owner material is protected by user policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, face_shape_id);
        return result;
    }
    CadDocument& document = *face_target.document;
    ensure_approximation_ledger(document);
    TopoDS_Shape before_shape;
    std::string source_id;
    if (!active_shape_for_id(document.model, owner_target.local_shape_id,
                             &before_shape, &source_id)) {
        result.command = session_error(session, "approximation_target_unavailable",
            "The owner solid could not be materialized.", face_shape_id);
        return result;
    }
    const SplineFitKind kind = spline_kind_from_public_name(selected->kind);
    const std::size_t synthetic_before = document.model.synthetic_bodies.size();
    bool ok = kind != SplineFitKind::none
        && replace_face_with_fit(document.model, face_target.local_shape_id,
                                 kind, true, policy.max_surface_deviation,
                                 nullptr, document.options);
    TopoDS_Shape after_shape;
    if (ok && document.model.synthetic_bodies.size() > synthetic_before) {
        SyntheticBody& replacement = document.model.synthetic_bodies.back();
        replacement.source_solid_id = source_id;
        ok = active_shape_for_id(document.model, replacement.id,
                                 &after_shape, nullptr);
    } else if (ok && selected->exact) {
        // An exact support substitution changes only the analytic
        // representation; a partially replaced owner has identical geometry.
        after_shape = before_shape;
    } else if (ok) {
        result.budget.code = "approximation_measurement_unknown";
        result.budget.message =
            "A non-exact partial face replacement cannot yet be certified as "
            "a closed owner solid; replace/materialize the complete owner in "
            "one transaction or request human review.";
        ok = false;
    }
    if (ok) {
        result.metrics = detail::measure_shape_approximation(
            before_shape, after_shape,
            source_baseline_volume(document.approximation_ledger,
                                   source_id, 0.0));
        result.metrics.max_surface_deviation = selected->max_deviation;
        result.metrics.rms_surface_deviation = selected->rms_deviation;
        if (!result.metrics.symmetric_difference_certified) {
            result.budget.code = "approximation_measurement_unknown";
            result.budget.message =
                "Surface replacement symmetric difference could not be certified.";
            ok = false;
        } else if (policy.require_containment_preservation
            && !result.metrics.containment_preserved) {
            result.budget.code = "approximation_containment_changed";
            result.budget.message = "Surface replacement added external volume.";
            ok = false;
        } else if (policy.require_topology_preservation
                   && !result.metrics.topology_preserved) {
            result.budget.code = "approximation_topology_changed";
            result.budget.message = "Surface replacement changed topology.";
            ok = false;
        } else {
            ok = commit_approximation_metrics(
                document.approximation_ledger, policy, source_id,
                result.metrics, &result.budget);
        }
    }
    if (!ok && result.budget.code.empty()) {
        result.budget.code = "approximation_surface_measurement_failed";
        result.budget.message =
            "Surface replacement or symmetric-difference measurement failed.";
    }
    document.options.approximation = policy;
    const std::vector<DecisionParam> params = {
        param_string("candidate_id", candidate_id),
        param_string("kind", selected->kind),
        param_string("source_solid_id", source_id),
        param_double("max_deviation", selected->max_deviation),
        param_double("changed_volume", result.metrics.changed_volume),
        param_double("added_volume", result.metrics.added_volume),
        param_double("removed_volume", result.metrics.removed_volume),
        param_double("approximation_global_limit", policy.max_global_changed_volume_fraction),
        param_double("approximation_body_limit", policy.max_body_changed_volume_fraction),
        param_double("approximation_action_limit", policy.max_action_changed_volume_fraction),
        param_double("approximation_global_added_limit", policy.max_global_added_volume_fraction),
        param_double("approximation_global_removed_limit", policy.max_global_removed_volume_fraction),
        param_double("approximation_body_added_limit", policy.max_body_added_volume_fraction),
        param_double("approximation_body_removed_limit", policy.max_body_removed_volume_fraction),
        param_double("approximation_surface_deviation_limit", policy.max_surface_deviation),
        param_bool("allow_surface_replacement", true),
        param_bool("require_containment_preservation", policy.require_containment_preservation),
        param_bool("require_topology_preservation", policy.require_topology_preservation),
        param_string("approximation_policy_result", result.budget.code),
    };
    if (session.log) {
        DecisionTarget log_target;
        log_target.kind = "face";
        log_target.shape_id = face_shape_id;
        session.log->record(log_target,
            "apply_budgeted_surface_replacement_candidate", params,
            ok ? "applied" : "failed");
    }
    result.command = finish_document_mutation(session, document, ok, id_mode);
    return result;
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

CommandResult decompose_prismatic(Session& session,
                                  const std::string& shape_id,
                                  ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, decompose_solid_prismatic, id_mode,
                        "decompose_prismatic");
}

CommandResult decompose_revolution(Session& session,
                                   const std::string& shape_id,
                                   ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, decompose_solid_revolution, id_mode,
                        "decompose_revolution");
}

CommandResult reuse_definition_conversion(
    Session& session,
    const std::string& target_shape_id,
    const std::string& source_shape_id,
    ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, target_shape_id);
    ShapeTarget source = find_shape_target(session, source_shape_id);
    if (!target.document) return shape_target_error(session, target, target_shape_id);
    if (!source.document) return shape_target_error(session, source, source_shape_id);
    if (target.document != source.document) {
        return session_error(session, "definition_cache_cross_document",
            "Definition conversion reuse requires occurrences in one document.",
            target_shape_id);
    }
    const ComponentInstance* target_instance = nullptr;
    const ComponentInstance* source_instance = nullptr;
    for (const ComponentInstance& instance : target.document->model.component_instances) {
        if (instance.source_shape_id == target.local_shape_id) target_instance = &instance;
        if (instance.source_shape_id == source.local_shape_id) source_instance = &instance;
    }
    const ComponentDefinition* target_definition = nullptr;
    const ComponentDefinition* source_definition = nullptr;
    for (const ComponentDefinition& definition : target.document->model.component_definitions) {
        if (target_instance && definition.id == target_instance->definition_id)
            target_definition = &definition;
        if (source_instance && definition.id == source_instance->definition_id)
            source_definition = &definition;
    }
    if (!target_instance || !source_instance || !target_definition
        || !source_definition || !target_definition->geometry_digest_verified
        || !source_definition->geometry_digest_verified
        || target_definition->geometry_digest.empty()
        || target_definition->geometry_digest != source_definition->geometry_digest) {
        return session_error(session, "definition_cache_identity_mismatch",
            "Conversion reuse requires matching verified geometry-definition digests.",
            target_shape_id);
    }
    const bool ok = reuse_definition_decomposition(
        target.document->model, source.local_shape_id,
        source_instance->transform, target.local_shape_id,
        target_instance->transform, target.document->options);
    log_solid_op(session, target.document, target.local_shape_id,
                 target_shape_id, "reuse_definition_conversion",
                 {param_string("source_solid_id", source_shape_id)}, ok);
    if (ok) ++target.document->performance.definition_conversion_cache_hits;
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

MediumGenerationProposal propose_contained_medium(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        MediumGenerationProposal proposal = propose_straight_pipe_medium(
            document.model, request, document.options);
        if (id_mode == ReportIdMode::scoped && !proposal.host_shape_id.empty()) {
            proposal.host_shape_id = scoped_shape_id(
                document.id, proposal.host_shape_id);
            for (DecisionRequest& decision : proposal.decision_requests) {
                if (!decision.target_id.empty())
                    decision.target_id = scoped_shape_id(
                        document.id, decision.target_id);
            }
        }
        return proposal;
    }
    MediumGenerationProposal proposal;
    proposal.request = request;
    proposal.diagnostics.push_back(make_error(
        "document_not_found",
        "Contained-medium proposal document was not found.", document_id));
    return proposal;
}

CommandResult stage_contained_medium_request(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        const bool ok = !request.id.empty();
        if (ok) {
            const auto existing = std::find_if(
                document.model.medium_requests.begin(),
                document.model.medium_requests.end(),
                [&](const MediumGenerationRequest& value) {
                    return value.id == request.id;
                });
            if (existing == document.model.medium_requests.end())
                document.model.medium_requests.push_back(request);
            else
                *existing = request;
        } else {
            document.model.diagnostics.push_back(make_error(
                "medium_request_missing_id",
                "A staged contained-medium request requires a stable ID.",
                document_id));
        }
        std::vector<DecisionParam> params = {
            param_string("request_id", request.id),
            param_string("material_id", request.material_id),
            param_string("complement_material_id",
                         request.complement_material_id),
            param_string("fill_mode", medium_fill_mode_name(request.fill_mode)),
            param_double("host_count",
                         static_cast<double>(request.host_component_ids.size())),
            param_double("port_count",
                         static_cast<double>(request.ports.size())),
            param_double("internal_component_count", static_cast<double>(
                request.internal_components_to_subtract.size())),
            param_double("seed_count", static_cast<double>(
                request.seed_points_or_faces.size())),
            param_bool("allow_disconnected_pockets",
                       request.allow_disconnected_pockets),
            param_double("max_worker_count",
                         static_cast<double>(request.max_worker_count)),
        };
        for (std::size_t index = 0;
             index < request.host_component_ids.size(); ++index)
            params.push_back(param_string(
                "host_" + std::to_string(index + 1) + "_component_id",
                request.host_component_ids[index]));
        if (request.fill_level)
            params.push_back(param_double("fill_level", *request.fill_level));
        if (request.target_volume_fraction)
            params.push_back(param_double("target_volume_fraction",
                                          *request.target_volume_fraction));
        for (std::size_t index = 0; index < request.ports.size(); ++index) {
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_id",
                request.ports[index].port_id));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_disposition",
                medium_port_disposition_name(request.ports[index].disposition)));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_peer_id",
                request.ports[index].peer_port_id));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_valve_state",
                medium_valve_state_name(request.ports[index].valve_state)));
        }
        for (std::size_t index = 0;
             index < request.internal_components_to_subtract.size(); ++index)
            params.push_back(param_string(
                "internal_" + std::to_string(index + 1) + "_component_id",
                request.internal_components_to_subtract[index]));
        for (std::size_t index = 0;
             index < request.seed_points_or_faces.size(); ++index) {
            const std::string prefix = "seed_" + std::to_string(index + 1);
            const GeometricSeed& seed = request.seed_points_or_faces[index];
            params.push_back(param_string(prefix + "_kind",
                geometric_seed_kind_name(seed.kind)));
            params.push_back(param_string(prefix + "_entity_id",
                                          seed.entity_id));
            params.push_back(param_double(prefix + "_x", seed.point[0]));
            params.push_back(param_double(prefix + "_y", seed.point[1]));
            params.push_back(param_double(prefix + "_z", seed.point[2]));
        }
        log_document_op(session, document_id,
                        "stage_contained_medium_request", params, ok);
        return finish_document_mutation(session, document, ok, id_mode);
    }
    return session_error(session, "document_not_found",
                         "Contained-medium staging document was not found.",
                         document_id);
}

bool find_contained_medium_request(
    const Session& session,
    const DocumentId& document_id,
    const std::string& request_id,
    MediumGenerationRequest* request) {
    if (!request || request_id.empty()) return false;
    for (const CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        const auto found = std::find_if(
            document.model.medium_requests.begin(),
            document.model.medium_requests.end(),
            [&](const MediumGenerationRequest& value) {
                return value.id == request_id;
            });
        if (found == document.model.medium_requests.end()) return false;
        *request = *found;
        return true;
    }
    return false;
}

CommandResult define_material(
    Session& session,
    const DocumentId& document_id,
    const MaterialDefinition& material,
    ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        bool ok = !material.id.empty() && std::isfinite(material.density)
            && material.phase != MaterialPhase::unknown;
        if (ok) {
            auto existing = std::find_if(
                document.model.materials.begin(), document.model.materials.end(),
                [&](const MaterialDefinition& value) {
                    return value.id == material.id;
                });
            if (existing == document.model.materials.end())
                document.model.materials.push_back(material);
            else
                *existing = material;
        } else {
            document.model.diagnostics.push_back(make_error(
                "invalid_material_definition",
                "Material ID, phase, and finite density are required.",
                document_id));
        }
        log_document_op(session, document_id, "define_material",
            {param_string("material_id", material.id),
             param_string("phase", material_phase_name(material.phase)),
             param_double("density", material.density)}, ok);
        return finish_document_mutation(session, document, ok, id_mode);
    }
    return session_error(session, "document_not_found",
                         "Material-definition document was not found.",
                         document_id);
}

CommandResult apply_contained_medium(
    Session& session,
    const DocumentId& document_id,
    const MediumGenerationRequest& request,
    bool user_approved,
    ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        const MediumGenerationProposal proposal = propose_straight_pipe_medium(
            document.model, request, document.options);
        const bool ok = proposal.ok && apply_straight_pipe_medium(
            document.model, proposal, user_approved, document.options);
        std::vector<DecisionParam> params = {
            param_string("request_id", request.id),
            param_string("material_id", request.material_id),
            param_string("complement_material_id",
                         request.complement_material_id),
            param_string("fill_mode", medium_fill_mode_name(request.fill_mode)),
            param_bool("user_approved", user_approved),
            param_double("host_count",
                         static_cast<double>(request.host_component_ids.size())),
            param_double("port_count",
                         static_cast<double>(request.ports.size())),
            param_double("internal_component_count", static_cast<double>(
                request.internal_components_to_subtract.size())),
            param_double("seed_count", static_cast<double>(
                request.seed_points_or_faces.size())),
            param_bool("allow_disconnected_pockets",
                       request.allow_disconnected_pockets),
            param_double("max_worker_count",
                         static_cast<double>(request.max_worker_count)),
        };
        for (std::size_t index = 0;
             index < request.host_component_ids.size(); ++index) {
            params.push_back(param_string(
                "host_" + std::to_string(index + 1) + "_component_id",
                request.host_component_ids[index]));
        }
        if (request.fill_level)
            params.push_back(param_double("fill_level", *request.fill_level));
        if (request.target_volume_fraction)
            params.push_back(param_double("target_volume_fraction",
                                          *request.target_volume_fraction));
        for (std::size_t index = 0; index < request.ports.size(); ++index) {
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_id",
                request.ports[index].port_id));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_disposition",
                medium_port_disposition_name(
                    request.ports[index].disposition)));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_peer_id",
                request.ports[index].peer_port_id));
            params.push_back(param_string(
                "port_" + std::to_string(index + 1) + "_valve_state",
                medium_valve_state_name(request.ports[index].valve_state)));
        }
        for (std::size_t index = 0;
             index < request.internal_components_to_subtract.size(); ++index)
            params.push_back(param_string(
                "internal_" + std::to_string(index + 1) + "_component_id",
                request.internal_components_to_subtract[index]));
        for (std::size_t index = 0;
             index < request.seed_points_or_faces.size(); ++index) {
            const std::string prefix = "seed_" + std::to_string(index + 1);
            const GeometricSeed& seed = request.seed_points_or_faces[index];
            params.push_back(param_string(prefix + "_kind",
                geometric_seed_kind_name(seed.kind)));
            params.push_back(param_string(prefix + "_entity_id",
                                          seed.entity_id));
            params.push_back(param_double(prefix + "_x", seed.point[0]));
            params.push_back(param_double(prefix + "_y", seed.point[1]));
            params.push_back(param_double(prefix + "_z", seed.point[2]));
        }
        log_document_op(session, document_id, "apply_contained_medium",
                        params, ok);
        return finish_document_mutation(session, document, ok, id_mode);
    }
    return session_error(session, "document_not_found",
                         "Contained-medium apply document was not found.",
                         document_id);
}

CommandResult decompose_shell_difference(Session& session,
                                         const std::string& shape_id,
                                         ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, decompose_solid_shell_difference,
                        id_mode, "decompose_shell_difference");
}

CommandResult decompose_pipe_annulus(Session& session,
                                     const std::string& shape_id,
                                     ReportIdMode id_mode) {
    return mutate_solid(session, shape_id, decompose_solid_pipe_annulus,
                        id_mode, "decompose_pipe_annulus");
}

CommandResult shatter(Session& session,
                      const std::string& shape_id,
                      int max_depth,
                      bool allow_volume_drift,
                      ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, shape_id);
    const std::vector<DecisionParam> params = {
        param_int("max_depth", max_depth),
        param_bool("allow_volume_drift", allow_volume_drift),
    };
    if (!target.document) {
        log_solid_op(session, nullptr, {}, shape_id, "shatter", params, false);
        return shape_target_error(session, target, shape_id);
    }

    // shatter_max_depth / allow_shatter_volume_drift are call-scoped, so copy
    // the document options and override rather than mutating the document.
    ConversionOptions options = target.document->options;
    if (max_depth > 0) {
        options.shatter_max_depth = max_depth;
    }
    options.allow_shatter_volume_drift = allow_volume_drift;

    const std::size_t before = target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = shatter_solid(target.document->model,
                                  target.local_shape_id,
                                  options);
    if (ok) {
        coalesce_decomposed_pieces(target.document->model, options);
    }
    target.document->performance.split_ms += elapsed_ms(start);
    target.document->performance.splitter_calls += 1;
    if (target.document->model.decomposed_pieces.size() > before) {
        target.document->performance.pieces_produced +=
            target.document->model.decomposed_pieces.size() - before;
    }
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "shatter", params, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

ApproximationCommandResult shatter_with_approximation(
    Session& session,
    const std::string& shape_id,
    int max_depth,
    const ApproximationPolicy& policy,
    ReportIdMode id_mode) {
    ApproximationCommandResult result;
    ShapeTarget target = find_shape_target(session, shape_id);
    if (!target.document) {
        result.command = shape_target_error(session, target, shape_id);
        return result;
    }
    if (!policy.enabled || !policy.allow_numerical_shatter_loss) {
        result.budget.code = "approximation_shatter_not_allowed";
        result.budget.message =
            "Numerical shatter loss requires an explicit approximation policy.";
        result.command = session_error(session, result.budget.code,
                                       result.budget.message, shape_id);
        return result;
    }
    ensure_approximation_ledger(*target.document);
    TopoDS_Shape before_shape;
    std::string source_id;
    if (!active_shape_for_id(target.document->model, target.local_shape_id,
                             &before_shape, &source_id)) {
        result.command = session_error(session, "approximation_target_unavailable",
            "The shatter target could not be materialized for measurement.", shape_id);
        return result;
    }
    ConversionOptions options = target.document->options;
    options.shatter_max_depth = max_depth > 0 ? max_depth : options.shatter_max_depth;
    options.allow_shatter_volume_drift = true;
    options.approximation = policy;
    const std::size_t pieces_before = target.document->model.decomposed_pieces.size();
    bool ok = shatter_solid(target.document->model, target.local_shape_id, options);
    if (ok) coalesce_decomposed_pieces(target.document->model, options);

    std::size_t active_new_pieces = 0;
    std::vector<TopoDS_Shape> after_parts;
    if (ok) {
        for (std::size_t i = pieces_before;
             i < target.document->model.decomposed_pieces.size(); ++i) {
            const DecomposedSolidPiece& piece =
                target.document->model.decomposed_pieces[i];
            if (!piece.active || piece.solid.IsNull()) continue;
            after_parts.push_back(piece.solid);
            ++active_new_pieces;
        }
        ok = active_new_pieces > 0;
    }
    if (ok) {
        result.metrics = detail::measure_partition_approximation(
            before_shape, after_parts,
            source_baseline_volume(target.document->approximation_ledger,
                                   source_id, 0.0));
        if (!result.metrics.symmetric_difference_certified) {
            result.budget.code = "approximation_measurement_unknown";
            result.budget.message =
                "Shatter partition symmetric difference could not be certified.";
            ok = false;
        } else if (policy.require_containment_preservation
            && !result.metrics.containment_preserved) {
            result.budget.code = "approximation_containment_changed";
            result.budget.message = "Approximate shatter added external volume.";
            ok = false;
        } else {
            ok = commit_approximation_metrics(
                target.document->approximation_ledger, policy, source_id,
                result.metrics, &result.budget);
        }
    }
    if (!ok && result.budget.code.empty()) {
        result.budget.code = "approximation_shatter_failed";
        result.budget.message = "Shatter or symmetric-difference measurement failed.";
    }
    target.document->options.approximation = policy;
    const std::vector<DecisionParam> params = {
        param_int("max_depth", max_depth),
        param_double("changed_volume", result.metrics.changed_volume),
        param_double("added_volume", result.metrics.added_volume),
        param_double("removed_volume", result.metrics.removed_volume),
        param_double("changed_volume_fraction", result.metrics.changed_volume_fraction),
        param_double("approximation_global_limit",
                     policy.max_global_changed_volume_fraction),
        param_double("approximation_body_limit",
                     policy.max_body_changed_volume_fraction),
        param_double("approximation_action_limit",
                     policy.max_action_changed_volume_fraction),
        param_double("approximation_global_added_limit",
                     policy.max_global_added_volume_fraction),
        param_double("approximation_global_removed_limit",
                     policy.max_global_removed_volume_fraction),
        param_double("approximation_body_added_limit",
                     policy.max_body_added_volume_fraction),
        param_double("approximation_body_removed_limit",
                     policy.max_body_removed_volume_fraction),
        param_bool("allow_numerical_shatter_loss",
                   policy.allow_numerical_shatter_loss),
        param_bool("require_containment_preservation",
                   policy.require_containment_preservation),
        param_bool("require_topology_preservation",
                   policy.require_topology_preservation),
        param_string("approximation_policy_result", result.budget.code),
    };
    log_solid_op(session, target.document, target.local_shape_id, shape_id,
                 "shatter_with_approximation", params, ok);
    result.command = finish_document_mutation(session, *target.document, ok, id_mode);
    return result;
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

    const std::size_t before = solid_target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = decompose_solid_by_face(solid_target.document->model,
                                            solid_target.local_shape_id,
                                            face_target.local_shape_id,
                                            solid_target.document->options);
    if (ok) {
        coalesce_decomposed_pieces(solid_target.document->model,
                                    solid_target.document->options);
    }
    solid_target.document->performance.split_ms += elapsed_ms(start);
    solid_target.document->performance.splitter_calls += 1;
    if (solid_target.document->model.decomposed_pieces.size() > before) {
        solid_target.document->performance.pieces_produced +=
            solid_target.document->model.decomposed_pieces.size() - before;
    }
    log_solid_op(session, solid_target.document, solid_target.local_shape_id,
                 solid_shape_id, "decompose_by_face", params, ok);
    return finish_document_mutation(session, *solid_target.document, ok, id_mode);
}

CommandResult decompose_by_plane(Session& session,
                                 const std::string& solid_shape_id,
                                 const Vec3& origin,
                                 const Vec3& normal,
                                 ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, solid_shape_id);
    const std::vector<DecisionParam> params = {
        param_double("origin_x", origin.x),
        param_double("origin_y", origin.y),
        param_double("origin_z", origin.z),
        param_double("normal_x", normal.x),
        param_double("normal_y", normal.y),
        param_double("normal_z", normal.z),
    };
    if (!target.document) {
        log_solid_op(session, nullptr, {}, solid_shape_id,
                     "decompose_by_plane", params, false);
        return shape_target_error(session, target, solid_shape_id);
    }
    const std::size_t before = target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = decompose_solid_by_plane(target.document->model,
                                            target.local_shape_id,
                                            origin,
                                            normal,
                                            target.document->options);
    if (ok) {
        coalesce_decomposed_pieces(target.document->model,
                                    target.document->options);
    }
    target.document->performance.split_ms += elapsed_ms(start);
    target.document->performance.splitter_calls += 1;
    if (target.document->model.decomposed_pieces.size() > before) {
        target.document->performance.pieces_produced +=
            target.document->model.decomposed_pieces.size() - before;
    }
    log_solid_op(session, target.document, target.local_shape_id,
                 solid_shape_id, "decompose_by_plane", params, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

CommandResult decompose_by_planes(Session& session,
                                  const std::string& solid_shape_id,
                                  const std::vector<PlaneSpec>& planes,
                                  ReportIdMode id_mode) {
    ShapeTarget target = find_shape_target(session, solid_shape_id);
    std::vector<DecisionParam> params = {
        param_int("plane_count", static_cast<long long>(planes.size())),
    };
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const PlaneSpec& p = planes[i];
        const std::string idx = std::to_string(i);
        params.push_back(param_double("plane_" + idx + "_ox", p.origin.x));
        params.push_back(param_double("plane_" + idx + "_oy", p.origin.y));
        params.push_back(param_double("plane_" + idx + "_oz", p.origin.z));
        params.push_back(param_double("plane_" + idx + "_nx", p.normal.x));
        params.push_back(param_double("plane_" + idx + "_ny", p.normal.y));
        params.push_back(param_double("plane_" + idx + "_nz", p.normal.z));
    }
    if (!target.document) {
        log_solid_op(session, nullptr, {}, solid_shape_id,
                     "decompose_by_planes", params, false);
        return shape_target_error(session, target, solid_shape_id);
    }
    const std::size_t before = target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = decompose_solid_by_planes(target.document->model,
                                             target.local_shape_id,
                                             planes,
                                             target.document->options);
    if (ok) {
        coalesce_decomposed_pieces(target.document->model,
                                    target.document->options);
    }
    target.document->performance.split_ms += elapsed_ms(start);
    target.document->performance.splitter_calls += 1;
    if (target.document->model.decomposed_pieces.size() > before) {
        target.document->performance.pieces_produced +=
            target.document->model.decomposed_pieces.size() - before;
    }
    log_solid_op(session, target.document, target.local_shape_id,
                 solid_shape_id, "decompose_by_planes", params, ok);
    return finish_document_mutation(session, *target.document, ok, id_mode);
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

    const auto start = Clock::now();
    DecompositionProposal local =
        propose_decomposition_candidates(target.document->model,
                                         target.local_shape_id,
                                         target.document->options);
    target.document->performance.propose_ms += elapsed_ms(start);
    target.document->performance.candidates_evaluated += local.candidates.size();
    DecompositionProposal proposal = id_mode == ReportIdMode::scoped
        ? scoped_proposal(*target.document, local)
        : local;
    session.last_decomposition_proposal_value = proposal;
    return proposal;
}

RemovalProposal suggest_removals(Session& session,
                                 const DocumentId& document_id,
                                 ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        const auto start = Clock::now();
        ConversionReport local_report = analyze_model(document.model, document.options);
        document.performance.analyze_ms += elapsed_ms(start);
        document.performance.definition_recognition_cache_hits +=
            local_report.performance.definition_recognition_cache_hits;
        document.performance.definition_recognition_cache_misses +=
            local_report.performance.definition_recognition_cache_misses;
        local_report.performance = document.performance;
        local_report.document_revision = document.revision;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local_report)
            : local_report;
        session.last_report_value = document.report;

        RemovalProposal local = build_removal_proposal(local_report);
        return id_mode == ReportIdMode::scoped
            ? scoped_removal_proposal(document, local)
            : local;
    }

    RemovalProposal proposal;
    proposal.diagnostics.push_back(make_error(
        "document_not_found",
        "Requested document ID was not found.",
        document_id));
    return proposal;
}

HoleFillProposal propose_hole_fills(Session& session,
                                    const DocumentId& document_id,
                                    const HoleFillOptions& hole_options,
                                    ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }

        HoleFillOptions local_options = hole_options;
        DocumentId parsed_document_id;
        std::string local_shape_id;
        if (parse_scoped_shape_id(local_options.host_solid_id,
                                  &parsed_document_id,
                                  &local_shape_id)) {
            if (parsed_document_id != document.id) {
                HoleFillProposal proposal;
                proposal.diagnostics.push_back(make_error(
                    "document_id_mismatch",
                    "Host solid belongs to a different document.",
                    local_options.host_solid_id));
                session.last_hole_fill_proposal_value = proposal;
                return proposal;
            }
            local_options.host_solid_id = local_shape_id;
        }
        if (parse_scoped_shape_id(local_options.near_removed_solid_id,
                                  &parsed_document_id,
                                  &local_shape_id)) {
            if (parsed_document_id != document.id) {
                HoleFillProposal proposal;
                proposal.diagnostics.push_back(make_error(
                    "document_id_mismatch",
                    "Removed solid belongs to a different document.",
                    local_options.near_removed_solid_id));
                session.last_hole_fill_proposal_value = proposal;
                return proposal;
            }
            local_options.near_removed_solid_id = local_shape_id;
        }

        const auto start = Clock::now();
        HoleFillProposal local =
            propose_hole_fill_candidates(document.model,
                                         local_options,
                                         document.options);
        document.performance.propose_ms += elapsed_ms(start);
        document.performance.candidates_evaluated += local.candidates.size();
        HoleFillProposal proposal = id_mode == ReportIdMode::scoped
            ? scoped_hole_fill_proposal(document, local)
            : local;
        session.last_hole_fill_proposal_value = proposal;
        return proposal;
    }

    HoleFillProposal proposal;
    proposal.diagnostics.push_back(make_error(
        "document_not_found",
        "Requested document ID was not found.",
        document_id));
    session.last_hole_fill_proposal_value = proposal;
    return proposal;
}

CommandResult fill_hole(Session& session,
                        const DocumentId& document_id,
                        const std::string& candidate_id,
                        ReportIdMode id_mode) {
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }

        const HoleFillCandidate* selected = nullptr;
        for (const HoleFillCandidate& candidate :
             session.last_hole_fill_proposal_value.candidates) {
            if (candidate.id == candidate_id) {
                selected = &candidate;
                break;
            }
        }
        if (!selected) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_candidate_not_found",
                "Hole-fill candidate was not found in the last proposal for this session.",
                candidate_id));
        }

        if (selected->kind != HoleFillKind::cylindrical_through) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_unsupported_kind",
                "Only cylindrical through-hole fill candidates can be applied currently.",
                candidate_id));
        }

        const std::string host_id =
            localize_shape_id_for_document(document, selected->host_solid_id);
        const SessionSolidLookup host = find_document_solid(document, host_id);
        if (!host.found) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_host_not_found",
                "Hole-fill host solid was not found in the current document.",
                host_id));
        }
        if (!host.active) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_host_inactive",
                "Hole-fill host solid is suppressed and cannot receive a fill body.",
                host_id));
        }

        if (!finite_positive(selected->radius)
            || !finite_positive(detail::vec_len(selected->height))
            || !finite_positive(selected->plug_volume)
            || !std::isfinite(selected->base.x)
            || !std::isfinite(selected->base.y)
            || !std::isfinite(selected->base.z)) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_invalid_geometry",
                "Hole-fill candidate has invalid cylinder geometry.",
                candidate_id));
        }

        const double length = detail::vec_len(selected->height);
        const double analytic_volume =
            kPi * selected->radius * selected->radius * length;
        const double volume_error = std::abs(analytic_volume - selected->plug_volume);
        const double volume_tolerance =
            std::max(1.0e-8, 1.0e-8 * std::abs(analytic_volume));
        if (volume_error > volume_tolerance) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id),
                             param_double("analytic_volume", analytic_volume),
                             param_double("candidate_volume", selected->plug_volume)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_volume_mismatch",
                "Hole-fill candidate volume does not match its cylinder parameters.",
                candidate_id));
        }

        const double host_volume = host.volume > 0.0 ? host.volume : selected->host_volume;
        if (host_volume <= 0.0
            || selected->plug_volume_fraction_of_host < 0.0
            || selected->plug_volume > host_volume * 1.000001) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_over_policy_limit",
                "Hole-fill plug volume is not plausible for the host solid.",
                candidate_id));
        }

        const Vec3 mid = detail::add(selected->base,
                                     detail::scale(selected->height, 0.5));
        if (detail::point_is_inside_or_on(host.solid,
                                          gp_Pnt(mid.x, mid.y, mid.z),
                                          document.options.linear_tolerance)) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_overlap_host",
                "Hole-fill plug center lies inside host material.",
                candidate_id));
        }

        std::string source_id;
        if (!selected->seed_removed_solid_ids.empty()) {
            source_id = localize_shape_id_for_document(
                document,
                selected->seed_removed_solid_ids.front());
            const SessionSolidLookup source = find_document_solid(document, source_id);
            if (source.found && source.active) {
                log_document_op(session,
                                document_id,
                                "fill_hole",
                                {param_string("candidate_id", candidate_id),
                                 param_string("host_solid_id", host_id),
                                 param_string("source_solid_id", source_id)},
                                false);
                return session_diagnostic(session, diagnostic(
                    DiagnosticSeverity::error,
                    "hole_fill_source_still_active",
                    "Hole-fill source solid is still active; remove it explicitly before filling the hole.",
                    source_id));
            }
        }

        std::size_t solid_index = 0;
        for (TopExp_Explorer exp(document.model.root, TopAbs_SOLID);
             exp.More();
             exp.Next()) {
            ++solid_index;
            const std::string id = detail::solid_id(solid_index);
            if (id == host_id || id == source_id
                || detail::contains_id(document.model.removed_solid_ids, id)) {
                continue;
            }
            const TopoDS_Solid other = TopoDS::Solid(exp.Current());
            if (detail::point_is_inside_or_on(other,
                                              gp_Pnt(mid.x, mid.y, mid.z),
                                              document.options.linear_tolerance)) {
                log_document_op(session,
                                document_id,
                                "fill_hole",
                                {param_string("candidate_id", candidate_id),
                                 param_string("host_solid_id", host_id),
                                 param_string("overlap_solid_id", id)},
                                false);
                return session_diagnostic(session, diagnostic(
                    DiagnosticSeverity::error,
                    "hole_fill_overlap_other_solid",
                    "Hole-fill plug center lies inside another active solid.",
                    id));
            }
        }

        if (selected->plug_volume_fraction_of_host > 0.25) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id),
                             param_double("plug_volume_fraction_of_host",
                                          selected->plug_volume_fraction_of_host)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_over_policy_limit",
                "Hole-fill plug is too large relative to the host for the default apply policy.",
                candidate_id));
        }

        if (detail::find_synthetic_body(document.model, candidate_id)) {
            log_document_op(session,
                            document_id,
                            "fill_hole",
                            {param_string("candidate_id", candidate_id),
                             param_string("host_solid_id", host_id)},
                            false);
            return session_diagnostic(session, diagnostic(
                DiagnosticSeverity::error,
                "hole_fill_already_applied",
                "Hole-fill candidate appears to have already been applied.",
                candidate_id));
        }

        /*
         * The candidate ID is proposal-local, so the emitted synthetic body
         * gets a normal synthetic:N identity. Provenance fields below keep
         * the fill connected to the host/source solids for reports, STEP
         * names, and later recipe replay.
         */
        std::string label = "fill_hole_from_" + host_id;
        if (!source_id.empty()) {
            label += "_after_removing_" + source_id;
        }

        const std::string synthetic_id =
            add_synthetic_cylinder(document.model,
                                   selected->base,
                                   selected->height,
                                   selected->radius,
                                   document.options.default_material_id,
                                   document.options.default_density,
                                   label);
        if (SyntheticBody* body =
                detail::find_synthetic_body_mut(document.model, synthetic_id)) {
            body->role = "hole_fill";
            body->host_solid_id = host_id;
            body->source_solid_id = source_id;
        }
        document.model.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::info,
            "hole_fill_applied",
            "Hole-fill plug was added as a synthetic body.",
            synthetic_id));

        std::vector<DecisionParam> params = {
            param_string("candidate_id", candidate_id),
            param_string("host_solid_id", host_id),
            param_string("source_solid_id", source_id),
            param_double("radius", selected->radius),
            param_double("length", selected->length),
            param_double("plug_volume", selected->plug_volume),
        };
        log_document_op(session, document_id, "fill_hole", params, true);
        return finish_document_mutation(session, document, true, id_mode);
    }

    log_document_op(session,
                    document_id,
                    "fill_hole",
                    {param_string("candidate_id", candidate_id)},
                    false);
    return session_error(session,
                         "document_not_found",
                         "Requested document ID was not found.",
                         document_id);
}

namespace {

bool target_exact_after_analysis(const CadDocument& document,
                                 const std::string& local_shape_id,
                                 bool* found_out) {
    return decomposition_target_exact_csg_supported(document.model,
                                                   local_shape_id,
                                                   document.options,
                                                   found_out);
}

const TopoDS_Solid* decomposed_solid_for_id(const CadModel& model,
                                            const std::string& id) {
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (piece.active && piece.id == id) return &piece.solid;
    }
    return nullptr;
}

TopoDS_Solid original_solid_for_id(const CadModel& model,
                                   const std::string& id) {
    std::size_t wanted = 0;
    if (!detail::parse_solid_id(id, &wanted)) return {};
    std::size_t index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++index;
        if (index == wanted) return TopoDS::Solid(exp.Current());
    }
    return {};
}

DecompositionProgressState collect_progress_state(
    CadModel& model,
    const ConversionOptions& options,
    ConversionReport* report_out = nullptr,
    PerformanceMetrics* performance = nullptr,
    detail::DecompositionProgressLedger* ledger_out = nullptr,
    const detail::DecompositionProgressLedger* reusable_ledger = nullptr,
    const detail::OverlapPairCache* reusable_overlaps = nullptr,
    detail::OverlapPairCache* overlap_cache_out = nullptr,
    const ConversionReport* authoritative_bodies = nullptr) {
    const auto start = Clock::now();
    const auto analysis_start = Clock::now();
    ConversionReport report = analyze_model(model, options);
    if (authoritative_bodies) {
        for (const AnalyzedSolid& authoritative :
             authoritative_bodies->solids) {
            if (authoritative.convexity_certificate_deferred) continue;
            const auto target = std::find_if(
                report.solids.begin(), report.solids.end(),
                [&](const AnalyzedSolid& solid) {
                    return solid.id == authoritative.id;
                });
            if (target != report.solids.end()) *target = authoritative;
        }
        // Decomposition children were certified with the ordinary exact
        // options before commit. Their stable IDs refer to unchanged child
        // shapes, so retain those certificates across the terminal lean pass.
        for (const AnalyzedDecomposedPiece& authoritative :
             authoritative_bodies->decomposed_pieces) {
            const auto target = std::find_if(
                report.decomposed_pieces.begin(),
                report.decomposed_pieces.end(),
                [&](const AnalyzedDecomposedPiece& piece) {
                    return piece.id == authoritative.id;
                });
            if (target != report.decomposed_pieces.end()) {
                *target = authoritative;
            }
        }
    }
    const auto overlap_start = Clock::now();
    if (performance)
        performance->full_analysis_ms += elapsed_ms(analysis_start);
    detail::detect_solid_overlaps(model, options, report, reusable_overlaps,
                                  overlap_cache_out);
    const auto ledger_start = Clock::now();
    if (performance) {
        performance->full_overlap_ms += elapsed_ms(overlap_start);
        performance->spatial_candidate_pairs +=
            report.performance.spatial_candidate_pairs;
        performance->exact_overlap_tests +=
            report.performance.exact_overlap_tests;
        performance->overlap_pair_cache_hits +=
            report.performance.overlap_pair_cache_hits;
        performance->overlap_pair_cache_misses +=
            report.performance.overlap_pair_cache_misses;
    }
    detail::DecompositionProgressLedger ledger =
        detail::build_decomposition_progress_ledger(
            model, report, options, performance, reusable_ledger);
    if (performance)
        performance->full_ledger_ms += elapsed_ms(ledger_start);
    const DecompositionProgressState state = ledger.state();
    if (ledger_out) *ledger_out = std::move(ledger);
    if (report_out) *report_out = std::move(report);
    if (performance) {
        performance->full_reconciliation_ms += elapsed_ms(start);
        ++performance->full_reconciliations;
    }
    return state;
}

bool candidate_confidence_allowed(const DecompositionCandidate& candidate,
                                  const FixedPointDecompositionPolicy& policy) {
    if (!candidate.usable) return false;
    if (candidate.reason == "bbox_axis_fallback" && !policy.allow_bbox_fallback) {
        return false;
    }
    if (candidate.confidence == "high") return true;
    if (candidate.confidence == "medium") {
        return policy.allow_medium_certified_progress;
    }
    if (candidate.confidence == "low") {
        return policy.allow_low_certified_progress;
    }
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
    if (candidate.confidence == "low"
        && policy.allow_low_confidence_auto) {
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

bool apply_candidate_geometry_to_model(
    CadModel& model,
    const ConversionOptions& options,
    const std::string& local_id,
    const DecompositionCandidate& candidate,
    std::vector<DecisionParam>* params_out = nullptr) {
    bool ok = false;
    std::vector<DecisionParam> params;
    params.push_back(param_string("tool_kind", candidate.tool_kind));
    params.push_back(param_double("score", candidate.score));
    params.push_back(param_string("confidence", candidate.confidence));

    if (!candidate.planes.empty()) {
        ok = decompose_solid_by_planes(model,
                                       local_id,
                                       candidate.planes,
                                       options);
        params.push_back(param_int(
            "plane_count", static_cast<long long>(candidate.planes.size())));
        for (std::size_t i = 0; i < candidate.planes.size(); ++i) {
            const PlaneSpec& p = candidate.planes[i];
            const std::string idx = std::to_string(i);
            params.push_back(param_double("plane_" + idx + "_ox", p.origin.x));
            params.push_back(param_double("plane_" + idx + "_oy", p.origin.y));
            params.push_back(param_double("plane_" + idx + "_oz", p.origin.z));
            params.push_back(param_double("plane_" + idx + "_nx", p.normal.x));
            params.push_back(param_double("plane_" + idx + "_ny", p.normal.y));
            params.push_back(param_double("plane_" + idx + "_nz", p.normal.z));
        }
    } else if (candidate.plane_valid && candidate.source_face_id.empty()) {
        ok = decompose_solid_by_plane(model,
                                      local_id,
                                      candidate.plane_origin,
                                      candidate.plane_normal,
                                      options);
        params.push_back(param_double("origin_x", candidate.plane_origin.x));
        params.push_back(param_double("origin_y", candidate.plane_origin.y));
        params.push_back(param_double("origin_z", candidate.plane_origin.z));
        params.push_back(param_double("normal_x", candidate.plane_normal.x));
        params.push_back(param_double("normal_y", candidate.plane_normal.y));
        params.push_back(param_double("normal_z", candidate.plane_normal.z));
    } else {
        ok = decompose_solid_by_face(model,
                                     local_id,
                                     candidate.source_face_id,
                                     options);
        params.push_back(param_string("face_id", candidate.source_face_id));
    }
    if (params_out) *params_out = std::move(params);
    return ok;
}

bool apply_candidate_geometry(Session& session,
                              CadDocument* document,
                              const std::string& local_id,
                              const std::string& public_id,
                              const DecompositionCandidate& candidate,
                              const std::string& log_op,
                              std::vector<DecisionParam> extra_params = {}) {
    const std::size_t before = document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    std::vector<DecisionParam> params;
    const bool ok = apply_candidate_geometry_to_model(
        document->model, document->options, local_id, candidate, &params);
    for (DecisionParam& param : params) {
        if (param.name == "face_id") {
            param = param_string("face_id",
                                 scoped_shape_id(document->id,
                                                 candidate.source_face_id));
        }
    }
    params.insert(params.begin(), extra_params.begin(), extra_params.end());
    document->performance.split_ms += elapsed_ms(start);
    document->performance.splitter_calls += 1;
    if (document->model.decomposed_pieces.size() > before) {
        document->performance.pieces_produced +=
            document->model.decomposed_pieces.size() - before;
    }

    log_solid_op(session,
                 document,
                 local_id,
                 public_id,
                 log_op,
                 params,
                 ok);
    return ok;
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

    const auto start = Clock::now();
    DecompositionProposal proposal =
        propose_decomposition_candidates(target.document->model,
                                         target.local_shape_id,
                                         target.document->options);
    target.document->performance.propose_ms += elapsed_ms(start);
    target.document->performance.candidates_evaluated += proposal.candidates.size();
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

    const bool ok = apply_candidate_geometry(
        session,
        target.document,
        target.local_shape_id,
        shape_id,
        *selected,
        "apply_decomposition_candidate",
        {param_string("candidate_id", candidate_id)});
    return finish_document_mutation(session, *target.document, ok, id_mode);
}

CommandResult auto_decompose_bsp(Session& session,
                                 const std::string& shape_id,
                                 int max_depth,
                                 const DecompositionPolicy& policy,
                                 ReportIdMode id_mode,
                                 bool defer_analysis) {
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
    const std::size_t transaction_log_start = session.log
        ? session.log->last_sequence() : 0;

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
            const auto propose_start = Clock::now();
            DecompositionProposal proposal =
                propose_decomposition_candidates(target.document->model,
                                                 item.local_id,
                                                 target.document->options);
            target.document->performance.propose_ms += elapsed_ms(propose_start);
            target.document->performance.candidates_evaluated +=
                proposal.candidates.size();
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
        if (splits_applied >= policy.max_splits_per_target) {
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::warning;
            diagnostic.code = "auto_decompose_bsp_split_budget";
            diagnostic.message =
                "BSP auto-decomposition stopped because max_splits_per_target was reached.";
            diagnostic.shape_id = item.local_id;
            diagnostic.shape_label = item.local_id;
            target.document->model.diagnostics.push_back(diagnostic);
            stopped = true;
            break;
        }

        const auto propose_start = Clock::now();
        DecompositionProposal proposal =
            propose_bsp_decomposition_candidates(target.document->model,
                                                 item.local_id,
                                                 target.document->options);
        target.document->performance.propose_ms += elapsed_ms(propose_start);
        target.document->performance.candidates_evaluated += proposal.candidates.size();
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
        const bool ok = apply_candidate_geometry(
            session,
            target.document,
            item.local_id,
            scoped_shape_id(target.document->id, item.local_id),
            *best,
            best->planes.empty()
                ? (best->plane_valid && best->source_face_id.empty()
                    ? "decompose_by_plane"
                    : "decompose_by_face")
                : "decompose_by_planes");
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

    if (splits_applied > 0) {
        coalesce_decomposed_pieces(target.document->model,
                                    target.document->options);
    }

    (void)defer_analysis;

    return finish_document_mutation(session,
                                    *target.document,
                                    !stopped,
                                    id_mode,
                                    transaction_log_start);
}

FixedPointDecompositionResult decompose_to_fixed_point(
    Session& session,
    const DocumentId& document_id,
    const FixedPointDecompositionPolicy& policy,
    ReportIdMode id_mode,
    FixedPointProgressCallback progress) {
    FixedPointDecompositionResult result;
    CadDocument* document = nullptr;
    for (CadDocument& candidate : session.documents) {
        if (candidate.id == document_id) { document = &candidate; break; }
    }
    if (!document) {
        result.command = session_error(session, "document_not_found",
                                       "Fixed-point decomposition document was not found.",
                                       document_id);
        return result;
    }
    if (policy.max_actions == 0 || policy.max_candidate_evaluations == 0
        || policy.max_splitter_calls == 0 || policy.max_active_pieces == 0
        || policy.max_residual_pieces == 0
        || policy.max_target_depth < 0
        || !std::isfinite(policy.max_action_relative_volume_delta)
        || policy.max_action_relative_volume_delta < 0.0
        || !std::isfinite(policy.max_cumulative_relative_volume_delta)
        || policy.max_cumulative_relative_volume_delta < 0.0) {
        result.command = session_error(session, "invalid_fixed_point_policy",
                                       "Fixed-point budgets must be positive and volume limits finite and non-negative.",
                                       document_id);
        return result;
    }

    const auto run_start = Clock::now();
    const std::size_t base_revision = document->revision;
    const std::size_t transaction_log_start = session.log
        ? session.log->last_sequence() : 0;
    CadModel working = document->model;
    ConversionOptions coordinator_options = document->options;
    coordinator_options.analyze_topology_metrics = false;
    coordinator_options.analyze_face_metrics = false;
    coordinator_options.analyze_fitted_replacements = false;
    std::size_t coordinator_solid_count = 0;
    for (TopExp_Explorer solid(document->model.root, TopAbs_SOLID);
         solid.More(); solid.Next()) {
        ++coordinator_solid_count;
    }
    // Staging is a large-assembly throughput mechanism. Small documents keep
    // the single authoritative pass, which preserves their immediate
    // structural-recognition routing and avoids needless provisional state.
    coordinator_options.defer_curved_halfspace_certificates =
        coordinator_solid_count >= 128;
    ConversionReport current_report;
    detail::DecompositionProgressLedger current_ledger;
    detail::OverlapPairCache current_overlap_cache;
    // Publish before the initial analysis/overlap reconciliation. Large STEP
    // imports and authoritative overlap baselines can each take minutes; a
    // caller must be able to distinguish that work from a hung shatter call.
    if (progress) {
        FixedPointProgressEvent event;
        event.phase = FixedPointProgressPhase::started;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = document->revision;
        try {
            if (!progress(event)) {
                result.summary.stop_reason = FixedPointStopReason::cancelled;
                result.summary.elapsed_ms = elapsed_ms(run_start);
                result.command.ok = true;
                event.phase = FixedPointProgressPhase::finished;
                event.stop_reason = FixedPointStopReason::cancelled;
                event.elapsed_ms = result.summary.elapsed_ms;
                event.committed_revision = document->revision;
                progress(event);
                return result;
            }
        } catch (...) {
            result.summary.stop_reason = FixedPointStopReason::cancelled;
            result.summary.elapsed_ms = elapsed_ms(run_start);
            result.command.ok = true;
            return result;
        }
    }
    DecompositionProgressState current = collect_progress_state(
        working, coordinator_options, &current_report, &document->performance,
        &current_ledger, nullptr, nullptr, &current_overlap_cache);
    // The initial authoritative sweep defines the pre-existing overlap
    // baseline for this transaction even when the CLI did not run a separate
    // analyze() call. Seed the mutation guard and its rollback snapshot with
    // those pairs before any exact local child IDs are allocated.
    document->known_overlap_pairs.clear();
    for (const SolidOverlap& overlap : current_report.overlaps) {
        document->known_overlap_pairs.push_back(
            overlap.solid_a_id < overlap.solid_b_id
                ? std::make_pair(overlap.solid_a_id, overlap.solid_b_id)
                : std::make_pair(overlap.solid_b_id, overlap.solid_a_id));
    }
    document->committed_state.known_overlap_pairs =
        document->known_overlap_pairs;
    result.summary.baseline = current;
    result.summary.final_state = current;
    result.summary.peak_active_pieces = current.active_piece_count;
    const double baseline_volume = current.total_volume;
    bool cancellation_requested = false;
    const auto publish = [&](FixedPointProgressPhase phase,
                             const std::string& target = {},
                             const std::string& candidate = {},
                             const std::string& strategy = {},
                             FixedPointStopReason reason =
                                 FixedPointStopReason::internal_error) {
        if (!progress) return true;
        FixedPointProgressEvent event;
        event.phase = phase;
        event.actions_applied = result.summary.actions_applied;
        event.candidates_evaluated = result.summary.candidates_evaluated;
        event.splitter_calls = result.summary.splitter_calls;
        event.target_id = target;
        event.candidate_id = candidate;
        event.strategy = strategy;
        event.state = current;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = document->revision;
        event.stop_reason = reason;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    const auto score_of = [&](const DecompositionProgressState& state) {
        return make_decomposition_progress_score(state, baseline_volume, policy);
    };

    if (!publish(FixedPointProgressPhase::initial_analysis_completed)) {
        cancellation_requested = true;
    }

    if (current.residual_count == 0) {
        result.summary.exact = true;
        result.summary.stop_reason = FixedPointStopReason::already_exact;
        result.summary.elapsed_ms = elapsed_ms(run_start);
        result.command = analyze(session, document_id, id_mode);
        result.command.ok = true;
        publish(FixedPointProgressPhase::finished, {}, {}, {},
                FixedPointStopReason::already_exact);
        return result;
    }

    struct ResidualTarget {
        std::string id;
        double volume = 0.0;
        bool unsupported = false;
        bool prismatic = false;
        bool revolution = false;
        bool shell_difference = false;
        bool pipe_annulus = false;
        bool certificate_deferred = false;
        int depth = 0;
    };
    struct DefinitionOccurrence {
        std::string geometry_digest;
        InstanceTransform transform;
    };
    std::unordered_map<std::string, const ComponentDefinition*> definitions;
    for (const ComponentDefinition& definition : working.component_definitions) {
        definitions.emplace(definition.id, &definition);
    }
    std::unordered_map<std::string, DefinitionOccurrence> occurrences;
    for (const ComponentInstance& instance : working.component_instances) {
        const auto definition = definitions.find(instance.definition_id);
        if (!instance.active || instance.source_shape_id.empty()
            || definition == definitions.end()
            || !definition->second->geometry_digest_verified
            || definition->second->geometry_digest.empty()) {
            continue;
        }
        occurrences.emplace(instance.source_shape_id,
            DefinitionOccurrence{definition->second->geometry_digest,
                                 instance.transform});
    }
    struct ConvertedDefinition {
        std::string source_solid_id;
        InstanceTransform transform;
    };
    std::unordered_map<std::string, ConvertedDefinition> converted_definitions;
    std::unordered_map<std::string, int> target_depth;
    std::unordered_map<std::string, std::size_t> stagnant_target_visits;
    for (const AnalyzedSolid& solid : current_report.solids)
        target_depth[solid.id] = 0;
    for (const AnalyzedDecomposedPiece& piece : current_report.decomposed_pieces)
        target_depth[piece.id] = 0;
    bool rejected_piece_budget = false;
    bool rejected_residual_budget = false;
    bool rejected_overlap = false;
    bool rejected_volume = false;
    struct AcceptedTrial {
        bool valid = false;
        CadModel model;
        DecompositionProgressState state;
        DecompositionCandidate candidate;
        std::string target_id;
        std::string strategy;
        std::string log_op;
        std::vector<DecisionParam> params;
        std::size_t before_coalesce = 0;
        std::size_t after_coalesce = 0;
        double action_delta = 0.0;
        bool definition_cache_hit = false;
        bool local_incremental = false;
        std::vector<TopoDS_Solid> local_children;
        std::vector<detail::BodyProgressCertificate>
            local_child_certificates;
        detail::LocalPartitionCertificate local_partition;
    };

    auto elapsed_limit_hit = [&]() {
        return policy.max_elapsed_ms > 0
            && elapsed_ms(run_start) >= static_cast<double>(policy.max_elapsed_ms);
    };
    auto cumulative_delta = [&](const DecompositionProgressState& state) {
        return std::abs(state.total_volume - baseline_volume)
            / std::max(std::abs(baseline_volume), 1.0e-30);
    };
    auto trial_is_better = [&](const AcceptedTrial& left,
                               const AcceptedTrial& right) {
        const auto ls = score_of(left.state);
        const auto rs = score_of(right.state);
        if (decomposition_progress_less(ls, rs)) return true;
        if (decomposition_progress_less(rs, ls)) return false;
        const double ld = cumulative_delta(left.state);
        const double rd = cumulative_delta(right.state);
        if (ld != rd) return ld < rd;
        if (left.state.active_piece_count != right.state.active_piece_count) {
            return left.state.active_piece_count < right.state.active_piece_count;
        }
        auto rank = [](const std::string& confidence) {
            return confidence == "high" ? 0 : confidence == "medium" ? 1 : 2;
        };
        const int lr = rank(left.candidate.confidence);
        const int rr = rank(right.candidate.confidence);
        if (lr != rr) return lr < rr;
        if (left.candidate.score != right.candidate.score) {
            return left.candidate.score > right.candidate.score;
        }
        return left.candidate.id < right.candidate.id;
    };

    auto validate_trial = [&](AcceptedTrial* trial) {
        ConversionReport trial_report;
        trial->state = collect_progress_state(
            trial->model, coordinator_options, &trial_report,
            &document->performance);
        trial->after_coalesce = std::count_if(
            trial->model.decomposed_pieces.begin(),
            trial->model.decomposed_pieces.end(),
            [](const DecomposedSolidPiece& piece) { return piece.active; });
        trial->action_delta = std::abs(trial->state.total_volume
                                      - current.total_volume)
            / std::max(std::abs(current.total_volume), 1.0e-30);
        if (trial->state.overlap_error_count > current.overlap_error_count) {
            rejected_overlap = true;
            return false;
        }
        if (trial->action_delta > policy.max_action_relative_volume_delta
            || cumulative_delta(trial->state)
                   > policy.max_cumulative_relative_volume_delta) {
            rejected_volume = true;
            return false;
        }
        if (trial->state.active_piece_count > policy.max_active_pieces) {
            rejected_piece_budget = true;
            return false;
        }
        if (trial->state.residual_count > policy.max_residual_pieces) {
            rejected_residual_budget = true;
            return false;
        }
        if (trial->state.unsupported_face_count
                   > current.unsupported_face_count
            || !decomposition_progress_less(score_of(trial->state),
                                             score_of(current))) {
            return false;
        }
        trial->valid = true;
        return true;
    };
    auto root_source_id = [](const CadModel& model,
                             const std::string& target_id) {
        if (target_id.rfind("solid:", 0) == 0) return target_id;
        for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
            if (piece.id == target_id && !piece.source_solid_id.empty()) {
                return piece.source_solid_id;
            }
        }
        return std::string{};
    };
    auto source_is_certified = [](const CadModel& model,
                                  const ConversionReport& report,
                                  const std::string& source_id) {
        bool found_piece = false;
        for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
            if (piece.source_solid_id != source_id) continue;
            found_piece = true;
            if (!piece.exact_csg_supported) return false;
        }
        for (const AnalyzedSolid& solid : report.solids) {
            if (solid.id == source_id && !solid.exact_csg_supported) return false;
        }
        for (const CadBooleanBody& body : model.cad_boolean_bodies) {
            if (body.active && body.source_solid_id == source_id) return true;
        }
        return found_piece;
    };

    FixedPointStopReason stop = FixedPointStopReason::no_improving_candidate;
    while (current.residual_count > 0) {
        rejected_piece_budget = false;
        rejected_residual_budget = false;
        rejected_overlap = false;
        rejected_volume = false;
        if (cancellation_requested) {
            stop = FixedPointStopReason::cancelled; break;
        }
        if (result.summary.actions_applied >= policy.max_actions) {
            stop = FixedPointStopReason::action_budget; break;
        }
        if (result.summary.candidates_evaluated
                >= policy.max_candidate_evaluations) {
            stop = FixedPointStopReason::candidate_budget; break;
        }
        if (result.summary.splitter_calls >= policy.max_splitter_calls) {
            stop = FixedPointStopReason::splitter_budget; break;
        }
        if (elapsed_limit_hit()) {
            stop = FixedPointStopReason::elapsed_time; break;
        }
        if (current.active_piece_count > policy.max_active_pieces) {
            stop = FixedPointStopReason::piece_budget; break;
        }
        if (current.residual_count > policy.max_residual_pieces) {
            stop = FixedPointStopReason::residual_budget; break;
        }

        std::vector<ResidualTarget> targets;
        for (const AnalyzedSolid& solid : current_report.solids) {
            if (solid.exact_csg_supported) continue;
            targets.push_back({solid.id, solid.volume,
                solid.needs_spline_replacement
                    || solid.needs_surface_replacement
                    || (solid.needs_user_decision
                        && !solid.needs_decomposition),
                solid.topology.prismatic_candidate,
                solid.topology.revolution.candidate
                    && solid.topology.revolution.full_revolution,
                solid.topology.cavity_shell_count > 0,
                solid.topology.pipe_annulus.candidate,
                solid.convexity_certificate_deferred,
                target_depth[solid.id]});
        }
        for (const AnalyzedDecomposedPiece& piece : current_report.decomposed_pieces) {
            if (piece.exact_csg_supported) continue;
            targets.push_back({piece.id, piece.volume,
                piece.needs_spline_replacement
                    || piece.needs_surface_replacement
                    || (piece.needs_user_decision
                        && !piece.needs_decomposition),
                false, false, false, false, false, target_depth[piece.id]});
        }
        std::sort(targets.begin(), targets.end(),
                  [](const ResidualTarget& a, const ResidualTarget& b) {
            if (a.unsupported != b.unsupported) return a.unsupported < b.unsupported;
            if (a.volume != b.volume) return a.volume > b.volume;
            return a.id < b.id;
        });

        AcceptedTrial chosen;
        bool saw_supported = false;
        bool saw_depth_block = false;
        bool refreshed_deferred_certificate = false;
        for (const ResidualTarget& target : targets) {
            if (result.summary.candidates_evaluated
                    >= policy.max_candidate_evaluations
                || result.summary.splitter_calls
                    >= policy.max_splitter_calls
                || elapsed_limit_hit()) {
                break;
            }
            if (target.unsupported) continue;
            if (policy.max_stagnant_target_visits > 0
                && stagnant_target_visits[target.id]
                    >= policy.max_stagnant_target_visits) {
                continue;
            }
            saw_supported = true;
            if (target.depth >= policy.max_target_depth) {
                saw_depth_block = true;
                continue;
            }
            if (!publish(FixedPointProgressPhase::target_started,
                         target.id, {},
                         target.certificate_deferred
                             ? "exact_certificate"
                             : "candidate_search")) {
                cancellation_requested = true;
                break;
            }

            // The initial large-assembly pass deliberately defers expensive
            // curved face/half-space Booleans. Resolve one selected target at
            // a safe progress boundary before proposing any mutation. A body
            // found convex simply leaves the residual set; a non-convex body
            // re-enters with its exact failure evidence and normal strategies.
            if (target.certificate_deferred) {
                ConversionOptions exact_options = coordinator_options;
                exact_options.defer_curved_halfspace_certificates = false;
                exact_options.analysis_target_ids = {target.id};
                const auto exact_start = Clock::now();
                ConversionReport exact_report =
                    analyze_model(working, exact_options);
                document->performance.local_analysis_ms +=
                    elapsed_ms(exact_start);
                const auto exact_solid = std::find_if(
                    exact_report.solids.begin(), exact_report.solids.end(),
                    [&](const AnalyzedSolid& solid) {
                        return solid.id == target.id;
                    });
                if (exact_solid != exact_report.solids.end()) {
                    const auto current_solid = std::find_if(
                        current_report.solids.begin(),
                        current_report.solids.end(),
                        [&](const AnalyzedSolid& solid) {
                            return solid.id == target.id;
                        });
                    if (current_solid != current_report.solids.end()) {
                        *current_solid = *exact_solid;
                        current_ledger =
                            detail::build_decomposition_progress_ledger(
                                working, current_report, coordinator_options,
                                &document->performance, &current_ledger);
                        current = current_ledger.state();
                        refreshed_deferred_certificate = true;
                        break;
                    }
                }
            }
            AcceptedTrial best;

            const auto target_occurrence = occurrences.find(target.id);
            if (target_occurrence != occurrences.end()) {
                const auto converted = converted_definitions.find(
                    target_occurrence->second.geometry_digest);
                if (converted != converted_definitions.end()
                    && converted->second.source_solid_id != target.id) {
                    AcceptedTrial trial;
                    ++document->performance.whole_model_trial_copies;
                    trial.model = working;
                    trial.target_id = target.id;
                    trial.strategy = "definition_conversion_cache";
                    trial.log_op = "reuse_definition_conversion";
                    trial.definition_cache_hit = true;
                    trial.params.push_back(param_string(
                        "source_solid_id",
                        scoped_shape_id(document->id,
                            converted->second.source_solid_id)));
                    trial.before_coalesce = std::count_if(
                        trial.model.decomposed_pieces.begin(),
                        trial.model.decomposed_pieces.end(),
                        [](const DecomposedSolidPiece& piece) {
                            return piece.active;
                        });
                    if (reuse_definition_decomposition(
                            trial.model,
                            converted->second.source_solid_id,
                            converted->second.transform,
                            target.id,
                            target_occurrence->second.transform,
                            document->options)
                        && validate_trial(&trial)) {
                        best = std::move(trial);
                    }
                }
            }

            if (policy.remove_residual_slivers
                && target.id.rfind("piece:", 0) == 0
                && target.volume <= std::abs(baseline_volume)
                       * policy.min_piece_volume_fraction) {
                AcceptedTrial trial;
                ++document->performance.whole_model_trial_copies;
                trial.model = working;
                trial.target_id = target.id;
                trial.strategy = "drop_residual_sliver";
                trial.log_op = "remove_solid";
                trial.before_coalesce = std::count_if(
                    trial.model.decomposed_pieces.begin(),
                    trial.model.decomposed_pieces.end(),
                    [](const DecomposedSolidPiece& piece) { return piece.active; });
                if (remove_solid(trial.model, target.id)
                    && validate_trial(&trial)) {
                    best = std::move(trial);
                }
            }
            // Removing a certified decomposition sliver strictly eliminates
            // its entire residual volume and cannot be beaten by splitting
            // that same numerical fragment. Avoid invoking OCCT proposal
            // generation on near-degenerate geometry.
            if (best.valid) {
                chosen = std::move(best);
                break;
            }

            auto try_direct = [&](const std::string& strategy,
                                  const std::string& log_op,
                                  bool (*operation)(CadModel&, const std::string&,
                                                    const ConversionOptions&)) {
                if (result.summary.splitter_calls >= policy.max_splitter_calls
                    || elapsed_limit_hit()) return;
                AcceptedTrial trial;
                const bool local_structural_adapter =
                    policy.use_incremental_trials
                    && (strategy == "prismatic"
                        || strategy == "revolution");
                TopoDS_Shape local_parent_shape;
                if (local_structural_adapter) {
                    if (!active_shape_for_id(working, target.id,
                                             &local_parent_shape, nullptr)
                        || local_parent_shape.ShapeType() != TopAbs_SOLID) {
                        return;
                    }
                    // Existing recognizers expose a mutating CadModel API.
                    // Run that API against a one-body sandbox rather than
                    // cloning the assembly; the resulting child tail is then
                    // certified and committed through the normal local path.
                    DecomposedSolidPiece seed;
                    seed.id = target.id;
                    seed.label = target.id + "/structural_trial";
                    seed.source_solid_id = root_source_id(working, target.id);
                    seed.solid = TopoDS::Solid(local_parent_shape);
                    seed.volume = target.volume;
                    seed.active = true;
                    trial.model.decomposed_pieces.push_back(std::move(seed));
                } else {
                    ++document->performance.whole_model_trial_copies;
                    trial.model = working;
                }
                trial.target_id = target.id;
                trial.strategy = strategy;
                trial.log_op = log_op;
                trial.before_coalesce = std::count_if(
                    working.decomposed_pieces.begin(),
                    working.decomposed_pieces.end(),
                    [](const DecomposedSolidPiece& piece) { return piece.active; });
                const std::size_t piece_tail =
                    trial.model.decomposed_pieces.size();
                ++result.summary.splitter_calls;
                if (!operation(trial.model, target.id, document->options)) return;
                // Prismatic/revolution recognizers materialize an exact local
                // partition as new decomposed pieces. Certify and project that
                // tail directly instead of analyzing the copied assembly.
                if (local_structural_adapter) {
                    std::vector<TopoDS_Solid> children;
                    for (std::size_t i = piece_tail;
                         i < trial.model.decomposed_pieces.size(); ++i) {
                        const DecomposedSolidPiece& piece =
                            trial.model.decomposed_pieces[i];
                        if (piece.active && !piece.solid.IsNull())
                            children.push_back(piece.solid);
                    }
                    if (children.size() > 1) {
                        children = detail::coalesce_local_partition(
                            children, root_source_id(working, target.id),
                            document->options);
                    }
                    TopoDS_Shape parent_shape;
                    const detail::BodyProgressCertificate* parent =
                        current_ledger.find(target.id);
                    if (!children.empty() && parent && parent->valid
                        && active_shape_for_id(working, target.id,
                                               &parent_shape, nullptr)
                        && parent_shape.ShapeType() == TopAbs_SOLID) {
                        trial.local_partition =
                            detail::certify_local_partition(
                                TopoDS::Solid(parent_shape), children,
                                document->options,
                                policy.max_local_boolean_calls,
                                &document->performance);
                        if (trial.local_partition.exact) {
                            const detail::LocalOverlapCertificate overlap =
                                detail::certify_local_neighbour_overlaps(
                                    working, target.id, children,
                                    current_report.overlaps,
                                    document->options,
                                    &document->performance,
                                    true);
                            if (!overlap.safe) return;
                            std::vector<detail::BodyProgressCertificate>
                                certificates;
                            certificates.reserve(children.size());
                            trial.candidate.id = strategy + ":local";
                            trial.candidate.target_id = target.id;
                            trial.candidate.tool_kind = strategy;
                            trial.candidate.reason =
                                "structural_local_partition";
                            trial.candidate.confidence = "high";
                            trial.candidate.usable = true;
                            trial.candidate.original_volume = parent->volume;
                            trial.candidate.piece_count = children.size();
                            trial.candidate.pieces_volume = 0.0;
                            const std::string source_id = root_source_id(
                                working, target.id);
                            std::size_t probe_index = 0;
                            for (const TopoDS_Solid& child_shape : children) {
                                ++probe_index;
                                DecomposedSolidPiece probe;
                                probe.id = "__structural_probe:"
                                    + std::to_string(probe_index);
                                probe.source_solid_id = source_id;
                                probe.solid = child_shape;
                                GProp_GProps properties;
                                BRepGProp::VolumeProperties(child_shape,
                                                           properties);
                                probe.volume = std::abs(properties.Mass());
                                const AnalyzedDecomposedPiece analyzed =
                                    detail::analyze_decomposed_piece(
                                        probe, probe_index,
                                        600000 + probe_index,
                                        document->options);
                                DecompositionCandidatePiece summary;
                                summary.index = probe_index;
                                summary.volume = analyzed.volume;
                                summary.volume_fraction = parent->volume > 0.0
                                    ? analyzed.volume / parent->volume : 0.0;
                                summary.bbox = analyzed.bbox;
                                summary.face_count = analyzed.face_count;
                                summary.exact_csg_supported =
                                    analyzed.exact_csg_supported;
                                summary.needs_decomposition =
                                    analyzed.needs_decomposition;
                                summary.needs_spline_replacement =
                                    analyzed.needs_spline_replacement;
                                summary.needs_surface_replacement =
                                    analyzed.needs_surface_replacement;
                                summary.needs_user_decision =
                                    analyzed.needs_user_decision;
                                summary.halfspace_violation_count =
                                    detail::convexity_halfspace_violation_count(
                                        child_shape, 600000 + probe_index,
                                        document->options);
                                trial.candidate.pieces.push_back(summary);
                                trial.candidate.pieces_volume += analyzed.volume;
                                if (analyzed.exact_csg_supported)
                                    ++trial.candidate.exact_piece_count;
                                if (analyzed.needs_decomposition)
                                    ++trial.candidate
                                        .pieces_requiring_decomposition;

                                detail::BodyProgressCertificate certificate;
                                certificate.body_id = "__local_structural:"
                                    + std::to_string(probe_index);
                                certificate.source_solid_id = source_id;
                                certificate.volume = analyzed.volume;
                                certificate.face_count = analyzed.face_count;
                                certificate.exact_csg_supported =
                                    analyzed.exact_csg_supported;
                                certificate.needs_decomposition =
                                    analyzed.needs_decomposition;
                                certificate.needs_user_decision =
                                    analyzed.needs_user_decision;
                                if (!analyzed.exact_csg_supported
                                    && (analyzed.needs_spline_replacement
                                        || analyzed.needs_surface_replacement
                                        || (analyzed.needs_user_decision
                                            && !analyzed.needs_decomposition))) {
                                    certificate.unsupported_face_count =
                                        std::max<std::size_t>(
                                            1, analyzed.face_count);
                                }
                                certificate.halfspace_violation_count =
                                    summary.halfspace_violation_count;
                                certificate.valid = true;
                                certificates.push_back(std::move(certificate));
                            }
                            trial.candidate.relative_delta =
                                trial.local_partition.relative_volume_delta;
                            if (current_ledger.project_replacement(
                                    target.id, certificates,
                                    current.overlap_error_count,
                                    &trial.state)
                                && trial.state.active_piece_count
                                    <= policy.max_active_pieces
                                && trial.state.residual_count
                                    <= policy.max_residual_pieces
                                && trial.state.unsupported_face_count
                                    <= current.unsupported_face_count
                                && cumulative_delta(trial.state)
                                    <= policy
                                        .max_cumulative_relative_volume_delta
                                && decomposition_progress_less(
                                    score_of(trial.state), score_of(current))) {
                                trial.action_delta = trial.local_partition
                                    .relative_volume_delta;
                                trial.after_coalesce =
                                    trial.before_coalesce
                                    - (target.id.rfind("piece:", 0) == 0
                                       ? 1 : 0)
                                    + children.size();
                                trial.local_children = std::move(children);
                                trial.local_child_certificates =
                                    std::move(certificates);
                                trial.local_incremental = true;
                                trial.model = {};
                                trial.valid = true;
                                if (!best.valid
                                    || trial_is_better(trial, best)) {
                                    best = std::move(trial);
                                }
                                return;
                            }
                        }
                    }
                    // A sandbox result is meaningful only through the exact
                    // local certificate above. Never feed the one-body model
                    // to document-wide legacy validation.
                    return;
                }
                coalesce_decomposed_pieces(trial.model, document->options);
                if (!validate_trial(&trial)) return;
                if (!best.valid || trial_is_better(trial, best)) {
                    best = std::move(trial);
                }
            };

            if (target.shell_difference && policy.run_shell_difference_strategy) {
                try_direct("shell_difference", "decompose_shell_difference",
                           decompose_solid_shell_difference);
            }
            if (target.pipe_annulus) {
                try_direct("pipe_annulus_difference", "decompose_pipe_annulus",
                           decompose_solid_pipe_annulus);
            }
            if (target.revolution) {
                try_direct("revolution", "decompose_revolution",
                           decompose_solid_revolution);
            }
            if (target.prismatic && policy.run_prismatic_strategy) {
                try_direct("prismatic", "decompose_prismatic",
                           decompose_solid_prismatic);
            }
            // A certified zero-residual structural recognition is the best
            // possible progress score. Do not spend memory and splitter
            // budget generating generic BSP or shatter alternatives.
            if (best.valid && best.state.residual_count == 0) {
                chosen = std::move(best);
                break;
            }
            if (elapsed_limit_hit()) break;
            const auto propose_start = Clock::now();
            detail::LocalBspProposal local_proposal;
            DecompositionProposal proposal;
            if (policy.use_incremental_trials) {
                const std::size_t candidate_remaining =
                    policy.max_candidate_evaluations
                        - result.summary.candidates_evaluated;
                const std::size_t splitter_remaining =
                    policy.max_splitter_calls - result.summary.splitter_calls;
                const std::size_t proposal_budget = std::min(
                    policy.max_candidates_per_target,
                    std::min(candidate_remaining, splitter_remaining));
                local_proposal =
                    detail::propose_bsp_decomposition_candidates_local(
                        working, target.id, document->options,
                        policy.run_parallel_plane_batches,
                        policy.max_parallel_families_per_target,
                        policy.max_planes_per_batch,
                        proposal_budget);
                proposal = local_proposal.proposal;
            } else {
                proposal = propose_bsp_decomposition_candidates(
                    working, target.id, document->options);
            }
            document->performance.propose_ms += elapsed_ms(propose_start);
            result.summary.splitter_calls += proposal.candidates.size();
            document->performance.splitter_calls += proposal.candidates.size();
            for (const DecompositionCandidate& proposed : proposal.candidates) {
                if (proposed.tool_kind == "parallel_face_planes") {
                    ++document->performance.parallel_plane_batches;
                }
            }
            const std::size_t limit = std::min(
                proposal.candidates.size(), policy.max_candidates_per_target);
            for (std::size_t i = 0; i < limit; ++i) {
                if (result.summary.candidates_evaluated
                        >= policy.max_candidate_evaluations
                    || result.summary.splitter_calls >= policy.max_splitter_calls
                    || elapsed_limit_hit()) break;
                const DecompositionCandidate& candidate = proposal.candidates[i];
                ++result.summary.candidates_evaluated;
                ++document->performance.candidates_evaluated;
                if (!publish(FixedPointProgressPhase::candidate_evaluated,
                             target.id, candidate.id, "bsp")) {
                    cancellation_requested = true;
                    break;
                }
                if (!candidate_confidence_allowed(candidate, policy)
                    || current.active_piece_count - 1 + candidate.piece_count
                           > policy.max_active_pieces
                    || candidate.sliver_piece_count > 0
                    || candidate.relative_delta
                           > policy.max_action_relative_volume_delta
                    || candidate.min_piece_volume_fraction
                           < policy.min_piece_volume_fraction) {
                    if (current.active_piece_count - 1 + candidate.piece_count
                            > policy.max_active_pieces) {
                        rejected_piece_budget = true;
                    }
                    continue;
                }
                AcceptedTrial trial;
                trial.target_id = target.id;
                trial.strategy = "bsp";
                trial.candidate = candidate;
                trial.log_op = candidate.planes.empty()
                    ? (candidate.plane_valid
                        ? "decompose_by_plane" : "decompose_by_face")
                    : "decompose_by_planes";
                trial.before_coalesce = std::count_if(
                    working.decomposed_pieces.begin(),
                    working.decomposed_pieces.end(),
                    [](const DecomposedSolidPiece& piece) { return piece.active; });
                if (policy.use_incremental_trials) {
                    const auto geometry =
                        local_proposal.candidate_children.find(candidate.id);
                    const detail::BodyProgressCertificate* parent =
                        current_ledger.find(target.id);
                    TopoDS_Shape parent_shape;
                    if (geometry == local_proposal.candidate_children.end()
                        || !parent || !parent->valid
                        || !active_shape_for_id(working, target.id,
                                                &parent_shape, nullptr)
                        || parent_shape.ShapeType() != TopAbs_SOLID
                        || geometry->second.size() != candidate.pieces.size()) {
                        continue;
                    }
                    std::vector<detail::BodyProgressCertificate> children;
                    children.reserve(candidate.pieces.size());
                    const std::string source_id = root_source_id(
                        working, target.id);
                    for (std::size_t child_index = 0;
                         child_index < candidate.pieces.size(); ++child_index) {
                        const DecompositionCandidatePiece& summary =
                            candidate.pieces[child_index];
                        detail::BodyProgressCertificate child;
                        child.body_id = "__local_trial:" + candidate.id + ":"
                            + std::to_string(child_index + 1);
                        child.source_solid_id = source_id;
                        child.volume = summary.volume;
                        child.face_count = summary.face_count;
                        child.exact_csg_supported =
                            summary.exact_csg_supported;
                        child.needs_decomposition = summary.needs_decomposition;
                        child.needs_user_decision =
                            summary.needs_user_decision;
                        if (!summary.exact_csg_supported
                            && (summary.needs_spline_replacement
                                || summary.needs_surface_replacement
                                || (summary.needs_user_decision
                                    && !summary.needs_decomposition))) {
                            child.unsupported_face_count =
                                std::max<std::size_t>(1, summary.face_count);
                        }
                        child.halfspace_violation_count =
                            summary.halfspace_violation_count;
                        child.valid = true;
                        children.push_back(std::move(child));
                    }
                    DecompositionProgressState projected_state;
                    if (!current_ledger.project_replacement(
                            target.id, children, current.overlap_error_count,
                            &projected_state)) {
                        continue;
                    }
                    // Candidate child analysis is already available from the
                    // proposer. Reject algebraically non-improving partitions
                    // before spending two potentially expensive OCCT Booleans
                    // on the exact union/parent certificate.
                    if (projected_state.overlap_error_count
                            > current.overlap_error_count
                        || candidate.relative_delta
                            > policy.max_action_relative_volume_delta
                        || cumulative_delta(projected_state)
                            > policy.max_cumulative_relative_volume_delta
                        || projected_state.active_piece_count
                            > policy.max_active_pieces
                        || projected_state.residual_count
                            > policy.max_residual_pieces
                        || projected_state.unsupported_face_count
                            > current.unsupported_face_count
                        || !decomposition_progress_less(score_of(projected_state),
                                                        score_of(current))) {
                        continue;
                    }
                    trial.state = projected_state;
                    // The proposer has already analyzed every child, so this
                    // comparison is exact for the coordinator's ordering. If
                    // the projected state cannot beat an already certified
                    // candidate, two more OCCT Boolean calls cannot change
                    // the winner and are unnecessary. Candidates that can
                    // become the winner are still certified in full.
                    if (best.valid && !trial_is_better(trial, best)) {
                        continue;
                    }
                    trial.local_partition = detail::certify_local_partition(
                        TopoDS::Solid(parent_shape), geometry->second,
                        document->options, policy.max_local_boolean_calls,
                        &document->performance);
                    if (!trial.local_partition.exact) continue;
                    const detail::LocalOverlapCertificate local_overlap =
                        detail::certify_local_neighbour_overlaps(
                            working, target.id, geometry->second,
                            current_report.overlaps, document->options,
                            &document->performance,
                            true);
                    if (!local_overlap.safe) continue;
                    trial.action_delta =
                        trial.local_partition.relative_volume_delta;
                    trial.after_coalesce = trial.before_coalesce -
                        (target.id.rfind("piece:", 0) == 0 ? 1 : 0)
                        + geometry->second.size();
                    trial.local_children = geometry->second;
                    trial.local_child_certificates = std::move(children);
                    trial.local_incremental = true;
                    trial.params.push_back(param_string(
                        "tool_kind", candidate.tool_kind));
                    trial.params.push_back(param_double("score", candidate.score));
                    trial.params.push_back(param_string(
                        "confidence", candidate.confidence));
                    if (!candidate.planes.empty()) {
                        trial.params.push_back(param_int(
                            "plane_count",
                            static_cast<long long>(candidate.planes.size())));
                        for (std::size_t plane_index = 0;
                             plane_index < candidate.planes.size(); ++plane_index) {
                            const PlaneSpec& plane = candidate.planes[plane_index];
                            const std::string index = std::to_string(plane_index);
                            trial.params.push_back(param_double(
                                "plane_" + index + "_ox", plane.origin.x));
                            trial.params.push_back(param_double(
                                "plane_" + index + "_oy", plane.origin.y));
                            trial.params.push_back(param_double(
                                "plane_" + index + "_oz", plane.origin.z));
                            trial.params.push_back(param_double(
                                "plane_" + index + "_nx", plane.normal.x));
                            trial.params.push_back(param_double(
                                "plane_" + index + "_ny", plane.normal.y));
                            trial.params.push_back(param_double(
                                "plane_" + index + "_nz", plane.normal.z));
                        }
                    } else if (candidate.plane_valid) {
                        trial.params.push_back(param_double(
                            "origin_x", candidate.plane_origin.x));
                        trial.params.push_back(param_double(
                            "origin_y", candidate.plane_origin.y));
                        trial.params.push_back(param_double(
                            "origin_z", candidate.plane_origin.z));
                        trial.params.push_back(param_double(
                            "normal_x", candidate.plane_normal.x));
                        trial.params.push_back(param_double(
                            "normal_y", candidate.plane_normal.y));
                        trial.params.push_back(param_double(
                            "normal_z", candidate.plane_normal.z));
                    } else {
                        trial.params.push_back(param_string(
                            "face_id", candidate.source_face_id));
                    }
                    trial.valid = true;
                } else {
                    ++document->performance.whole_model_trial_copies;
                    trial.model = working;
                    ++result.summary.splitter_calls;
                    if (!apply_candidate_geometry_to_model(
                            trial.model, document->options, target.id,
                            candidate, &trial.params)) {
                        continue;
                    }
                    coalesce_decomposed_pieces(trial.model, document->options);
                    if (!validate_trial(&trial)) continue;
                }
                if (!best.valid || trial_is_better(trial, best)) {
                    best = std::move(trial);
                }
            }
            if (best.valid) {
                stagnant_target_visits.erase(target.id);
                chosen = std::move(best);
                break;
            }
            if (!cancellation_requested && !elapsed_limit_hit()
                && result.summary.candidates_evaluated
                    < policy.max_candidate_evaluations
                && result.summary.splitter_calls
                    < policy.max_splitter_calls) {
                ++stagnant_target_visits[target.id];
            }
        }

        if (refreshed_deferred_certificate) continue;

        // Shatter is deliberately a global fallback phase.  In particular,
        // do not compare it with ordinary BSP candidates for each target:
        // shatter usually removes a complete residual and therefore wins the
        // progress score, but it is much more expensive and can make a large
        // assembly appear to "go directly to shatter".  Reach this phase only
        // after structural recognition and bounded BSP failed to produce a
        // certified improvement for every supported residual target.
        if (!chosen.valid && !cancellation_requested
            && policy.run_shatter_fallback) {
            for (const ResidualTarget& target : targets) {
                if (target.unsupported
                    || target.depth >= policy.max_target_depth) {
                    continue;
                }
                if (result.summary.splitter_calls
                        >= policy.max_splitter_calls
                    || elapsed_limit_hit()) {
                    break;
                }
                if (!publish(FixedPointProgressPhase::target_started,
                             target.id, {}, "shatter_fallback")) {
                    cancellation_requested = true;
                    break;
                }

                AcceptedTrial trial;
                ++document->performance.whole_model_trial_copies;
                trial.model = working;
                trial.target_id = target.id;
                trial.strategy = "shatter";
                trial.log_op = "shatter";
                trial.before_coalesce = std::count_if(
                    trial.model.decomposed_pieces.begin(),
                    trial.model.decomposed_pieces.end(),
                    [](const DecomposedSolidPiece& piece) {
                        return piece.active;
                    });
                ++result.summary.splitter_calls;
                if (!shatter_solid(trial.model, target.id,
                                   document->options)) {
                    continue;
                }
                coalesce_decomposed_pieces(trial.model,
                                            document->options);
                if (!validate_trial(&trial)) continue;
                chosen = std::move(trial);
                break;
            }
        }

        if (cancellation_requested) {
            stop = FixedPointStopReason::cancelled;
            break;
        }

        if (!chosen.valid) {
            stop = elapsed_limit_hit()
                ? FixedPointStopReason::elapsed_time
                : result.summary.candidates_evaluated
                        >= policy.max_candidate_evaluations
                ? FixedPointStopReason::candidate_budget
                : result.summary.splitter_calls >= policy.max_splitter_calls
                ? FixedPointStopReason::splitter_budget
                : rejected_overlap ? FixedPointStopReason::overlap
                : rejected_volume ? FixedPointStopReason::volume_drift
                : rejected_piece_budget ? FixedPointStopReason::piece_budget
                : rejected_residual_budget ? FixedPointStopReason::residual_budget
                : saw_depth_block ? FixedPointStopReason::depth_budget
                : saw_supported ? FixedPointStopReason::no_improving_candidate
                                : FixedPointStopReason::unsupported_geometry;
            break;
        }

        const std::string converted_source_id =
            root_source_id(working, chosen.target_id);
        if (chosen.local_incremental) {
            CadModel before_local_commit = working;
            std::vector<std::string> committed_ids;
            if (!detail::commit_local_decomposition_partition(
                    working, chosen.target_id, chosen.local_children,
                    &committed_ids)
                || committed_ids.size()
                    != chosen.local_child_certificates.size()) {
                working = std::move(before_local_commit);
                stop = FixedPointStopReason::internal_error;
                break;
            }
            for (std::size_t child_index = 0;
                 child_index < committed_ids.size(); ++child_index) {
                chosen.local_child_certificates[child_index].body_id =
                    committed_ids[child_index];
            }
            // An exact child subset cannot introduce overlap volume outside
            // its parent. Preserve known parent-neighbour overlap lineage so
            // the bounded post-mutation scan does not misclassify the same
            // pre-existing region under freshly allocated child IDs.
            std::vector<std::string> inherited_overlap_neighbours;
            for (const auto& pair : document->known_overlap_pairs) {
                if (pair.first == chosen.target_id)
                    inherited_overlap_neighbours.push_back(pair.second);
                else if (pair.second == chosen.target_id)
                    inherited_overlap_neighbours.push_back(pair.first);
            }
            for (const std::string& child_id : committed_ids) {
                for (const std::string& neighbour_id
                     : inherited_overlap_neighbours) {
                    const auto key = child_id < neighbour_id
                        ? std::make_pair(child_id, neighbour_id)
                        : std::make_pair(neighbour_id, child_id);
                    if (std::find(document->known_overlap_pairs.begin(),
                                  document->known_overlap_pairs.end(), key)
                            == document->known_overlap_pairs.end()) {
                        document->known_overlap_pairs.push_back(key);
                    }
                }
            }
            const std::size_t unchanged_bodies = current_ledger.bodies().size()
                > 0 ? current_ledger.bodies().size() - 1 : 0;
            if (!current_ledger.commit_replacement(
                    chosen.target_id, chosen.local_child_certificates,
                    current.overlap_error_count)) {
                working = std::move(before_local_commit);
                stop = FixedPointStopReason::internal_error;
                break;
            }
            document->performance.body_certificate_cache_hits +=
                unchanged_bodies;
            document->performance.body_certificate_cache_misses +=
                chosen.local_child_certificates.size();
            current = current_ledger.state();
            chosen.state = current;

            current_report.solids.erase(
                std::remove_if(current_report.solids.begin(),
                               current_report.solids.end(),
                    [&](const AnalyzedSolid& solid) {
                        return solid.id == chosen.target_id;
                    }), current_report.solids.end());
            current_report.decomposed_pieces.erase(
                std::remove_if(current_report.decomposed_pieces.begin(),
                               current_report.decomposed_pieces.end(),
                    [&](const AnalyzedDecomposedPiece& piece) {
                        return piece.id == chosen.target_id;
                    }), current_report.decomposed_pieces.end());
            for (std::size_t child_index = 0;
                 child_index < committed_ids.size(); ++child_index) {
                const DecompositionCandidatePiece& summary =
                    chosen.candidate.pieces[child_index];
                AnalyzedDecomposedPiece piece;
                piece.id = committed_ids[child_index];
                piece.label = piece.id + "/from_" + chosen.target_id;
                piece.source_solid_id = converted_source_id;
                piece.index = current_report.decomposed_pieces.size() + 1;
                piece.volume = summary.volume;
                piece.bbox = summary.bbox;
                piece.face_count = summary.face_count;
                piece.exact_csg_supported = summary.exact_csg_supported;
                piece.needs_decomposition = summary.needs_decomposition;
                piece.needs_spline_replacement =
                    summary.needs_spline_replacement;
                piece.needs_surface_replacement =
                    summary.needs_surface_replacement;
                piece.needs_user_decision = summary.needs_user_decision;
                current_report.decomposed_pieces.push_back(std::move(piece));
            }
            current_report.total_volume = current.total_volume;

            if (policy.verify_incremental_state_each_action) {
                ConversionReport reconciled_report;
                detail::DecompositionProgressLedger reconciled_ledger;
                const DecompositionProgressState reconciled =
                    collect_progress_state(
                        working, coordinator_options, &reconciled_report,
                        &document->performance, &reconciled_ledger);
                if (!detail::progress_states_equivalent(
                        reconciled, current, 1.0e-9, 1.0e-9)) {
                    working = std::move(before_local_commit);
                    collect_progress_state(working, coordinator_options,
                                           &current_report,
                                           &document->performance,
                                           &current_ledger);
                    stop = FixedPointStopReason::internal_error;
                    break;
                }
                current = reconciled;
                chosen.state = reconciled;
                current_report = std::move(reconciled_report);
                current_ledger = std::move(reconciled_ledger);
            }
        } else {
            working = std::move(chosen.model);
            current = chosen.state;
            collect_progress_state(working, coordinator_options, &current_report,
                                   &document->performance, &current_ledger);
        }

        FixedPointIterationRecord record;
        record.iteration = result.summary.actions_applied + 1;
        record.target_id = chosen.target_id;
        record.candidate_id = chosen.candidate.id;
        record.strategy = chosen.strategy;
        record.confidence = chosen.candidate.confidence;
        record.before = current;
        record.after = chosen.state;
        record.action_relative_volume_delta = chosen.action_delta;
        record.cumulative_relative_volume_delta = cumulative_delta(chosen.state);
        record.pieces_before_coalesce = chosen.before_coalesce;
        record.pieces_after_coalesce = chosen.after_coalesce;
        record.candidates_evaluated = result.summary.candidates_evaluated;
        record.accepted = true;

        if (chosen.definition_cache_hit) {
            ++document->performance.definition_conversion_cache_hits;
        }
        const auto converted_occurrence = occurrences.find(converted_source_id);
        if (!converted_source_id.empty()
            && converted_occurrence != occurrences.end()
            && source_is_certified(working, current_report,
                                   converted_source_id)
            && converted_definitions.find(
                   converted_occurrence->second.geometry_digest)
                   == converted_definitions.end()) {
            converted_definitions.emplace(
                converted_occurrence->second.geometry_digest,
                ConvertedDefinition{converted_source_id,
                                    converted_occurrence->second.transform});
            ++document->performance.definition_conversion_cache_published;
        }
        const int child_depth = target_depth[chosen.target_id] + 1;
        for (const AnalyzedSolid& solid : current_report.solids) {
            if (target_depth.find(solid.id) == target_depth.end())
                target_depth[solid.id] = child_depth;
        }
        for (const AnalyzedDecomposedPiece& piece : current_report.decomposed_pieces) {
            if (target_depth.find(piece.id) == target_depth.end())
                target_depth[piece.id] = child_depth;
        }
        ++result.summary.actions_applied;
        result.summary.peak_active_pieces = std::max(
            result.summary.peak_active_pieces, current.active_piece_count);
        result.summary.iterations.push_back(std::move(record));

        for (DecisionParam& param : chosen.params) {
            if (param.name == "face_id") {
                param = param_string("face_id",
                    scoped_shape_id(document->id, chosen.candidate.source_face_id));
            }
        }
        const FixedPointIterationRecord& audit = result.summary.iterations.back();
        chosen.params.push_back(param_int(
            "fixed_point_iteration", static_cast<long long>(audit.iteration)));
        chosen.params.push_back(param_string(
            "fixed_point_policy_rule", "decomposition.global_certified_progress"));
        chosen.params.push_back(param_int(
            "residual_count_before",
            static_cast<long long>(audit.before.residual_count)));
        chosen.params.push_back(param_int(
            "residual_count_after",
            static_cast<long long>(audit.after.residual_count)));
        chosen.params.push_back(param_double(
            "residual_volume_before", audit.before.residual_volume));
        chosen.params.push_back(param_double(
            "residual_volume_after", audit.after.residual_volume));
        chosen.params.push_back(param_int(
            "active_piece_count_after",
            static_cast<long long>(audit.after.active_piece_count)));
        chosen.params.push_back(param_double(
            "action_relative_volume_delta",
            audit.action_relative_volume_delta));
        chosen.params.push_back(param_double(
            "cumulative_relative_volume_delta",
            audit.cumulative_relative_volume_delta));
        log_solid_op(session, document, chosen.target_id,
                     scoped_shape_id(document->id, chosen.target_id),
                     chosen.log_op, chosen.params, true);
        if (!publish(FixedPointProgressPhase::action_accepted,
                     audit.target_id, audit.candidate_id, audit.strategy)) {
            cancellation_requested = true;
        }
    }

    // Cached local evidence is sufficient for bounded search, but never for a
    // production terminal state. Rebuild the authoritative report, overlap
    // set, and body certificates once before deciding exactness or publishing
    // output.
    ConversionReport terminal_report;
    detail::DecompositionProgressLedger terminal_ledger;
    detail::OverlapPairCache terminal_overlap_cache;
    const DecompositionProgressState incremental_state = current;
    // Terminal verification is mandatory even after cancellation: it is the
    // transaction boundary that determines whether certified partial work may
    // be committed safely.
    publish(FixedPointProgressPhase::terminal_verification_started);
    const DecompositionProgressState terminal_state = collect_progress_state(
        working, coordinator_options, &terminal_report, &document->performance,
        &terminal_ledger, &current_ledger, &current_overlap_cache,
        &terminal_overlap_cache, &current_report);
    const bool terminal_state_matches = detail::progress_states_equivalent(
        terminal_state, incremental_state, 1.0e-9, 1.0e-9);
    current = terminal_state;
    current_report = std::move(terminal_report);
    current_ledger = std::move(terminal_ledger);
    if (!terminal_state_matches) {
        stop = FixedPointStopReason::internal_error;
    } else if (current.residual_count == 0) {
        stop = FixedPointStopReason::exact;
    }
    result.summary.exact = current.residual_count == 0;
    result.summary.mutated = result.summary.actions_applied > 0;
    result.summary.stop_reason = stop;
    result.summary.final_state = current;
    result.summary.elapsed_ms = elapsed_ms(run_start);

    const bool commit = result.summary.mutated
        && (result.summary.exact || policy.commit_certified_partial_progress);
    if (commit) document->model = std::move(working);

    Diagnostic diagnostic;
    diagnostic.severity = result.summary.exact
        ? DiagnosticSeverity::info : DiagnosticSeverity::warning;
    diagnostic.code = result.summary.exact
        ? "fixed_point_decomposition_complete"
        : result.summary.mutated
            ? "fixed_point_decomposition_partial"
            : "fixed_point_no_improving_candidate";
    std::ostringstream message;
    message << "Fixed-point decomposition stopped with reason="
            << fixed_point_stop_reason_name(stop)
            << ", actions=" << result.summary.actions_applied
            << ", residuals=" << current.residual_count
            << ", active_pieces=" << current.active_piece_count
            << ", cumulative_relative_volume_delta="
            << cumulative_delta(current) << ".";
    if (!terminal_state_matches) {
        message << " Incremental/terminal state mismatch: cached(residuals="
                << incremental_state.residual_count
                << ", residual_volume=" << incremental_state.residual_volume
                << ", total_volume=" << incremental_state.total_volume
                << ", exact=" << incremental_state.exact_body_count
                << ", unsupported="
                << incremental_state.unsupported_face_count
                << ", halfspace_violations="
                << incremental_state.halfspace_violation_count
                << ") terminal(residuals=" << current.residual_count
                << ", residual_volume=" << current.residual_volume
                << ", total_volume=" << current.total_volume
                << ", exact=" << current.exact_body_count
                << ", unsupported=" << current.unsupported_face_count
                << ", halfspace_violations="
                << current.halfspace_violation_count << ").";
    }
    if (stop == FixedPointStopReason::no_improving_candidate
        && current.residual_count > 0) {
        message << " The residual geometry needs bounded shatter or human "
                   "review: candidates may exist, but none passed the global "
                   "monotonic-progress and safety gates."
                   " recommended_action=shatter_or_review.";
    }
    diagnostic.message = message.str();
    diagnostic.shape_id = document_id;
    current_report.diagnostics.push_back(diagnostic);
    if (commit) document->model.diagnostics.push_back(diagnostic);

    if (commit) {
        result.command = finish_document_mutation(
            session, *document, true, id_mode, transaction_log_start,
            &current_report);
    } else if (result.summary.mutated) {
        result.command = finish_document_mutation(
            session, *document, false, id_mode, transaction_log_start);
    } else {
        ConversionReport report = current_report;
        report.performance = document->performance;
        report.document_revision = document->revision;
        result.command.report = id_mode == ReportIdMode::scoped
            ? scoped_report(*document, report) : report;
        result.command.ok = false;
        session.last_report_value = result.command.report;
    }
    publish(FixedPointProgressPhase::finished, {}, {}, {}, stop);
    return result;
}

DecompositionProgressState decomposition_progress_state(
    Session& session,
    const DocumentId& document_id) {
    for (CadDocument& document : session.documents) {
        if (document.id == document_id) {
            return collect_progress_state(document.model, document.options,
                                          nullptr, &document.performance);
        }
    }
    return {};
}

CommandResult emit_document(Session& session,
                            const DocumentId& document_id,
                            const std::string& output_path,
                            ReportIdMode id_mode,
                            JobProgressCallback progress) {
    static std::atomic<std::uint64_t> next_emit_job{0};
    const auto run_start = Clock::now();
    const auto wall_tick = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string job_id = "emit-csg:" + std::to_string(wall_tick) + ":"
        + std::to_string(++next_emit_job);
    std::uint64_t sequence = 0;
    std::size_t base_revision = 0;
    const auto publish = [&](JobPhase phase, const std::string& code,
                             std::size_t completed, std::size_t total,
                             bool total_known, const std::string& target,
                             JobOutcome outcome,
                             const ConversionReport* report = nullptr) {
        if (!progress) return true;
        JobProgressEvent event;
        event.job_id = job_id;
        event.sequence = ++sequence;
        event.operation = JobOperation::emit_csg;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.target_id = target;
        event.completed = completed;
        event.total = total;
        event.total_known = total_known;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = report
            ? report->document_revision : base_revision;
        if (report) event.diagnostics = report->diagnostics;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    const std::vector<DecisionParam> params = {
        param_string("output_path", output_path),
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) {
            continue;
        }
        base_revision = document.revision;
        std::size_t completed = 0;
        std::size_t total = 0;
        bool total_known = false;
        const auto cancelled_result = [&](const std::string& message) {
            log_document_op(session, document_id, "emit_mcnp", params, false);
            CommandResult result = session_error(
                session, "job_cancelled", message, document_id);
            publish(JobPhase::finished, "job_cancelled", completed, total,
                    total_known, {}, JobOutcome::cancelled, &result.report);
            return result;
        };
        if (!publish(JobPhase::resolving_targets, "emit_resolving_document",
                     0, 0, false, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before document resolution.");
        }
        const auto emit_start = Clock::now();
        // Production MCNP safety is independent of approximation and of the
        // legacy analysis/intermediate `allow_overlaps` override. Keep only a
        // scale-independent numerical-volume floor; any larger positive
        // intersection, and every unknown Boolean result, blocks emission.
        ConversionOptions safety_options = document.options;
        safety_options.check_overlaps = true;
        safety_options.allow_overlaps = false;
        safety_options.overlap_volume_fraction =
            std::numeric_limits<double>::infinity();
        CadModel emission_model = document.model;
        TopoDS_Shape domain_shape;
        bool domain_ready = true;
        if (!safety_options.mcnp_domain_id.empty()) {
            std::string domain_source;
            domain_ready = active_shape_for_id(
                document.model, safety_options.mcnp_domain_id,
                &domain_shape, &domain_source)
                && remove_solid(emission_model,
                                safety_options.mcnp_domain_id);
        }
        if (!publish(JobPhase::analyzing_solids, "emit_readiness_analysis",
                     0, 0, false, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before readiness analysis.");
        }
        bool analysis_cancelled = false;
        const AnalysisProgressCallback readiness_progress =
            [&](std::size_t current, std::size_t count,
                const std::string& target) {
                completed = current;
                total = count;
                total_known = true;
                return publish(JobPhase::analyzing_solids,
                               "emit_readiness_solid_completed", current,
                               count, true, target, JobOutcome::none);
            };
        ConversionReport readiness_report = analyze_model(
            emission_model, document.options, readiness_progress,
            &analysis_cancelled);
        if (analysis_cancelled) {
            return cancelled_result(
                "CSG emission was cancelled at a readiness-analysis boundary.");
        }
        if (!publish(JobPhase::checking_overlaps,
                     "emit_readiness_checking_overlaps", completed, total,
                     total_known, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before overlap checking.");
        }
        detail::detect_solid_overlaps(emission_model,
                                      safety_options,
                                      readiness_report);
        if (!publish(JobPhase::terminal_verification,
                     "emit_validating_geometry_safety", completed, total,
                     total_known, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before geometry safety validation.");
        }
        readiness_report.mcnp_geometry_safety = validate_mcnp_geometry(
            emission_model, readiness_report, safety_options,
            domain_ready ? domain_shape : TopoDS_Shape{});
        const bool has_unsafe_membership =
            !readiness_report.mcnp_geometry_safety.safe_to_emit;
        if (has_unsafe_membership) {
            readiness_report.diagnostics.push_back(diagnostic(
                DiagnosticSeverity::error,
                "mcnp_geometry_safety_blocked",
                "Production MCNP emission requires zero positive-volume cell "
                "overlaps, no unknown result, and—when configured—certified "
                "exact coverage of the modeled domain. Approximation budgets "
                "and allow_overlaps cannot override this gate.",
                document_id));
            document.performance.emit_ms += elapsed_ms(emit_start);
            readiness_report.performance = document.performance;
            readiness_report.approximation_policy = document.options.approximation;
            readiness_report.approximation_ledger = document.approximation_ledger;
            readiness_report.document_revision = document.revision;
            document.report = id_mode == ReportIdMode::scoped
                ? scoped_report(document, readiness_report)
                : readiness_report;
            log_document_op(session, document_id, "emit_mcnp", params, false);
            session.last_report_value = document.report;
            CommandResult blocked{false, document.report};
            publish(JobPhase::finished, "emit_geometry_safety_blocked",
                    completed, total, total_known, {}, JobOutcome::failed,
                    &blocked.report);
            return blocked;
        }

        if (!publish(JobPhase::certifying_solids, "emit_converting_geometry",
                     0, 0, false, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before geometry conversion.");
        }
        ConversionResult conversion = convert_model(emission_model, document.options);
        if (!publish(JobPhase::certifying_solids, "emit_final_analysis",
                     0, 0, false, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled after conversion and before final analysis.");
        }
        analysis_cancelled = false;
        completed = 0;
        total = 0;
        total_known = false;
        const AnalysisProgressCallback final_progress =
            [&](std::size_t current, std::size_t count,
                const std::string& target) {
                completed = current;
                total = count;
                total_known = true;
                return publish(JobPhase::certifying_solids,
                               "emit_final_solid_completed", current, count,
                               true, target, JobOutcome::none);
            };
        ConversionReport final_report = analyze_model(
            emission_model, document.options, final_progress,
            &analysis_cancelled);
        if (analysis_cancelled) {
            return cancelled_result(
                "CSG emission was cancelled at a final-analysis boundary.");
        }
        if (!publish(JobPhase::checking_overlaps,
                     "emit_final_checking_overlaps", completed, total,
                     total_known, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before final overlap checking.");
        }
        detail::detect_solid_overlaps(emission_model,
                                      safety_options,
                                      final_report);
        if (!publish(JobPhase::terminal_verification,
                     "emit_final_verification", completed, total,
                     total_known, {}, JobOutcome::none)) {
            return cancelled_result(
                "CSG emission was cancelled before final verification.");
        }
        final_report.mcnp_geometry_safety = validate_mcnp_geometry(
            emission_model, final_report, safety_options, domain_shape);
        final_report.solids_converted = conversion.report.solids_converted;
        final_report.decomposition_attempts =
            conversion.report.decomposition_attempts;
        final_report.decomposition_pieces =
            conversion.report.decomposition_pieces;
        final_report.alea_handoff = conversion.report.alea_handoff;
        auto has_diag = [](const ConversionReport& report,
                           const Diagnostic& diagnostic) {
            return std::any_of(report.diagnostics.begin(),
                               report.diagnostics.end(),
                               [&](const Diagnostic& existing) {
                                   return existing.severity == diagnostic.severity
                                       && existing.code == diagnostic.code
                                       && existing.shape_id == diagnostic.shape_id
                                       && existing.message == diagnostic.message;
                               });
        };
        for (const Diagnostic& diagnostic : conversion.report.diagnostics) {
            if (!has_diag(final_report, diagnostic)) {
                final_report.diagnostics.push_back(diagnostic);
            }
        }
        if (conversion.report.ok()) {
            for (Diagnostic& diagnostic : final_report.diagnostics) {
                if (diagnostic.severity == DiagnosticSeverity::error
                    && diagnostic.code == "nonconvex_solid_requires_decomposition") {
                    diagnostic.severity = DiagnosticSeverity::warning;
                }
            }
        }
        final_report.document_revision = document.revision;
        final_report.approximation_policy = document.options.approximation;
        final_report.approximation_ledger = document.approximation_ledger;
        if (!conversion.report.ok())
            final_report.mcnp_geometry_safety.safe_to_emit = false;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, final_report)
            : final_report;
        if (conversion.report.ok() && document.report.ok()) {
            if (!publish(JobPhase::committing, "emit_writing_output",
                         completed, total, total_known, {},
                         JobOutcome::none)) {
                return cancelled_result(
                    "CSG emission was cancelled before writing the output file.");
            }
            const std::string temporary_path = output_path
                + ".csgify-tmp-"
                + std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            const int export_status = export_mcnp(conversion.system.get(),
                                                  temporary_path);
            const bool installed = export_status == 0
                && std::rename(temporary_path.c_str(), output_path.c_str()) == 0;
            if (!installed) {
                std::remove(temporary_path.c_str());
                Diagnostic diagnostic = make_error(
                    "export_failed",
                    "Alea MCNP export or atomic output installation failed.",
                    document_id);
                diagnostic.document_id = document.id;
                document.report.diagnostics.push_back(diagnostic);
            }
        }
        document.performance.emit_ms += elapsed_ms(emit_start);
        document.report.performance = document.performance;
        log_document_op(session, document_id, "emit_mcnp", params,
                        document.report.ok());
        session.last_report_value = document.report;
        CommandResult result{document.report.ok(), document.report};
        publish(JobPhase::finished,
                result.ok ? "emit_finished" : "emit_failed",
                completed, total, total_known, {},
                result.ok ? JobOutcome::committed : JobOutcome::failed,
                &result.report);
        return result;
    }
    log_document_op(session, document_id, "emit_mcnp", params, false);
    CommandResult missing = session_error(session,
                                          "document_not_found",
                                          "Requested document ID was not found.",
                                          document_id);
    publish(JobPhase::finished, "document_not_found", 0, 0, false, {},
            JobOutcome::failed, &missing.report);
    return missing;
}

CommandResult export_document_step(Session& session,
                                   const DocumentId& document_id,
                                   const std::string& output_path,
                                   ReportIdMode id_mode,
                                   JobProgressCallback progress) {
    static std::atomic<std::uint64_t> next_export_job{0};
    const auto run_start = Clock::now();
    const auto wall_tick = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string job_id = "export-step:" + std::to_string(wall_tick)
        + ":" + std::to_string(++next_export_job);
    std::uint64_t sequence = 0;
    std::size_t base_revision = 0;
    const auto publish = [&](JobPhase phase, const std::string& code,
                             std::size_t completed, std::size_t total,
                             bool total_known, const std::string& target,
                             JobOutcome outcome,
                             const ConversionReport* report = nullptr) {
        if (!progress) return true;
        JobProgressEvent event;
        event.job_id = job_id;
        event.sequence = ++sequence;
        event.operation = JobOperation::export_step;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.target_id = target;
        event.completed = completed;
        event.total = total;
        event.total_known = total_known;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = base_revision;
        event.committed_revision = report
            ? report->document_revision : base_revision;
        if (report) event.diagnostics = report->diagnostics;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    const std::vector<DecisionParam> params = {
        param_string("output_path", output_path),
    };
    for (CadDocument& document : session.documents) {
        if (document.id != document_id) continue;
        base_revision = document.revision;
        if (!publish(JobPhase::loading_geometry, "step_export_started",
                     0, 0, false, {}, JobOutcome::none)) {
            CommandResult cancelled = session_error(
                session, "job_cancelled",
                "STEP export was cancelled before geometry collection.",
                document_id);
            publish(JobPhase::finished, "job_cancelled", 0, 0, false, {},
                    JobOutcome::cancelled, &cancelled.report);
            return cancelled;
        }
        const auto start = Clock::now();
        std::size_t completed = 0;
        std::size_t total = 0;
        bool total_known = false;
        const StepExportProgressCallback export_progress =
            [&](std::size_t current, std::size_t count,
                const std::string& target) {
                completed = current;
                total = count;
                total_known = true;
                const JobPhase phase = target.empty()
                    ? JobPhase::committing : JobPhase::loading_geometry;
                const char* code = target.empty()
                    ? "step_export_writing" : "step_export_body_collected";
                return publish(phase, code, current, count, true, target,
                               JobOutcome::none);
            };
        const int rc = export_step(document.model, output_path, export_progress);
        document.performance.export_step_ms += elapsed_ms(start);
        const bool cancelled =
            rc == static_cast<int>(StepExportStatus::cancelled);
        const bool ok = rc == static_cast<int>(StepExportStatus::success);
        if (cancelled) {
            log_document_op(session, document_id, "export_step", params, false);
            CommandResult result = session_error(
                session, "job_cancelled",
                "STEP export was cancelled before writing the output file.",
                document_id);
            publish(JobPhase::finished, "job_cancelled", completed, total,
                    total_known, {}, JobOutcome::cancelled, &result.report);
            return result;
        }
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
        local.performance = document.performance;
        local.document_revision = document.revision;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local)
            : local;
        session.last_report_value = document.report;
        CommandResult result{ok, document.report};
        publish(JobPhase::finished,
                ok ? "step_export_finished" : "step_export_failed",
                completed, total, total_known, {},
                ok ? JobOutcome::committed : JobOutcome::failed,
                &result.report);
        return result;
    }
    log_document_op(session, document_id, "export_step", params, false);
    CommandResult missing = session_error(session,
                                          "document_not_found",
                                          "Requested document ID was not found.",
                                          document_id);
    publish(JobPhase::finished, "document_not_found", 0, 0, false, {},
            JobOutcome::failed, &missing.report);
    return missing;
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

const std::string& document_source_path(const Session& session,
                                         const DocumentId& document_id) {
    static const std::string kEmpty;
    for (const CadDocument& doc : session.documents) {
        if (doc.id == document_id) return doc.source_path;
    }
    return kEmpty;
}

const DecompositionProposal& last_decomposition_proposal(const Session& session) {
    return session.last_decomposition_proposal_value;
}

const HoleFillProposal& last_hole_fill_proposal(const Session& session) {
    return session.last_hole_fill_proposal_value;
}

const SurfaceReplacementProposal& last_surface_replacement_proposal(
    const Session& session) {
    return session.last_surface_replacement_proposal_value;
}

std::size_t document_count(const Session& session) {
    return session.documents.size();
}

std::vector<SolidViewEntry> collect_view_entries(const Session& session) {
    std::vector<SolidViewEntry> result;
    if (session.documents.empty()) return result;
    const CadDocument& doc = session.documents.front();
    const CadModel& model = doc.model;
    if (!model.loaded()) return result;

    // Collect local IDs of solids with interior_point_not_found errors.
    std::set<std::string> no_interior_ids;
    std::set<std::string> overlap_ids;
    for (const Diagnostic& d : doc.report.diagnostics) {
        if (d.code == "interior_point_not_found") {
            const std::string& lid = d.local_shape_id.empty() ? d.shape_id : d.local_shape_id;
            no_interior_ids.insert(lid);
        }
    }
    for (const SolidOverlap& overlap : doc.report.overlaps) {
        auto local_id = [](std::string id) {
            const std::size_t slash = id.rfind('/');
            return slash == std::string::npos ? id : id.substr(slash + 1);
        };
        overlap_ids.insert(local_id(overlap.solid_a_id));
        overlap_ids.insert(local_id(overlap.solid_b_id));
    }

    // Map local solid id → exact_csg_supported from last analysis report.
    std::unordered_map<std::string, bool> exact_csg;
    for (const AnalyzedSolid& s : doc.report.solids) {
        std::string lid = s.id;
        const std::size_t slash = lid.rfind('/');
        if (slash != std::string::npos) lid = lid.substr(slash + 1);
        exact_csg[lid] = s.exact_csg_supported;
    }

    // Original solids from root shape.
    std::size_t index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++index;
        const std::string lid = detail::solid_id(index);
        if (detail::contains_id(model.removed_solid_ids, lid)) continue;

        SolidViewEntry entry;
        entry.shape_id = scoped_shape_id(doc.id, lid);
        entry.label    = detail::solid_label(index);
        entry.shape    = exp.Current();
        entry.overlap_conflict = overlap_ids.count(lid) != 0;

        for (const ShapeMetadata& m : model.solid_metadata) {
            if (m.shape_id == lid) { entry.name = m.name; break; }
        }

        if (no_interior_ids.count(lid)) {
            entry.status = SolidViewStatus::no_interior_point;
        } else {
            auto it = exact_csg.find(lid);
            entry.status = (it != exact_csg.end() && it->second)
                ? SolidViewStatus::exact_csg
                : SolidViewStatus::needs_decomposition;
        }
        result.push_back(std::move(entry));
    }

    // Decomposed pieces.
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) continue;
        SolidViewEntry entry;
        entry.shape_id        = scoped_shape_id(doc.id, piece.id);
        entry.label           = piece.label;
        entry.shape           = piece.solid;
        entry.status          = SolidViewStatus::piece;
        entry.source_solid_id = scoped_shape_id(doc.id, piece.source_solid_id);
        entry.overlap_conflict = overlap_ids.count(piece.source_solid_id) != 0;
        result.push_back(std::move(entry));
    }

    // Generated media remain independent material cells and are shown as a
    // distinct overlay instead of being visually merged into their host wall.
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell || body.role != "contained_medium") continue;
        const TopoDS_Shape shape = detail::build_synthetic_shape(model, body);
        if (shape.IsNull()) continue;
        SolidViewEntry entry;
        entry.shape_id = scoped_shape_id(doc.id, body.id);
        entry.label = body.label;
        entry.name = "Contained medium";
        entry.shape = shape;
        entry.status = SolidViewStatus::generated_medium;
        entry.source_solid_id = scoped_shape_id(doc.id, body.host_solid_id);
        result.push_back(std::move(entry));
    }

    return result;
}

DecompositionPreview collect_decomposition_preview(
    const Session& session,
    const DecompositionCandidate& candidate,
    JobProgressCallback progress) {
    static std::atomic<std::uint64_t> next_preview_job{0};
    const auto run_start = Clock::now();
    const auto wall_tick = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string job_id = "preview:" + std::to_string(wall_tick) + ":"
        + std::to_string(++next_preview_job);
    std::uint64_t sequence = 0;
    DecompositionPreview preview;
    preview.target_id = candidate.target_id;
    preview.candidate_id = candidate.id;
    const auto publish = [&](JobPhase phase, const std::string& code,
                             std::size_t completed, std::size_t total,
                             bool total_known, JobOutcome outcome) {
        if (!progress) return true;
        JobProgressEvent event;
        event.job_id = job_id;
        event.sequence = ++sequence;
        event.operation = JobOperation::decomposition_preview;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.target_id = candidate.target_id;
        event.candidate_id = candidate.id;
        event.completed = completed;
        event.total = total;
        event.total_known = total_known;
        event.elapsed_ms = elapsed_ms(run_start);
        event.base_revision = preview.document_revision;
        event.committed_revision = preview.document_revision;
        event.diagnostics = preview.diagnostics;
        try {
            return progress(event);
        } catch (...) {
            return false;
        }
    };
    const auto cancel = [&](const std::string& message) {
        preview.ok = false;
        preview.diagnostics.push_back(make_error(
            "job_cancelled", message, candidate.target_id));
        publish(JobPhase::finished, "job_cancelled", preview.pieces.size(),
                preview.pieces.size(), false, JobOutcome::cancelled);
    };
    if (!publish(JobPhase::resolving_targets, "preview_resolving_target",
                 0, 0, false, JobOutcome::none)) {
        cancel("Decomposition preview was cancelled before target resolution.");
        return preview;
    }

    DocumentId requested_document;
    std::string local_target = candidate.target_id;
    parse_scoped_shape_id(candidate.target_id, &requested_document, &local_target);

    const CadDocument* document = nullptr;
    for (const CadDocument& item : session.documents) {
        if (requested_document.empty() || item.id == requested_document) {
            document = &item;
            if (!requested_document.empty()) break;
        }
    }
    if (!document) {
        preview.diagnostics.push_back(make_error(
            "preview_document_not_found",
            "The candidate's source document is no longer available.",
            candidate.target_id));
        publish(JobPhase::finished, "preview_document_not_found", 0, 0, false,
                JobOutcome::failed);
        return preview;
    }
    preview.document_revision = document->revision;

    if (!publish(JobPhase::loading_geometry, "preview_generating_split",
                 0, 0, false, JobOutcome::none)) {
        cancel("Decomposition preview was cancelled before candidate generation.");
        return preview;
    }

    TopoDS_Shape parent_shape;
    active_shape_for_id(document->model, local_target, &parent_shape, nullptr);

    CadModel model = document->model;
    const std::size_t before = model.decomposed_pieces.size();
    bool split_ok = false;
    if (!candidate.planes.empty()) {
        split_ok = decompose_solid_by_planes(model, local_target,
                                             candidate.planes,
                                             document->options);
    } else if (candidate.plane_valid && candidate.source_face_id.empty()) {
        split_ok = decompose_solid_by_plane(model, local_target,
                                            candidate.plane_origin,
                                            candidate.plane_normal,
                                            document->options);
    } else {
        DocumentId face_document;
        std::string local_face = candidate.source_face_id;
        parse_scoped_shape_id(candidate.source_face_id,
                              &face_document, &local_face);
        split_ok = decompose_solid_by_face(model, local_target, local_face,
                                           document->options);
    }
    if (!split_ok || model.decomposed_pieces.size() <= before) {
        preview.diagnostics.push_back(make_error(
            "preview_split_failed",
            "OCCT could not regenerate this candidate without mutating the session.",
            candidate.target_id));
        publish(JobPhase::finished, "preview_split_failed", 0, 0, false,
                JobOutcome::failed);
        return preview;
    }

    std::size_t summary_index = 0;
    std::vector<TopoDS_Solid> preview_children;
    std::size_t preview_piece_total = 0;
    for (std::size_t i = before; i < model.decomposed_pieces.size(); ++i) {
        if (model.decomposed_pieces[i].active) ++preview_piece_total;
    }
    for (std::size_t i = before; i < model.decomposed_pieces.size(); ++i) {
        const DecomposedSolidPiece& piece = model.decomposed_pieces[i];
        if (!piece.active) continue;
        const DecompositionCandidatePiece* summary =
            summary_index < candidate.pieces.size()
                ? &candidate.pieces[summary_index] : nullptr;
        ++summary_index;

        SolidViewEntry entry;
        entry.shape_id = "preview:" + candidate.id + "/piece:"
            + std::to_string(summary_index);
        entry.label = entry.shape_id;
        entry.name = "proposed piece " + std::to_string(summary_index);
        entry.shape = piece.solid;
        entry.source_solid_id = candidate.target_id;

        const bool sliver = summary
            && summary->volume_fraction <= 1.0e-8;
        const bool exact = summary && summary->exact_csg_supported;
        if (sliver) {
            entry.status = SolidViewStatus::preview_sliver;
            ++preview.sliver_count;
        } else if (exact) {
            entry.status = SolidViewStatus::preview_exact;
            ++preview.exact_count;
        } else {
            entry.status = SolidViewStatus::preview_residual;
            ++preview.residual_count;
        }
        preview.total_volume += piece.volume;
        if (!piece.solid.IsNull()) preview_children.push_back(piece.solid);
        preview.pieces.push_back(std::move(entry));
        if (!publish(JobPhase::certifying_solids,
                     "preview_piece_materialized", preview.pieces.size(),
                     preview_piece_total, true, JobOutcome::none)) {
            cancel("Decomposition preview was cancelled at a piece boundary.");
            return preview;
        }
    }
    preview.ok = !preview.pieces.empty();
    const double volume_scale = std::max(1.0, std::abs(candidate.pieces_volume));
    preview.summary_matches = preview.ok
        && preview.pieces.size() == candidate.piece_count
        && std::abs(preview.total_volume - candidate.pieces_volume)
               <= 1.0e-9 * volume_scale;

    if (!parent_shape.IsNull() && parent_shape.ShapeType() == TopAbs_SOLID
        && !preview_children.empty()) {
        preview.certificate_attempted = true;
        if (!publish(JobPhase::terminal_verification,
                     "preview_certifying_partition", preview.pieces.size(),
                     preview_piece_total, true, JobOutcome::none)) {
            cancel("Decomposition preview was cancelled before partition certification.");
            return preview;
        }
        const detail::LocalPartitionCertificate partition =
            detail::certify_local_partition(
                TopoDS::Solid(parent_shape), preview_children,
                document->options, 2048, nullptr);
        preview.partition_exact = partition.exact;
        preview.valid_children = partition.valid_children;
        preview.contained_in_parent = partition.contained_in_parent;
        preview.covers_parent = partition.covers_parent;
        preview.siblings_disjoint = partition.siblings_disjoint;
        preview.volume_within_tolerance = partition.volume_within_tolerance;
        preview.relative_volume_delta = partition.relative_volume_delta;
        preview.outside_parent_volume = partition.outside_parent_volume;
        preview.uncovered_parent_volume = partition.uncovered_parent_volume;
        preview.sibling_overlap_volume = partition.sibling_overlap_volume;
        preview.certificate_boolean_calls = partition.exact_boolean_calls;
        preview.certificate_failure_code = partition.failure_code;

        if (!publish(JobPhase::checking_overlaps,
                     "preview_checking_overlaps", preview.pieces.size(),
                     preview_piece_total, true, JobOutcome::none)) {
            cancel("Decomposition preview was cancelled before overlap certification.");
            return preview;
        }
        CadModel overlap_model = document->model;
        ConversionReport baseline = analyze_model(
            overlap_model, document->options);
        detail::detect_solid_overlaps(
            overlap_model, document->options, baseline);
        const detail::LocalOverlapCertificate overlap =
            detail::certify_local_neighbour_overlaps(
                document->model, local_target, preview_children,
                baseline.overlaps, document->options, nullptr,
                partition.contained_in_parent);
        preview.overlap_safe = overlap.safe;
        preview.overlap_unknown = overlap.unknown;
        preview.overlap_spatial_candidates = overlap.spatial_candidates;
        preview.overlap_exact_tests = overlap.exact_tests;
        preview.overlap_inherited_hits = overlap.inherited_overlap_hits;
        preview.overlap_failure_code = overlap.failure_code;
        preview.overlap_conflicting_body_id = overlap.conflicting_body_id;
        preview.overlap_intersection_volume = overlap.intersection_volume;

        if (overlap.unknown
            || partition.failure_code.find("boolean") != std::string::npos) {
            preview.recommended_next_action =
                "repair or heal the source CAD, then regenerate the certificate";
        } else if (!overlap.safe) {
            preview.recommended_next_action =
                "resolve overlap ownership or choose an alternate plane batch";
        } else if (!partition.contained_in_parent
                   || !partition.covers_parent
                   || !partition.siblings_disjoint) {
            preview.recommended_next_action =
                "choose an alternate plane batch; use targeted shatter only if exact BSP remains stuck";
        } else if (preview.sliver_count > 0) {
            preview.recommended_next_action =
                "review the sliver under the configured approximation budget";
        } else if (preview.residual_count > 0) {
            preview.recommended_next_action =
                "apply only with approval, then run targeted shatter on the residual pieces";
        } else {
            preview.recommended_next_action =
                "exact local certificate passed; this candidate may be applied";
        }
    } else {
        preview.recommended_next_action =
            "source geometry could not be certified; inspect or repair the CAD";
    }
    if (!publish(JobPhase::awaiting_review, "preview_ready_for_review",
                 preview.pieces.size(), preview_piece_total, true,
                 JobOutcome::none)) {
        cancel("Decomposition preview was cancelled before review.");
        return preview;
    }
    publish(JobPhase::finished,
            preview.ok ? "preview_finished" : "preview_failed",
            preview.pieces.size(), preview_piece_total, true,
            preview.ok ? JobOutcome::certified_partial : JobOutcome::failed);
    return preview;
}

} // namespace csgify
