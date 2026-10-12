#include "qt/cad_job_controller.hpp"
#include "recipe.hpp"
#include "viewer_controller.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    QCoreApplication application(argc, argv);

    const std::filesystem::path log =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-load-controller-" + std::to_string(::getpid()) + ".json");
    const std::filesystem::path emitted =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-emission-controller-" + std::to_string(::getpid()) + ".i");
    const std::filesystem::path exported =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-step-controller-" + std::to_string(::getpid()) + ".step");
    const std::filesystem::path recipe =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-reconstruct-" + std::to_string(::getpid()) + ".lua");
    const std::filesystem::path recipe_json = recipe.parent_path()
        / (recipe.stem().string() + ".json");
    const std::filesystem::path replay_report =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-reconstruct-report-" + std::to_string(::getpid()) + ".json");
    const std::filesystem::path changed_source =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-reconstruct-changed-" + std::to_string(::getpid()) + ".step");
    const std::filesystem::path incompatible_recipe =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-reconstruct-incompatible-" + std::to_string(::getpid()) + ".json");
    const std::filesystem::path oversized_context =
        std::filesystem::temp_directory_path()
        / ("csgify-qt-context-limit-" + std::to_string(::getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(log, error);
    std::filesystem::remove(emitted, error);
    std::filesystem::remove(exported, error);
    std::filesystem::remove(recipe, error);
    std::filesystem::remove(recipe_json, error);
    std::filesystem::remove(replay_report, error);
    std::filesystem::remove(changed_source, error);
    std::filesystem::remove(incompatible_recipe, error);
    std::filesystem::remove(oversized_context, error);

    csgify::CadJobController controller;
    std::vector<csgify::JobProgressEvent> events;
    csgify::LoadedDocumentPtr loaded;
    QString failure;
    QObject::connect(&controller, &csgify::CadJobController::progress,
                     [&events](const csgify::JobProgressEvent& event) {
                         events.push_back(event);
                     });
    QObject::connect(&controller, &csgify::CadJobController::documentLoaded,
                     [&loaded](const csgify::LoadedDocumentPtr& value) {
                         loaded = value;
                     });
    QObject::connect(&controller, &csgify::CadJobController::failed,
                     [&failure](const QString&, const QString& message) {
                         failure = message;
                     });

    QEventLoop loop;
    QObject::connect(&controller, &csgify::CadJobController::activeChanged,
                     [&loop](bool active) {
                         if (!active) loop.quit();
                     });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    csgify::ConversionOptions load_options;
    load_options.heal_shape = false;
    load_options.unit_override = "cm";
    assert(controller.startDocumentLoad(argv[1], log.string(), load_options));
    assert(controller.active());
    timeout.start(30000);
    loop.exec();

    assert(!timeout.isActive() || !controller.active());
    assert(failure.isEmpty());
    assert(loaded);
    assert(loaded->session);
    assert(!loaded->document_id.empty());
    assert(loaded->analysis.ok);
    assert(loaded->analysis.report.solids_seen == 4);
    assert(loaded->analysis.report.length_unit == "cm");
    assert(std::filesystem::is_regular_file(log));

    csgify::AutonomyContextOptions bounded_context;
    bounded_context.target_id = "solid:1";
    bounded_context.max_serialized_bytes = 64;
    assert(csgify::write_autonomy_context_json(
        *loaded->session, loaded->document_id, {}, bounded_context,
        oversized_context.string()) != 0);
    assert(!std::filesystem::exists(oversized_context));

    assert(events.size() >= 4);
    const std::string job_id = events.front().job_id;
    assert(!job_id.empty());
    assert(events.front().phase == csgify::JobPhase::loading_geometry);
    bool saw_analysis = false;
    std::uint64_t previous_sequence = 0;
    for (const csgify::JobProgressEvent& event : events) {
        assert(event.job_id == job_id);
        assert(event.sequence > previous_sequence);
        previous_sequence = event.sequence;
        saw_analysis = saw_analysis
            || event.phase == csgify::JobPhase::analyzing_solids;
    }
    assert(saw_analysis);
    assert(events.back().phase == csgify::JobPhase::finished);
    assert(events.back().outcome == csgify::JobOutcome::committed);

    // Agent evidence is generated on the geometry worker. A second controller
    // cannot concurrently touch the same Session, and the reservation is
    // released when context generation reaches its safe boundary.
    csgify::AgentContextSnapshotPtr agent_context;
    QObject::connect(&controller, &csgify::CadJobController::agentContextCompleted,
        [&agent_context](const csgify::AgentContextSnapshotPtr& value) {
            agent_context = value;
        });
    csgify::AutonomyContextOptions context_options;
    context_options.project_id = "qt-agent-test";
    context_options.intended_analysis = "geometry review";
    context_options.target_id = loaded->document_id + "/solid:1";
    csgify::AutonomyPolicy context_policy;
    context_policy.mode = csgify::AutonomyMode::propose;
    assert(controller.startAgentContext(
        *loaded->session, loaded->document_id, context_policy,
        context_options, "conversion:qt-test"));
    csgify::CadJobController competing_controller;
    assert(!competing_controller.startAnalysis(
        *loaded->session, loaded->document_id));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(agent_context);
    assert(agent_context->target_id == context_options.target_id);
    assert(agent_context->document_revision == 0);
    assert(agent_context->conversion_policy_digest == "conversion:qt-test");

    const csgify::CommandResult removal = csgify::remove_solid(
        *loaded->session, "solid:1");
    assert(removal.ok);
    const std::size_t saved_revision = csgify::document_revision(
        *loaded->session, loaded->document_id);
    assert(saved_revision == 1);
    csgify::DecisionAuditContext review_audit;
    review_audit.actor = "human:test";
    review_audit.proposal_actor = "engine:test";
    review_audit.proposal_id = "removal:solid:2";
    review_audit.rationale = "retain this component after inspection";
    review_audit.policy_rule = "removal.manual_review";
    review_audit.approval_mode = "manual";
    csgify::record_decision_review(*loaded->session, "solid:2",
        "removal:solid:2", "pending", review_audit,
        "removal", "remove_solid", {}, saved_revision);
    csgify::record_decision_review(*loaded->session, "solid:2",
        "removal:solid:2", "rejected", review_audit,
        "removal", "remove_solid", {}, saved_revision);
    csgify::DecisionAuditContext pending_audit = review_audit;
    pending_audit.proposal_id = "decomposition:solid:3";
    pending_audit.rationale = "inspect regenerated split evidence after restart";
    csgify::record_decision_review(*loaded->session, "solid:3",
        "decomposition:solid:3", "pending", pending_audit,
        "decomposition", "apply_decomposition_candidate", "candidate:best",
        saved_revision);
    loaded->session.reset();
    assert(csgify::compact_log_to_recipe(log.string(), recipe.string()) == 0);
    assert(std::filesystem::is_regular_file(recipe_json));

    loaded.reset();
    failure.clear();
    events.clear();
    assert(controller.startDocumentReconstruction(
        argv[1], log.string(), recipe_json.string(), replay_report.string(),
        load_options));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(failure.isEmpty());
    assert(loaded && loaded->session && loaded->reconstructed);
    assert(loaded->reconstructed_operations >= 2);
    const std::vector<csgify::DecisionReviewRecord> restored_reviews =
        csgify::decision_reviews(*loaded->session);
    assert(restored_reviews.size() == 2);
    assert(restored_reviews.front().target_id == "solid:2");
    assert(restored_reviews.front().proposal_id == "removal:solid:2");
    assert(restored_reviews.front().decision == "rejected");
    assert(restored_reviews.front().audit.rationale
           == "retain this component after inspection");
    const std::vector<csgify::DecisionReviewRecord> pending_reviews =
        csgify::latest_decision_reviews(*loaded->session, "pending");
    assert(pending_reviews.size() == 1);
    assert(pending_reviews.front().target_id == "solid:3");
    assert(pending_reviews.front().review_kind == "decomposition");
    assert(pending_reviews.front().action
           == "apply_decomposition_candidate");
    assert(pending_reviews.front().candidate_id == "candidate:best");
    assert(pending_reviews.front().base_revision == saved_revision);
    assert(csgify::decision_review_status(*loaded->session, "solid:2",
        "removal:solid:2") == "rejected");
    csgify::ViewerReviewContext restored_review;
    restored_review.kind = "removal_set";
    restored_review.action = "remove_solid";
    csgify::ViewerReviewAlternative rejected_alternative;
    rejected_alternative.target_id = "solid:2";
    rejected_alternative.review_id = "removal:solid:2";
    csgify::ViewerReviewAlternative pending_alternative;
    pending_alternative.target_id = "solid:3";
    pending_alternative.review_id = "removal:solid:3";
    restored_review.alternatives = {
        rejected_alternative, pending_alternative};
    csgify::ViewerController restored_controller(
        *loaded->session, loaded->document_id, restored_review);
    assert(restored_controller.alternatives().size() == 2);
    assert(restored_controller.alternatives()[0].state
           == csgify::AltState::rejected);
    assert(restored_controller.alternatives()[1].state
           == csgify::AltState::pending);
    assert(restored_controller.active_index() == 1);
    assert(csgify::document_revision(*loaded->session, loaded->document_id)
           == saved_revision);
    assert(std::filesystem::is_regular_file(replay_report));

    // Human-approved autonomy operations use the same worker boundary as the
    // geometry workspace instead of blocking the Qt event loop.
    csgify::AutonomyOperationResultPtr autonomy_result;
    QObject::connect(&controller,
        &csgify::CadJobController::autonomyOperationCompleted,
        [&autonomy_result](const csgify::AutonomyOperationResultPtr& value) {
            autonomy_result = value;
        });
    csgify::AutonomyQueueItem fixed_point_item;
    fixed_point_item.proposal.id = "review:fixed-point";
    fixed_point_item.proposal.actor = "human:test";
    fixed_point_item.proposal.op = "decompose_to_fixed_point";
    fixed_point_item.proposal.target_id = loaded->document_id;
    fixed_point_item.policy_rule = "decomposition.global_certified_progress";
    csgify::AutonomyPolicy fixed_point_policy;
    fixed_point_policy.mode = csgify::AutonomyMode::propose;
    assert(controller.startAutonomyOperation(
        *loaded->session, loaded->document_id, fixed_point_item,
        fixed_point_policy));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(autonomy_result);
    assert(autonomy_result->item.executed);
    assert(autonomy_result->item.execution_ok);
    assert(autonomy_result->execution.ok);

    // The same controller must hand final emission to a worker and report one
    // independently ordered job stream before the UI closes.
    events.clear();
    csgify::CommandResultPtr emission;
    QObject::connect(&controller, &csgify::CadJobController::commandCompleted,
                     [&emission](const csgify::CommandResultPtr& value) {
                         emission = value;
                     });
    assert(controller.startEmission(*loaded->session, loaded->document_id,
                                    emitted.string()));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(emission);
    assert(emission->ok);
    assert(std::filesystem::is_regular_file(emitted));
    assert(!events.empty());
    const std::string emission_job_id = events.front().job_id;
    previous_sequence = 0;
    for (const csgify::JobProgressEvent& event : events) {
        assert(event.job_id == emission_job_id);
        assert(event.sequence > previous_sequence);
        assert(event.operation == csgify::JobOperation::emit_csg);
        previous_sequence = event.sequence;
    }
    assert(events.back().phase == csgify::JobPhase::finished);
    assert(events.back().outcome == csgify::JobOutcome::committed);

    events.clear();
    emission.reset();
    assert(controller.startStepExport(*loaded->session, loaded->document_id,
                                      exported.string()));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(emission);
    assert(emission->ok);
    assert(std::filesystem::is_regular_file(exported));
    assert(!events.empty());
    for (const csgify::JobProgressEvent& event : events)
        assert(event.operation == csgify::JobOperation::export_step);
    assert(events.back().phase == csgify::JobPhase::finished);
    assert(events.back().outcome == csgify::JobOutcome::committed);

    // A current project policy that differs from the journaled preparation
    // must not inherit the old approvals.
    loaded->session.reset();
    loaded.reset();
    failure.clear();
    csgify::ConversionOptions changed_options = load_options;
    changed_options.unit_override = "mm";
    assert(controller.startDocumentReconstruction(
        argv[1], log.string(), recipe_json.string(), replay_report.string(),
        changed_options));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(!failure.isEmpty());
    assert(!loaded);

    std::filesystem::copy_file(argv[1], changed_source,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream changed(changed_source, std::ios::app);
        changed << "\n/* content identity changed */\n";
    }
    failure.clear();
    assert(controller.startDocumentLoad(
        changed_source.string(), log.string(), load_options));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(!failure.isEmpty());
    assert(!loaded);

    {
        std::ifstream source(recipe_json);
        std::ostringstream contents;
        contents << source.rdbuf();
        std::string incompatible = contents.str();
        const std::string current = "\"csgify_version\":\""
            + std::string(csgify::csgify_version()) + "\"";
        const std::size_t version = incompatible.find(current);
        assert(version != std::string::npos);
        incompatible.replace(version, current.size(),
                             "\"csgify_version\":\"999.0.0\"");
        std::ofstream output(incompatible_recipe);
        output << incompatible;
    }
    failure.clear();
    assert(controller.startDocumentReconstruction(
        argv[1], log.string(), incompatible_recipe.string(),
        replay_report.string(), load_options));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(!failure.isEmpty());
    assert(!loaded);

    failure.clear();
    assert(controller.startDocumentReconstruction(
        changed_source.string(), log.string(), recipe_json.string(),
        replay_report.string(), load_options));
    timeout.start(30000);
    loop.exec();
    assert(!controller.active());
    assert(!failure.isEmpty());
    assert(!loaded);

    std::filesystem::remove(log, error);
    std::filesystem::remove(emitted, error);
    std::filesystem::remove(exported, error);
    std::filesystem::remove(recipe, error);
    std::filesystem::remove(recipe_json, error);
    std::filesystem::remove(replay_report, error);
    std::filesystem::remove(changed_source, error);
    std::filesystem::remove(incompatible_recipe, error);
    std::filesystem::remove(oversized_context, error);
    return 0;
}
