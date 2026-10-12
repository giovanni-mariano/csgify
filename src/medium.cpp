#include "csgify/medium.hpp"
#include "occt_boolean.hpp"

#include "engine_model.hpp"
#include "engine_internal.hpp"
#include "topology_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>

namespace csgify {

const char* medium_fill_mode_name(MediumFillMode mode) {
    switch (mode) {
    case MediumFillMode::full: return "full";
    case MediumFillMode::level: return "level";
    case MediumFillMode::bounded_segment: return "bounded_segment";
    }
    return "full";
}

const char* medium_port_disposition_name(MediumPortDisposition disposition) {
    switch (disposition) {
    case MediumPortDisposition::connect_to_port: return "connect_to_port";
    case MediumPortDisposition::cap_here: return "cap_here";
    case MediumPortDisposition::open_to_domain: return "open_to_domain";
    case MediumPortDisposition::interface_to_external_model:
        return "interface_to_external_model";
    case MediumPortDisposition::unresolved: return "unresolved";
    }
    return "unresolved";
}

const char* geometric_seed_kind_name(GeometricSeedKind kind) {
    switch (kind) {
    case GeometricSeedKind::point: return "point";
    case GeometricSeedKind::face: return "face";
    }
    return "point";
}

const char* medium_valve_state_name(MediumValveState state) {
    switch (state) {
    case MediumValveState::not_applicable: return "not_applicable";
    case MediumValveState::open: return "open";
    case MediumValveState::closed: return "closed";
    case MediumValveState::unresolved: return "unresolved";
    }
    return "unresolved";
}

const char* medium_junction_kind_name(MediumJunctionKind kind) {
    switch (kind) {
    case MediumJunctionKind::straight: return "straight";
    case MediumJunctionKind::reducer: return "reducer";
    case MediumJunctionKind::elbow: return "elbow";
    case MediumJunctionKind::tee: return "tee";
    case MediumJunctionKind::manifold: return "manifold";
    case MediumJunctionKind::unknown: return "unknown";
    }
    return "unknown";
}

MediumRequestValidation validate_medium_generation_request(
    const MediumGenerationRequest& request,
    const std::vector<MaterialDefinition>& materials) {
    MediumRequestValidation result;
    auto error = [&](const char* code) {
        if (std::find(result.error_codes.begin(), result.error_codes.end(), code)
            == result.error_codes.end()) {
            result.error_codes.emplace_back(code);
        }
    };

    if (request.id.empty()) error("medium_request_missing_id");
    if (request.host_component_ids.empty()
        && request.seed_points_or_faces.empty()) {
        error("medium_request_missing_host_or_seed");
    }
    if (request.material_id.empty()) {
        error("medium_request_missing_material");
    } else {
        const auto material = std::find_if(
            materials.begin(), materials.end(), [&](const MaterialDefinition& value) {
                return value.id == request.material_id;
            });
        if (material == materials.end()) {
            error("medium_request_unknown_material");
        } else if (material->phase != MaterialPhase::liquid
                   && material->phase != MaterialPhase::gas) {
            error("medium_request_material_not_fluid");
        }
    }

    if (request.target_volume_fraction
        && (!std::isfinite(*request.target_volume_fraction)
            || *request.target_volume_fraction < 0.0
            || *request.target_volume_fraction > 1.0)) {
        error("medium_request_invalid_volume_fraction");
    }

    if (request.max_worker_count == 0)
        error("medium_request_invalid_worker_count");

    if (!request.complement_material_id.empty()) {
        const auto complement = std::find_if(
            materials.begin(), materials.end(), [&](const MaterialDefinition& value) {
                return value.id == request.complement_material_id;
            });
        if (complement == materials.end())
            error("medium_request_unknown_complement_material");
        else if (complement->phase != MaterialPhase::gas)
            error("medium_request_complement_not_gas");
    }

    if (request.fill_mode == MediumFillMode::level) {
        const double gravity_norm = std::sqrt(
            request.gravity_direction[0] * request.gravity_direction[0]
            + request.gravity_direction[1] * request.gravity_direction[1]
            + request.gravity_direction[2] * request.gravity_direction[2]);
        if (!std::isfinite(gravity_norm) || gravity_norm <= 0.0) {
            error("medium_request_invalid_gravity");
        }
        if (!request.fill_level && !request.target_volume_fraction) {
            error("medium_request_missing_fill_level");
        }
    } else if (request.fill_mode == MediumFillMode::full
               && (request.fill_level || request.target_volume_fraction)) {
        error("medium_request_full_has_partial_fill_parameters");
    }

    std::unordered_set<std::string> port_ids;
    for (const MediumPortDecision& port : request.ports) {
        if (port.port_id.empty() || !port_ids.insert(port.port_id).second) {
            error("medium_request_invalid_or_duplicate_port");
        }
        if (port.disposition == MediumPortDisposition::unresolved) {
            error("medium_request_unresolved_port");
        }
        if (port.disposition == MediumPortDisposition::connect_to_port
            && port.peer_port_id.empty()) {
            error("medium_request_connected_port_missing_peer");
        }
        if (port.valve_state == MediumValveState::unresolved)
            error("medium_request_unresolved_valve");
        if (port.valve_state == MediumValveState::closed
            && port.disposition == MediumPortDisposition::connect_to_port)
            error("medium_request_closed_valve_connected");
    }

    result.valid = result.error_codes.empty();
    return result;
}

namespace {

void proposal_error(MediumGenerationProposal& proposal,
                    const std::string& code,
                    const std::string& message,
                    const std::string& shape_id = {}) {
    proposal.diagnostics.push_back({DiagnosticSeverity::error, code, message,
                                    shape_id, shape_id});
}

DecisionRequest medium_decision(const MediumGenerationRequest& request,
                                const std::string& reason,
                                const std::string& summary) {
    DecisionRequest decision;
    decision.id = "decision:contained_medium:" + request.id;
    decision.kind = "contained_medium";
    decision.target_id = request.id;
    decision.reason = reason;
    decision.recommended_action = "review_medium_ports";
    decision.summary = summary;
    decision.candidates.push_back({
        "action:resolve_medium_ports", "apply_contained_medium", "review",
        "Select a fluid material and explicitly resolve both pipe ports.", {}});
    return decision;
}

bool root_solid_even_if_removed(const CadModel& model,
                                const std::string& id,
                                TopoDS_Solid* solid) {
    std::size_t target = 0;
    if (!detail::parse_solid_id(id, &target) || target == 0) return false;
    std::size_t index = 0;
    for (TopExp_Explorer it(model.root, TopAbs_SOLID); it.More(); it.Next()) {
        if (++index == target) {
            *solid = TopoDS::Solid(it.Current());
            return true;
        }
    }
    return false;
}

double shape_volume(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}

bool common_volume(const TopoDS_Shape& left,
                   const TopoDS_Shape& right,
                   double* volume) {
    try {
        BRepAlgoAPI_Common common;
        detail::build_binary_boolean(common, left, right);
        if (!common.IsDone()) return false;
        *volume = shape_volume(common.Shape());
        return std::isfinite(*volume);
    } catch (const Standard_Failure&) {
        return false;
    }
}

TopoDS_Solid make_lumen(const MediumGenerationProposal& proposal) {
    const double length = std::sqrt(
        proposal.height.x * proposal.height.x
        + proposal.height.y * proposal.height.y
        + proposal.height.z * proposal.height.z);
    if (!(length > 0.0) || !(proposal.radius > 0.0)) return {};
    const gp_Dir direction(proposal.height.x / length,
                           proposal.height.y / length,
                           proposal.height.z / length);
    const gp_Ax2 axis(
        gp_Pnt(proposal.base.x, proposal.base.y, proposal.base.z), direction);
    if (proposal.conical) {
        if (!(proposal.top_radius > 0.0)) return {};
        return BRepPrimAPI_MakeCone(
            axis, proposal.radius, proposal.top_radius, length).Solid();
    }
    return BRepPrimAPI_MakeCylinder(axis, proposal.radius, length).Solid();
}

std::size_t next_metadata_id(const CadModel& model) {
    MetadataId maximum = 0;
    for (const EntityMetadata& metadata : model.metadata)
        maximum = std::max(maximum, metadata.id);
    return static_cast<std::size_t>(maximum + 1);
}

int transport_material_number(const MaterialDefinition& material) {
    const std::size_t separator = material.id.find_last_of(':');
    const std::string digits = separator == std::string::npos
        ? material.id : material.id.substr(separator + 1);
    if (digits.empty()) return 0;
    long long value = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') return 0;
        value = value * 10 + (ch - '0');
        if (value > std::numeric_limits<int>::max()) return 0;
    }
    return value > 0 ? static_cast<int>(value) : 0;
}

MediumGenerationProposal propose_pipe_network_medium(
    const CadModel& model,
    const MediumGenerationRequest& request,
    const ConversionOptions& options);

bool request_needs_v2(const MediumGenerationRequest& request) {
    return request.host_component_ids.size() != 1
        || request.fill_mode != MediumFillMode::full
        || !request.seed_points_or_faces.empty()
        || !request.internal_components_to_subtract.empty()
        || std::any_of(request.ports.begin(), request.ports.end(),
            [](const MediumPortDecision& port) {
                return port.disposition == MediumPortDisposition::connect_to_port
                    || port.valve_state != MediumValveState::not_applicable;
            });
}

} // namespace

MediumGenerationProposal propose_straight_pipe_medium(
    const CadModel& model,
    const MediumGenerationRequest& request,
    const ConversionOptions& options) {
    if (request_needs_v2(request))
        return propose_pipe_network_medium(model, request, options);
    MediumGenerationProposal proposal;
    proposal.request = request;
    proposal.certificate.unresolved_port_count = static_cast<std::size_t>(
        std::count_if(request.ports.begin(), request.ports.end(),
            [](const MediumPortDecision& port) {
                return port.disposition == MediumPortDisposition::unresolved;
            }));
    const MediumRequestValidation validation =
        validate_medium_generation_request(request, model.materials);
    if (!validation.valid) {
        for (const std::string& code : validation.error_codes)
            proposal_error(proposal, code,
                           "Contained-medium request validation failed: " + code);
        proposal.decision_requests.push_back(medium_decision(
            request, validation.error_codes.empty() ? "invalid_request"
                                                     : validation.error_codes.front(),
            "The contained-medium request must be completed before geometry can be proposed."));
        return proposal;
    }
    if (request.fill_mode != MediumFillMode::full) {
        proposal_error(proposal, "medium_v1_requires_full_fill",
                       "Phase 5 v1 supports only full straight-pipe fill.");
        return proposal;
    }
    const auto requested_material = std::find_if(
        model.materials.begin(), model.materials.end(),
        [&](const MaterialDefinition& value) {
            return value.id == request.material_id;
        });
    if (requested_material == model.materials.end()
        || transport_material_number(*requested_material) <= 0) {
        proposal_error(proposal, "medium_material_missing_transport_id",
                       "The material ID must end in a positive MCNP material number, for example material:7.");
        return proposal;
    }
    if (request.host_component_ids.size() != 1) {
        proposal_error(proposal, "medium_v1_requires_one_host",
                       "Straight-pipe fill requires exactly one host component occurrence.");
        return proposal;
    }
    if (!request.internal_components_to_subtract.empty()) {
        proposal_error(proposal, "medium_v1_internal_subtraction_unsupported",
                       "Internal-component subtraction is deferred to contained-medium v2.");
        return proposal;
    }

    const ComponentInstance* instance = nullptr;
    for (const ComponentInstance& candidate : model.component_instances) {
        if (candidate.active && candidate.id == request.host_component_ids.front()) {
            instance = &candidate;
            break;
        }
    }
    if (!instance || instance->source_shape_id.empty()) {
        proposal_error(proposal, "medium_host_component_not_found",
                       "The requested active host component occurrence was not found.");
        return proposal;
    }
    proposal.host_component_id = instance->id;
    proposal.host_shape_id = instance->source_shape_id;

    TopoDS_Solid host;
    if (!root_solid_even_if_removed(model, proposal.host_shape_id, &host)) {
        proposal_error(proposal, "medium_host_solid_not_found",
                       "The host occurrence does not resolve to a source solid.",
                       proposal.host_shape_id);
        return proposal;
    }
    const double host_volume = shape_volume(host);
    const SolidTopology topology = detail::analyze_solid_topology(
        host, host_volume, options);
    if (!topology.pipe_annulus.candidate
        || topology.pipe_annulus.ports.size() != 2) {
        proposal_error(proposal, "medium_host_not_straight_pipe",
                       "The host is not a recognized straight full-annulus pipe.",
                       proposal.host_shape_id);
        return proposal;
    }

    std::unordered_set<std::string> expected_ports;
    for (const PipePortTopology& port : topology.pipe_annulus.ports)
        expected_ports.insert(port.id);
    if (request.ports.size() != expected_ports.size()) {
        proposal.certificate.unresolved_port_count +=
            expected_ports.size() > request.ports.size()
                ? expected_ports.size() - request.ports.size() : 0;
        proposal_error(proposal, "medium_request_incomplete_port_set",
                       "The request must explicitly decide both recognized lumen ports.",
                       proposal.host_shape_id);
        proposal.decision_requests.push_back(medium_decision(
            request, "incomplete_port_set",
            "Choose cap or external-interface behavior for both pipe ports."));
        return proposal;
    }
    for (const MediumPortDecision& decision : request.ports) {
        if (expected_ports.erase(decision.port_id) == 0) {
            proposal_error(proposal, "medium_request_unknown_port",
                           "The request references a port not present on the host pipe.",
                           proposal.host_shape_id);
            return proposal;
        }
        if (decision.disposition != MediumPortDisposition::cap_here
            && decision.disposition
                   != MediumPortDisposition::interface_to_external_model) {
            proposal_error(proposal, "medium_v1_port_connection_unsupported",
                           "Straight-pipe v1 accepts only cap_here or interface_to_external_model ports.",
                           proposal.host_shape_id);
            proposal.decision_requests.push_back(medium_decision(
                request, "unsupported_port_disposition",
                "Connected and domain-open ports require network generation in Phase 6."));
            return proposal;
        }
    }

    const PipePortTopology& first = topology.pipe_annulus.ports[0];
    const PipePortTopology& second = topology.pipe_annulus.ports[1];
    proposal.base = first.center;
    proposal.height = {second.center.x - first.center.x,
                       second.center.y - first.center.y,
                       second.center.z - first.center.z};
    proposal.radius = topology.pipe_annulus.inner_radius;
    proposal.top_radius = topology.pipe_annulus.top_inner_radius;
    proposal.conical = topology.pipe_annulus.conical;
    TopoDS_Solid lumen;
    try {
        lumen = make_lumen(proposal);
    } catch (const Standard_Failure&) {
        proposal_error(proposal, "medium_lumen_construction_failed",
                       "OCCT failed to construct the cylindrical lumen.",
                       proposal.host_shape_id);
        return proposal;
    }
    if (lumen.IsNull()) {
        proposal_error(proposal, "medium_lumen_construction_failed",
                       "The cylindrical lumen dimensions are invalid.",
                       proposal.host_shape_id);
        return proposal;
    }
    proposal.certificate.solid_valid = BRepCheck_Analyzer(lumen, true).IsValid();
    const SolidTopology lumen_topology = detail::analyze_solid_topology(
        lumen, shape_volume(lumen), options);
    proposal.certificate.closed = lumen_topology.shell_count > 0
        && lumen_topology.closed_shell_count == lumen_topology.shell_count
        && lumen_topology.boundary_edge_count == 0
        && lumen_topology.nonmanifold_edge_count == 0;
    proposal.certificate.leak_test_passed = proposal.certificate.closed;
    proposal.certificate.obstacle_subtraction_passed = true;
    proposal.certificate.phase_coverage_passed = true;
    proposal.certificate.protected_interface_passed = true;
    proposal.certificate.network_segment_count = 1;
    proposal.certificate.network_connected_component_count = 1;
    proposal.certificate.generated_volume = shape_volume(lumen);
    proposal.certificate.unresolved_port_count = 0;

    double total_overlap = 0.0;
    bool overlap_known = true;
    auto accumulate_overlap = [&](const TopoDS_Shape& material) {
        double overlap = 0.0;
        if (!common_volume(lumen, material, &overlap)) {
            overlap_known = false;
            return;
        }
        total_overlap += overlap;
    };
    accumulate_overlap(host);
    std::size_t solid_index = 0;
    for (TopExp_Explorer it(model.root, TopAbs_SOLID); it.More(); it.Next()) {
        const std::string id = detail::solid_id(++solid_index);
        if (id == proposal.host_shape_id
            || detail::contains_id(model.removed_solid_ids, id)) continue;
        accumulate_overlap(it.Current());
    }
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces)
        if (piece.active && piece.source_solid_id != proposal.host_shape_id)
            accumulate_overlap(piece.solid);
    for (const CadBooleanBody& body : model.cad_boolean_bodies)
        if (body.active && body.source_solid_id != proposal.host_shape_id)
            accumulate_overlap(body.materialized_solid);
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) continue;
        const TopoDS_Shape shape = detail::build_synthetic_shape(model, body);
        if (!shape.IsNull()) accumulate_overlap(shape);
    }
    proposal.certificate.material_overlap_volume = total_overlap;
    const double allowed_overlap = std::max(
        1.0e-12, proposal.certificate.generated_volume * 1.0e-10);
    proposal.certificate.overlap_test_passed = overlap_known
        && total_overlap <= allowed_overlap;
    if (!overlap_known) {
        proposal_error(proposal, "medium_overlap_unknown",
                       "OCCT could not certify lumen overlap against material geometry.",
                       proposal.host_shape_id);
    } else if (!proposal.certificate.overlap_test_passed) {
        std::ostringstream message;
        message << "Generated lumen intersects material by " << total_overlap
                << " volume units; allowed=" << allowed_overlap << ".";
        proposal_error(proposal, "medium_material_overlap",
                       message.str(), proposal.host_shape_id);
    }
    proposal.ok = proposal.certificate.solid_valid
        && proposal.certificate.closed
        && proposal.certificate.leak_test_passed
        && proposal.certificate.overlap_test_passed;
    return proposal;
}

namespace {

struct BuiltNetworkGeometry {
    std::vector<TopoDS_Solid> lumen_cells;
    std::vector<TopoDS_Solid> obstacle_cells;
    std::vector<TopoDS_Solid> primary_cells;
    std::vector<TopoDS_Solid> complement_cells;
    TopoDS_Solid materialized;
    TopoDS_Shape primary_materialized;
    TopoDS_Shape complement_materialized;
};

Vec3 subtract(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double length(Vec3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y
                     + value.z * value.z);
}

double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

std::string qualified_port_id(const std::string& host,
                              const std::string& local) {
    return host + "/" + local;
}

const ComponentInstance* find_instance(const CadModel& model,
                                       const std::string& id) {
    for (const ComponentInstance& instance : model.component_instances)
        if (instance.active && instance.id == id) return &instance;
    return nullptr;
}

bool extract_single_valid_solid(const TopoDS_Shape& shape,
                                TopoDS_Solid* result) {
    std::size_t count = 0;
    TopoDS_Solid found;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
        found = TopoDS::Solid(it.Current());
        ++count;
    }
    if (count != 1 || found.IsNull()
        || !BRepCheck_Analyzer(found, true).IsValid()) return false;
    *result = found;
    return true;
}

bool fuse_solids(const std::vector<TopoDS_Solid>& cells,
                 TopoDS_Solid* result) {
    if (cells.empty()) return false;
    TopoDS_Shape accumulated = cells.front();
    try {
        for (std::size_t i = 1; i < cells.size(); ++i) {
            BRepAlgoAPI_Fuse fuse;
            detail::build_binary_boolean(fuse, accumulated, cells[i]);
            if (!fuse.IsDone()) return false;
            accumulated = fuse.Shape();
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    return extract_single_valid_solid(accumulated, result);
}

bool unite_cells(const std::vector<TopoDS_Solid>& cells,
                 TopoDS_Shape* result) {
    if (cells.empty()) return false;
    TopoDS_Shape accumulated = cells.front();
    try {
        for (std::size_t i = 1; i < cells.size(); ++i) {
            BRepAlgoAPI_Fuse fuse;
            detail::build_binary_boolean(fuse, accumulated, cells[i]);
            if (!fuse.IsDone()) return false;
            accumulated = fuse.Shape();
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    std::size_t count = 0;
    for (TopExp_Explorer it(accumulated, TopAbs_SOLID); it.More(); it.Next()) {
        if (!BRepCheck_Analyzer(it.Current(), true).IsValid()) return false;
        ++count;
    }
    if (count == 0) return false;
    *result = accumulated;
    return true;
}

bool cut_obstacles(const TopoDS_Solid& lumen,
                   const std::vector<TopoDS_Solid>& obstacles,
                   TopoDS_Solid* result) {
    TopoDS_Shape current = lumen;
    try {
        for (const TopoDS_Solid& obstacle : obstacles) {
            BRepAlgoAPI_Cut cut;
            detail::build_binary_boolean(cut, current, obstacle);
            if (!cut.IsDone()) return false;
            current = cut.Shape();
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    return extract_single_valid_solid(current, result);
}

bool convex_cell(const TopoDS_Solid& solid,
                 const ConversionOptions& options) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    gp_Pnt interior;
    if (!detail::find_interior_point(solid, props, options.linear_tolerance,
                                     &interior)) return false;
    ConversionReport scratch;
    return detail::validate_convex_halfspace_cell(
        solid, interior, 900000, options, scratch, false, false, false);
}

bool halfspace_below(const Vec3& up, double level, TopoDS_Solid* solid) {
    try {
        const gp_Dir normal(up.x, up.y, up.z);
        const gp_Pnt origin(up.x * level, up.y * level, up.z * level);
        const TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(origin, normal));
        *solid = BRepPrimAPI_MakeHalfSpace(
            face, gp_Pnt(origin.X() - up.x, origin.Y() - up.y,
                         origin.Z() - up.z)).Solid();
        return !solid->IsNull();
    } catch (const Standard_Failure&) {
        return false;
    }
}

bool clip_cells(const std::vector<TopoDS_Solid>& source,
                const TopoDS_Solid& halfspace,
                bool keep_below,
                std::vector<TopoDS_Solid>* result) {
    result->clear();
    try {
        for (const TopoDS_Solid& cell : source) {
            TopoDS_Shape clipped;
            if (keep_below) {
                BRepAlgoAPI_Common common;
                detail::build_binary_boolean(common, cell, halfspace);
                if (!common.IsDone()) return false;
                clipped = common.Shape();
            } else {
                BRepAlgoAPI_Cut cut;
                detail::build_binary_boolean(cut, cell, halfspace);
                if (!cut.IsDone()) return false;
                clipped = cut.Shape();
            }
            for (TopExp_Explorer it(clipped, TopAbs_SOLID); it.More(); it.Next()) {
                const TopoDS_Solid solid = TopoDS::Solid(it.Current());
                if (shape_volume(solid) > 1.0e-15) result->push_back(solid);
            }
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    return !result->empty();
}

std::size_t solid_count(const TopoDS_Shape& shape) {
    std::size_t count = 0;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) ++count;
    return count;
}

bool face_has_inner_wire(const TopoDS_Face& face) {
    std::size_t count = 0;
    for (TopExp_Explorer it(face, TopAbs_WIRE); it.More(); it.Next()) ++count;
    return count > 1;
}

bool seeded_lumen(const CadModel& model,
                  const MediumGenerationRequest& request,
                  const ConversionOptions& options,
                  TopoDS_Solid* lumen,
                  std::vector<TopoDS_Solid>* analytic_cells,
                  std::vector<PipePortTopology>* ports,
                  std::string* host_shape_id,
                  std::string* error_code) {
    if (request.host_component_ids.size() != 1) {
        *error_code = "medium_seed_requires_one_host";
        return false;
    }
    const ComponentInstance* instance = find_instance(
        model, request.host_component_ids.front());
    TopoDS_Solid host;
    if (!instance || !root_solid_even_if_removed(
            model, instance->source_shape_id, &host)) {
        *error_code = "medium_host_component_not_found";
        return false;
    }
    *host_shape_id = instance->source_shape_id;
    std::size_t wanted_solid = 0, wanted_face = 0;
    std::optional<std::array<double, 3>> point_seed;
    for (const GeometricSeed& seed : request.seed_points_or_faces) {
        if (seed.kind == GeometricSeedKind::face
            && detail::parse_face_id(seed.entity_id, &wanted_solid,
                                     &wanted_face)) break;
        if (seed.kind == GeometricSeedKind::point) point_seed = seed.point;
    }
    std::size_t host_index = 0;
    if (!detail::parse_solid_id(instance->source_shape_id, &host_index)
        || (wanted_face != 0 && wanted_solid != host_index)) {
        *error_code = "medium_seed_face_not_on_host";
        return false;
    }

    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(host, TopAbs_FACE, faces);
    std::vector<bool> boundary(static_cast<std::size_t>(faces.Extent()), false);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face face = TopoDS::Face(faces(i));
        BRepAdaptor_Surface surface(face, false);
        boundary[static_cast<std::size_t>(i - 1)] =
            surface.GetType() == GeomAbs_Plane && face_has_inner_wire(face);
    }
    if (wanted_face == 0 && point_seed) {
        const TopoDS_Vertex point = BRepBuilderAPI_MakeVertex(gp_Pnt(
            (*point_seed)[0], (*point_seed)[1], (*point_seed)[2])).Vertex();
        double nearest = std::numeric_limits<double>::infinity();
        for (int index = 1; index <= faces.Extent(); ++index) {
            if (boundary[static_cast<std::size_t>(index - 1)]) continue;
            try {
                BRepExtrema_DistShapeShape distance(point, faces(index));
                distance.Perform();
                if (distance.IsDone() && distance.Value() < nearest) {
                    nearest = distance.Value();
                    wanted_face = static_cast<std::size_t>(index);
                }
            } catch (const Standard_Failure&) {
            }
        }
    }
    if (wanted_face == 0
        || wanted_face > static_cast<std::size_t>(faces.Extent())) {
        *error_code = "medium_seed_face_not_found";
        return false;
    }
    if (boundary[wanted_face - 1]) {
        *error_code = "medium_seed_face_is_boundary";
        return false;
    }
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(host, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    std::vector<bool> selected(static_cast<std::size_t>(faces.Extent()), false);
    std::vector<int> queue{static_cast<int>(wanted_face)};
    selected[wanted_face - 1] = true;
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const int current = queue[cursor];
        const TopoDS_Face face = TopoDS::Face(faces(current));
        for (TopExp_Explorer edge(face, TopAbs_EDGE); edge.More(); edge.Next()) {
            if (!edge_faces.Contains(edge.Current())) continue;
            const TopTools_ListOfShape& adjacent =
                edge_faces.FindFromKey(edge.Current());
            for (TopTools_ListIteratorOfListOfShape it(adjacent);
                 it.More(); it.Next()) {
                const int neighbor = faces.FindIndex(it.Value());
                if (neighbor <= 0 || selected[static_cast<std::size_t>(neighbor - 1)]
                    || boundary[static_cast<std::size_t>(neighbor - 1)]) continue;
                selected[static_cast<std::size_t>(neighbor - 1)] = true;
                queue.push_back(neighbor);
            }
        }
    }
    if (queue.empty()) {
        *error_code = "medium_seed_flood_empty";
        return false;
    }

    struct CylinderSupport {
        gp_Pnt origin;
        gp_Dir direction;
        double radius = 0.0;
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
    };
    std::vector<CylinderSupport> cylinder_supports;
    const double support_tolerance = std::max(
        options.linear_tolerance * 10.0, 1.0e-7);
    for (int index : queue) {
        const TopoDS_Face face = TopoDS::Face(faces(index));
        BRepAdaptor_Surface surface(face, false);
        if (surface.GetType() != GeomAbs_Cylinder) continue;
        const gp_Cylinder cylinder = surface.Cylinder();
        gp_Dir direction = cylinder.Axis().Direction();
        if (direction.X() < -1.0e-12
            || (std::abs(direction.X()) <= 1.0e-12
                && direction.Y() < -1.0e-12)
            || (std::abs(direction.X()) <= 1.0e-12
                && std::abs(direction.Y()) <= 1.0e-12
                && direction.Z() < 0.0)) direction.Reverse();
        gp_Pnt origin = cylinder.Axis().Location();
        const double along = gp_Vec(gp_Pnt(0.0, 0.0, 0.0), origin)
            .Dot(gp_Vec(direction));
        origin.Translate(gp_Vec(direction).Multiplied(-along));
        auto found = std::find_if(
            cylinder_supports.begin(), cylinder_supports.end(),
            [&](const CylinderSupport& value) {
                return std::abs(value.radius - cylinder.Radius())
                           <= support_tolerance
                    && std::abs(value.direction.Dot(direction))
                           >= 1.0 - 1.0e-9
                    && gp_Vec(value.origin, origin)
                           .Crossed(gp_Vec(direction)).Magnitude()
                           <= support_tolerance;
            });
        if (found == cylinder_supports.end()) {
            cylinder_supports.push_back(
                {origin, direction, cylinder.Radius()});
            found = std::prev(cylinder_supports.end());
        }
        for (TopExp_Explorer vertex(face, TopAbs_VERTEX);
             vertex.More(); vertex.Next()) {
            const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()));
            const double level = gp_Vec(found->origin, point)
                .Dot(gp_Vec(found->direction));
            found->low = std::min(found->low, level);
            found->high = std::max(found->high, level);
        }
    }

    BRepBuilderAPI_Sewing sewing(std::max(options.linear_tolerance * 10.0,
                                          1.0e-7));
    for (int index : queue)
        sewing.Add(TopoDS::Face(faces(index)).Reversed());

    std::size_t port_number = 0;
    std::unordered_map<std::string, std::size_t> port_id_counts;
    for (int boundary_index = 1; boundary_index <= faces.Extent();
         ++boundary_index) {
        if (!boundary[static_cast<std::size_t>(boundary_index - 1)]) continue;
        const TopoDS_Face boundary_face = TopoDS::Face(faces(boundary_index));
        for (TopExp_Explorer wire_it(boundary_face, TopAbs_WIRE);
             wire_it.More(); wire_it.Next()) {
            const TopoDS_Wire wire = TopoDS::Wire(wire_it.Current());
            bool touches_selected = false;
            for (TopExp_Explorer edge(wire, TopAbs_EDGE);
                 edge.More() && !touches_selected; edge.Next()) {
                if (!edge_faces.Contains(edge.Current())) continue;
                const TopTools_ListOfShape& adjacent =
                    edge_faces.FindFromKey(edge.Current());
                for (TopTools_ListIteratorOfListOfShape it(adjacent);
                     it.More(); it.Next()) {
                    const int face_index = faces.FindIndex(it.Value());
                    if (face_index > 0
                        && selected[static_cast<std::size_t>(face_index - 1)]) {
                        touches_selected = true;
                        break;
                    }
                }
            }
            if (!touches_selected) continue;
            TopoDS_Face cap;
            try {
                cap = BRepBuilderAPI_MakeFace(wire, true).Face();
            } catch (const Standard_Failure&) {
                *error_code = "medium_seed_port_cap_failed";
                return false;
            }
            sewing.Add(cap);
            GProp_GProps props;
            BRepGProp::SurfaceProperties(cap, props);
            const gp_Pnt center = props.CentreOfMass();
            BRepAdaptor_Surface surface(boundary_face, false);
            const gp_Dir normal = surface.Plane().Axis().Direction();
            const std::string boundary_id =
                "face:" + std::to_string(host_index) + "."
                    + std::to_string(boundary_index);
            const std::size_t duplicate = ++port_id_counts[boundary_id];
            const std::string port_id = duplicate == 1 ? boundary_id
                : boundary_id + "/wire:" + std::to_string(duplicate);
            ports->push_back({
                port_id,
                {center.X(), center.Y(), center.Z()},
                {normal.X(), normal.Y(), normal.Z()},
                std::sqrt(std::abs(props.Mass()) / std::acos(-1.0))});
            ++port_number;
        }
    }
    if (port_number < 2) {
        *error_code = "medium_seed_has_too_few_ports";
        return false;
    }
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    TopoDS_Shell shell;
    std::size_t shell_count = 0;
    for (TopExp_Explorer it(sewn, TopAbs_SHELL); it.More(); it.Next()) {
        shell = TopoDS::Shell(it.Current());
        ++shell_count;
    }
    if (shell_count != 1) {
        *error_code = "medium_seed_shell_not_closed";
        return false;
    }
    TopoDS_Solid result = BRepBuilderAPI_MakeSolid(shell).Solid();
    BRepLib::OrientClosedSolid(result);
    if (result.IsNull() || !BRepCheck_Analyzer(result, true).IsValid()
        || !(shape_volume(result) > 0.0)) {
        *error_code = "medium_seed_solid_invalid";
        return false;
    }
    std::vector<std::pair<double, TopoDS_Solid>> cylinder_candidates;
    for (const CylinderSupport& support : cylinder_supports) {
        if (!(support.high > support.low + support_tolerance)) continue;
        const gp_Pnt base = support.origin.Translated(
            gp_Vec(support.direction).Multiplied(support.low));
        cylinder_candidates.push_back({support.radius,
            BRepPrimAPI_MakeCylinder(
            gp_Ax2(base, support.direction), support.radius,
            support.high - support.low).Solid()});
    }
    auto matches_lumen = [&](const std::vector<TopoDS_Solid>& candidates) {
        TopoDS_Solid candidate_union;
        double common = 0.0;
        if (candidates.empty() || !fuse_solids(candidates, &candidate_union)
            || !common_volume(candidate_union, result, &common)) return false;
        const double expected = shape_volume(result);
        const double candidate_volume = shape_volume(candidate_union);
        const double tolerance = std::max(1.0e-10, expected * 1.0e-9);
        if (std::abs(expected - common) <= tolerance
            && std::abs(candidate_volume - common) <= tolerance) {
            *analytic_cells = candidates;
            return true;
        }
        return false;
    };
    std::vector<double> radii;
    for (const auto& candidate : cylinder_candidates)
        if (std::none_of(radii.begin(), radii.end(), [&](double radius) {
                return std::abs(radius - candidate.first)
                    <= support_tolerance;
            })) radii.push_back(candidate.first);
    std::sort(radii.begin(), radii.end());
    for (double radius : radii) {
        std::vector<TopoDS_Solid> same_radius;
        for (const auto& candidate : cylinder_candidates)
            if (std::abs(candidate.first - radius) <= support_tolerance)
                same_radius.push_back(candidate.second);
        if (matches_lumen(same_radius)) break;
    }
    *lumen = result;
    return true;
}

bool certified_positive_cells(const TopoDS_Solid& solid,
                              const ConversionOptions& options,
                              std::vector<TopoDS_Solid>* cells) {
    if (convex_cell(solid, options)) {
        cells->push_back(solid);
        return true;
    }
    CadModel scratch;
    scratch.root = solid;
    if (!shatter_solid(scratch, "solid:1", options)) return false;
    for (const DecomposedSolidPiece& piece : scratch.decomposed_pieces)
        if (piece.active) cells->push_back(piece.solid);
    return !cells->empty();
}

bool build_network_geometry(const CadModel& model,
                            const MediumGenerationProposal& proposal,
                            const ConversionOptions& options,
                            BuiltNetworkGeometry* built,
                            std::string* error_code) {
    if (!proposal.request.seed_points_or_faces.empty()) {
        TopoDS_Solid seeded;
        std::vector<TopoDS_Solid> analytic_cells;
        std::vector<PipePortTopology> ignored_ports;
        std::string ignored_host;
        if (!seeded_lumen(model, proposal.request, options, &seeded,
                          &analytic_cells,
                          &ignored_ports, &ignored_host, error_code)
            || (analytic_cells.empty()
                && !certified_positive_cells(seeded, options,
                                             &built->lumen_cells))) {
            if (error_code->empty())
                *error_code = "medium_seed_partition_failed";
            return false;
        }
        if (!analytic_cells.empty())
            built->lumen_cells = std::move(analytic_cells);
    }
    if (proposal.request.seed_points_or_faces.empty())
    for (const MediumNetworkSegment& segment : proposal.network.segments) {
        MediumGenerationProposal primitive;
        primitive.conical = segment.conical;
        primitive.base = {segment.base[0], segment.base[1], segment.base[2]};
        primitive.height = {segment.height[0], segment.height[1], segment.height[2]};
        primitive.radius = segment.radius;
        primitive.top_radius = segment.top_radius;
        const TopoDS_Solid lumen = make_lumen(primitive);
        if (lumen.IsNull()) {
            *error_code = "medium_network_segment_construction_failed";
            return false;
        }
        built->lumen_cells.push_back(lumen);
    }
    if (!fuse_solids(built->lumen_cells, &built->materialized)) {
        *error_code = "medium_network_not_single_connected_solid";
        return false;
    }
    for (const std::string& component_id
         : proposal.request.internal_components_to_subtract) {
        const ComponentInstance* instance = find_instance(model, component_id);
        TopoDS_Shape shape;
        if (!instance || !active_shape_for_id(model, instance->source_shape_id,
                                              &shape)
            || shape.ShapeType() != TopAbs_SOLID) {
            *error_code = "medium_obstacle_component_not_found";
            return false;
        }
        const TopoDS_Solid obstacle = TopoDS::Solid(shape);
        std::vector<TopoDS_Solid> obstacle_cells;
        if (!certified_positive_cells(obstacle, options, &obstacle_cells)) {
            *error_code = "medium_obstacle_decomposition_failed";
            return false;
        }
        built->obstacle_cells.insert(built->obstacle_cells.end(),
                                     obstacle_cells.begin(),
                                     obstacle_cells.end());
    }
    if (!built->obstacle_cells.empty()) {
        TopoDS_Solid cut;
        if (!cut_obstacles(built->materialized, built->obstacle_cells, &cut)) {
            *error_code = "medium_obstacle_subtraction_failed";
            return false;
        }
        built->materialized = cut;
    }
    built->primary_cells = built->lumen_cells;
    built->primary_materialized = built->materialized;
    if (proposal.request.fill_mode == MediumFillMode::full) return true;

    const auto& gravity = proposal.request.gravity_direction;
    Vec3 up{-gravity[0], -gravity[1], -gravity[2]};
    const double norm = length(up);
    up = {up.x / norm, up.y / norm, up.z / norm};
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (const gp_Pnt& point : detail::solid_vertices(built->materialized)) {
        const double projection = up.x * point.X() + up.y * point.Y()
            + up.z * point.Z();
        minimum = std::min(minimum, projection);
        maximum = std::max(maximum, projection);
    }
    if (!std::isfinite(minimum) || !(maximum > minimum)) {
        *error_code = "medium_partial_fill_invalid_extent";
        return false;
    }
    double level = proposal.request.fill_level.value_or(
        0.5 * (minimum + maximum));
    if (proposal.request.target_volume_fraction) {
        const double target = *proposal.request.target_volume_fraction;
        const double total = shape_volume(built->materialized);
        double low = minimum, high = maximum;
        for (int iteration = 0; iteration < 60; ++iteration) {
            const double trial = 0.5 * (low + high);
            TopoDS_Solid halfspace;
            if (!halfspace_below(up, trial, &halfspace)) return false;
            double volume = 0.0;
            if (!common_volume(built->materialized, halfspace, &volume)) return false;
            if (volume / total < target) low = trial;
            else high = trial;
        }
        level = 0.5 * (low + high);
    }
    TopoDS_Solid halfspace;
    if (!halfspace_below(up, level, &halfspace)
        || !clip_cells(built->lumen_cells, halfspace, true,
                       &built->primary_cells)
        || !clip_cells(built->lumen_cells, halfspace, false,
                       &built->complement_cells)
        || !unite_cells(built->primary_cells, &built->primary_materialized)
        || !unite_cells(built->complement_cells,
                        &built->complement_materialized)) {
        *error_code = "medium_partial_fill_partition_failed";
        return false;
    }
    if (!built->obstacle_cells.empty()) {
        auto cut_shape = [&](TopoDS_Shape* phase) {
            try {
                for (const TopoDS_Solid& obstacle : built->obstacle_cells) {
                    BRepAlgoAPI_Cut cut;
                    detail::build_binary_boolean(cut, *phase, obstacle);
                    if (!cut.IsDone()) return false;
                    *phase = cut.Shape();
                }
            } catch (const Standard_Failure&) {
                return false;
            }
            return solid_count(*phase) > 0;
        };
        if (!cut_shape(&built->primary_materialized)
            || !cut_shape(&built->complement_materialized)) {
            *error_code = "medium_phase_obstacle_subtraction_failed";
            return false;
        }
    }
    return true;
}

MediumGenerationProposal propose_pipe_network_medium(
    const CadModel& model,
    const MediumGenerationRequest& request,
    const ConversionOptions& options) {
    MediumGenerationProposal proposal;
    proposal.request = request;
    const MediumRequestValidation validation =
        validate_medium_generation_request(request, model.materials);
    if (!validation.valid) {
        for (const std::string& code : validation.error_codes)
            proposal_error(proposal, code,
                "Contained-medium v2 request validation failed: " + code);
        return proposal;
    }
    if (request.fill_mode == MediumFillMode::bounded_segment) {
        proposal_error(proposal, "medium_bounded_segment_requires_seed_bounds",
            "Bounded-segment fill requires two certified seed boundaries.");
        return proposal;
    }
    const auto primary_material = std::find_if(
        model.materials.begin(), model.materials.end(),
        [&](const MaterialDefinition& material) {
            return material.id == request.material_id;
        });
    if (primary_material == model.materials.end()
        || transport_material_number(*primary_material) <= 0) {
        proposal_error(proposal, "medium_material_missing_transport_id",
            "The primary material needs a positive MCNP material number.");
        return proposal;
    }
    if (request.fill_mode == MediumFillMode::level
        && primary_material->phase != MaterialPhase::liquid) {
        proposal_error(proposal, "medium_partial_primary_not_liquid",
            "The primary material of a level fill must be liquid.");
        return proposal;
    }

    std::unordered_map<std::string, std::size_t> port_index;
    std::unordered_map<std::string, std::string> local_alias;
    if (!request.seed_points_or_faces.empty()) {
        TopoDS_Solid lumen;
        std::vector<TopoDS_Solid> ignored_cells;
        std::vector<PipePortTopology> extracted_ports;
        std::string host_shape;
        std::string extraction_error;
        if (!seeded_lumen(model, request, options, &lumen, &ignored_cells,
                          &extracted_ports,
                          &host_shape, &extraction_error)) {
            proposal_error(proposal, extraction_error,
                "The selected lumen-side face did not produce a closed seeded channel.");
            return proposal;
        }
        MediumNetworkSegment segment;
        segment.id = "medium-segment:1";
        segment.host_component_id = request.host_component_ids.front();
        segment.source_shape_id = host_shape;
        segment.kind = extracted_ports.size() == 3 ? MediumJunctionKind::tee
            : extracted_ports.size() > 3 ? MediumJunctionKind::manifold
                                          : MediumJunctionKind::elbow;
        for (const PipePortTopology& source : extracted_ports) {
            MediumNetworkPort port;
            port.id = qualified_port_id(segment.host_component_id, source.id);
            port.host_component_id = segment.host_component_id;
            port.local_port_id = source.id;
            port.center = {{source.center.x, source.center.y, source.center.z}};
            port.normal = {{source.normal.x, source.normal.y, source.normal.z}};
            port.radius = source.radius;
            port_index.emplace(port.id, proposal.network.ports.size());
            local_alias.emplace(source.id, port.id);
            segment.port_ids.push_back(port.id);
            proposal.network.ports.push_back(std::move(port));
        }
        if (segment.kind == MediumJunctionKind::tee)
            ++proposal.network.tee_count;
        else if (segment.kind == MediumJunctionKind::manifold)
            ++proposal.network.manifold_count;
        else
            ++proposal.network.elbow_count;
        proposal.network.segments.push_back(std::move(segment));
    } else for (const std::string& host_id : request.host_component_ids) {
        const ComponentInstance* instance = find_instance(model, host_id);
        TopoDS_Solid host;
        if (!instance || instance->source_shape_id.empty()
            || !root_solid_even_if_removed(model, instance->source_shape_id,
                                           &host)) {
            proposal_error(proposal, "medium_host_component_not_found",
                "A requested network host occurrence was not found.", host_id);
            return proposal;
        }
        const SolidTopology topology = detail::analyze_solid_topology(
            host, shape_volume(host), options);
        if (!topology.pipe_annulus.candidate
            || topology.pipe_annulus.ports.size() != 2) {
            proposal_error(proposal, "medium_network_host_not_pipe_segment",
                "Each v2 network host must currently be a certified straight or conical annular segment.",
                instance->source_shape_id);
            return proposal;
        }
        MediumNetworkSegment segment;
        segment.id = "medium-segment:" + std::to_string(
            proposal.network.segments.size() + 1);
        segment.host_component_id = host_id;
        segment.source_shape_id = instance->source_shape_id;
        segment.conical = topology.pipe_annulus.conical;
        segment.kind = segment.conical ? MediumJunctionKind::reducer
                                       : MediumJunctionKind::straight;
        const PipePortTopology& first = topology.pipe_annulus.ports[0];
        const PipePortTopology& second = topology.pipe_annulus.ports[1];
        segment.base = {{first.center.x, first.center.y, first.center.z}};
        segment.height = {{second.center.x - first.center.x,
                           second.center.y - first.center.y,
                           second.center.z - first.center.z}};
        segment.radius = topology.pipe_annulus.inner_radius;
        segment.top_radius = topology.pipe_annulus.top_inner_radius;
        for (const PipePortTopology& source : topology.pipe_annulus.ports) {
            MediumNetworkPort port;
            port.id = qualified_port_id(host_id, source.id);
            port.host_component_id = host_id;
            port.local_port_id = source.id;
            port.center = {{source.center.x, source.center.y, source.center.z}};
            port.normal = {{source.normal.x, source.normal.y, source.normal.z}};
            port.radius = source.radius;
            port_index.emplace(port.id, proposal.network.ports.size());
            if (request.host_component_ids.size() == 1)
                local_alias.emplace(source.id, port.id);
            segment.port_ids.push_back(port.id);
            proposal.network.ports.push_back(std::move(port));
        }
        if (segment.conical) ++proposal.network.reducer_count;
        proposal.network.segments.push_back(std::move(segment));
    }
    if (proposal.network.segments.empty()) {
        proposal_error(proposal, "medium_network_has_no_segments",
            "No recognized host segment was supplied.");
        return proposal;
    }

    auto canonical = [&](const std::string& id) {
        const auto alias = local_alias.find(id);
        return alias == local_alias.end() ? id : alias->second;
    };
    for (const MediumPortDecision& decision : request.ports) {
        const std::string id = canonical(decision.port_id);
        const auto found = port_index.find(id);
        if (found == port_index.end()) {
            proposal_error(proposal, "medium_request_unknown_port",
                "A network port decision does not resolve to a host port.", id);
            return proposal;
        }
        MediumNetworkPort& port = proposal.network.ports[found->second];
        if (port.disposition != MediumPortDisposition::unresolved) {
            proposal_error(proposal, "medium_request_duplicate_port",
                "A network port has more than one decision.", id);
            return proposal;
        }
        port.disposition = decision.disposition;
        port.peer_port_id = canonical(decision.peer_port_id);
        port.valve_state = decision.valve_state;
    }
    for (const MediumNetworkPort& port : proposal.network.ports) {
        if (port.disposition == MediumPortDisposition::unresolved) {
            ++proposal.certificate.unresolved_port_count;
            proposal_error(proposal, "medium_request_incomplete_port_set",
                "Every occurrence-qualified network port needs a disposition.",
                port.id);
        }
    }
    if (proposal.certificate.unresolved_port_count) return proposal;

    const std::size_t n = proposal.network.segments.size();
    std::vector<std::size_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](std::size_t value) {
        while (parent[value] != value) {
            parent[value] = parent[parent[value]];
            value = parent[value];
        }
        return value;
    };
    auto segment_for_port = [&](const MediumNetworkPort& port) {
        for (std::size_t i = 0; i < proposal.network.segments.size(); ++i)
            if (proposal.network.segments[i].host_component_id
                == port.host_component_id) return i;
        return n;
    };
    std::unordered_set<std::string> counted_connections;
    for (const MediumNetworkPort& port : proposal.network.ports) {
        if (port.disposition != MediumPortDisposition::connect_to_port) continue;
        const auto peer_it = port_index.find(port.peer_port_id);
        if (peer_it == port_index.end() || port.peer_port_id == port.id) {
            proposal_error(proposal, "medium_network_peer_not_found",
                "A connected port peer is missing or self-referential.", port.id);
            return proposal;
        }
        const MediumNetworkPort& peer = proposal.network.ports[peer_it->second];
        if (peer.disposition != MediumPortDisposition::connect_to_port
            || peer.peer_port_id != port.id) {
            proposal_error(proposal, "medium_network_connection_not_reciprocal",
                "Connected port decisions must be reciprocal.", port.id);
            return proposal;
        }
        const Vec3 a{port.center[0], port.center[1], port.center[2]};
        const Vec3 b{peer.center[0], peer.center[1], peer.center[2]};
        const Vec3 an{port.normal[0], port.normal[1], port.normal[2]};
        const Vec3 bn{peer.normal[0], peer.normal[1], peer.normal[2]};
        const double tolerance = std::max(
            options.linear_tolerance * 100.0,
            std::min(port.radius, peer.radius) * 1.0e-6);
        if (length(subtract(a, b)) > tolerance
            || std::abs(port.radius - peer.radius) > tolerance
            || dot(an, bn) > -1.0 + std::max(1.0e-8,
                                             options.angular_tolerance * 100.0)) {
            proposal_error(proposal, "medium_network_connection_geometry_mismatch",
                "Connected ports do not share a compatible center, radius, and opposing normal.",
                port.id);
            return proposal;
        }
        const std::string key = std::min(port.id, peer.id) + "|"
            + std::max(port.id, peer.id);
        if (counted_connections.insert(key).second) {
            ++proposal.network.connection_count;
            const std::size_t left = segment_for_port(port);
            const std::size_t right = segment_for_port(peer);
            const std::size_t lr = root(left), rr = root(right);
            if (lr != rr) parent[rr] = lr;
            const auto& ls = proposal.network.segments[left];
            const auto& rs = proposal.network.segments[right];
            const Vec3 la{ls.height[0], ls.height[1], ls.height[2]};
            const Vec3 ra{rs.height[0], rs.height[1], rs.height[2]};
            const double cosine = std::abs(dot(la, ra) / (length(la) * length(ra)));
            if (cosine < 1.0 - 1.0e-8) ++proposal.network.elbow_count;
        }
    }
    std::unordered_set<std::size_t> roots;
    for (std::size_t i = 0; i < n; ++i) roots.insert(root(i));
    proposal.network.connected_component_count = roots.size();
    if (roots.size() != 1) {
        proposal_error(proposal, "medium_network_disconnected",
            "The selected hosts contain more than one connected circuit.");
        return proposal;
    }

    BuiltNetworkGeometry built;
    std::string build_error;
    if (!build_network_geometry(model, proposal, options, &built, &build_error)) {
        proposal_error(proposal, build_error,
            "OCCT could not construct and certify the requested medium network.");
        return proposal;
    }
    proposal.certificate.network_segment_count = n;
    proposal.certificate.network_connection_count =
        proposal.network.connection_count;
    proposal.certificate.network_connected_component_count = roots.size();
    // A proposal is one independently replayable circuit job. OCCT Boolean
    // work inside that job is deliberately serialized; project schedulers may
    // execute several circuit jobs concurrently, bounded by the request.
    proposal.certificate.worker_count_used = 1;
    proposal.certificate.solid_valid = true;
    proposal.certificate.closed = true;
    proposal.certificate.leak_test_passed = true;
    proposal.certificate.obstacle_subtraction_passed = true;
    proposal.certificate.protected_interface_passed = true;
    proposal.certificate.generated_volume = shape_volume(built.materialized);
    proposal.certificate.liquid_volume =
        request.fill_mode == MediumFillMode::level
            ? shape_volume(built.primary_materialized)
            : primary_material->phase == MaterialPhase::liquid
                ? proposal.certificate.generated_volume : 0.0;
    proposal.certificate.gas_volume = built.complement_materialized.IsNull()
        ? (primary_material->phase == MaterialPhase::gas
            ? proposal.certificate.generated_volume : 0.0)
        : shape_volume(built.complement_materialized);
    proposal.certificate.liquid_pocket_count =
        request.fill_mode == MediumFillMode::level
            ? solid_count(built.primary_materialized) : 0;
    proposal.certificate.gas_pocket_count = built.complement_materialized.IsNull()
        ? 0 : solid_count(built.complement_materialized);
    proposal.certificate.phase_coverage_delta = std::abs(
        proposal.certificate.generated_volume
        - proposal.certificate.liquid_volume
        - proposal.certificate.gas_volume);
    const double coverage_tolerance = std::max(
        1.0e-10, proposal.certificate.generated_volume * 1.0e-9);
    bool phase_geometry_exact = true;
    if (!built.complement_materialized.IsNull()) {
        double phase_overlap = 0.0;
        phase_geometry_exact = common_volume(
            built.primary_materialized, built.complement_materialized,
            &phase_overlap);
        proposal.certificate.phase_overlap_volume = phase_overlap;
        TopoDS_Shape phase_union;
        double covered = 0.0;
        bool union_ok = false;
        try {
            BRepAlgoAPI_Fuse fuse;
            detail::build_binary_boolean(
                fuse, built.primary_materialized,
                built.complement_materialized);
            if (fuse.IsDone()) {
                phase_union = fuse.Shape();
                union_ok = !phase_union.IsNull();
            }
        } catch (const Standard_Failure&) {
        }
        phase_geometry_exact = phase_geometry_exact
            && phase_overlap <= coverage_tolerance
            && union_ok
            && common_volume(phase_union, built.materialized, &covered)
            && std::abs(shape_volume(phase_union) - covered)
                   <= coverage_tolerance
            && std::abs(proposal.certificate.generated_volume - covered)
                   <= coverage_tolerance;
    }
    proposal.certificate.phase_coverage_passed = phase_geometry_exact
        && (request.fill_mode == MediumFillMode::full
            || proposal.certificate.phase_coverage_delta
                   <= coverage_tolerance);
    proposal.certificate.protected_interface_passed =
        proposal.certificate.phase_coverage_passed
        && std::all_of(proposal.network.ports.begin(),
            proposal.network.ports.end(), [](const MediumNetworkPort& port) {
                return port.disposition != MediumPortDisposition::unresolved
                    && !(port.valve_state == MediumValveState::closed
                         && port.disposition
                            == MediumPortDisposition::connect_to_port);
            });
    if (!request.allow_disconnected_pockets
        && (proposal.certificate.liquid_pocket_count > 1
            || proposal.certificate.gas_pocket_count > 1)) {
        proposal_error(proposal, "medium_disconnected_pockets_require_review",
            "Partial fill produced disconnected liquid or gas pockets.");
        return proposal;
    }

    std::unordered_set<std::string> excluded;
    for (const MediumNetworkSegment& segment : proposal.network.segments)
        excluded.insert(segment.source_shape_id);
    for (const std::string& component : request.internal_components_to_subtract)
        if (const ComponentInstance* instance = find_instance(model, component))
            excluded.insert(instance->source_shape_id);
    double total_overlap = 0.0;
    bool overlap_known = true;
    std::size_t index = 0;
    for (TopExp_Explorer it(model.root, TopAbs_SOLID); it.More(); it.Next()) {
        const std::string id = detail::solid_id(++index);
        if (excluded.count(id) || detail::contains_id(model.removed_solid_ids, id))
            continue;
        double overlap = 0.0;
        if (!common_volume(built.materialized, it.Current(), &overlap))
            overlap_known = false;
        else total_overlap += overlap;
    }
    auto accumulate_material_overlap = [&](const TopoDS_Shape& material) {
        double overlap = 0.0;
        if (!common_volume(built.materialized, material, &overlap))
            overlap_known = false;
        else total_overlap += overlap;
    };
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces)
        if (piece.active && !excluded.count(piece.source_solid_id))
            accumulate_material_overlap(piece.solid);
    for (const CadBooleanBody& body : model.cad_boolean_bodies)
        if (body.active && body.emit_as_cell
            && !excluded.count(body.source_solid_id))
            accumulate_material_overlap(body.materialized_solid);
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell || excluded.count(body.source_solid_id)) continue;
        const TopoDS_Shape shape = detail::build_synthetic_shape(model, body);
        if (!shape.IsNull()) accumulate_material_overlap(shape);
    }
    proposal.certificate.material_overlap_volume = total_overlap;
    proposal.certificate.overlap_test_passed = overlap_known
        && total_overlap <= std::max(
            1.0e-12, proposal.certificate.generated_volume * 1.0e-10);
    if (!proposal.certificate.overlap_test_passed) {
        proposal_error(proposal, overlap_known ? "medium_material_overlap"
                                               : "medium_overlap_unknown",
            "The connected medium is not certified disjoint from unselected material.");
        return proposal;
    }
    proposal.host_component_id = request.host_component_ids.front();
    proposal.host_shape_id = proposal.network.segments.front().source_shape_id;
    proposal.ok = proposal.certificate.solid_valid
        && proposal.certificate.closed
        && proposal.certificate.leak_test_passed
        && proposal.certificate.overlap_test_passed
        && proposal.certificate.obstacle_subtraction_passed
        && proposal.certificate.phase_coverage_passed
        && proposal.certificate.protected_interface_passed
        && proposal.certificate.unresolved_port_count == 0;
    return proposal;
}

} // namespace

bool apply_straight_pipe_medium(
    CadModel& model,
    const MediumGenerationProposal& proposal,
    bool user_approved,
    const ConversionOptions& options) {
    if (!user_approved) {
        model.diagnostics.push_back({DiagnosticSeverity::error,
            "medium_generation_not_authorized",
            "Contained-medium geometry requires explicit user or authorized-agent approval.",
            proposal.request.id});
        return false;
    }
    for (const DerivedRegionRecord& existing : model.derived_regions) {
        if (existing.request_id == proposal.request.id) {
            model.diagnostics.push_back({DiagnosticSeverity::error,
                "medium_request_already_applied",
                "This contained-medium request has already been applied.",
                proposal.request.id});
            return false;
        }
    }
    const MediumGenerationProposal fresh = propose_straight_pipe_medium(
        model, proposal.request, options);
    if (!fresh.ok) {
        model.diagnostics.insert(model.diagnostics.end(),
                                 fresh.diagnostics.begin(),
                                 fresh.diagnostics.end());
        return false;
    }
    const auto material = std::find_if(
        model.materials.begin(), model.materials.end(),
        [&](const MaterialDefinition& value) {
            return value.id == fresh.request.material_id;
        });
    if (material == model.materials.end()) return false;

    MetadataId metadata_id = fresh.request.metadata_id;
    const bool metadata_exists = std::any_of(
        model.metadata.begin(), model.metadata.end(),
        [&](const EntityMetadata& value) { return value.id == metadata_id; });
    if (metadata_id == kInvalidMetadataId || !metadata_exists) {
        EntityMetadata metadata;
        metadata.id = static_cast<MetadataId>(next_metadata_id(model));
        metadata.stable_name = model.strings.intern(fresh.request.id);
        metadata.display_name = model.strings.intern(
            "Contained medium " + fresh.request.id);
        metadata.description = model.strings.intern(
            "Generated full-fill fluid region inside a certified straight pipe lumen.");
        metadata.engineering_role = model.strings.intern("contained_medium");
        metadata.tags.push_back(model.strings.intern("generated_medium"));
        metadata.annotations.push_back({
            model.strings.intern("material_id"),
            model.strings.intern(fresh.request.material_id),
            AnnotationOrigin::csgify,
            MetadataReviewState::approved,
            true});
        metadata.annotations.push_back({
            model.strings.intern("host_component_id"),
            model.strings.intern(fresh.host_component_id),
            AnnotationOrigin::csgify,
            MetadataReviewState::approved,
            true});
        metadata_id = metadata.id;
        model.metadata.push_back(std::move(metadata));
    }

    if (request_needs_v2(fresh.request)) {
        BuiltNetworkGeometry built;
        std::string build_error;
        if (!build_network_geometry(model, fresh, options, &built, &build_error)) {
            model.diagnostics.push_back({DiagnosticSeverity::error, build_error,
                "Fresh contained-medium geometry could not be materialized.",
                fresh.request.id});
            return false;
        }
        CadBooleanBody body;
        body.id = "cadbool:" + std::to_string(model.cad_boolean_bodies.size() + 1);
        body.label = body.id + "/contained_medium/" + fresh.request.id;
        body.source_solid_id = fresh.host_shape_id;
        body.materialized_solid = built.primary_materialized;
        body.positive_cells = built.primary_cells;
        body.negative_cells = built.obstacle_cells;
        body.role = "contained_medium";
        body.request_id = fresh.request.id;
        body.material_id = transport_material_number(*material);
        body.density = material->density;
        model.cad_boolean_bodies.push_back(std::move(body));

        if (!built.complement_materialized.IsNull()
            && !fresh.request.complement_material_id.empty()) {
            const auto complement = std::find_if(
                model.materials.begin(), model.materials.end(),
                [&](const MaterialDefinition& value) {
                    return value.id == fresh.request.complement_material_id;
                });
            if (complement == model.materials.end()
                || transport_material_number(*complement) <= 0) {
                model.cad_boolean_bodies.pop_back();
                model.diagnostics.push_back({DiagnosticSeverity::error,
                    "medium_complement_material_missing_transport_id",
                    "The gas complement needs a positive MCNP material number.",
                    fresh.request.id});
                return false;
            }
            CadBooleanBody gas;
            gas.id = "cadbool:"
                + std::to_string(model.cad_boolean_bodies.size() + 1);
            gas.label = gas.id + "/contained_medium_gas/" + fresh.request.id;
            gas.source_solid_id = fresh.host_shape_id;
            gas.materialized_solid = built.complement_materialized;
            gas.positive_cells = built.complement_cells;
            gas.negative_cells = built.obstacle_cells;
            gas.role = "contained_medium_complement";
            gas.request_id = fresh.request.id;
            gas.material_id = transport_material_number(*complement);
            gas.density = complement->density;
            model.cad_boolean_bodies.push_back(std::move(gas));
        }
    } else {
        SyntheticBody body;
        body.id = "synthetic:" + std::to_string(model.synthetic_bodies.size() + 1);
        body.label = body.id + "/contained_medium/" + fresh.request.id;
        body.kind = fresh.conical ? SyntheticBodyKind::cone
                                  : SyntheticBodyKind::cylinder;
        body.base = fresh.base;
        body.height = fresh.height;
        body.radius = fresh.radius;
        body.top_radius = fresh.top_radius;
        body.role = "contained_medium";
        body.host_solid_id = fresh.host_shape_id;
        body.source_solid_id = fresh.host_shape_id;
        body.material_id = transport_material_number(*material);
        body.density = material->density;
        body.emit_as_cell = true;
        model.synthetic_bodies.push_back(std::move(body));
    }

    MediumGenerationRequest stored_request = fresh.request;
    stored_request.metadata_id = metadata_id;
    const auto staged = std::find_if(
        model.medium_requests.begin(), model.medium_requests.end(),
        [&](const MediumGenerationRequest& value) {
            return value.id == stored_request.id;
        });
    if (staged == model.medium_requests.end())
        model.medium_requests.push_back(std::move(stored_request));
    else
        *staged = std::move(stored_request);
    DerivedRegionRecord region;
    region.region_id = "region:medium:"
        + std::to_string(model.derived_regions.size() + 1);
    region.request_id = fresh.request.id;
    region.operation = model.strings.intern(
        request_needs_v2(fresh.request)
            ? "generate_connected_medium_network"
            : "generate_straight_pipe_medium");
    region.host_components = fresh.request.host_component_ids;
    region.material_id = fresh.request.material_id;
    region.subtracted_components =
        fresh.request.internal_components_to_subtract;
    region.certificate = fresh.certificate;
    region.user_approved = true;
    region.metadata_id = metadata_id;
    for (const MediumPortDecision& port : fresh.request.ports) {
        if (port.disposition == MediumPortDisposition::cap_here
            || port.valve_state == MediumValveState::closed)
            region.capped_ports.push_back(port.port_id);
        else region.connected_ports.push_back(port.port_id);
    }
    model.derived_regions.push_back(std::move(region));
    model.diagnostics.push_back({DiagnosticSeverity::info,
        "contained_medium_generated",
        request_needs_v2(fresh.request)
            ? "Generated and certified a connected/partitioned medium region."
            : "Generated and certified a full-fill straight-pipe medium region.",
        fresh.host_shape_id});
    return true;
}

} // namespace csgify
