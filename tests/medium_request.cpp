#include "engine_model.hpp"
#include "fnv.hpp"
#include "csgify/autonomy.hpp"
#include "csgify/session.hpp"
#include "lua_runner.hpp"
#include "recipe.hpp"
#include "viewer_controller.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <TopoDS_Compound.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>

#include <unistd.h>

namespace {
bool contains(const csgify::MediumRequestValidation& result, const char* code) {
    return std::find(result.error_codes.begin(), result.error_codes.end(), code)
        != result.error_codes.end();
}
} // namespace

int main() {
    csgify::MaterialDefinition water;
    water.id = "material:7";
    water.phase = csgify::MaterialPhase::liquid;

    csgify::MediumGenerationRequest valid;
    valid.id = "medium:loop-a";
    valid.host_component_ids = {"inst:pipe-a"};
    valid.material_id = water.id;
    valid.ports = {
        {"port:in", csgify::MediumPortDisposition::cap_here, {}, 0},
        {"port:out", csgify::MediumPortDisposition::interface_to_external_model,
         {}, 0},
    };
    const csgify::MediumRequestValidation accepted =
        csgify::validate_medium_generation_request(valid, {water});
    assert(accepted.valid);

    valid.ports.push_back(
        {"port:unknown", csgify::MediumPortDisposition::unresolved, {}, 0});
    const csgify::MediumRequestValidation unresolved =
        csgify::validate_medium_generation_request(valid, {water});
    assert(!unresolved.valid);
    assert(contains(unresolved, "medium_request_unresolved_port"));

    csgify::MediumGenerationRequest partial;
    partial.id = "medium:partial";
    partial.seed_points_or_faces.push_back({});
    partial.material_id = water.id;
    partial.fill_mode = csgify::MediumFillMode::level;
    partial.gravity_direction = {{0.0, 0.0, 0.0}};
    const csgify::MediumRequestValidation invalid_partial =
        csgify::validate_medium_generation_request(partial, {water});
    assert(contains(invalid_partial, "medium_request_invalid_gravity"));
    assert(contains(invalid_partial, "medium_request_missing_fill_level"));

    partial.gravity_direction = {{0.0, 0.0, -1.0}};
    partial.target_volume_fraction = 0.5;
    const csgify::MediumRequestValidation accepted_partial =
        csgify::validate_medium_generation_request(partial, {water});
    assert(accepted_partial.valid);

    const TopoDS_Shape pipe = BRepAlgoAPI_Cut(
        BRepPrimAPI_MakeCylinder(10.0, 40.0).Shape(),
        BRepPrimAPI_MakeCylinder(6.0, 40.0).Shape()).Shape();
    csgify::CadModel model;
    model.root = pipe;
    model.materials.push_back(water);
    csgify::ComponentDefinition definition;
    definition.id = "def:pipe";
    definition.geometry_digest = "pipe-digest";
    definition.geometry_digest_verified = true;
    model.component_definitions.push_back(definition);
    csgify::ComponentInstance instance;
    instance.id = "inst:pipe-a";
    instance.definition_id = definition.id;
    instance.source_shape_id = "solid:1";
    model.component_instances.push_back(instance);

    csgify::MediumGenerationRequest full;
    full.id = "medium:pipe-a-water";
    full.host_component_ids = {instance.id};
    full.material_id = water.id;
    full.ports = {
        {"port:1", csgify::MediumPortDisposition::cap_here, {}, 0},
        {"port:2",
         csgify::MediumPortDisposition::interface_to_external_model, {}, 0},
    };
    csgify::ConversionOptions options;
    options.check_overlaps = false;
    const csgify::MediumGenerationProposal proposal =
        csgify::propose_straight_pipe_medium(model, full, options);
    assert(proposal.ok);
    assert(proposal.certificate.solid_valid);
    assert(proposal.certificate.closed);
    assert(proposal.certificate.leak_test_passed);
    assert(proposal.certificate.overlap_test_passed);
    assert(proposal.certificate.unresolved_port_count == 0);
    const double expected_volume = std::acos(-1.0) * 6.0 * 6.0 * 40.0;
    assert(std::abs(proposal.certificate.generated_volume - expected_volume)
           / expected_volume < 1.0e-10);

    csgify::CadModel unauthorized = model;
    assert(!csgify::apply_straight_pipe_medium(
        unauthorized, proposal, false, options));
    assert(unauthorized.synthetic_bodies.empty());
    assert(csgify::apply_straight_pipe_medium(model, proposal, true, options));
    assert(model.synthetic_bodies.size() == 1);
    assert(model.synthetic_bodies.front().role == "contained_medium");
    assert(model.synthetic_bodies.front().material_id == 7);
    assert(model.medium_requests.size() == 1);
    assert(model.derived_regions.size() == 1);
    assert(model.derived_regions.front().certificate.overlap_test_passed);
    assert(model.derived_regions.front().capped_ports.size() == 1);
    assert(model.derived_regions.front().connected_ports.size() == 1);
    assert(!model.metadata.empty());

    csgify::MediumGenerationRequest incomplete = full;
    incomplete.id = "medium:incomplete";
    incomplete.ports.pop_back();
    const csgify::MediumGenerationProposal incomplete_proposal =
        csgify::propose_straight_pipe_medium(unauthorized, incomplete, options);
    assert(!incomplete_proposal.ok);
    assert(!incomplete_proposal.decision_requests.empty());

    TopoDS_Compound obstructed_shape;
    BRep_Builder builder;
    builder.MakeCompound(obstructed_shape);
    builder.Add(obstructed_shape, pipe);
    builder.Add(obstructed_shape,
                BRepPrimAPI_MakeCylinder(2.0, 10.0).Shape());
    csgify::CadModel obstructed = unauthorized;
    obstructed.root = obstructed_shape;
    const csgify::MediumGenerationProposal obstructed_proposal =
        csgify::propose_straight_pipe_medium(obstructed, full, options);
    assert(!obstructed_proposal.ok);
    assert(obstructed_proposal.certificate.material_overlap_volume > 0.0);

    const gp_Pnt cone_base(3.0, -4.0, 5.0);
    const gp_Dir cone_axis(1.0, 2.0, 3.0);
    const gp_Ax2 cone_placement(cone_base, cone_axis);
    const TopoDS_Shape conical_pipe = BRepAlgoAPI_Cut(
        BRepPrimAPI_MakeCone(cone_placement, 8.0, 5.0, 30.0).Shape(),
        BRepPrimAPI_MakeCone(cone_placement, 5.0, 2.0, 30.0).Shape()).Shape();
    csgify::CadModel conical_model;
    conical_model.root = conical_pipe;
    conical_model.materials.push_back(water);
    csgify::ComponentDefinition conical_definition;
    conical_definition.id = "def:conical-pipe";
    conical_definition.geometry_digest = "conical-pipe-digest";
    conical_definition.geometry_digest_verified = true;
    conical_model.component_definitions.push_back(conical_definition);
    csgify::ComponentInstance conical_instance;
    conical_instance.id = "inst:conical-pipe";
    conical_instance.definition_id = conical_definition.id;
    conical_instance.source_shape_id = "solid:1";
    conical_model.component_instances.push_back(conical_instance);
    csgify::MediumGenerationRequest conical_request = full;
    conical_request.id = "medium:conical-water";
    conical_request.host_component_ids = {conical_instance.id};
    const csgify::MediumGenerationProposal conical_proposal =
        csgify::propose_straight_pipe_medium(
            conical_model, conical_request, options);
    assert(conical_proposal.ok);
    assert(conical_proposal.conical);
    assert(std::abs(conical_proposal.radius - 5.0) < 1.0e-7);
    assert(std::abs(conical_proposal.top_radius - 2.0) < 1.0e-7);
    const double conical_expected = std::acos(-1.0) * 30.0 / 3.0
        * (5.0 * 5.0 + 5.0 * 2.0 + 2.0 * 2.0);
    assert(std::abs(conical_proposal.certificate.generated_volume
                    - conical_expected) / conical_expected < 1.0e-10);
    assert(csgify::apply_straight_pipe_medium(
        conical_model, conical_proposal, true, options));
    assert(conical_model.synthetic_bodies.size() == 1);
    assert(conical_model.synthetic_bodies.front().kind
           == csgify::SyntheticBodyKind::cone);
    assert(std::abs(conical_model.synthetic_bodies.front().top_radius - 2.0)
           < 1.0e-7);
    assert(csgify::decompose_solid_pipe_annulus(
        conical_model, "solid:1", options));
    const csgify::ConversionResult conical_conversion =
        csgify::convert_model(conical_model, options);
    const double conical_outer_volume = std::acos(-1.0) * 30.0 / 3.0
        * (8.0 * 8.0 + 8.0 * 5.0 + 5.0 * 5.0);
    assert(conical_conversion.report.ok());
    assert(std::abs(conical_conversion.report.total_volume
                    - conical_outer_volume) / conical_outer_volume < 1.0e-10);

    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path()
        / ("csgify-medium-v1-" + std::to_string(::getpid()) + ".step");
    const std::filesystem::path output_step =
        temporary.parent_path()
        / ("csgify-medium-v1-output-" + std::to_string(::getpid()) + ".step");
    const std::filesystem::path output_mcnp =
        temporary.parent_path()
        / ("csgify-medium-v1-" + std::to_string(::getpid()) + ".i");
    const std::filesystem::path decision_log =
        temporary.parent_path()
        / ("csgify-medium-v1-" + std::to_string(::getpid()) + ".jsonl");
    const std::filesystem::path recipe =
        temporary.parent_path()
        / ("csgify-medium-v1-" + std::to_string(::getpid()) + ".lua");
    const std::filesystem::path autonomy_queue_path =
        temporary.parent_path()
        / ("csgify-medium-autonomy-" + std::to_string(::getpid()) + ".json");
    STEPControl_Writer writer;
    assert(writer.Transfer(pipe, STEPControl_AsIs) == IFSelect_RetDone);
    assert(writer.Write(temporary.c_str()) == IFSelect_RetDone);

    csgify::SessionPtr session = csgify::create_session(options);
    csgify::attach_decision_log(
        *session, decision_log.string(), temporary.string());
    const csgify::DocumentId document = csgify::load_step(
        *session, temporary.string());
    csgify::CommandResult inspected = csgify::analyze(
        *session, document, csgify::ReportIdMode::local);
    assert(!inspected.report.component_instances.empty());
    assert(csgify::define_material(
        *session, document, water, csgify::ReportIdMode::local).ok);
    csgify::MediumGenerationRequest session_request = full;
    session_request.id = "medium:session-water";
    session_request.host_component_ids = {
        inspected.report.component_instances.front().id};
    csgify::MediumGenerationRequest staged_request = session_request;
    staged_request.ports[1].disposition =
        csgify::MediumPortDisposition::unresolved;
    assert(csgify::stage_contained_medium_request(
        *session, document, staged_request,
        csgify::ReportIdMode::local).ok);
    csgify::MediumGenerationRequest restored_staged_request;
    assert(csgify::find_contained_medium_request(
        *session, document, staged_request.id, &restored_staged_request));
    assert(restored_staged_request.id == staged_request.id);
    assert(restored_staged_request.material_id == staged_request.material_id);
    assert(restored_staged_request.ports.size() == staged_request.ports.size());
    assert(restored_staged_request.ports[1].disposition
           == csgify::MediumPortDisposition::unresolved);
    assert(!csgify::find_contained_medium_request(
        *session, document, "medium:missing", &restored_staged_request));
    csgify::CommandResult staged_report = csgify::analyze(
        *session, document, csgify::ReportIdMode::local);
    assert(staged_report.report.medium_requests.size() == 1);
    assert(std::any_of(
        staged_report.report.decision_requests.begin(),
        staged_report.report.decision_requests.end(),
        [](const csgify::DecisionRequest& decision) {
            return decision.kind == "contained_medium_port"
                && decision.reason == "unresolved_medium_port";
        }));
    const csgify::MediumGenerationProposal session_proposal =
        csgify::propose_contained_medium(
            *session, document, session_request,
            csgify::ReportIdMode::local);
    assert(session_proposal.ok);
    csgify::CommandResult applied = csgify::apply_contained_medium(
        *session, document, session_request, true,
        csgify::ReportIdMode::local);
    assert(applied.ok);
    assert(applied.report.medium_requests.size() == 1);
    assert(applied.report.derived_regions.size() == 1);
    assert(csgify::decompose_pipe_annulus(
        *session, "solid:1", csgify::ReportIdMode::local).ok);
    assert(csgify::export_document_step(
        *session, document, output_step.string(),
        csgify::ReportIdMode::local).ok);
    assert(csgify::emit_document(
        *session, document, output_mcnp.string(),
        csgify::ReportIdMode::local).ok);
    assert(std::filesystem::file_size(output_step) > 0);
    assert(std::filesystem::file_size(output_mcnp) > 0);
    csgify::CompactRecipeOptions compact_options;
    compact_options.preserve_step_exports = true;
    assert(csgify::compact_log_to_recipe(
        decision_log.string(), recipe.string(), compact_options) == 0);
    assert(csgify::run_lua_script(recipe.string()) == 0);

    csgify::SessionPtr autonomous_session = csgify::create_session(options);
    const csgify::DocumentId autonomous_document = csgify::load_step(
        *autonomous_session, temporary.string());
    const csgify::CommandResult autonomous_inspection = csgify::analyze(
        *autonomous_session, autonomous_document,
        csgify::ReportIdMode::local);
    assert(csgify::define_material(
        *autonomous_session, autonomous_document, water,
        csgify::ReportIdMode::local).ok);
    const std::string autonomous_action_json =
        std::string("{\"schema\":\"csgify.autonomy-actions\","
        "\"schema_version\":2,\"request_id\":\"request:generate-water\","
        "\"source_digest\":\"")
        + csgify::fnv1a128_file_hex(temporary.string())
        + "\",\"expected_revision\":0,\"actor\":\"llm:test\",\"actions\":[{"
        "\"id\":\"generate-water\",\"op\":\"apply_contained_medium\","
        "\"rationale\":\"configured cooling water\",\"medium_request\":{"
        "\"id\":\"medium:auto-water\",\"material_id\":\"material:7\","
        "\"fill_mode\":\"full\",\"require_exact_geometry\":true,"
        "\"host_component_ids\":[\""
        + autonomous_inspection.report.component_instances.front().id
        + "\"],\"ports\":[{\"port_id\":\"port:1\","
          "\"disposition\":\"cap_here\"},{\"port_id\":\"port:2\","
          "\"disposition\":\"interface_to_external_model\"}]}}]}";
    const csgify::AutonomyRequest autonomous_request =
        csgify::parse_autonomy_request_json(autonomous_action_json);
    assert(autonomous_request.actions.size() == 1);
    assert(autonomous_request.actions.front().medium_request.has_value());
    csgify::AutonomyPolicy denied_policy;
    denied_policy.mode = csgify::AutonomyMode::automatic;
    csgify::AutonomyQueue denied_queue = csgify::evaluate_autonomy_queue(
        *autonomous_session, autonomous_document,
        autonomous_request.actions, denied_policy);
    assert(denied_queue.human_review_count == 1);
    csgify::AutonomyPolicy autonomous_policy = denied_policy;
    autonomous_policy.allow_contained_medium_generation = true;
    autonomous_policy.contained_medium_materials.push_back(water);
    csgify::AutonomyQueue autonomous_queue = csgify::evaluate_autonomy_queue(
        *autonomous_session, autonomous_document,
        autonomous_request.actions, autonomous_policy);
    assert(autonomous_queue.auto_apply_count == 1);
    csgify::execute_autonomy_queue(
        *autonomous_session, autonomous_queue, autonomous_policy);
    assert(autonomous_queue.applied_count == 1);
    assert(autonomous_queue.items.front().evidence.mutation_applied);
    autonomous_queue.source_path = temporary.string();
    assert(csgify::write_autonomy_queue_json(
        autonomous_queue, autonomy_queue_path.string()) == 0);
    const csgify::AutonomyQueue restored_queue =
        csgify::read_autonomy_queue_json(autonomy_queue_path.string());
    assert(restored_queue.policy.allow_contained_medium_generation);
    assert(restored_queue.policy.contained_medium_materials.size() == 1);
    assert(restored_queue.items.front().proposal.medium_request.has_value());

    csgify::SessionPtr viewer_session = csgify::create_session(options);
    const csgify::DocumentId viewer_document = csgify::load_step(
        *viewer_session, temporary.string());
    const csgify::CommandResult viewer_inspection = csgify::analyze(
        *viewer_session, viewer_document, csgify::ReportIdMode::local);
    assert(csgify::define_material(
        *viewer_session, viewer_document, water,
        csgify::ReportIdMode::local).ok);
    csgify::MediumGenerationRequest viewer_request = full;
    viewer_request.id = "medium:viewer-water";
    viewer_request.host_component_ids = {
        viewer_inspection.report.component_instances.front().id};
    csgify::ViewerReviewContext viewer_review;
    viewer_review.kind = "contained_medium";
    viewer_review.review_id = "review:viewer-water";
    viewer_review.action = "apply_contained_medium";
    viewer_review.target_id = viewer_document;
    viewer_review.medium_request = viewer_request;
    viewer_review.medium_proposal = csgify::propose_contained_medium(
        *viewer_session, viewer_document, viewer_request,
        csgify::ReportIdMode::local);
    viewer_review.has_medium = viewer_review.medium_proposal.ok;
    assert(viewer_review.has_medium);
    csgify::ViewerController viewer_controller(
        *viewer_session, viewer_document, viewer_review);
    const csgify::ViewerAction viewer_action = viewer_controller.accept_active();
    assert(viewer_action.ok && viewer_action.mutated);
    assert(viewer_action.op == "apply_contained_medium");
    assert(csgify::last_report(*viewer_session).derived_regions.size() == 1);

    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    std::filesystem::remove(output_step, ec);
    std::filesystem::remove(output_mcnp, ec);
    std::filesystem::remove(decision_log, ec);
    std::filesystem::remove(recipe, ec);
    std::filesystem::remove(autonomy_queue_path, ec);
    return 0;
}
