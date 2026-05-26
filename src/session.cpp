// Session/workspace API. This is the future user/agent boundary: callers
// work with documents, IDs, commands, and reports. OCCT-backed CadModel state
// stays behind the Session object.

#include "cad2csg/session.hpp"

#include "decision_log.hpp"
#include "engine_internal.hpp"
#include "engine_model.hpp"
#include "hole_fill.hpp"
#include "session_impl.hpp"
#include "session_view.hpp"
#include "step_export.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

namespace cad2csg {
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
    candidate.reason = "analytic_fit";
    candidate.exact = fit.relative_max_deviation <= 1.0e-10;
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
    document.performance.load_step_ms += elapsed_ms(start);
    document.report.diagnostics = document.model.diagnostics;
    document.report = scoped_report(document, document.report);
    session.last_report_value = document.report;
    session.documents.push_back(document);
    const DocumentId new_id = session.documents.back().id;
    if (session.log) {
        // Journal the step path plus any non-default overlap-related
        // conversion options, so compaction and replay can reproduce the
        // emit gate (recipe.lua / recipe.json re-emit these onto load_step;
        // replay applies them when it reloads the STEP). Defaults are left
        // out to keep ordinary recipes as a bare s:load_step(path).
        std::vector<DecisionParam> load_params{param_string("step_path", step_path)};
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
        log_document_op(session, new_id, "load_step", load_params,
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
        const auto start = Clock::now();
        ConversionReport local_report = analyze_model(document.model, document.options);
        // Pairwise overlap detection only runs on the explicit
        // analyze() entry, NOT on the per-mutation analyze inside
        // finish_document_mutation. The O(N^2) bbox-prescreened
        // intersection passes add up quickly during BSP recursion.
        // The scoped diagnostics + report.overlaps[] are populated
        // here so the caller sees them after `s:analyze(doc)` or
        // `cad2csg inspect`.
        detail::detect_solid_overlaps(document.model, document.options,
                                       local_report);
        // Seed the known-overlap set so subsequent mutation-overlap
        // checks treat these pairs as pre-existing (not introduced by
        // the mutation). Each pair is canonicalized (lo<->hi) for
        // stable lookup.
        for (const SolidOverlap& ov : local_report.overlaps) {
            auto key = ov.solid_a_id < ov.solid_b_id
                ? std::make_pair(ov.solid_a_id, ov.solid_b_id)
                : std::make_pair(ov.solid_b_id, ov.solid_a_id);
            bool exists = false;
            for (const auto& k : document.known_overlap_pairs) {
                if (k == key) { exists = true; break; }
            }
            if (!exists) document.known_overlap_pairs.push_back(key);
        }
        document.performance.analyze_ms += elapsed_ms(start);
        local_report.performance = document.performance;
        document.report = id_mode == ReportIdMode::scoped
            ? scoped_report(document, local_report)
            : local_report;
        session.last_report_value = document.report;
        // analyze always succeeds at producing a report; result.ok means
        // "analysis ran", not "the model is clean". Callers should check
        // `report.ok()` or per-solid flags to decide what's needed next.
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

CommandResult finish_document_mutation(Session& session,
                                       CadDocument& document,
                                       bool ok,
                                       ReportIdMode id_mode = ReportIdMode::scoped) {
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
    ConversionReport local_report = analyze_model(document.model, document.options);
    // Mutation-overlap check: detect whether the most recent op
    // introduced new overlaps. Only inspects synthetic bodies and
    // decomposed pieces ADDED since the last call (high-water marked
    // on the document), so cost is bounded by the new geometry's
    // count, not the model size. Pre-existing overlaps are not
    // re-reported here -- those come from the explicit analyze pass.
    detail::detect_mutation_overlaps(document.model,
                                      document.options,
                                      local_report,
                                      &document.synthetic_body_high_water,
                                      &document.decomposed_piece_high_water,
                                      &document.known_overlap_pairs);
    document.performance.analyze_ms += elapsed_ms(start);
    local_report.performance = document.performance;
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

    const std::size_t before = solid_target.document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    const bool ok = decompose_solid_by_face(solid_target.document->model,
                                            solid_target.local_shape_id,
                                            face_target.local_shape_id,
                                            solid_target.document->options);
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
        local_report.performance = document.performance;
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

bool apply_candidate_geometry(Session& session,
                              CadDocument* document,
                              const std::string& local_id,
                              const std::string& public_id,
                              const DecompositionCandidate& candidate,
                              const std::string& log_op,
                              std::vector<DecisionParam> extra_params = {}) {
    bool ok = false;
    const std::size_t before = document->model.decomposed_pieces.size();
    const auto start = Clock::now();
    std::vector<DecisionParam> params = std::move(extra_params);
    params.push_back(param_string("tool_kind", candidate.tool_kind));
    params.push_back(param_double("score", candidate.score));
    params.push_back(param_string("confidence", candidate.confidence));

    if (!candidate.planes.empty()) {
        ok = decompose_solid_by_planes(document->model,
                                       local_id,
                                       candidate.planes,
                                       document->options);
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
        ok = decompose_solid_by_plane(document->model,
                                      local_id,
                                      candidate.plane_origin,
                                      candidate.plane_normal,
                                      document->options);
        params.push_back(param_double("origin_x", candidate.plane_origin.x));
        params.push_back(param_double("origin_y", candidate.plane_origin.y));
        params.push_back(param_double("origin_z", candidate.plane_origin.z));
        params.push_back(param_double("normal_x", candidate.plane_normal.x));
        params.push_back(param_double("normal_y", candidate.plane_normal.y));
        params.push_back(param_double("normal_z", candidate.plane_normal.z));
    } else {
        ok = decompose_solid_by_face(document->model,
                                     local_id,
                                     candidate.source_face_id,
                                     document->options);
        params.push_back(param_string("face_id",
                                      scoped_shape_id(document->id,
                                                      candidate.source_face_id)));
    }
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

    if (defer_analysis) {
        // Batched caller (decompose-apply) is responsible for running a
        // final analyze before emit/export. Skip the costly per-call
        // analyze_model pass; build a minimal CommandResult that
        // carries only the diagnostics accumulated so far so the caller
        // can still inspect outcomes.
        CommandResult result;
        result.ok = !stopped;
        result.report.diagnostics = target.document->model.diagnostics;
        if (id_mode == ReportIdMode::scoped) {
            result.report = scoped_report(*target.document, result.report);
        }
        return result;
    }

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
        const auto emit_start = Clock::now();
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
        document.performance.emit_ms += elapsed_ms(emit_start);
        document.report.performance = document.performance;
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
        const auto start = Clock::now();
        const int rc = export_step(document.model, output_path);
        document.performance.export_step_ms += elapsed_ms(start);
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
        local.performance = document.performance;
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
    for (const Diagnostic& d : doc.report.diagnostics) {
        if (d.code == "interior_point_not_found") {
            const std::string& lid = d.local_shape_id.empty() ? d.shape_id : d.local_shape_id;
            no_interior_ids.insert(lid);
        }
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
        result.push_back(std::move(entry));
    }

    return result;
}

} // namespace cad2csg
