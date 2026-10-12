#include "csgify/autonomy.hpp"

#include "json_parser.hpp"
#include "operation_registry.hpp"
#include "fnv.hpp"
#include "session_view.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace csgify {
namespace {

std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<int>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

bool id_matches(const std::string& left, const std::string& right) {
    if (left == right) return true;
    const auto local = [](const std::string& id) {
        const std::size_t slash = id.rfind('/');
        return slash == std::string::npos ? id : id.substr(slash + 1);
    };
    return local(left) == local(right);
}

MaterialPhase material_phase_from_text(const std::string& value) {
    return value == "solid" ? MaterialPhase::solid
        : value == "liquid" ? MaterialPhase::liquid
        : value == "gas" ? MaterialPhase::gas
        : value == "vacuum" ? MaterialPhase::vacuum
                             : MaterialPhase::unknown;
}

MediumPortDisposition medium_port_disposition_from_text(
    const std::string& value) {
    return value == "connect_to_port" ? MediumPortDisposition::connect_to_port
        : value == "cap_here" ? MediumPortDisposition::cap_here
        : value == "open_to_domain" ? MediumPortDisposition::open_to_domain
        : value == "interface_to_external_model"
            ? MediumPortDisposition::interface_to_external_model
            : MediumPortDisposition::unresolved;
}

MediumValveState medium_valve_state_from_text(const std::string& value) {
    return value == "open" ? MediumValveState::open
        : value == "closed" ? MediumValveState::closed
        : value == "unresolved" ? MediumValveState::unresolved
                                  : MediumValveState::not_applicable;
}

std::optional<MediumGenerationRequest> medium_request_from_json(
    const JsonValue* raw) {
    if (!raw || raw->kind != JsonKind::object) return std::nullopt;
    MediumGenerationRequest request;
    if (const JsonValue* value = raw->get("id"))
        request.id = value->as_string();
    if (const JsonValue* value = raw->get("material_id"))
        request.material_id = value->as_string();
    if (const JsonValue* value = raw->get("complement_material_id"))
        request.complement_material_id = value->as_string();
    if (const JsonValue* value = raw->get("fill_mode")) {
        const std::string mode = value->as_string();
        request.fill_mode = mode == "level" ? MediumFillMode::level
            : mode == "bounded_segment" ? MediumFillMode::bounded_segment
                                          : MediumFillMode::full;
    }
    if (const JsonValue* hosts = raw->get("host_component_ids");
        hosts && hosts->kind == JsonKind::array) {
        for (const auto& host : hosts->arr)
            if (host && host->kind == JsonKind::string)
                request.host_component_ids.push_back(host->str);
    }
    if (const JsonValue* gravity = raw->get("gravity_direction");
        gravity && gravity->kind == JsonKind::array && gravity->arr.size() == 3) {
        for (std::size_t i = 0; i < 3; ++i)
            request.gravity_direction[i] = gravity->arr[i]->as_number();
    }
    if (const JsonValue* value = raw->get("fill_level");
        value && value->kind == JsonKind::number)
        request.fill_level = value->num;
    if (const JsonValue* value = raw->get("target_volume_fraction");
        value && value->kind == JsonKind::number)
        request.target_volume_fraction = value->num;
    if (const JsonValue* components = raw->get("internal_components_to_subtract");
        components && components->kind == JsonKind::array) {
        for (const auto& component : components->arr)
            if (component && component->kind == JsonKind::string)
                request.internal_components_to_subtract.push_back(component->str);
    }
    if (const JsonValue* seeds = raw->get("seed_points_or_faces");
        seeds && seeds->kind == JsonKind::array) {
        for (const auto& item : seeds->arr) {
            if (!item || item->kind != JsonKind::object) continue;
            GeometricSeed seed;
            if (const JsonValue* kind = item->get("kind"))
                seed.kind = kind->as_string() == "face"
                    ? GeometricSeedKind::face : GeometricSeedKind::point;
            if (const JsonValue* value = item->get("entity_id"))
                seed.entity_id = value->as_string();
            if (const JsonValue* point = item->get("point");
                point && point->kind == JsonKind::array
                && point->arr.size() == 3) {
                for (std::size_t i = 0; i < 3; ++i)
                    seed.point[i] = point->arr[i]->as_number();
            }
            request.seed_points_or_faces.push_back(std::move(seed));
        }
    }
    if (const JsonValue* ports = raw->get("ports");
        ports && ports->kind == JsonKind::array) {
        for (const auto& item : ports->arr) {
            if (!item || item->kind != JsonKind::object) continue;
            MediumPortDecision port;
            if (const JsonValue* value = item->get("port_id"))
                port.port_id = value->as_string();
            if (const JsonValue* value = item->get("disposition"))
                port.disposition = medium_port_disposition_from_text(
                    value->as_string());
            if (const JsonValue* value = item->get("peer_port_id"))
                port.peer_port_id = value->as_string();
            if (const JsonValue* value = item->get("valve_state"))
                port.valve_state = medium_valve_state_from_text(
                    value->as_string());
            request.ports.push_back(std::move(port));
        }
    }
    request.require_exact_geometry = !raw->get("require_exact_geometry")
        || raw->get("require_exact_geometry")->as_bool(true);
    request.allow_disconnected_pockets = raw->get("allow_disconnected_pockets")
        && raw->get("allow_disconnected_pockets")->as_bool(false);
    if (const JsonValue* value = raw->get("max_worker_count"))
        request.max_worker_count = static_cast<std::size_t>(
            std::max(0.0, value->as_number(1.0)));
    return request;
}

void write_medium_request_json(std::ostream& out,
                               const MediumGenerationRequest& request) {
    out << "{\"id\":\"" << escape_json(request.id)
        << "\",\"material_id\":\"" << escape_json(request.material_id)
        << "\",\"complement_material_id\":\""
        << escape_json(request.complement_material_id)
        << "\",\"fill_mode\":\"" << medium_fill_mode_name(request.fill_mode)
        << "\",\"require_exact_geometry\":"
        << (request.require_exact_geometry ? "true" : "false")
        << ",\"gravity_direction\":["
        << request.gravity_direction[0] << ','
        << request.gravity_direction[1] << ','
        << request.gravity_direction[2] << ']'
        << ",\"fill_level\":";
    if (request.fill_level) out << *request.fill_level;
    else out << "null";
    out << ",\"target_volume_fraction\":";
    if (request.target_volume_fraction) out << *request.target_volume_fraction;
    else out << "null";
    out << ",\"allow_disconnected_pockets\":"
        << (request.allow_disconnected_pockets ? "true" : "false")
        << ",\"max_worker_count\":" << request.max_worker_count
        << ",\"host_component_ids\":[";
    for (std::size_t i = 0; i < request.host_component_ids.size(); ++i) {
        if (i) out << ',';
        out << '\"' << escape_json(request.host_component_ids[i]) << '\"';
    }
    out << "],\"ports\":[";
    for (std::size_t i = 0; i < request.ports.size(); ++i) {
        if (i) out << ',';
        const MediumPortDecision& port = request.ports[i];
        out << "{\"port_id\":\"" << escape_json(port.port_id)
            << "\",\"disposition\":\""
            << medium_port_disposition_name(port.disposition)
            << "\",\"peer_port_id\":\""
            << escape_json(port.peer_port_id)
            << "\",\"valve_state\":\""
            << medium_valve_state_name(port.valve_state) << "\"}";
    }
    out << "],\"internal_components_to_subtract\":[";
    for (std::size_t i = 0;
         i < request.internal_components_to_subtract.size(); ++i) {
        if (i) out << ',';
        out << '"' << escape_json(request.internal_components_to_subtract[i])
            << '"';
    }
    out << "],\"seed_points_or_faces\":[";
    for (std::size_t i = 0; i < request.seed_points_or_faces.size(); ++i) {
        if (i) out << ',';
        const GeometricSeed& seed = request.seed_points_or_faces[i];
        out << "{\"kind\":\"" << geometric_seed_kind_name(seed.kind)
            << "\",\"entity_id\":\"" << escape_json(seed.entity_id)
            << "\",\"point\":[" << seed.point[0] << ',' << seed.point[1]
            << ',' << seed.point[2] << "]}";
    }
    out << "]}";
}

void classify(AutonomyQueueItem& item,
              bool safe,
              const std::string& rule,
              const std::string& unsafe_reason,
              const AutonomyPolicy& policy) {
    item.policy_rule = rule;
    if (!safe) {
        item.disposition = AutonomyDisposition::human_review;
        item.reason = unsafe_reason;
    } else if (policy.mode == AutonomyMode::automatic) {
        item.disposition = AutonomyDisposition::auto_apply;
        item.reason = "all deterministic policy gates passed";
    } else {
        item.disposition = AutonomyDisposition::human_review;
        item.reason = policy.mode == AutonomyMode::explain
            ? "explain mode never mutates" : "propose mode requires approval";
    }
}

AutonomyQueueItem evaluate_one(Session& session,
                               const DocumentId& document_id,
                               AutonomyProposal proposal,
                               const AutonomyPolicy& policy) {
    AutonomyQueueItem item;
    item.proposal = std::move(proposal);
    item.evaluated_revision = document_revision(session, document_id);
    const OperationDescriptor* descriptor = find_operation(item.proposal.op);
    if (!descriptor || !descriptor->mutating) {
        item.disposition = AutonomyDisposition::denied;
        item.policy_rule = "operation.registered_mutation";
        item.reason = "unknown or non-mutating operation is not executable by autonomy";
        return item;
    }
    if (!descriptor->autonomy_supported || !descriptor->restart_supported) {
        item.disposition = AutonomyDisposition::denied;
        item.policy_rule = "operation.autonomy_contract_available";
        item.reason = "registered mutation is not available through the untrusted autonomy contract";
        return item;
    }
    const AutonomyOperationKind operation = descriptor->autonomy_kind;

    if (operation == AutonomyOperationKind::apply_decomposition_candidate) {
        DecompositionProposal proposal_data =
            propose_decomposition(session, item.proposal.target_id);
        const std::string wanted = item.proposal.candidate_id == "best"
            ? proposal_data.best_candidate_id : item.proposal.candidate_id;
        const DecompositionCandidate* candidate = nullptr;
        for (const DecompositionCandidate& value : proposal_data.candidates) {
            if (value.id == wanted) { candidate = &value; break; }
        }
        if (!candidate) {
            item.disposition = AutonomyDisposition::denied;
            item.policy_rule = "decomposition.candidate_exists";
            item.reason = "candidate was not found in a fresh proposal";
            return item;
        }
        item.proposal.candidate_id = candidate->id;
        item.evidence.confidence = candidate->confidence;
        item.evidence.usable = candidate->usable;
        item.evidence.exact = candidate->exact_piece_count == candidate->piece_count;
        item.evidence.relative_volume_delta = candidate->relative_delta;
        item.evidence.piece_count = candidate->piece_count;
        item.evidence.residual_count = candidate->pieces_requiring_decomposition
            + candidate->pieces_requiring_spline_replacement
            + candidate->pieces_requiring_user_decision;
        item.evidence.sliver_count = candidate->sliver_piece_count;
        const DecompositionPreview preview =
            collect_decomposition_preview(session, *candidate);
        item.evidence.certificate_passed = preview.summary_matches;
        const bool terminal_safe = policy.allow_high_confidence_decomposition
            && candidate->usable && candidate->confidence == "high"
            && item.evidence.exact && preview.summary_matches
            && item.evidence.residual_count == 0
            && candidate->sliver_piece_count == 0
            && candidate->relative_delta <= policy.max_relative_volume_delta
            && candidate->min_piece_volume_fraction
                   >= policy.min_piece_volume_fraction;
        const bool intermediate_safe =
            policy.allow_certified_intermediate_decomposition
            && candidate->usable && candidate->confidence != "low"
            && preview.summary_matches && candidate->sliver_piece_count == 0
            && candidate->relative_delta
                   <= policy.max_intermediate_relative_volume_delta
            && candidate->piece_count <= policy.max_intermediate_pieces
            && candidate->exact_piece_count > 0
            && candidate->pieces_requiring_decomposition
                   <= policy.max_intermediate_residual_pieces
            && candidate->pieces_requiring_spline_replacement == 0;
        const bool safe = terminal_safe || intermediate_safe;
        classify(item, safe,
                 intermediate_safe && !terminal_safe
                     ? "decomposition.certified_intermediate_progress"
                     : "decomposition.high_exact_certified",
                 "decomposition requires human review because one or more "
                 "confidence, exactness/progress, preview, residual, piece-count, "
                 "sliver, or volume gates failed",
                 policy);
        return item;
    }

    if (operation == AutonomyOperationKind::decompose_to_fixed_point) {
        item.proposal.target_id = document_id;
        item.evidence.confidence = "certified";
        item.evidence.usable = true;
        item.evidence.certificate_passed = true;
        classify(item, true, "decomposition.global_certified_progress", "",
                 policy);
        return item;
    }

    if (operation
        == AutonomyOperationKind::apply_surface_replacement_candidate) {
        SurfaceReplacementProposal proposal_data =
            propose_surface_replacement(session, item.proposal.target_id);
        const std::string wanted = item.proposal.candidate_id == "best"
            ? proposal_data.best_candidate_id : item.proposal.candidate_id;
        const SurfaceReplacementCandidate* candidate = nullptr;
        for (const SurfaceReplacementCandidate& value : proposal_data.candidates) {
            if (value.id == wanted) { candidate = &value; break; }
        }
        if (!candidate) {
            item.disposition = AutonomyDisposition::denied;
            item.policy_rule = "surface.candidate_exists";
            item.reason = "surface candidate was not found in a fresh proposal";
            return item;
        }
        item.proposal.candidate_id = candidate->id;
        item.evidence.confidence = candidate->confidence;
        item.evidence.usable = candidate->usable;
        item.evidence.exact = candidate->exact;
        item.evidence.relative_volume_delta =
            std::abs(candidate->owner_relative_volume_delta);
        item.evidence.max_deviation = candidate->max_deviation;
        const bool exact_safe = policy.allow_exact_surface_replacement
            && candidate->exact;
        const bool approximate_safe = policy.allow_approximate_surface_replacement
            && policy.approximation.enabled
            && policy.approximation.allow_surface_replacement
            && policy.approximation.max_surface_deviation > 0.0
            && candidate->max_deviation
                   <= policy.approximation.max_surface_deviation;
        const bool safe = candidate->usable && candidate->confidence == "high"
            && (exact_safe || approximate_safe)
            && item.evidence.relative_volume_delta
                   <= policy.max_relative_volume_delta;
        item.evidence.certificate_passed = safe;
        classify(item, safe, "surface.high_bounded_replacement",
                 "surface replacement exceeds exactness, confidence, deviation, or volume policy",
                 policy);
        return item;
    }

    if (operation == AutonomyOperationKind::remove_solid) {
        const RemovalProposal removals = suggest_removals(session, document_id);
        const RemovalCandidate* candidate = nullptr;
        for (const RemovalCandidate& value : removals.candidates) {
            if (id_matches(value.solid_id, item.proposal.target_id)) {
                candidate = &value; break;
            }
        }
        if (!candidate) {
            item.disposition = AutonomyDisposition::human_review;
            item.policy_rule = "removal.recognized_candidate";
            item.reason = "target is not a recognized removal candidate";
            return item;
        }
        item.evidence.confidence = candidate->confidence;
        item.evidence.volume_fraction = candidate->volume_fraction;
        item.evidence.usable = true;
        const bool safe = policy.allow_removal
            && candidate->confidence == "high"
            && candidate->volume_fraction <= policy.max_removal_volume_fraction;
        item.evidence.certificate_passed = safe;
        classify(item, safe, "removal.explicit_small_high_confidence",
                 "removal is disabled or exceeds confidence/volume policy",
                 policy);
        return item;
    }

    if (operation == AutonomyOperationKind::shatter_with_approximation) {
        const bool safe = policy.approximation.enabled
            && policy.approximation.allow_numerical_shatter_loss;
        classify(item, safe, "approximation.explicit_budgeted_shatter",
                 "numerical shatter loss was not explicitly enabled by the user",
                 policy);
        return item;
    }

    if (operation
        == AutonomyOperationKind::remove_residual_with_approximation) {
        const bool piece = item.proposal.target_id.find("piece:")
            != std::string::npos;
        const bool safe = piece && policy.approximation.enabled
            && policy.approximation.allow_sliver_removal
            && !policy.approximation.require_topology_preservation;
        classify(item, safe, "approximation.explicit_budgeted_residual_removal",
                 "target is not a residual piece or user policy does not permit its topology-changing removal",
                 policy);
        return item;
    }

    if (operation
        == AutonomyOperationKind::resolve_overlap_with_approximation) {
        const bool safe = policy.approximation.enabled
            && policy.approximation.allow_overlap_resolution
            && !item.proposal.candidate_id.empty();
        classify(item, safe, "approximation.budgeted_overlap_ownership",
                 "user policy does not permit overlap ownership or the "
                 "proposal omitted its owner component",
                 policy);
        return item;
    }

    if (operation == AutonomyOperationKind::apply_contained_medium) {
        item.proposal.target_id = document_id;
        if (!item.proposal.medium_request) {
            item.disposition = AutonomyDisposition::denied;
            item.policy_rule = "contained_medium.typed_request_required";
            item.reason = "apply_contained_medium requires a typed medium_request payload";
            return item;
        }
        const MediumGenerationRequest& request = *item.proposal.medium_request;
        const auto configured = std::find_if(
            policy.contained_medium_materials.begin(),
            policy.contained_medium_materials.end(),
            [&](const MaterialDefinition& material) {
                return material.id == request.material_id;
            });
        if (!policy.allow_contained_medium_generation
            || configured == policy.contained_medium_materials.end()) {
            item.disposition = AutonomyDisposition::human_review;
            item.policy_rule = "contained_medium.explicit_material_policy";
            item.reason = "contained-medium generation or its material is not authorized by immutable user policy";
            return item;
        }
        if (!request.complement_material_id.empty()) {
            const auto complement = std::find_if(
                policy.contained_medium_materials.begin(),
                policy.contained_medium_materials.end(),
                [&](const MaterialDefinition& material) {
                    return material.id == request.complement_material_id;
                });
            if (complement == policy.contained_medium_materials.end()) {
                item.disposition = AutonomyDisposition::human_review;
                item.policy_rule =
                    "contained_medium.explicit_complement_material_policy";
                item.reason = "the gas complement material is not authorized by immutable user policy";
                return item;
            }
        }
        const MediumGenerationProposal proposed = propose_contained_medium(
            session, document_id, request);
        item.evidence.confidence = proposed.ok ? "certified" : "blocked";
        item.evidence.usable = proposed.ok;
        item.evidence.exact = request.require_exact_geometry && proposed.ok;
        item.evidence.certificate_passed = proposed.ok
            && proposed.certificate.solid_valid
            && proposed.certificate.closed
            && proposed.certificate.leak_test_passed
            && proposed.certificate.overlap_test_passed
            && proposed.certificate.obstacle_subtraction_passed
            && proposed.certificate.phase_coverage_passed
            && proposed.certificate.protected_interface_passed
            && proposed.certificate.unresolved_port_count == 0;
        item.evidence.volume_fraction = proposed.certificate.generated_volume;
        classify(item, item.evidence.certificate_passed,
                 "contained_medium.configured_exact_certified",
                 "fresh network validity, closure, leak, port, overlap, obstacle, phase-coverage, or interface certification failed",
                 policy);
        return item;
    }

    item.disposition = AutonomyDisposition::human_review;
    item.policy_rule = "operation.human_only_default";
    item.reason = "registered mutation has no autonomous safety evaluator";
    return item;
}

void recount(AutonomyQueue& queue) {
    queue.auto_apply_count = queue.human_review_count = queue.denied_count = 0;
    queue.applied_count = queue.failed_count = 0;
    for (const AutonomyQueueItem& item : queue.items) {
        if (item.rejected) continue;
        if (item.requires_followup) {
            if (item.executed && item.execution_ok
                && item.evidence.mutation_applied) {
                ++queue.applied_count;
            }
            ++queue.human_review_count;
            continue;
        }
        if (item.executed) {
            if (item.execution_ok) ++queue.applied_count;
            else ++queue.failed_count;
            continue;
        }
        if (item.disposition == AutonomyDisposition::auto_apply)
            ++queue.auto_apply_count;
        else if (item.disposition == AutonomyDisposition::human_review)
            ++queue.human_review_count;
        else
            ++queue.denied_count;
    }
}

void append_approximation_policy(std::ostream& out,
                                 const ApproximationPolicy& policy) {
    out << std::setprecision(17)
        << policy.enabled << '|'
        << policy.max_global_changed_volume_fraction << '|'
        << policy.max_body_changed_volume_fraction << '|'
        << policy.max_action_changed_volume_fraction << '|'
        << policy.max_global_added_volume_fraction << '|'
        << policy.max_global_removed_volume_fraction << '|'
        << policy.max_body_added_volume_fraction << '|'
        << policy.max_body_removed_volume_fraction << '|'
        << policy.max_surface_deviation << '|'
        << policy.allow_surface_replacement << '|'
        << policy.allow_sliver_removal << '|'
        << policy.allow_bbox_replacement << '|'
        << policy.allow_numerical_shatter_loss << '|'
        << policy.allow_overlap_resolution << '|'
        << policy.require_containment_preservation << '|'
        << policy.require_topology_preservation << '|';
    std::vector<std::string> protected_targets = policy.protected_target_ids;
    std::sort(protected_targets.begin(), protected_targets.end());
    for (const std::string& id : protected_targets) out << id << ',';
    out << '|';
    std::vector<int> protected_materials = policy.protected_material_ids;
    std::sort(protected_materials.begin(), protected_materials.end());
    for (int id : protected_materials) out << id << ',';
}

std::string digest_text(const std::string& value) {
    return fnv1a128_hex(fnv1a128(value.data(), value.size()));
}

void append_digest_field(std::ostream& out, const std::string& value) {
    out << value.size() << ':' << value << '|';
}

void reject_unknown_fields(const JsonValue& value,
                           std::initializer_list<const char*> allowed,
                           const std::string& context) {
    if (value.kind != JsonKind::object)
        throw std::runtime_error(context + " must be an object");
    for (const auto& entry : value.obj) {
        bool known = false;
        for (const char* key : allowed) {
            if (entry.first == key) { known = true; break; }
        }
        if (!known)
            throw std::runtime_error(
                context + " contains unknown field '" + entry.first + "'");
    }
}

void validate_medium_request_shape(const JsonValue& request) {
    reject_unknown_fields(
        request,
        {"id", "material_id", "complement_material_id", "fill_mode",
         "host_component_ids", "gravity_direction", "fill_level",
         "target_volume_fraction", "internal_components_to_subtract",
         "seed_points_or_faces", "ports", "require_exact_geometry",
         "allow_disconnected_pockets", "max_worker_count"},
        "medium_request");
    if (const JsonValue* seeds = request.get("seed_points_or_faces")) {
        if (seeds->kind != JsonKind::array)
            throw std::runtime_error("medium_request.seed_points_or_faces must be an array");
        for (const auto& seed : seeds->arr) {
            if (!seed) throw std::runtime_error("medium request contains a null seed");
            reject_unknown_fields(*seed, {"kind", "entity_id", "point"},
                                  "medium_request seed");
        }
    }
    if (const JsonValue* ports = request.get("ports")) {
        if (ports->kind != JsonKind::array)
            throw std::runtime_error("medium_request.ports must be an array");
        for (const auto& port : ports->arr) {
            if (!port) throw std::runtime_error("medium request contains a null port");
            reject_unknown_fields(
                *port, {"port_id", "disposition", "peer_port_id", "valve_state"},
                "medium_request port");
        }
    }
}

} // namespace

const char* autonomy_mode_name(AutonomyMode mode) {
    switch (mode) {
    case AutonomyMode::explain: return "explain";
    case AutonomyMode::propose: return "propose";
    case AutonomyMode::automatic: return "auto";
    }
    return "propose";
}

const char* autonomy_disposition_name(AutonomyDisposition disposition) {
    switch (disposition) {
    case AutonomyDisposition::auto_apply: return "auto_apply";
    case AutonomyDisposition::human_review: return "human_review";
    case AutonomyDisposition::denied: return "denied";
    }
    return "denied";
}

std::string autonomy_policy_digest(const AutonomyPolicy& policy) {
    std::ostringstream value;
    value << std::setprecision(17)
          << "autonomy-policy-v1|"
          << policy.allow_high_confidence_decomposition << '|'
          << policy.allow_certified_intermediate_decomposition << '|'
          << policy.allow_exact_surface_replacement << '|'
          << policy.allow_approximate_surface_replacement << '|'
          << policy.allow_removal << '|'
          << policy.allow_contained_medium_generation << '|'
          << policy.max_relative_volume_delta << '|'
          << policy.max_intermediate_relative_volume_delta << '|'
          << policy.min_piece_volume_fraction << '|'
          << policy.max_surface_deviation << '|'
          << policy.max_removal_volume_fraction << '|'
          << policy.max_auto_actions << '|'
          << policy.max_intermediate_pieces << '|'
          << policy.max_intermediate_residual_pieces << '|';
    std::vector<std::string> materials;
    materials.reserve(policy.contained_medium_materials.size());
    for (const MaterialDefinition& material
         : policy.contained_medium_materials) {
        std::ostringstream entry;
        entry << std::setprecision(17) << material.id << ':'
              << material_phase_name(material.phase) << ':' << material.density;
        materials.push_back(entry.str());
    }
    std::sort(materials.begin(), materials.end());
    for (const std::string& material : materials) value << material << ',';
    value << '|';
    append_approximation_policy(value, policy.approximation);
    return digest_text(value.str());
}

std::string autonomy_conversion_policy_digest(
    const ConversionOptions& options) {
    std::ostringstream value;
    value << std::setprecision(17)
          << "autonomy-conversion-v1|"
          << options.linear_tolerance << '|'
          << options.angular_tolerance << '|'
          << options.small_volume_fraction << '|'
          << options.small_bbox_fraction << '|'
          << options.thin_plate_ratio << '|'
          << options.long_body_ratio << '|'
          << options.default_material_id << '|'
          << options.default_density << '|'
          << options.heal_shape << '|'
          << options.require_convex_cells << '|'
          << options.analyze_topology_metrics << '|'
          << options.analyze_face_metrics << '|'
          << options.analyze_fitted_replacements << '|'
          << options.defer_curved_halfspace_certificates << '|'
          << options.try_planar_decomposition << '|'
          << options.split_union_cells << '|'
          << options.decomposition_relative_volume_tolerance << '|'
          << options.shatter_max_depth << '|'
          << options.shatter_relative_volume_tolerance << '|'
          << options.allow_shatter_volume_drift << '|'
          << options.union_decomposed_pieces << '|'
          << options.prismatic_max_pieces << '|'
          << options.prismatic_max_splitter_calls << '|'
          << options.check_overlaps << '|'
          << options.overlap_parallelism << '|'
          << options.allow_overlaps << '|'
          << options.overlap_volume_threshold << '|'
          << options.overlap_volume_fraction << '|'
          << options.mcnp_domain_id << '|'
          << options.require_domain_coverage << '|'
          << options.unit_override << '|'
          << options.unit_scale_to_mm_override << '|'
          << options.normalize_to_unit << '|';
    std::vector<std::string> targets = options.analysis_target_ids;
    std::sort(targets.begin(), targets.end());
    for (const std::string& target : targets) value << target << ',';
    value << '|';
    append_approximation_policy(value, options.approximation);
    return digest_text(value.str());
}

std::string autonomy_request_digest(const AutonomyRequest& request) {
    std::ostringstream value;
    value << "autonomy-request-v2|";
    append_digest_field(value, request.request_id);
    append_digest_field(value, request.source_digest);
    value << request.expected_revision << '|' << request.actions.size() << '|';
    for (const AutonomyProposal& action : request.actions) {
        append_digest_field(value, action.id);
        append_digest_field(value, action.actor);
        append_digest_field(value, action.op);
        append_digest_field(value, action.target_id);
        append_digest_field(value, action.candidate_id);
        append_digest_field(value, action.rationale);
        value << action.parameters.shatter_max_depth << '|';
        if (action.medium_request) {
            std::ostringstream medium;
            write_medium_request_json(medium, *action.medium_request);
            append_digest_field(value, medium.str());
        } else {
            append_digest_field(value, {});
        }
    }
    return digest_text(value.str());
}

static AutonomyRequest parse_autonomy_request_root(
    const std::shared_ptr<JsonValue>& root) {
    if (!root || root->kind != JsonKind::object)
        throw std::runtime_error("autonomy request is not a JSON object");
    reject_unknown_fields(*root,
                          {"schema", "schema_version", "request_id",
                           "source_digest", "expected_revision", "actor",
                           "actions"},
                          "autonomy request");
    const JsonValue* schema = root->get("schema");
    if (!schema || schema->kind != JsonKind::string
        || schema->str != "csgify.autonomy-actions")
        throw std::runtime_error("unsupported or missing autonomy request schema");
    const JsonValue* version = root->get("schema_version");
    if (!version || version->kind != JsonKind::number
        || version->num != 2.0)
        throw std::runtime_error(
            "unsupported or missing autonomy request schema_version");
    AutonomyRequest request;
    const JsonValue* request_id = root->get("request_id");
    if (!request_id || request_id->kind != JsonKind::string
        || request_id->str.empty() || request_id->str.size() > 256) {
        throw std::runtime_error(
            "autonomy request requires a non-empty request_id of at most 256 bytes");
    }
    request.request_id = request_id->str;
    const JsonValue* source_digest = root->get("source_digest");
    if (!source_digest || source_digest->kind != JsonKind::string
        || source_digest->str.empty() || source_digest->str.size() > 128) {
        throw std::runtime_error(
            "autonomy request requires a non-empty source_digest of at most 128 bytes");
    }
    request.source_digest = source_digest->str;
    const JsonValue* expected_revision = root->get("expected_revision");
    if (!expected_revision || expected_revision->kind != JsonKind::number
        || !std::isfinite(expected_revision->num)
        || std::floor(expected_revision->num) != expected_revision->num
        || expected_revision->num < 0.0
        || expected_revision->num
               >= static_cast<double>(
                   std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            "autonomy request expected_revision must be a non-negative integer");
    }
    request.expected_revision =
        static_cast<std::size_t>(expected_revision->num);
    if (const JsonValue* actor = root->get("actor")) {
        if (actor->kind != JsonKind::string)
            throw std::runtime_error("autonomy request actor must be a string");
        request.actor = actor->str;
    }
    if (request.actor != "llm" && request.actor.rfind("llm:", 0) != 0)
        request.actor = "llm";
    const JsonValue* actions = root->get("actions");
    if (!actions || actions->kind != JsonKind::array)
        throw std::runtime_error("autonomy request is missing actions array");
    if (actions->arr.empty())
        throw std::runtime_error("autonomy request actions array is empty");
    std::size_t index = 0;
    std::set<std::string> proposal_ids;
    for (const auto& raw : actions->arr) {
        if (index >= 256)
            throw std::runtime_error("autonomy request exceeds 256 actions");
        ++index;
        if (!raw || raw->kind != JsonKind::object)
            throw std::runtime_error("every autonomy action must be an object");
        reject_unknown_fields(
            *raw,
            {"id", "actor", "op", "target", "candidate", "rationale",
             "parameters", "medium_request"},
            "autonomy action");
        AutonomyProposal action;
        action.actor = request.actor;
        const JsonValue* id = raw->get("id");
        if (!id || id->kind != JsonKind::string || id->str.empty())
            throw std::runtime_error("autonomy action requires a non-empty string id");
        action.id = id->str;
        if (action.id.size() > 256)
            throw std::runtime_error("autonomy action id exceeds 256 bytes");
        if (!proposal_ids.insert(action.id).second)
            throw std::runtime_error("duplicate autonomy action id: " + action.id);
        if (const JsonValue* v = raw->get("actor")) {
            if (v->kind != JsonKind::string)
                throw std::runtime_error("autonomy action actor must be a string");
            const std::string actor = v->str;
            if (actor == "llm" || actor.rfind("llm:", 0) == 0)
                action.actor = actor;
        }
        const JsonValue* op = raw->get("op");
        if (!op || op->kind != JsonKind::string || op->str.empty())
            throw std::runtime_error("autonomy action requires a non-empty string op");
        action.op = op->str;
        if (const JsonValue* v = raw->get("target")) {
            if (v->kind != JsonKind::string)
                throw std::runtime_error("autonomy action target must be a string");
            action.target_id = v->str;
        }
        if (const JsonValue* v = raw->get("candidate")) {
            if (v->kind != JsonKind::string)
                throw std::runtime_error("autonomy action candidate must be a string");
            action.candidate_id = v->str;
        }
        if (const JsonValue* v = raw->get("rationale")) {
            if (v->kind != JsonKind::string)
                throw std::runtime_error("autonomy action rationale must be a string");
            action.rationale = v->str;
        }
        bool parameters_supplied = false;
        bool max_depth_supplied = false;
        if (const JsonValue* parameters = raw->get("parameters")) {
            parameters_supplied = true;
            if (parameters->kind != JsonKind::object)
                throw std::runtime_error("action parameters must be an object");
            reject_unknown_fields(*parameters, {"max_depth"},
                                  "action parameters");
            if (const JsonValue* depth = parameters->get("max_depth")) {
                max_depth_supplied = true;
                const double raw_depth = depth->as_number();
                if (!std::isfinite(raw_depth) || std::floor(raw_depth) != raw_depth
                    || raw_depth < 1.0 || raw_depth > 64.0) {
                    throw std::runtime_error(
                        "action parameters.max_depth must be an integer from 1 to 64");
                }
                action.parameters.shatter_max_depth =
                    static_cast<int>(raw_depth);
            }
        }
        if (max_depth_supplied
            && action.op != "shatter_with_approximation") {
            throw std::runtime_error(
                "action parameters.max_depth is only valid for shatter_with_approximation");
        }
        if (const JsonValue* medium = raw->get("medium_request"))
            validate_medium_request_shape(*medium);
        action.medium_request = medium_request_from_json(raw->get("medium_request"));
        if (action.rationale.size() > 4096)
            throw std::runtime_error("autonomy action rationale exceeds 4096 bytes");

        if (const OperationDescriptor* descriptor = find_operation(action.op);
            descriptor && descriptor->autonomy_supported) {
            const AutonomyOperationKind kind = descriptor->autonomy_kind;
            const bool document_operation =
                kind == AutonomyOperationKind::apply_contained_medium
                || kind == AutonomyOperationKind::decompose_to_fixed_point;
            if (!document_operation && action.target_id.empty())
                throw std::runtime_error(
                    "autonomy operation requires a non-empty target: "
                    + action.op);
            if (document_operation && !action.target_id.empty())
                throw std::runtime_error(
                    "autonomy document operation does not accept target: "
                    + action.op);
            const bool candidate_required =
                kind == AutonomyOperationKind::apply_decomposition_candidate
                || kind
                    == AutonomyOperationKind::apply_surface_replacement_candidate
                || kind
                    == AutonomyOperationKind::resolve_overlap_with_approximation;
            if (candidate_required && action.candidate_id.empty())
                throw std::runtime_error(
                    "autonomy operation requires a non-empty candidate: "
                    + action.op);
            if (!candidate_required && !action.candidate_id.empty())
                throw std::runtime_error(
                    "autonomy operation does not accept candidate: "
                    + action.op);
            if (parameters_supplied
                && kind != AutonomyOperationKind::shatter_with_approximation) {
                throw std::runtime_error(
                    "parameters are only valid for shatter_with_approximation");
            }
            if (kind == AutonomyOperationKind::apply_contained_medium
                && !action.medium_request) {
                throw std::runtime_error(
                    "apply_contained_medium requires medium_request");
            }
            if (kind != AutonomyOperationKind::apply_contained_medium
                && action.medium_request) {
                throw std::runtime_error(
                    "medium_request is only valid for apply_contained_medium");
            }
        }
        request.actions.push_back(std::move(action));
    }
    return request;
}

AutonomyRequest read_autonomy_request_json(const std::string& path) {
    return parse_autonomy_request_root(load_json_file(path));
}

AutonomyRequest parse_autonomy_request_json(const std::string& json) {
    return parse_autonomy_request_root(JsonParser(json).parse());
}

AutonomyProjectPolicy read_autonomy_project_policy_json(
    const std::string& path) {
    const std::shared_ptr<JsonValue> root = load_json_file(path);
    if (!root || root->kind != JsonKind::object)
        throw std::runtime_error("project policy is not a JSON object");
    reject_unknown_fields(
        *root,
        {"schema", "schema_version", "project_id", "intended_analysis",
         "preparation", "authority"},
        "project policy");
    const JsonValue* schema = root->get("schema");
    const JsonValue* version = root->get("schema_version");
    if (!schema || schema->kind != JsonKind::string
        || schema->str != "csgify.project-policy")
        throw std::runtime_error(
            "file is not a csgify.project-policy object");
    if (!version || version->kind != JsonKind::number
        || version->num != 1.0)
        throw std::runtime_error(
            "unsupported project policy schema_version");

    const auto required_string = [](const JsonValue& object,
                                    const char* key,
                                    const char* context) -> std::string {
        const JsonValue* value = object.get(key);
        if (!value || value->kind != JsonKind::string || value->str.empty())
            throw std::runtime_error(std::string(context) + "." + key
                                     + " must be a non-empty string");
        return value->str;
    };
    const auto optional_bool = [](const JsonValue& object, const char* key,
                                  bool fallback, const char* context) {
        const JsonValue* value = object.get(key);
        if (!value) return fallback;
        if (value->kind != JsonKind::boolean)
            throw std::runtime_error(std::string(context) + "." + key
                                     + " must be boolean");
        return value->b;
    };
    const auto optional_number = [](const JsonValue& object, const char* key,
                                    double fallback, const char* context) {
        const JsonValue* value = object.get(key);
        if (!value) return fallback;
        if (value->kind != JsonKind::number || !std::isfinite(value->num)
            || value->num < 0.0)
            throw std::runtime_error(std::string(context) + "." + key
                                     + " must be a finite non-negative number");
        return value->num;
    };
    const auto optional_size = [&](const JsonValue& object, const char* key,
                                   std::size_t fallback,
                                   const char* context) {
        const double value = optional_number(
            object, key, static_cast<double>(fallback), context);
        if (std::floor(value) != value
            || value >= static_cast<double>(
                std::numeric_limits<std::size_t>::max()))
            throw std::runtime_error(std::string(context) + "." + key
                                     + " must be a non-negative integer");
        return static_cast<std::size_t>(value);
    };

    AutonomyProjectPolicy result;
    result.project_id = required_string(*root, "project_id", "project policy");
    result.intended_analysis = required_string(
        *root, "intended_analysis", "project policy");

    const JsonValue* preparation = root->get("preparation");
    if (!preparation || preparation->kind != JsonKind::object)
        throw std::runtime_error("project policy.preparation must be an object");
    reject_unknown_fields(
        *preparation,
        {"heal_shape", "unit_override", "unit_scale_to_mm_override",
         "normalize_to_unit"},
        "project policy.preparation");
    result.preparation.heal_shape = optional_bool(
        *preparation, "heal_shape", true, "project policy.preparation");
    if (const JsonValue* value = preparation->get("unit_override")) {
        if (value->kind != JsonKind::string)
            throw std::runtime_error(
                "project policy.preparation.unit_override must be a string");
        result.preparation.unit_override = value->str;
    }
    result.preparation.unit_scale_to_mm_override = optional_number(
        *preparation, "unit_scale_to_mm_override", 0.0,
        "project policy.preparation");
    if (const JsonValue* value = preparation->get("normalize_to_unit")) {
        if (value->kind != JsonKind::string)
            throw std::runtime_error(
                "project policy.preparation.normalize_to_unit must be a string");
        result.preparation.normalize_to_unit = value->str;
    }

    const JsonValue* authority = root->get("authority");
    if (!authority || authority->kind != JsonKind::object)
        throw std::runtime_error("project policy.authority must be an object");
    reject_unknown_fields(
        *authority,
        {"mode", "allow_high_confidence_decomposition",
         "allow_certified_intermediate_decomposition",
         "allow_exact_surface_replacement",
         "allow_approximate_surface_replacement", "allow_removal",
         "allow_contained_medium_generation", "contained_medium_materials",
         "max_relative_volume_delta",
         "max_intermediate_relative_volume_delta", "min_piece_volume_fraction",
         "max_surface_deviation", "max_removal_volume_fraction",
         "max_auto_actions", "max_intermediate_pieces",
         "max_intermediate_residual_pieces", "approximation"},
        "project policy.authority");
    const std::string mode = required_string(
        *authority, "mode", "project policy.authority");
    if (mode == "explain") result.authority.mode = AutonomyMode::explain;
    else if (mode == "propose") result.authority.mode = AutonomyMode::propose;
    else if (mode == "auto") result.authority.mode = AutonomyMode::automatic;
    else throw std::runtime_error(
        "project policy.authority.mode must be explain, propose, or auto");
    AutonomyPolicy& policy = result.authority;
    policy.allow_high_confidence_decomposition = optional_bool(
        *authority, "allow_high_confidence_decomposition",
        policy.allow_high_confidence_decomposition, "project policy.authority");
    policy.allow_certified_intermediate_decomposition = optional_bool(
        *authority, "allow_certified_intermediate_decomposition",
        policy.allow_certified_intermediate_decomposition,
        "project policy.authority");
    policy.allow_exact_surface_replacement = optional_bool(
        *authority, "allow_exact_surface_replacement",
        policy.allow_exact_surface_replacement, "project policy.authority");
    policy.allow_approximate_surface_replacement = optional_bool(
        *authority, "allow_approximate_surface_replacement",
        policy.allow_approximate_surface_replacement,
        "project policy.authority");
    policy.allow_removal = optional_bool(
        *authority, "allow_removal", policy.allow_removal,
        "project policy.authority");
    policy.allow_contained_medium_generation = optional_bool(
        *authority, "allow_contained_medium_generation",
        policy.allow_contained_medium_generation, "project policy.authority");
    policy.max_relative_volume_delta = optional_number(
        *authority, "max_relative_volume_delta",
        policy.max_relative_volume_delta, "project policy.authority");
    policy.max_intermediate_relative_volume_delta = optional_number(
        *authority, "max_intermediate_relative_volume_delta",
        policy.max_intermediate_relative_volume_delta,
        "project policy.authority");
    policy.min_piece_volume_fraction = optional_number(
        *authority, "min_piece_volume_fraction",
        policy.min_piece_volume_fraction, "project policy.authority");
    policy.max_surface_deviation = optional_number(
        *authority, "max_surface_deviation", policy.max_surface_deviation,
        "project policy.authority");
    policy.max_removal_volume_fraction = optional_number(
        *authority, "max_removal_volume_fraction",
        policy.max_removal_volume_fraction, "project policy.authority");
    policy.max_auto_actions = optional_size(
        *authority, "max_auto_actions", policy.max_auto_actions,
        "project policy.authority");
    policy.max_intermediate_pieces = optional_size(
        *authority, "max_intermediate_pieces", policy.max_intermediate_pieces,
        "project policy.authority");
    policy.max_intermediate_residual_pieces = optional_size(
        *authority, "max_intermediate_residual_pieces",
        policy.max_intermediate_residual_pieces, "project policy.authority");

    if (const JsonValue* materials = authority->get(
            "contained_medium_materials")) {
        if (materials->kind != JsonKind::array)
            throw std::runtime_error(
                "project policy.authority.contained_medium_materials must be an array");
        std::set<std::string> material_ids;
        for (const auto& item : materials->arr) {
            if (!item || item->kind != JsonKind::object)
                throw std::runtime_error(
                    "contained medium material must be an object");
            reject_unknown_fields(*item, {"id", "phase", "density"},
                                  "contained medium material");
            MaterialDefinition material;
            material.id = required_string(*item, "id", "contained medium material");
            const std::string phase = required_string(
                *item, "phase", "contained medium material");
            material.phase = material_phase_from_text(phase);
            if (material.phase != MaterialPhase::liquid
                && material.phase != MaterialPhase::gas)
                throw std::runtime_error(
                    "contained medium material.phase must be liquid or gas");
            const JsonValue* density = item->get("density");
            if (!density || density->kind != JsonKind::number
                || !std::isfinite(density->num) || density->num <= 0.0)
                throw std::runtime_error(
                    "contained medium material.density must be finite and positive");
            material.density = density->num;
            if (!material_ids.insert(material.id).second)
                throw std::runtime_error(
                    "contained medium material ids must be unique");
            policy.contained_medium_materials.push_back(std::move(material));
        }
    }

    if (const JsonValue* approximation = authority->get("approximation")) {
        if (approximation->kind != JsonKind::object)
            throw std::runtime_error(
                "project policy.authority.approximation must be an object");
        reject_unknown_fields(
            *approximation,
            {"enabled", "global_limit", "body_limit", "action_limit",
             "global_added_limit", "global_removed_limit",
             "body_added_limit", "body_removed_limit",
             "surface_deviation_limit", "allow_surface_replacement",
             "allow_sliver_removal", "allow_bbox_replacement",
             "allow_numerical_shatter_loss", "allow_overlap_resolution",
             "require_containment_preservation",
             "require_topology_preservation", "protected_target_ids",
             "protected_material_ids"},
            "project policy.authority.approximation");
        ApproximationPolicy& a = policy.approximation;
        const char* context = "project policy.authority.approximation";
        a.enabled = optional_bool(*approximation, "enabled", a.enabled, context);
        a.max_global_changed_volume_fraction = optional_number(
            *approximation, "global_limit",
            a.max_global_changed_volume_fraction, context);
        a.max_body_changed_volume_fraction = optional_number(
            *approximation, "body_limit", a.max_body_changed_volume_fraction,
            context);
        a.max_action_changed_volume_fraction = optional_number(
            *approximation, "action_limit",
            a.max_action_changed_volume_fraction, context);
        a.max_global_added_volume_fraction = optional_number(
            *approximation, "global_added_limit",
            a.max_global_added_volume_fraction, context);
        a.max_global_removed_volume_fraction = optional_number(
            *approximation, "global_removed_limit",
            a.max_global_removed_volume_fraction, context);
        a.max_body_added_volume_fraction = optional_number(
            *approximation, "body_added_limit",
            a.max_body_added_volume_fraction, context);
        a.max_body_removed_volume_fraction = optional_number(
            *approximation, "body_removed_limit",
            a.max_body_removed_volume_fraction, context);
        a.max_surface_deviation = optional_number(
            *approximation, "surface_deviation_limit",
            a.max_surface_deviation, context);
        a.allow_surface_replacement = optional_bool(
            *approximation, "allow_surface_replacement",
            a.allow_surface_replacement, context);
        a.allow_sliver_removal = optional_bool(
            *approximation, "allow_sliver_removal", a.allow_sliver_removal,
            context);
        a.allow_bbox_replacement = optional_bool(
            *approximation, "allow_bbox_replacement", a.allow_bbox_replacement,
            context);
        a.allow_numerical_shatter_loss = optional_bool(
            *approximation, "allow_numerical_shatter_loss",
            a.allow_numerical_shatter_loss, context);
        a.allow_overlap_resolution = optional_bool(
            *approximation, "allow_overlap_resolution",
            a.allow_overlap_resolution, context);
        a.require_containment_preservation = optional_bool(
            *approximation, "require_containment_preservation",
            a.require_containment_preservation, context);
        a.require_topology_preservation = optional_bool(
            *approximation, "require_topology_preservation",
            a.require_topology_preservation, context);
        if (const JsonValue* targets = approximation->get("protected_target_ids")) {
            if (targets->kind != JsonKind::array)
                throw std::runtime_error(
                    "project policy protected_target_ids must be an array");
            for (const auto& target : targets->arr) {
                if (!target || target->kind != JsonKind::string
                    || target->str.empty())
                    throw std::runtime_error(
                        "project policy protected target must be a non-empty string");
                a.protected_target_ids.push_back(target->str);
            }
        }
        if (const JsonValue* materials = approximation->get(
                "protected_material_ids")) {
            if (materials->kind != JsonKind::array)
                throw std::runtime_error(
                    "project policy protected_material_ids must be an array");
            for (const auto& material : materials->arr) {
                if (!material || material->kind != JsonKind::number
                    || !std::isfinite(material->num)
                    || std::floor(material->num) != material->num
                    || material->num < static_cast<double>(
                        std::numeric_limits<int>::min())
                    || material->num > static_cast<double>(
                        std::numeric_limits<int>::max()))
                    throw std::runtime_error(
                        "project policy protected material must be an integer");
                a.protected_material_ids.push_back(
                    static_cast<int>(material->num));
            }
        }
    }
    return result;
}

int write_autonomy_project_policy_json(
    const std::string& path,
    const AutonomyProjectPolicy& project) {
    if (project.project_id.empty() || project.intended_analysis.empty())
        return -1;
    const AutonomyPolicy& policy = project.authority;
    const ApproximationPolicy& approximation = policy.approximation;
    const auto valid_nonnegative = [](double value) {
        return std::isfinite(value) && value >= 0.0;
    };
    const double authority_numbers[] = {
        policy.max_relative_volume_delta,
        policy.max_intermediate_relative_volume_delta,
        policy.min_piece_volume_fraction,
        policy.max_surface_deviation,
        policy.max_removal_volume_fraction,
        project.preparation.unit_scale_to_mm_override,
        approximation.max_global_changed_volume_fraction,
        approximation.max_body_changed_volume_fraction,
        approximation.max_action_changed_volume_fraction,
        approximation.max_global_added_volume_fraction,
        approximation.max_global_removed_volume_fraction,
        approximation.max_body_added_volume_fraction,
        approximation.max_body_removed_volume_fraction,
        approximation.max_surface_deviation,
    };
    for (double value : authority_numbers)
        if (!valid_nonnegative(value)) return -1;
    std::set<std::string> material_ids;
    for (const MaterialDefinition& material
         : policy.contained_medium_materials) {
        if (material.id.empty() || !std::isfinite(material.density)
            || material.density <= 0.0
            || (material.phase != MaterialPhase::liquid
                && material.phase != MaterialPhase::gas)
            || !material_ids.insert(material.id).second)
            return -1;
    }
    for (const std::string& target : approximation.protected_target_ids)
        if (target.empty()) return -1;
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    const auto mode = policy.mode == AutonomyMode::explain ? "explain"
        : policy.mode == AutonomyMode::automatic ? "auto" : "propose";
    const std::filesystem::path destination(path);
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return -1;
    out << std::setprecision(17);
    out << "{\n"
        << "  \"schema\": \"csgify.project-policy\",\n"
        << "  \"schema_version\": 1,\n"
        << "  \"project_id\": \"" << escape_json(project.project_id) << "\",\n"
        << "  \"intended_analysis\": \""
        << escape_json(project.intended_analysis) << "\",\n"
        << "  \"preparation\": {\n"
        << "    \"heal_shape\": " << boolean(project.preparation.heal_shape) << ",\n"
        << "    \"unit_override\": \""
        << escape_json(project.preparation.unit_override) << "\",\n"
        << "    \"unit_scale_to_mm_override\": "
        << project.preparation.unit_scale_to_mm_override << ",\n"
        << "    \"normalize_to_unit\": \""
        << escape_json(project.preparation.normalize_to_unit) << "\"\n"
        << "  },\n"
        << "  \"authority\": {\n"
        << "    \"mode\": \"" << mode << "\",\n"
        << "    \"allow_high_confidence_decomposition\": " << boolean(policy.allow_high_confidence_decomposition) << ",\n"
        << "    \"allow_certified_intermediate_decomposition\": " << boolean(policy.allow_certified_intermediate_decomposition) << ",\n"
        << "    \"allow_exact_surface_replacement\": " << boolean(policy.allow_exact_surface_replacement) << ",\n"
        << "    \"allow_approximate_surface_replacement\": " << boolean(policy.allow_approximate_surface_replacement) << ",\n"
        << "    \"allow_removal\": " << boolean(policy.allow_removal) << ",\n"
        << "    \"allow_contained_medium_generation\": " << boolean(policy.allow_contained_medium_generation) << ",\n"
        << "    \"contained_medium_materials\": [";
    for (std::size_t i = 0; i < policy.contained_medium_materials.size(); ++i) {
        const MaterialDefinition& material = policy.contained_medium_materials[i];
        if (i) out << ',';
        out << "\n      {\"id\":\"" << escape_json(material.id)
            << "\",\"phase\":\"" << material_phase_name(material.phase)
            << "\",\"density\":" << material.density << '}';
    }
    if (!policy.contained_medium_materials.empty()) out << '\n' << "    ";
    out << "],\n"
        << "    \"max_relative_volume_delta\": " << policy.max_relative_volume_delta << ",\n"
        << "    \"max_intermediate_relative_volume_delta\": " << policy.max_intermediate_relative_volume_delta << ",\n"
        << "    \"min_piece_volume_fraction\": " << policy.min_piece_volume_fraction << ",\n"
        << "    \"max_surface_deviation\": " << policy.max_surface_deviation << ",\n"
        << "    \"max_removal_volume_fraction\": " << policy.max_removal_volume_fraction << ",\n"
        << "    \"max_auto_actions\": " << policy.max_auto_actions << ",\n"
        << "    \"max_intermediate_pieces\": " << policy.max_intermediate_pieces << ",\n"
        << "    \"max_intermediate_residual_pieces\": " << policy.max_intermediate_residual_pieces << ",\n"
        << "    \"approximation\": {\n"
        << "      \"enabled\": " << boolean(approximation.enabled) << ",\n"
        << "      \"global_limit\": " << approximation.max_global_changed_volume_fraction << ",\n"
        << "      \"body_limit\": " << approximation.max_body_changed_volume_fraction << ",\n"
        << "      \"action_limit\": " << approximation.max_action_changed_volume_fraction << ",\n"
        << "      \"global_added_limit\": " << approximation.max_global_added_volume_fraction << ",\n"
        << "      \"global_removed_limit\": " << approximation.max_global_removed_volume_fraction << ",\n"
        << "      \"body_added_limit\": " << approximation.max_body_added_volume_fraction << ",\n"
        << "      \"body_removed_limit\": " << approximation.max_body_removed_volume_fraction << ",\n"
        << "      \"surface_deviation_limit\": " << approximation.max_surface_deviation << ",\n"
        << "      \"allow_surface_replacement\": " << boolean(approximation.allow_surface_replacement) << ",\n"
        << "      \"allow_sliver_removal\": " << boolean(approximation.allow_sliver_removal) << ",\n"
        << "      \"allow_bbox_replacement\": " << boolean(approximation.allow_bbox_replacement) << ",\n"
        << "      \"allow_numerical_shatter_loss\": " << boolean(approximation.allow_numerical_shatter_loss) << ",\n"
        << "      \"allow_overlap_resolution\": " << boolean(approximation.allow_overlap_resolution) << ",\n"
        << "      \"require_containment_preservation\": " << boolean(approximation.require_containment_preservation) << ",\n"
        << "      \"require_topology_preservation\": " << boolean(approximation.require_topology_preservation) << ",\n"
        << "      \"protected_target_ids\": [";
    for (std::size_t i = 0; i < approximation.protected_target_ids.size(); ++i) {
        if (i) out << ',';
        out << '"' << escape_json(approximation.protected_target_ids[i]) << '"';
    }
    out << "],\n"
        << "      \"protected_material_ids\": [";
    for (std::size_t i = 0; i < approximation.protected_material_ids.size(); ++i) {
        if (i) out << ',';
        out << approximation.protected_material_ids[i];
    }
    out << "]\n"
        << "    }\n"
        << "  }\n"
        << "}\n";
    out.flush();
    if (!out) {
        out.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return -1;
    }
    out.close();
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return -1;
    }
    return 0;
}

int write_default_autonomy_project_policy_json(
    const std::string& path,
    const std::string& project_id,
    const std::string& intended_analysis) {
    AutonomyProjectPolicy policy;
    policy.project_id = project_id;
    policy.intended_analysis = intended_analysis;
    return write_autonomy_project_policy_json(path, policy);
}

AutonomyQueue read_autonomy_queue_json(const std::string& path) {
    const std::shared_ptr<JsonValue> root = load_json_file(path);
    if (!root || root->kind != JsonKind::object
        || !root->get("schema")
        || root->get("schema")->as_string() != "csgify.autonomy-queue") {
        throw std::runtime_error("file is not a csgify.autonomy-queue object");
    }
    const JsonValue* schema_version = root->get("schema_version");
    if (!schema_version || schema_version->kind != JsonKind::number
        || schema_version->num != 4.0) {
        throw std::runtime_error(
            "unsupported autonomy queue schema_version; fresh evaluation is required");
    }
    AutonomyQueue queue;
    const JsonValue* request_id = root->get("request_id");
    const JsonValue* request_digest = root->get("request_digest");
    const JsonValue* expected_revision = root->get("expected_revision");
    if (!request_id || request_id->kind != JsonKind::string
        || request_id->str.empty() || !request_digest
        || request_digest->kind != JsonKind::string
        || request_digest->str.empty()) {
        throw std::runtime_error(
            "autonomy queue is missing its request identity");
    }
    if (!expected_revision || expected_revision->kind != JsonKind::number
        || !std::isfinite(expected_revision->num)
        || std::floor(expected_revision->num) != expected_revision->num
        || expected_revision->num < 0.0
        || expected_revision->num
               >= static_cast<double>(
                   std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            "autonomy queue expected_revision is invalid");
    }
    queue.request_id = request_id->str;
    queue.request_digest = request_digest->str;
    queue.expected_revision =
        static_cast<std::size_t>(expected_revision->num);
    if (const JsonValue* v = root->get("document_id")) queue.document_id = v->as_string();
    if (const JsonValue* v = root->get("source_path")) queue.source_path = v->as_string();
    if (const JsonValue* v = root->get("source_digest"))
        queue.source_digest = v->as_string();
    if (const JsonValue* v = root->get("policy_digest"))
        queue.policy_digest = v->as_string();
    if (const JsonValue* v = root->get("conversion_policy_digest"))
        queue.conversion_policy_digest = v->as_string();
    if (const JsonValue* preparation = root->get("preparation")) {
        if (const JsonValue* v = preparation->get("heal_shape"))
            queue.preparation.heal_shape = v->as_bool(true);
        if (const JsonValue* v = preparation->get("unit_override"))
            queue.preparation.unit_override = v->as_string();
        if (const JsonValue* v = preparation->get("unit_scale_to_mm_override"))
            queue.preparation.unit_scale_to_mm_override = v->as_number();
        if (const JsonValue* v = preparation->get("normalize_to_unit"))
            queue.preparation.normalize_to_unit = v->as_string();
    }
    if (const JsonValue* v = root->get("coverage_certificate_path"))
        queue.coverage_certificate_path = v->as_string();
    if (const JsonValue* v = root->get("coverage_certificate_digest"))
        queue.coverage_certificate_digest = v->as_string();
    const std::string mode = root->get("mode") ? root->get("mode")->as_string() : "propose";
    queue.mode = mode == "auto" ? AutonomyMode::automatic
        : mode == "explain" ? AutonomyMode::explain : AutonomyMode::propose;
    queue.policy.mode = queue.mode;
    if (const JsonValue* p = root->get("policy")) {
        if (const JsonValue* v = p->get("allow_high_confidence_decomposition"))
            queue.policy.allow_high_confidence_decomposition = v->as_bool();
        if (const JsonValue* v = p->get("allow_exact_surface_replacement"))
            queue.policy.allow_exact_surface_replacement = v->as_bool();
        if (const JsonValue* v = p->get("max_relative_volume_delta"))
            queue.policy.max_relative_volume_delta = v->as_number();
        if (const JsonValue* v = p->get("allow_certified_intermediate_decomposition"))
            queue.policy.allow_certified_intermediate_decomposition = v->as_bool();
        if (const JsonValue* v = p->get("max_intermediate_relative_volume_delta"))
            queue.policy.max_intermediate_relative_volume_delta = v->as_number();
        if (const JsonValue* v = p->get("min_piece_volume_fraction"))
            queue.policy.min_piece_volume_fraction = v->as_number();
        if (const JsonValue* v = p->get("allow_removal"))
            queue.policy.allow_removal = v->as_bool();
        if (const JsonValue* v = p->get("allow_contained_medium_generation"))
            queue.policy.allow_contained_medium_generation = v->as_bool();
        if (const JsonValue* materials = p->get("contained_medium_materials");
            materials && materials->kind == JsonKind::array) {
            for (const auto& raw_material : materials->arr) {
                if (!raw_material || raw_material->kind != JsonKind::object)
                    continue;
                MaterialDefinition material;
                if (const JsonValue* v = raw_material->get("id"))
                    material.id = v->as_string();
                if (const JsonValue* v = raw_material->get("phase"))
                    material.phase = material_phase_from_text(v->as_string());
                if (const JsonValue* v = raw_material->get("density"))
                    material.density = v->as_number();
                queue.policy.contained_medium_materials.push_back(
                    std::move(material));
            }
        }
        if (const JsonValue* v = p->get("max_removal_volume_fraction"))
            queue.policy.max_removal_volume_fraction = v->as_number();
        if (const JsonValue* v = p->get("allow_approximate_surface"))
            queue.policy.allow_approximate_surface_replacement = v->as_bool();
        if (const JsonValue* v = p->get("max_surface_deviation"))
            queue.policy.max_surface_deviation = v->as_number();
        if (const JsonValue* v = p->get("max_auto_actions"))
            queue.policy.max_auto_actions = static_cast<std::size_t>(v->as_number(32));
        if (const JsonValue* v = p->get("max_intermediate_pieces"))
            queue.policy.max_intermediate_pieces =
                static_cast<std::size_t>(v->as_number(64));
        if (const JsonValue* v = p->get("max_intermediate_residual_pieces"))
            queue.policy.max_intermediate_residual_pieces =
                static_cast<std::size_t>(v->as_number(4));
        if (const JsonValue* a = p->get("approximation")) {
            ApproximationPolicy& q = queue.policy.approximation;
            q.enabled = a->get("enabled")
                ? a->get("enabled")->as_bool(false) : false;
            auto number = [&](const char* key, double fallback) {
                const JsonValue* v = a->get(key);
                return v ? v->as_number(fallback) : fallback;
            };
            auto boolean = [&](const char* key, bool fallback) {
                const JsonValue* v = a->get(key);
                return v ? v->as_bool(fallback) : fallback;
            };
            q.max_global_changed_volume_fraction = number("global_limit", 0.0);
            q.max_body_changed_volume_fraction = number("body_limit", q.max_global_changed_volume_fraction);
            q.max_action_changed_volume_fraction = number("action_limit", q.max_body_changed_volume_fraction);
            q.max_global_added_volume_fraction = number("global_added_limit", q.max_global_changed_volume_fraction);
            q.max_global_removed_volume_fraction = number("global_removed_limit", q.max_global_changed_volume_fraction);
            q.max_body_added_volume_fraction = number("body_added_limit", q.max_body_changed_volume_fraction);
            q.max_body_removed_volume_fraction = number("body_removed_limit", q.max_body_changed_volume_fraction);
            q.max_surface_deviation = number("surface_deviation_limit", 0.0);
            q.allow_surface_replacement = boolean("allow_surface_replacement", false);
            q.allow_sliver_removal = boolean("allow_sliver_removal", false);
            q.allow_bbox_replacement = boolean("allow_bbox_replacement", false);
            q.allow_numerical_shatter_loss = boolean("allow_numerical_shatter_loss", false);
            q.allow_overlap_resolution = boolean("allow_overlap_resolution", false);
            q.require_containment_preservation = boolean("require_containment_preservation", true);
            q.require_topology_preservation = boolean("require_topology_preservation", true);
            if (const JsonValue* targets = a->get("protected_target_ids");
                targets && targets->kind == JsonKind::array) {
                for (const auto& target : targets->arr)
                    if (target && target->kind == JsonKind::string)
                        q.protected_target_ids.push_back(target->str);
            }
            if (const JsonValue* materials = a->get("protected_material_ids");
                materials && materials->kind == JsonKind::array) {
                for (const auto& material : materials->arr)
                    if (material && material->kind == JsonKind::number)
                        q.protected_material_ids.push_back(
                            static_cast<int>(material->num));
            }
        }
    }
    const JsonValue* items = root->get("items");
    if (!items || items->kind != JsonKind::array)
        throw std::runtime_error("autonomy queue is missing items array");
    for (const auto& raw : items->arr) {
        if (!raw || raw->kind != JsonKind::object) continue;
        AutonomyQueueItem item;
        if (const JsonValue* v = raw->get("id")) item.proposal.id = v->as_string();
        if (const JsonValue* v = raw->get("actor")) item.proposal.actor = v->as_string();
        if (const JsonValue* v = raw->get("op")) item.proposal.op = v->as_string();
        if (const JsonValue* v = raw->get("target")) item.proposal.target_id = v->as_string();
        if (const JsonValue* v = raw->get("candidate")) item.proposal.candidate_id = v->as_string();
        if (const JsonValue* v = raw->get("rationale")) item.proposal.rationale = v->as_string();
        if (const JsonValue* parameters = raw->get("effective_parameters")) {
            if (const JsonValue* depth = parameters->get("max_depth")) {
                const double raw_depth = depth->as_number(8.0);
                if (!std::isfinite(raw_depth) || std::floor(raw_depth) != raw_depth
                    || raw_depth < 1.0 || raw_depth > 64.0) {
                    throw std::runtime_error(
                        "queue effective max_depth must be an integer from 1 to 64");
                }
                item.proposal.parameters.shatter_max_depth =
                    static_cast<int>(raw_depth);
            }
        }
        item.proposal.medium_request = medium_request_from_json(
            raw->get("medium_request"));
        const std::string disposition = raw->get("disposition")
            ? raw->get("disposition")->as_string() : "denied";
        item.disposition = disposition == "auto_apply"
            ? AutonomyDisposition::auto_apply
            : disposition == "human_review"
                ? AutonomyDisposition::human_review : AutonomyDisposition::denied;
        if (const JsonValue* v = raw->get("policy_rule")) item.policy_rule = v->as_string();
        if (const JsonValue* v = raw->get("reason")) item.reason = v->as_string();
        if (const JsonValue* v = raw->get("evaluated_revision"))
            item.evaluated_revision = static_cast<std::size_t>(v->as_number());
        if (const JsonValue* v = raw->get("executed")) item.executed = v->as_bool();
        if (const JsonValue* v = raw->get("execution_ok")) item.execution_ok = v->as_bool();
        if (const JsonValue* v = raw->get("requires_followup"))
            item.requires_followup = v->as_bool();
        if (const JsonValue* v = raw->get("rejected")) item.rejected = v->as_bool();
        if (const JsonValue* v = raw->get("review_rationale"))
            item.review_rationale = v->as_string();
        if (const JsonValue* e = raw->get("evidence")) {
            if (const JsonValue* v = e->get("confidence")) item.evidence.confidence = v->as_string();
            if (const JsonValue* v = e->get("usable")) item.evidence.usable = v->as_bool();
            if (const JsonValue* v = e->get("exact")) item.evidence.exact = v->as_bool();
            if (const JsonValue* v = e->get("certificate_passed")) item.evidence.certificate_passed = v->as_bool();
            if (const JsonValue* v = e->get("relative_volume_delta")) item.evidence.relative_volume_delta = v->as_number();
            if (const JsonValue* v = e->get("max_deviation")) item.evidence.max_deviation = v->as_number();
            if (const JsonValue* v = e->get("volume_fraction")) item.evidence.volume_fraction = v->as_number();
            if (const JsonValue* v = e->get("piece_count")) item.evidence.piece_count = static_cast<std::size_t>(v->as_number());
            if (const JsonValue* v = e->get("residual_count")) item.evidence.residual_count = static_cast<std::size_t>(v->as_number());
            if (const JsonValue* v = e->get("sliver_count")) item.evidence.sliver_count = static_cast<std::size_t>(v->as_number());
            if (const JsonValue* v = e->get("mutation_applied"))
                item.evidence.mutation_applied = v->as_bool();
        }
        queue.items.push_back(std::move(item));
    }
    AutonomyRequest identity;
    identity.request_id = queue.request_id;
    identity.source_digest = queue.source_digest;
    identity.expected_revision = queue.expected_revision;
    for (const AutonomyQueueItem& item : queue.items)
        identity.actions.push_back(item.proposal);
    if (queue.request_digest != autonomy_request_digest(identity))
        throw std::runtime_error(
            "autonomy queue request_digest does not match persisted actions");
    recount(queue);
    return queue;
}

bool reject_autonomy_queue_item(AutonomyQueue& queue,
                                const std::string& item_id,
                                const std::string& rationale) {
    for (AutonomyQueueItem& item : queue.items) {
        if (item.proposal.id != item_id || item.executed || item.rejected) continue;
        // Rejecting a suggested corrective action must not make an unsafe
        // ownership finding disappear. Only a replacement certificate may
        // transition these items to verified-resolved.
        if (item.proposal.op.rfind("coverage_", 0) == 0) return false;
        item.rejected = true;
        item.review_rationale = rationale;
        item.reason = "rejected by human reviewer";
        recount(queue);
        return true;
    }
    return false;
}

void refresh_autonomy_queue_counts(AutonomyQueue& queue) {
    recount(queue);
}

void reevaluate_pending_autonomy_queue(Session& session,
                                       AutonomyQueue& queue,
                                       const AutonomyPolicy& policy) {
    std::vector<AutonomyQueueItem> terminal;
    std::vector<AutonomyProposal> pending;
    for (const AutonomyQueueItem& item : queue.items) {
        if (item.executed || item.rejected
            || item.proposal.op.rfind("coverage_", 0) == 0
            || item.disposition == AutonomyDisposition::denied)
            terminal.push_back(item);
        else
            pending.push_back(item.proposal);
    }
    AutonomyQueue refreshed = evaluate_autonomy_queue(
        session, queue.document_id, pending, policy);
    terminal.insert(terminal.end(), refreshed.items.begin(), refreshed.items.end());
    queue.items = std::move(terminal);
    queue.mode = policy.mode;
    queue.policy = policy;
    queue.policy_digest = autonomy_policy_digest(policy);
    recount(queue);
}

AutonomyQueue evaluate_autonomy_queue(Session& session,
                                      const DocumentId& document_id,
                                      const std::vector<AutonomyProposal>& proposals,
                                      const AutonomyPolicy& policy) {
    AutonomyQueue queue;
    queue.document_id = document_id;
    queue.mode = policy.mode;
    queue.policy = policy;
    queue.policy_digest = autonomy_policy_digest(policy);
    for (const AutonomyProposal& proposal : proposals)
        queue.items.push_back(evaluate_one(session, document_id, proposal, policy));
    recount(queue);
    return queue;
}

AutonomyQueue make_coverage_autonomy_queue(
    const AleaCoverageCertificate& certificate,
    std::string certificate_path,
    std::string source_path,
    const AutonomyPolicy& policy) {
    std::string validation_error;
    if (!validate_alea_coverage_certificate(certificate, &validation_error)) {
        throw std::runtime_error(
            "cannot queue invalid coverage certificate: " + validation_error);
    }
    AutonomyQueue queue;
    queue.document_id = certificate.domain.domain_id;
    queue.source_path = std::move(source_path);
    if (!queue.source_path.empty())
        queue.source_digest = fnv1a128_file_hex(queue.source_path);
    queue.coverage_certificate_path = std::move(certificate_path);
    queue.coverage_certificate_digest = certificate.certificate_digest;
    queue.mode = policy.mode;
    queue.policy = policy;
    queue.policy_digest = autonomy_policy_digest(policy);
    for (const AleaCoverageReviewItem& finding
         : alea_coverage_review_items(certificate)) {
        if (!finding.blocking) continue;
        AutonomyQueueItem item;
        item.proposal.id = finding.id;
        item.proposal.actor = "csgify";
        item.proposal.target_id = finding.target_ids.empty()
            ? certificate.domain.domain_id : finding.target_ids.front();
        item.proposal.rationale = finding.summary;
        if (finding.target_ids.size() > 1) {
            item.proposal.rationale += " Competing owner: "
                + finding.target_ids[1] + ".";
        }
        if (finding.category == "overlap") {
            item.proposal.op = "coverage_resolve_overlap_ownership";
        } else if (finding.category == "gap") {
            item.proposal.op = "coverage_repair_gap";
        } else if (finding.category == "undefined_fill") {
            item.proposal.op = "coverage_repair_undefined_fill";
        } else {
            item.proposal.op = "coverage_regenerate_evidence";
        }
        item.disposition = AutonomyDisposition::human_review;
        item.policy_rule = "coverage.exact_one_membership";
        item.reason = finding.recommended_action;
        item.evidence.confidence = "blocking";
        item.evidence.usable = false;
        item.evidence.exact = false;
        item.evidence.certificate_passed = false;
        item.evidence.residual_count = static_cast<std::size_t>(finding.count);
        item.requires_followup = true;
        queue.items.push_back(std::move(item));
    }
    queue.request_id = "coverage:" + certificate.certificate_digest;
    AutonomyRequest identity;
    identity.request_id = queue.request_id;
    identity.source_digest = queue.source_digest;
    identity.expected_revision = queue.expected_revision;
    for (const AutonomyQueueItem& item : queue.items)
        identity.actions.push_back(item.proposal);
    queue.request_digest = autonomy_request_digest(identity);
    recount(queue);
    return queue;
}

void reconcile_coverage_autonomy_queue(
    AutonomyQueue& queue,
    const AleaCoverageCertificate& replacement,
    std::string replacement_path) {
    AutonomyQueue fresh = make_coverage_autonomy_queue(
        replacement, replacement_path, queue.source_path, queue.policy);
    std::map<std::string, AutonomyQueueItem> current;
    for (AutonomyQueueItem& item : fresh.items)
        current.emplace(item.proposal.id, std::move(item));

    std::vector<AutonomyQueueItem> merged;
    for (AutonomyQueueItem& item : queue.items) {
        if (item.proposal.op.rfind("coverage_", 0) != 0) {
            merged.push_back(std::move(item));
            continue;
        }
        auto still_blocking = current.find(item.proposal.id);
        if (still_blocking != current.end()) {
            // Retain review history while refreshing measured count/action.
            still_blocking->second.review_rationale = item.review_rationale;
            still_blocking->second.rejected = false;
            merged.push_back(std::move(still_blocking->second));
            current.erase(still_blocking);
            continue;
        }
        item.executed = true;
        item.execution_ok = true;
        item.requires_followup = false;
        item.rejected = false;
        item.evidence.usable = true;
        item.evidence.exact = true;
        item.evidence.certificate_passed = true;
        item.evidence.residual_count = 0;
        item.reason = "verified resolved by replacement coverage certificate";
        item.review_rationale = replacement.certificate_digest;
        merged.push_back(std::move(item));
    }
    for (auto& entry : current) merged.push_back(std::move(entry.second));
    queue.items = std::move(merged);
    queue.document_id = replacement.domain.domain_id;
    queue.coverage_certificate_path = std::move(replacement_path);
    queue.coverage_certificate_digest = replacement.certificate_digest;
    queue.request_id = "coverage:" + replacement.certificate_digest;
    AutonomyRequest identity;
    identity.request_id = queue.request_id;
    identity.source_digest = queue.source_digest;
    identity.expected_revision = queue.expected_revision;
    for (const AutonomyQueueItem& item : queue.items)
        identity.actions.push_back(item.proposal);
    queue.request_digest = autonomy_request_digest(identity);
    recount(queue);
}

AutonomyOperationExecution execute_autonomy_operation(
    Session& session,
    const DocumentId& document_id,
    AutonomyQueueItem& item,
    const AutonomyPolicy& policy) {
    AutonomyOperationExecution execution;
    const OperationDescriptor* descriptor = find_operation(item.proposal.op);
    if (!descriptor || !descriptor->autonomy_supported
        || !descriptor->restart_supported) return execution;
    const AutonomyOperationKind operation = descriptor->autonomy_kind;
    if (operation == AutonomyOperationKind::apply_decomposition_candidate) {
        execution.command = apply_decomposition_candidate(
            session, item.proposal.target_id, item.proposal.candidate_id);
    } else if (operation
               == AutonomyOperationKind::apply_surface_replacement_candidate) {
        if (!item.evidence.exact && policy.approximation.enabled
            && policy.approximation.allow_surface_replacement) {
            execution.command = apply_budgeted_surface_replacement_candidate(
                session, item.proposal.target_id, item.proposal.candidate_id,
                policy.approximation).command;
        } else {
            execution.command = apply_surface_replacement_candidate(
                session, item.proposal.target_id, item.proposal.candidate_id);
        }
    } else if (operation == AutonomyOperationKind::remove_solid) {
        execution.command = remove_solid(session, item.proposal.target_id);
    } else if (operation == AutonomyOperationKind::shatter_with_approximation) {
        const ApproximationCommandResult approximate =
            shatter_with_approximation(
                session, item.proposal.target_id,
                item.proposal.parameters.shatter_max_depth,
                policy.approximation);
        execution.command = approximate.command;
        item.evidence.relative_volume_delta =
            approximate.metrics.changed_volume_fraction;
        item.evidence.certificate_passed =
            approximate.metrics.symmetric_difference_certified;
        item.evidence.mutation_applied = execution.command.ok;
        item.evidence.exact = execution.command.ok
            && execution.command.report.solids_requiring_decomposition == 0;
    } else if (operation
               == AutonomyOperationKind::remove_residual_with_approximation) {
        const ApproximationCommandResult approximate =
            remove_residual_with_approximation(
                session, item.proposal.target_id, policy.approximation);
        execution.command = approximate.command;
        item.evidence.relative_volume_delta =
            approximate.metrics.changed_volume_fraction;
        item.evidence.certificate_passed =
            approximate.metrics.symmetric_difference_certified;
    } else if (operation
               == AutonomyOperationKind::resolve_overlap_with_approximation) {
        const ApproximationCommandResult approximate =
            resolve_overlap_with_approximation(
                session, item.proposal.target_id,
                item.proposal.candidate_id, policy.approximation);
        execution.command = approximate.command;
        item.evidence.relative_volume_delta =
            approximate.metrics.changed_volume_fraction;
        item.evidence.certificate_passed =
            approximate.metrics.symmetric_difference_certified;
    } else if (operation == AutonomyOperationKind::apply_contained_medium
               && item.proposal.medium_request) {
        execution.command = apply_contained_medium(
            session, document_id, *item.proposal.medium_request, true);
        item.evidence.mutation_applied = execution.command.ok;
    } else if (operation == AutonomyOperationKind::decompose_to_fixed_point) {
        FixedPointDecompositionPolicy fixed_policy;
        fixed_policy.max_action_relative_volume_delta =
            policy.max_relative_volume_delta;
        fixed_policy.max_cumulative_relative_volume_delta =
            policy.max_relative_volume_delta;
        fixed_policy.min_piece_volume_fraction =
            policy.min_piece_volume_fraction;
        fixed_policy.allow_medium_certified_progress =
            policy.allow_certified_intermediate_decomposition;
        const FixedPointDecompositionResult fixed =
            decompose_to_fixed_point(session, document_id, fixed_policy);
        execution.command = fixed.command;
        item.evidence.exact = fixed.summary.exact;
        item.evidence.piece_count = fixed.summary.final_state.active_piece_count;
        item.evidence.residual_count = fixed.summary.final_state.residual_count;
        item.evidence.mutation_applied = fixed.summary.mutated;
        item.evidence.certificate_passed = fixed.summary.exact
            || fixed.summary.mutated
            || fixed.summary.stop_reason == FixedPointStopReason::already_exact;
        item.requires_followup = !fixed.summary.exact
            && fixed.summary.stop_reason != FixedPointStopReason::already_exact;
        std::ostringstream outcome;
        outcome << "fixed-point stop: "
                << fixed_point_stop_reason_name(fixed.summary.stop_reason)
                << "; mutated=" << (fixed.summary.mutated ? "true" : "false")
                << "; residual_count="
                << fixed.summary.final_state.residual_count;
        if (item.requires_followup)
            outcome << "; recommended_action=shatter_or_review";
        item.reason = outcome.str();
        execution.ok = fixed.summary.exact || fixed.summary.mutated
            || fixed.summary.stop_reason == FixedPointStopReason::already_exact;
        return execution;
    }
    execution.ok = execution.command.ok;
    return execution;
}

void execute_autonomy_queue(Session& session,
                            AutonomyQueue& queue,
                            const AutonomyPolicy& policy) {
    if (policy.mode != AutonomyMode::automatic) return;
    std::size_t applied = 0;
    for (AutonomyQueueItem& item : queue.items) {
        if (item.executed || item.rejected) continue;
        if (item.disposition != AutonomyDisposition::auto_apply) continue;
        if (applied >= policy.max_auto_actions) {
            item.disposition = AutonomyDisposition::human_review;
            item.reason = "automatic action budget exhausted";
            item.policy_rule = "queue.max_auto_actions";
            continue;
        }
        // Re-evaluate against the revision produced by preceding queue items.
        AutonomyQueueItem fresh = evaluate_one(session, queue.document_id,
                                               item.proposal, policy);
        if (fresh.disposition != AutonomyDisposition::auto_apply) {
            item = std::move(fresh);
            continue;
        }
        item = std::move(fresh);
        DecisionAuditContext audit;
        audit.actor = item.proposal.actor.empty() ? "llm" : item.proposal.actor;
        audit.proposal_actor = audit.actor;
        audit.proposal_id = item.proposal.id;
        audit.rationale = item.proposal.rationale;
        audit.policy_rule = item.policy_rule;
        audit.approval_mode = "automatic";
        set_decision_audit_context(session, audit);
        const AutonomyOperationExecution execution = execute_autonomy_operation(
            session, queue.document_id, item, policy);
        clear_decision_audit_context(session);
        item.executed = true;
        item.execution_ok = execution.ok;
        if (!item.execution_ok && item.reason.empty())
            item.reason = "atomic session mutation failed or rolled back";
        if (item.execution_ok) ++applied;
    }
    recount(queue);
}

int write_autonomy_queue_json(const AutonomyQueue& queue,
                              const std::string& path) {
    std::ofstream file;
    std::ostream* out = nullptr;
    if (path == "-") out = &std::cout;
    else { file.open(path); if (!file) return -1; out = &file; }
    AutonomyRequest identity;
    identity.request_id = queue.request_id;
    identity.source_digest = queue.source_digest;
    identity.expected_revision = queue.expected_revision;
    for (const AutonomyQueueItem& item : queue.items)
        identity.actions.push_back(item.proposal);
    if (identity.request_id.empty()) {
        identity.request_id = "internal";
        identity.request_id += ':' + autonomy_request_digest(identity);
    }
    const std::string effective_request_digest =
        autonomy_request_digest(identity);
    *out << "{\n  \"schema\": \"csgify.autonomy-queue\",\n"
         << "  \"schema_version\": 4,\n"
         << "  \"request_id\": \"" << escape_json(identity.request_id)
         << "\",\n"
         << "  \"request_digest\": \""
         << escape_json(effective_request_digest)
         << "\",\n"
         << "  \"expected_revision\": " << queue.expected_revision << ",\n"
         << "  \"document_id\": \"" << escape_json(queue.document_id) << "\",\n"
         << "  \"source_path\": \"" << escape_json(queue.source_path) << "\",\n"
         << "  \"source_digest\": \"" << escape_json(queue.source_digest)
         << "\",\n"
         << "  \"policy_digest\": \"" << escape_json(queue.policy_digest)
         << "\",\n"
         << "  \"conversion_policy_digest\": \""
         << escape_json(queue.conversion_policy_digest) << "\",\n"
         << "  \"preparation\": {\"heal_shape\":"
         << (queue.preparation.heal_shape ? "true" : "false")
         << ",\"unit_override\":\""
         << escape_json(queue.preparation.unit_override)
         << "\",\"unit_scale_to_mm_override\":" << std::setprecision(17)
         << queue.preparation.unit_scale_to_mm_override
         << ",\"normalize_to_unit\":\""
         << escape_json(queue.preparation.normalize_to_unit) << "\"},\n"
         << "  \"coverage_certificate_path\": \""
         << escape_json(queue.coverage_certificate_path) << "\",\n"
         << "  \"coverage_certificate_digest\": \""
         << escape_json(queue.coverage_certificate_digest) << "\",\n"
         << "  \"mode\": \"" << autonomy_mode_name(queue.mode) << "\",\n"
         << "  \"policy\": {\"allow_high_confidence_decomposition\":"
         << (queue.policy.allow_high_confidence_decomposition ? "true" : "false")
         << ",\"allow_exact_surface_replacement\":"
         << (queue.policy.allow_exact_surface_replacement ? "true" : "false")
         << ",\"max_relative_volume_delta\":"
         << std::setprecision(17) << queue.policy.max_relative_volume_delta
         << ",\"allow_certified_intermediate_decomposition\":"
         << (queue.policy.allow_certified_intermediate_decomposition
                 ? "true" : "false")
         << ",\"max_intermediate_relative_volume_delta\":"
         << queue.policy.max_intermediate_relative_volume_delta
         << ",\"max_intermediate_pieces\":"
         << queue.policy.max_intermediate_pieces
         << ",\"max_intermediate_residual_pieces\":"
         << queue.policy.max_intermediate_residual_pieces
         << ",\"min_piece_volume_fraction\":"
         << queue.policy.min_piece_volume_fraction
         << ",\"allow_removal\":" << (queue.policy.allow_removal ? "true" : "false")
         << ",\"allow_contained_medium_generation\":"
         << (queue.policy.allow_contained_medium_generation ? "true" : "false")
         << ",\"contained_medium_materials\":[";
    for (std::size_t i = 0;
         i < queue.policy.contained_medium_materials.size(); ++i) {
        if (i) *out << ',';
        const MaterialDefinition& material =
            queue.policy.contained_medium_materials[i];
        *out << "{\"id\":\"" << escape_json(material.id)
             << "\",\"phase\":\"" << material_phase_name(material.phase)
             << "\",\"density\":" << material.density << '}';
    }
    *out << ']'
         << ",\"max_removal_volume_fraction\":"
         << queue.policy.max_removal_volume_fraction
         << ",\"allow_approximate_surface\":"
         << (queue.policy.allow_approximate_surface_replacement ? "true" : "false")
         << ",\"max_surface_deviation\":" << queue.policy.max_surface_deviation
         << ",\"max_auto_actions\":" << queue.policy.max_auto_actions
         << ",\"approximation\":{"
         << "\"enabled\":" << (queue.policy.approximation.enabled ? "true" : "false")
         << ",\"global_limit\":" << queue.policy.approximation.max_global_changed_volume_fraction
         << ",\"body_limit\":" << queue.policy.approximation.max_body_changed_volume_fraction
         << ",\"action_limit\":" << queue.policy.approximation.max_action_changed_volume_fraction
         << ",\"global_added_limit\":" << queue.policy.approximation.max_global_added_volume_fraction
         << ",\"global_removed_limit\":" << queue.policy.approximation.max_global_removed_volume_fraction
         << ",\"body_added_limit\":" << queue.policy.approximation.max_body_added_volume_fraction
         << ",\"body_removed_limit\":" << queue.policy.approximation.max_body_removed_volume_fraction
         << ",\"surface_deviation_limit\":" << queue.policy.approximation.max_surface_deviation
         << ",\"allow_surface_replacement\":" << (queue.policy.approximation.allow_surface_replacement ? "true" : "false")
         << ",\"allow_sliver_removal\":" << (queue.policy.approximation.allow_sliver_removal ? "true" : "false")
         << ",\"allow_bbox_replacement\":" << (queue.policy.approximation.allow_bbox_replacement ? "true" : "false")
         << ",\"allow_numerical_shatter_loss\":" << (queue.policy.approximation.allow_numerical_shatter_loss ? "true" : "false")
         << ",\"allow_overlap_resolution\":" << (queue.policy.approximation.allow_overlap_resolution ? "true" : "false")
         << ",\"require_containment_preservation\":" << (queue.policy.approximation.require_containment_preservation ? "true" : "false")
         << ",\"require_topology_preservation\":" << (queue.policy.approximation.require_topology_preservation ? "true" : "false")
         << ",\"protected_target_ids\":[";
    for (std::size_t i = 0;
         i < queue.policy.approximation.protected_target_ids.size(); ++i) {
        if (i) *out << ',';
        *out << '"' << escape_json(
            queue.policy.approximation.protected_target_ids[i]) << '"';
    }
    *out << "],\"protected_material_ids\":[";
    for (std::size_t i = 0;
         i < queue.policy.approximation.protected_material_ids.size(); ++i) {
        if (i) *out << ',';
        *out << queue.policy.approximation.protected_material_ids[i];
    }
    *out << "]}},\n"
         << "  \"summary\": {\"auto_apply\": " << queue.auto_apply_count
         << ", \"human_review\": " << queue.human_review_count
         << ", \"denied\": " << queue.denied_count
         << ", \"applied\": " << queue.applied_count
         << ", \"failed\": " << queue.failed_count << "},\n"
         << "  \"items\": [\n";
    for (std::size_t i = 0; i < queue.items.size(); ++i) {
        const AutonomyQueueItem& item = queue.items[i];
        *out << "    {\"id\":\"" << escape_json(item.proposal.id)
             << "\",\"actor\":\"" << escape_json(item.proposal.actor)
             << "\",\"op\":\"" << escape_json(item.proposal.op)
             << "\",\"target\":\"" << escape_json(item.proposal.target_id)
             << "\",\"candidate\":\"" << escape_json(item.proposal.candidate_id)
             << "\",\"rationale\":\"" << escape_json(item.proposal.rationale)
             << '"';
        if (item.proposal.op == "shatter_with_approximation") {
            *out << ",\"effective_parameters\":{\"max_depth\":"
                 << item.proposal.parameters.shatter_max_depth << '}';
        }
        if (item.proposal.medium_request) {
            *out << ",\"medium_request\":";
            write_medium_request_json(*out, *item.proposal.medium_request);
        }
        *out << ",\"disposition\":\""
             << autonomy_disposition_name(item.disposition)
             << "\",\"policy_rule\":\"" << escape_json(item.policy_rule)
             << "\",\"reason\":\"" << escape_json(item.reason)
             << "\",\"evaluated_revision\":" << item.evaluated_revision
             << ",\"executed\":" << (item.executed ? "true" : "false")
             << ",\"execution_ok\":" << (item.execution_ok ? "true" : "false")
             << ",\"requires_followup\":"
             << (item.requires_followup ? "true" : "false")
             << ",\"rejected\":" << (item.rejected ? "true" : "false")
             << ",\"review_rationale\":\""
             << escape_json(item.review_rationale) << "\""
             << ",\"evidence\":{\"confidence\":\""
             << escape_json(item.evidence.confidence)
             << "\",\"usable\":" << (item.evidence.usable ? "true" : "false")
             << ",\"exact\":" << (item.evidence.exact ? "true" : "false")
             << ",\"certificate_passed\":"
             << (item.evidence.certificate_passed ? "true" : "false")
             << ",\"relative_volume_delta\":" << std::setprecision(17)
             << item.evidence.relative_volume_delta
             << ",\"max_deviation\":" << item.evidence.max_deviation
             << ",\"volume_fraction\":" << item.evidence.volume_fraction
             << ",\"piece_count\":" << item.evidence.piece_count
             << ",\"residual_count\":" << item.evidence.residual_count
             << ",\"sliver_count\":" << item.evidence.sliver_count
             << ",\"mutation_applied\":"
             << (item.evidence.mutation_applied ? "true" : "false") << "}}";
        if (i + 1 < queue.items.size()) *out << ',';
        *out << '\n';
    }
    *out << "  ]\n}\n";
    return *out ? 0 : -1;
}

std::string build_autonomy_context_json(
    Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AutonomyContextOptions& options) {
    ConversionReport report = analyze(session, document_id).report;
    const DecompositionProgressState global_progress =
        decomposition_progress_state(session, document_id);
    ApproximationLedger approximation_ledger =
        document_approximation_ledger(session, document_id);
    if (approximation_ledger.document_baseline_volume <= 0.0)
        approximation_ledger.document_baseline_volume = report.total_volume;
    const FixedPointDecompositionPolicy fixed_policy;
    const DecompositionProgressScore global_score =
        make_decomposition_progress_score(global_progress,
                                           report.total_volume, fixed_policy);
    DecompositionProposal decomposition;
    SurfaceReplacementProposal surface;
    RemovalProposal removals;
    const AnalyzedFace* selected_face = nullptr;
    const AnalyzedDecomposedPiece* selected_piece = nullptr;
    if (options.target_id.find("face:") != std::string::npos) {
        const auto found = std::find_if(
            report.faces.begin(), report.faces.end(),
            [&](const AnalyzedFace& face) {
                return id_matches(face.id, options.target_id);
            });
        if (found != report.faces.end()) selected_face = &*found;
    }
    {
        const auto found = std::find_if(
            report.decomposed_pieces.begin(), report.decomposed_pieces.end(),
            [&](const AnalyzedDecomposedPiece& piece) {
                return id_matches(piece.id, options.target_id);
            });
        if (found != report.decomposed_pieces.end()) selected_piece = &*found;
    }
    if (options.include_candidates && !options.target_id.empty()) {
        if (options.target_id.find("face:") != std::string::npos)
            surface = propose_surface_replacement(session, options.target_id);
        else
            decomposition = propose_decomposition(session, options.target_id);
    }
    if (options.include_removal_candidates)
        removals = suggest_removals(session, document_id);

    const bool has_usable_decomposition = std::any_of(
        decomposition.candidates.begin(), decomposition.candidates.end(),
        [](const DecompositionCandidate& c) { return c.usable; });
    std::vector<const DecompositionCandidate*> decomposition_candidates;
    for (const DecompositionCandidate& candidate : decomposition.candidates) {
        if (!has_usable_decomposition || candidate.usable)
            decomposition_candidates.push_back(&candidate);
    }
    std::vector<const SurfaceReplacementCandidate*> surface_candidates;
    for (const SurfaceReplacementCandidate& candidate : surface.candidates)
        surface_candidates.push_back(&candidate);
    std::vector<const RemovalCandidate*> removal_candidates;
    for (const RemovalCandidate& candidate : removals.candidates)
        removal_candidates.push_back(&candidate);

    std::deque<std::shared_ptr<JsonValue>> history;
    if (!options.history_path.empty()) {
        std::ifstream input(options.history_path);
        std::string line;
        while (std::getline(input, line)) {
            try {
                std::shared_ptr<JsonValue> value = JsonParser(line).parse();
                if (!value || value->kind != JsonKind::object) continue;
                const std::string kind = value->get("kind")
                    ? value->get("kind")->as_string() : std::string();
                if (kind == "manifest") continue;
                history.push_back(std::move(value));
                while (history.size() > options.max_history_records)
                    history.pop_front();
            } catch (...) {
            }
        }
    }

    std::ostringstream serialized;
    std::ostream* out = &serialized;
    *out << "{\n  \"schema\":\"csgify.llm-context\",\n"
         << "  \"schema_version\":1,\n"
         << "  \"project\":{\"id\":\""
         << escape_json(options.project_id)
         << "\",\"intended_analysis\":\""
         << escape_json(options.intended_analysis) << "\"},\n"
         << "  \"preparation\":{\"heal_shape\":"
         << (options.preparation.heal_shape ? "true" : "false")
         << ",\"unit_override\":\""
         << escape_json(options.preparation.unit_override)
         << "\",\"unit_scale_to_mm_override\":"
         << options.preparation.unit_scale_to_mm_override
         << ",\"normalize_to_unit\":\""
         << escape_json(options.preparation.normalize_to_unit) << "\"},\n"
         << "  \"document\":{\"id\":\"" << escape_json(document_id)
         << "\",\"source_path\":\"" << escape_json(report.source_path)
         << "\",\"source_digest\":\""
         << escape_json(fnv1a128_file_hex(report.source_path))
         << "\",\"revision\":" << report.document_revision
         << ",\"units\":\"" << escape_json(report.length_unit)
         << "\",\"total_volume\":" << std::setprecision(17)
         << report.total_volume << "},\n"
         << "  \"selection\":\"" << escape_json(options.target_id) << "\",\n"
         << "  \"selection_context\":{\"kind\":\""
         << (options.target_id.find("face:") != std::string::npos ? "face"
             : selected_piece ? "residual_piece"
             : options.target_id.empty() ? "document" : "solid")
         << "\",\"resolved\":" << (options.target_id.empty() || selected_face
             || selected_piece
             || std::any_of(report.solids.begin(), report.solids.end(),
                 [&](const AnalyzedSolid& solid) {
                     return id_matches(solid.id, options.target_id);
                 }) ? "true" : "false")
         << ",\"owner_solid_id\":\""
         << escape_json(selected_face ? selected_face->solid_id
                : selected_piece ? selected_piece->source_solid_id
                                 : std::string())
         << "\"},\n"
         << "  \"context_limits\":{\"max_diagnostics\":"
         << options.max_diagnostics << ",\"max_history_records\":"
         << options.max_history_records << ",\"max_candidates\":"
         << options.max_candidates << ",\"candidate_offset\":"
         << options.candidate_offset << ",\"solid_offset\":"
         << options.solid_offset << ",\"max_solids\":"
         << options.max_solids << ",\"max_faces_per_solid\":"
         << options.max_faces_per_solid << ",\"occurrence_offset\":"
         << options.occurrence_offset << ",\"max_occurrences\":"
         << options.max_occurrences << ",\"max_materials\":"
         << options.max_materials << ",\"max_interfaces\":"
         << options.max_interfaces << ",\"overlap_offset\":"
         << options.overlap_offset << ",\"max_local_overlaps\":"
         << options.max_local_overlaps << ",\"max_serialized_bytes\":"
         << options.max_serialized_bytes << "},\n"
         << "  \"summary\":{\"ok\":" << (report.ok() ? "true" : "false")
         << ",\"solids\":" << report.solids_seen
         << ",\"faces\":" << report.faces_seen
         << ",\"needs_decomposition\":" << report.solids_requiring_decomposition
         << ",\"spline_faces\":" << report.spline_faces
         << ",\"overlaps\":" << report.overlaps.size() << "},\n"
         << "  \"global_progress\":{\"unsupported_faces\":"
         << global_progress.unsupported_face_count
         << ",\"residual_volume\":" << global_progress.residual_volume
         << ",\"residual_volume_bucket\":"
         << global_score.residual_volume_bucket
         << ",\"halfspace_violations\":"
         << global_progress.halfspace_violation_count
         << ",\"residual_count\":" << global_progress.residual_count
         << ",\"active_pieces\":" << global_progress.active_piece_count
         << ",\"exact_bodies\":" << global_progress.exact_body_count
         << ",\"overlap_errors\":" << global_progress.overlap_error_count
         << ",\"total_volume\":" << global_progress.total_volume << "},\n"
         << "  \"policy\":{\"mode\":\"" << autonomy_mode_name(policy.mode)
         << "\",\"max_relative_volume_delta\":"
         << policy.max_relative_volume_delta
         << ",\"allow_certified_intermediate_decomposition\":"
         << (policy.allow_certified_intermediate_decomposition ? "true" : "false")
         << ",\"max_intermediate_relative_volume_delta\":"
         << policy.max_intermediate_relative_volume_delta
         << ",\"max_intermediate_pieces\":" << policy.max_intermediate_pieces
         << ",\"max_intermediate_residual_pieces\":"
         << policy.max_intermediate_residual_pieces
         << ",\"min_piece_volume_fraction\":"
         << policy.min_piece_volume_fraction
         << ",\"allow_removal\":" << (policy.allow_removal ? "true" : "false")
         << ",\"allow_contained_medium_generation\":"
         << (policy.allow_contained_medium_generation ? "true" : "false")
         << ",\"contained_medium_materials\":[";
    for (std::size_t i = 0;
         i < policy.contained_medium_materials.size(); ++i) {
        if (i) *out << ',';
        const MaterialDefinition& material = policy.contained_medium_materials[i];
        *out << "{\"id\":\"" << escape_json(material.id)
             << "\",\"phase\":\"" << material_phase_name(material.phase)
             << "\",\"density\":" << material.density << '}';
    }
    *out << ']'
         << ",\"max_removal_volume_fraction\":"
         << policy.max_removal_volume_fraction
         << ",\"allow_approximate_surface\":"
         << (policy.allow_approximate_surface_replacement ? "true" : "false")
         << ",\"max_surface_deviation\":" << policy.max_surface_deviation
         << ",\"max_auto_actions\":" << policy.max_auto_actions
         << ",\"approximation\":{"
         << "\"enabled\":" << (policy.approximation.enabled ? "true" : "false")
         << ",\"global_limit\":" << policy.approximation.max_global_changed_volume_fraction
         << ",\"body_limit\":" << policy.approximation.max_body_changed_volume_fraction
         << ",\"action_limit\":" << policy.approximation.max_action_changed_volume_fraction
         << ",\"surface_deviation_limit\":" << policy.approximation.max_surface_deviation
         << ",\"allow_surface_replacement\":" << (policy.approximation.allow_surface_replacement ? "true" : "false")
         << ",\"allow_sliver_removal\":" << (policy.approximation.allow_sliver_removal ? "true" : "false")
         << ",\"allow_numerical_shatter_loss\":" << (policy.approximation.allow_numerical_shatter_loss ? "true" : "false")
         << ",\"allow_overlap_resolution\":" << (policy.approximation.allow_overlap_resolution ? "true" : "false")
         << ",\"require_containment_preservation\":" << (policy.approximation.require_containment_preservation ? "true" : "false")
         << ",\"require_topology_preservation\":" << (policy.approximation.require_topology_preservation ? "true" : "false")
         << ",\"protected_target_ids\":[";
    for (std::size_t index = 0;
         index < policy.approximation.protected_target_ids.size(); ++index) {
        if (index) *out << ',';
        *out << '"' << escape_json(
            policy.approximation.protected_target_ids[index]) << '"';
    }
    *out << "],\"protected_material_ids\":[";
    for (std::size_t index = 0;
         index < policy.approximation.protected_material_ids.size(); ++index) {
        if (index) *out << ',';
        *out << policy.approximation.protected_material_ids[index];
    }
    *out << "],\"ledger\":{"
         << "\"document_baseline_volume\":" << approximation_ledger.document_baseline_volume
         << ",\"cumulative_changed_volume\":" << approximation_ledger.cumulative_changed_volume
         << ",\"cumulative_added_volume\":" << approximation_ledger.cumulative_added_volume
         << ",\"cumulative_removed_volume\":" << approximation_ledger.cumulative_removed_volume
         << "}}},\n";

    *out << "  \"action_contract\":{\"schema\":\"csgify.autonomy-actions\","
            "\"schema_version\":2,\"requires\":[\"request_id\","
            "\"source_digest\",\"expected_revision\",\"actions\"]},\n"
            "  \"diagnostics\":[";
    std::size_t diagnostic_count = 0;
    for (const Diagnostic& diagnostic : report.diagnostics) {
        if (diagnostic_count >= options.max_diagnostics) break;
        if (diagnostic_count++) *out << ',';
        const char* severity = diagnostic.severity == DiagnosticSeverity::error
            ? "error" : diagnostic.severity == DiagnosticSeverity::warning
                ? "warning" : "info";
        *out << "{\"severity\":\"" << severity << "\",\"code\":\""
             << escape_json(diagnostic.code) << "\",\"target\":\""
             << escape_json(diagnostic.shape_id) << "\",\"message\":\""
             << escape_json(diagnostic.message) << "\"}";
    }
    const std::string focused_solid = selected_face
        ? selected_face->solid_id
        : selected_piece ? selected_piece->source_solid_id : options.target_id;
    std::vector<const AnalyzedSolid*> focused_solids;
    for (const AnalyzedSolid& solid : report.solids) {
        if (focused_solid.empty() || id_matches(solid.id, focused_solid))
            focused_solids.push_back(&solid);
    }
    const std::size_t solid_begin = std::min(
        options.solid_offset, focused_solids.size());
    const std::size_t emitted_solids = std::min(
        options.max_solids, focused_solids.size() - solid_begin);
    *out << "],\n  \"solids_page\":{\"total\":"
         << focused_solids.size() << ",\"offset\":" << solid_begin
         << ",\"emitted\":" << emitted_solids
         << ",\"next_offset\":" << solid_begin + emitted_solids
         << ",\"truncated\":"
         << (solid_begin + emitted_solids < focused_solids.size()
                ? "true" : "false")
         << "},\n  \"solids\":[";
    bool first = true;
    for (std::size_t solid_index = 0;
         solid_index < emitted_solids; ++solid_index) {
        const AnalyzedSolid& solid =
            *focused_solids[solid_begin + solid_index];
        if (!first) *out << ',';
        first = false;
        *out << "{\"id\":\"" << escape_json(solid.id)
             << "\",\"label\":\"" << escape_json(solid.label)
             << "\",\"component_instance_id\":\""
             << escape_json(solid.component_instance_id)
             << "\",\"component_definition_id\":\""
             << escape_json(solid.component_definition_id)
             << "\",\"name\":\"" << escape_json(solid.name)
             << "\",\"hierarchy_path\":\""
             << escape_json(solid.hierarchy_path)
             << "\",\"volume\":" << solid.volume
             << ",\"volume_fraction\":" << solid.volume_fraction
             << ",\"faces\":" << solid.face_count
             << ",\"bbox\":{\"valid\":"
             << (solid.bbox.valid ? "true" : "false")
             << ",\"min\":[" << solid.bbox.xmin << ',' << solid.bbox.ymin
             << ',' << solid.bbox.zmin << "],\"max\":["
             << solid.bbox.xmax << ',' << solid.bbox.ymax << ','
             << solid.bbox.zmax << "]}"
             << ",\"exact_csg\":" << (solid.exact_csg_supported ? "true" : "false")
             << ",\"needs_decomposition\":"
             << (solid.needs_decomposition ? "true" : "false")
             << ",\"needs_surface_replacement\":"
             << (solid.needs_surface_replacement ? "true" : "false")
             << ",\"needs_user_decision\":"
             << (solid.needs_user_decision ? "true" : "false")
             << ",\"face_ids\":[";
        const std::size_t emitted_faces = std::min(
            options.max_faces_per_solid, solid.face_ids.size());
        for (std::size_t face_index = 0;
             face_index < emitted_faces; ++face_index) {
            if (face_index) *out << ',';
            *out << '"' << escape_json(solid.face_ids[face_index]) << '"';
        }
        *out << "],\"face_ids_truncated\":"
             << (emitted_faces < solid.face_ids.size() ? "true" : "false")
             << "}";
    }
    *out << "],\n  \"faces\":[";
    if (selected_face) {
        *out << "{\"id\":\"" << escape_json(selected_face->id)
             << "\",\"owner_solid_id\":\""
             << escape_json(selected_face->solid_id)
             << "\",\"name\":\"" << escape_json(selected_face->name)
             << "\",\"surface_type\":\""
             << escape_json(selected_face->surface_type)
             << "\",\"support_kind\":\""
             << escape_json(selected_face->support_kind)
             << "\",\"area\":" << selected_face->area
             << ",\"exact_csg\":"
             << (selected_face->exact_csg_supported ? "true" : "false")
             << ",\"needs_surface_replacement\":"
             << (selected_face->needs_surface_replacement ? "true" : "false")
             << "}";
    }
    *out << "],\n  \"residual_pieces\":[";
    if (selected_piece) {
        *out << "{\"id\":\"" << escape_json(selected_piece->id)
             << "\",\"owner_solid_id\":\""
             << escape_json(selected_piece->source_solid_id)
             << "\",\"label\":\"" << escape_json(selected_piece->label)
             << "\",\"volume\":" << selected_piece->volume
             << ",\"faces\":" << selected_piece->face_count
             << ",\"exact_csg\":"
             << (selected_piece->exact_csg_supported ? "true" : "false")
             << ",\"needs_decomposition\":"
             << (selected_piece->needs_decomposition ? "true" : "false")
             << ",\"needs_surface_replacement\":"
             << (selected_piece->needs_surface_replacement ? "true" : "false")
             << ",\"bbox\":{\"valid\":"
             << (selected_piece->bbox.valid ? "true" : "false")
             << ",\"min\":[" << selected_piece->bbox.xmin << ','
             << selected_piece->bbox.ymin << ',' << selected_piece->bbox.zmin
             << "],\"max\":[" << selected_piece->bbox.xmax << ','
             << selected_piece->bbox.ymax << ',' << selected_piece->bbox.zmax
             << "]}}";
    }
    const AnalyzedSolid* selected_solid = nullptr;
    for (const AnalyzedSolid& solid : report.solids) {
        if (id_matches(solid.id, focused_solid)) {
            selected_solid = &solid;
            break;
        }
    }
    const ComponentInstance* selected_instance = nullptr;
    for (const ComponentInstance& instance : report.component_instances) {
        if ((selected_solid
                && instance.id == selected_solid->component_instance_id)
            || id_matches(instance.source_shape_id, focused_solid)) {
            selected_instance = &instance;
            break;
        }
    }
    std::vector<const ComponentInstance*> related_occurrences;
    if (selected_instance) {
        for (const ComponentInstance& instance : report.component_instances) {
            if (instance.definition_id == selected_instance->definition_id)
                related_occurrences.push_back(&instance);
        }
    }
    const std::size_t occurrence_begin = std::min(
        options.occurrence_offset, related_occurrences.size());
    const std::size_t emitted_occurrences = std::min(
        options.max_occurrences,
        related_occurrences.size() - occurrence_begin);
    *out << "],\n  \"semantic_context\":{\"owner_occurrence_id\":\""
         << escape_json(selected_instance ? selected_instance->id : std::string())
         << "\",\"definition_id\":\""
         << escape_json(selected_instance
                ? selected_instance->definition_id : std::string())
         << "\",\"occurrences_page\":{\"total\":"
         << related_occurrences.size() << ",\"offset\":" << occurrence_begin
         << ",\"emitted\":" << emitted_occurrences
         << ",\"next_offset\":" << occurrence_begin + emitted_occurrences
         << ",\"truncated\":"
         << (occurrence_begin + emitted_occurrences
                < related_occurrences.size() ? "true" : "false")
         << "},\"occurrences\":[";
    for (std::size_t index = 0; index < emitted_occurrences; ++index) {
        if (index) *out << ',';
        const ComponentInstance& instance =
            *related_occurrences[occurrence_begin + index];
        const EntityMetadata* metadata = nullptr;
        for (const EntityMetadata& value : report.metadata) {
            if (value.id == instance.metadata_id) {
                metadata = &value;
                break;
            }
        }
        *out << "{\"id\":\"" << escape_json(instance.id)
             << "\",\"selected\":"
             << (selected_instance && instance.id == selected_instance->id
                    ? "true" : "false")
             << ",\"source_shape_id\":\""
             << escape_json(instance.source_shape_id)
             << "\",\"parent_assembly_id\":\""
             << escape_json(instance.parent_assembly_id)
             << "\",\"display_name\":\""
             << escape_json(metadata
                    ? report.strings.lookup(metadata->display_name)
                    : std::string())
             << "\",\"component_path\":\""
             << escape_json(metadata
                    ? report.strings.lookup(metadata->source_component_path)
                    : std::string())
             << "\",\"material_override\":\""
             << escape_json(instance.material_override)
             << "\",\"active\":" << (instance.active ? "true" : "false")
             << ",\"transform\":[";
        for (std::size_t value = 0;
             value < instance.transform.values.size(); ++value) {
            if (value) *out << ',';
            *out << instance.transform.values[value];
        }
        *out << "]}";
    }
    const std::string effective_material = selected_instance
            && !selected_instance->material_override.empty()
        ? selected_instance->material_override
        : report.materials.empty() ? std::string() : report.materials.front().id;
    *out << "],\"material\":{\"effective_id\":\""
         << escape_json(effective_material) << "\",\"source\":\""
         << (selected_instance && !selected_instance->material_override.empty()
                ? "instance_override"
                : report.materials.empty() ? "unresolved" : "document_default")
         << "\",\"available_total\":" << report.materials.size()
         << ",\"available_truncated\":"
         << (report.materials.size() > options.max_materials ? "true" : "false")
         << ",\"available\":[";
    const std::size_t material_count = std::min(
        options.max_materials, report.materials.size());
    for (std::size_t index = 0; index < material_count; ++index) {
        if (index) *out << ',';
        const MaterialDefinition& material = report.materials[index];
        *out << "{\"id\":\"" << escape_json(material.id)
             << "\",\"phase\":\"" << material_phase_name(material.phase)
             << "\",\"density\":" << material.density << '}';
    }
    *out << "],\"protected_ids\":[";
    for (std::size_t index = 0;
         index < policy.approximation.protected_material_ids.size(); ++index) {
        if (index) *out << ',';
        *out << policy.approximation.protected_material_ids[index];
    }
    *out << "]},\"interfaces\":[";
    const std::size_t interface_count = selected_solid
        ? std::min(options.max_interfaces,
                   selected_solid->topology.pipe_annulus.ports.size()) : 0;
    for (std::size_t index = 0; index < interface_count; ++index) {
        if (index) *out << ',';
        const PipePortTopology& port =
            selected_solid->topology.pipe_annulus.ports[index];
        const MediumGenerationRequest* request = nullptr;
        const MediumPortDecision* decision = nullptr;
        if (selected_instance) {
            for (const MediumGenerationRequest& candidate : report.medium_requests) {
                if (std::find(candidate.host_component_ids.begin(),
                              candidate.host_component_ids.end(),
                              selected_instance->id)
                    == candidate.host_component_ids.end()) continue;
                const auto found = std::find_if(
                    candidate.ports.begin(), candidate.ports.end(),
                    [&](const MediumPortDecision& value) {
                        return value.port_id == port.id;
                    });
                if (found != candidate.ports.end()) {
                    request = &candidate;
                    decision = &*found;
                    break;
                }
            }
        }
        *out << "{\"id\":\"" << escape_json(port.id)
             << "\",\"kind\":\"pipe_port\",\"center\":["
             << port.center.x << ',' << port.center.y << ',' << port.center.z
             << "],\"normal\":[" << port.normal.x << ',' << port.normal.y
             << ',' << port.normal.z << "],\"radius\":" << port.radius
             << ",\"request_id\":\""
             << escape_json(request ? request->id : std::string())
             << "\",\"disposition\":\""
             << (decision
                    ? medium_port_disposition_name(decision->disposition)
                    : "unresolved")
             << "\",\"peer_port_id\":\""
             << escape_json(decision ? decision->peer_port_id : std::string())
             << "\"}";
    }
    *out << "],\"interfaces_total\":"
         << (selected_solid
                ? selected_solid->topology.pipe_annulus.ports.size() : 0)
         << ",\"interfaces_truncated\":"
         << (selected_solid
                && selected_solid->topology.pipe_annulus.ports.size()
                    > options.max_interfaces ? "true" : "false")
         << "},\n";
    std::vector<const SolidOverlap*> local_overlaps;
    for (const SolidOverlap& overlap : report.overlaps) {
        if (!focused_solid.empty()
            && !id_matches(overlap.solid_a_id, focused_solid)
            && !id_matches(overlap.solid_b_id, focused_solid)) continue;
        local_overlaps.push_back(&overlap);
    }
    const std::size_t overlap_begin = std::min(
        options.overlap_offset, local_overlaps.size());
    const std::size_t emitted_overlaps = std::min(
        options.max_local_overlaps, local_overlaps.size() - overlap_begin);
    *out << "  \"local_overlaps_page\":{\"total\":"
         << local_overlaps.size() << ",\"offset\":" << overlap_begin
         << ",\"emitted\":" << emitted_overlaps
         << ",\"next_offset\":" << overlap_begin + emitted_overlaps
         << ",\"truncated\":"
         << (overlap_begin + emitted_overlaps < local_overlaps.size()
                ? "true" : "false")
         << "},\n  \"local_overlaps\":[";
    for (std::size_t index = 0; index < emitted_overlaps; ++index) {
        if (index) *out << ',';
        const SolidOverlap& overlap = *local_overlaps[overlap_begin + index];
        *out << "{\"solid_a_id\":\"" << escape_json(overlap.solid_a_id)
             << "\",\"solid_b_id\":\"" << escape_json(overlap.solid_b_id)
             << "\",\"intersection_volume\":"
             << overlap.intersection_volume << ",\"min_fraction\":"
             << overlap.min_fraction << "}";
    }
    const auto write_page = [&](std::size_t total) {
        const std::size_t begin = std::min(options.candidate_offset, total);
        const std::size_t emitted = std::min(
            options.max_candidates, total - begin);
        *out << "{\"total\":" << total << ",\"offset\":" << begin
             << ",\"emitted\":" << emitted << ",\"next_offset\":"
             << begin + emitted << ",\"truncated\":"
             << (begin + emitted < total ? "true" : "false") << '}';
    };
    *out << "],\n  \"candidate_pages\":{\"decomposition\":";
    write_page(decomposition_candidates.size());
    *out << ",\"surface\":";
    write_page(surface_candidates.size());
    *out << ",\"removal\":";
    write_page(removal_candidates.size());
    *out << "},\n  \"candidates\":{\"decomposition\":[";
    const std::size_t decomposition_begin = std::min(
        options.candidate_offset, decomposition_candidates.size());
    const std::size_t decomposition_end = decomposition_begin + std::min(
        options.max_candidates,
        decomposition_candidates.size() - decomposition_begin);
    for (std::size_t index = decomposition_begin;
         index < decomposition_end; ++index) {
        if (index != decomposition_begin) *out << ',';
        const DecompositionCandidate& c = *decomposition_candidates[index];
        *out << "{\"id\":\"" << escape_json(c.id)
             << "\",\"action\":\"apply_decomposition_candidate\""
             << ",\"target\":\"" << escape_json(c.target_id)
             << "\",\"confidence\":\"" << escape_json(c.confidence)
             << "\",\"usable\":" << (c.usable ? "true" : "false")
             << ",\"pieces\":" << c.piece_count
             << ",\"exact_pieces\":" << c.exact_piece_count
             << ",\"residual_pieces\":" << c.pieces_requiring_decomposition
             << ",\"slivers\":" << c.sliver_piece_count
             << ",\"relative_volume_delta\":" << c.relative_delta << "}";
    }
    *out << "],\"surface\":[";
    const std::size_t surface_begin = std::min(
        options.candidate_offset, surface_candidates.size());
    const std::size_t surface_end = surface_begin + std::min(
        options.max_candidates, surface_candidates.size() - surface_begin);
    for (std::size_t index = surface_begin; index < surface_end; ++index) {
        if (index != surface_begin) *out << ',';
        const SurfaceReplacementCandidate& c = *surface_candidates[index];
        *out << "{\"id\":\"" << escape_json(c.id)
             << "\",\"action\":\"apply_surface_replacement_candidate\""
             << ",\"target\":\"" << escape_json(surface.face_id)
             << "\",\"owner_solid_id\":\""
             << escape_json(c.owner_target_id)
             << "\",\"kind\":\"" << escape_json(c.kind)
             << "\",\"confidence\":\"" << escape_json(c.confidence)
             << "\",\"usable\":" << (c.usable ? "true" : "false")
             << ",\"exact\":" << (c.exact ? "true" : "false")
             << ",\"max_deviation\":" << c.max_deviation
             << ",\"relative_volume_delta\":"
             << std::abs(c.owner_relative_volume_delta) << "}";
    }
    *out << "],\"removal\":[";
    const std::size_t removal_begin = std::min(
        options.candidate_offset, removal_candidates.size());
    const std::size_t removal_end = removal_begin + std::min(
        options.max_candidates, removal_candidates.size() - removal_begin);
    for (std::size_t index = removal_begin; index < removal_end; ++index) {
        if (index != removal_begin) *out << ',';
        const RemovalCandidate& c = *removal_candidates[index];
        *out << "{\"id\":\"" << escape_json(c.id)
             << "\",\"action\":\"remove_solid\",\"target\":\""
             << escape_json(c.solid_id) << "\",\"confidence\":\""
             << escape_json(c.confidence) << "\",\"volume_fraction\":"
             << c.volume_fraction << "}";
    }
    *out << "]},\n  \"operations\":[";
    first = true;
    for (const OperationDescriptor& op : operation_registry()) {
        if (!op.mutating || !op.autonomy_supported
            || !op.restart_supported) continue;
        if (!first) *out << ',';
        first = false;
        const char* target = op.target == OperationTargetKind::document
            ? "document" : op.target == OperationTargetKind::solid ? "solid" : "face";
        *out << "{\"name\":\"" << escape_json(op.name)
             << "\",\"category\":\"" << escape_json(op.category)
             << "\",\"target_kind\":\"" << target
             << "\",\"approximate\":" << (op.approximate ? "true" : "false")
             << ",\"argument_schema\":" << op.autonomy_argument_schema
             << ",\"qt_review_supported\":"
             << (op.qt_review_supported ? "true" : "false")
             << ",\"restart_supported\":"
             << (op.restart_supported ? "true" : "false")
             << ",\"recovery_action\":\""
             << escape_json(op.recovery_action)
             << "\"}";
    }
    *out << "],\n  \"history\":[";
    for (std::size_t i = 0; i < history.size(); ++i) {
        const JsonValue& value = *history[i];
        if (i) *out << ',';
        const std::string kind = value.get("kind")
            ? value.get("kind")->as_string() : std::string();
        const std::string op = value.get("op")
            ? value.get("op")->as_string() : std::string();
        const std::string decision = value.get("decision")
            ? value.get("decision")->as_string() : std::string();
        const std::string proposal_id = value.get("proposal_id")
            ? value.get("proposal_id")->as_string() : std::string();
        const std::string target_id = value.get("target_id")
            ? value.get("target_id")->as_string() : std::string();
        const std::string candidate_id = value.get("candidate_id")
            ? value.get("candidate_id")->as_string() : std::string();
        const JsonValue* audit = value.get("audit");
        const auto audit_string = [audit](const std::string& key) {
            const JsonValue* field = audit ? audit->get(key) : nullptr;
            return field ? field->as_string() : std::string();
        };
        *out << "{\"kind\":\"" << escape_json(kind)
             << "\",\"op\":\"" << escape_json(op)
             << "\",\"proposal_id\":\"" << escape_json(proposal_id)
             << "\",\"target_id\":\"" << escape_json(target_id)
             << "\",\"candidate_id\":\"" << escape_json(candidate_id)
             << "\",\"decision\":\"" << escape_json(decision)
             << "\",\"actor\":\"" << escape_json(audit_string("actor"))
             << "\",\"rationale\":\""
             << escape_json(audit_string("rationale"))
             << "\",\"policy_rule\":\""
             << escape_json(audit_string("policy_rule")) << "\"}";
    }
    *out << "]\n}\n";
    if (!*out)
        throw std::runtime_error("failed to serialize autonomy context");
    const std::string contents = serialized.str();
    if (contents.size() > options.max_serialized_bytes)
        throw std::runtime_error("autonomy context exceeds max_serialized_bytes");
    return contents;
}

int write_autonomy_context_json(Session& session,
                                const DocumentId& document_id,
                                const AutonomyPolicy& policy,
                                const AutonomyContextOptions& options,
                                const std::string& path) {
    std::string contents;
    try {
        contents = build_autonomy_context_json(
            session, document_id, policy, options);
    } catch (...) {
        return -1;
    }
    if (path == "-") {
        std::cout << contents;
        return std::cout ? 0 : -1;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return -1;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return file ? 0 : -1;
}

} // namespace csgify
