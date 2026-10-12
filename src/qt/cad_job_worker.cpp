#include "cad_job_worker.hpp"
#include "../replay.hpp"

#include <exception>
#include <chrono>
#include <stdexcept>

namespace csgify {
namespace {

bool mustPublishProgress(const JobProgressEvent& event,
                         bool have_previous,
                         JobPhase previous_phase,
                         const std::string& previous_target,
                         std::chrono::steady_clock::time_point previous_time) {
    if (!have_previous || event.phase == JobPhase::finished
        || event.phase != previous_phase || event.target_id != previous_target
        || !event.diagnostics.empty()) {
        return true;
    }
    const std::string& code = event.message_code;
    if (code.find("accepted") != std::string::npos
        || code.find("committed") != std::string::npos
        || code.find("resolved") != std::string::npos
        || code.find("cancel") != std::string::npos
        || code.find("failed") != std::string::npos) {
        return true;
    }
    return std::chrono::steady_clock::now() - previous_time
        >= std::chrono::milliseconds(75);
}

} // namespace

CadJobWorker::CadJobWorker(Session* session, std::string document_id, Kind kind,
                           std::vector<std::string> targets,
                           std::shared_ptr<std::atomic_bool> cancel,
                           DecompositionCandidate candidate,
                           std::string output_path)
    : session_(session), document_id_(std::move(document_id)), kind_(kind),
      targets_(std::move(targets)), cancel_(std::move(cancel)),
      candidate_(std::move(candidate)), output_path_(std::move(output_path)) {}

CadJobWorker::CadJobWorker(std::string step_path,
                           std::string decision_log_path,
                           ConversionOptions options,
                           std::shared_ptr<std::atomic_bool> cancel)
    : kind_(Kind::load_document), cancel_(std::move(cancel)),
      step_path_(std::move(step_path)),
      decision_log_path_(std::move(decision_log_path)),
      load_options_(std::move(options)) {}

CadJobWorker::CadJobWorker(std::string step_path,
                           std::string decision_log_path,
                           std::string recipe_path,
                           std::string replay_report_path,
                           ConversionOptions options,
                           std::shared_ptr<std::atomic_bool> cancel)
    : kind_(Kind::reconstruct_document), cancel_(std::move(cancel)),
      step_path_(std::move(step_path)),
      decision_log_path_(std::move(decision_log_path)),
      recipe_path_(std::move(recipe_path)),
      replay_report_path_(std::move(replay_report_path)),
      load_options_(std::move(options)) {}

CadJobWorker::CadJobWorker(Session* session, std::string document_id,
                           AutonomyQueueItem item, AutonomyPolicy policy,
                           std::shared_ptr<std::atomic_bool> cancel)
    : session_(session), document_id_(std::move(document_id)),
      kind_(Kind::autonomy_operation), cancel_(std::move(cancel)),
      autonomy_item_(std::move(item)), autonomy_policy_(std::move(policy)) {}

CadJobWorker::CadJobWorker(Session* session, std::string document_id,
                           AutonomyPolicy policy,
                           AutonomyContextOptions options,
                           std::string conversion_policy_digest,
                           std::shared_ptr<std::atomic_bool> cancel)
    : session_(session), document_id_(std::move(document_id)),
      kind_(Kind::agent_context), cancel_(std::move(cancel)),
      autonomy_policy_(std::move(policy)),
      autonomy_context_options_(std::move(options)),
      conversion_policy_digest_(std::move(conversion_policy_digest)) {}

void CadJobWorker::run() {
    try {
        if (kind_ == Kind::load_document
            || kind_ == Kind::reconstruct_document) {
            const bool reconstructing = kind_ == Kind::reconstruct_document;
            const auto started = std::chrono::steady_clock::now();
            const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const std::string job_id = "load-document:" + std::to_string(tick);
            std::uint64_t sequence = 0;
            const auto publish = [&](JobPhase phase, const std::string& code,
                                     JobOutcome outcome = JobOutcome::none) {
                JobProgressEvent event;
                event.job_id = job_id;
                event.sequence = ++sequence;
                event.operation = JobOperation::analyze;
                event.phase = phase;
                event.outcome = outcome;
                event.message_code = code;
                event.target_id = step_path_;
                event.elapsed_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                emit progress(event);
            };

            publish(JobPhase::loading_geometry, reconstructing
                ? "project_reconstruction_started" : "step_load_started");
            if (cancel_->load()) {
                publish(JobPhase::finished, "job_cancelled",
                        JobOutcome::cancelled);
                emit done();
                return;
            }
            SessionPtr session;
            DocumentId document_id;
            std::size_t reconstructed_operations = 0;
            if (reconstructing) {
                ReconstructedSession restored = reconstruct_recipe_session(
                    recipe_path_, step_path_, load_options_, replay_report_path_);
                if (restored.status != 0 || !restored.session) {
                    throw std::runtime_error(restored.error.empty()
                        ? "saved decisions could not be reconstructed"
                        : restored.error);
                }
                reconstructed_operations = restored.applied_count;
                session = std::move(restored.session);
                document_id = std::move(restored.document_id);
            } else {
                session = create_session(load_options_);
                if (!decision_log_path_.empty()) {
                    attach_decision_log(*session, decision_log_path_, step_path_);
                }
                document_id = load_step(*session, step_path_);
            }
            publish(JobPhase::loading_geometry, reconstructing
                ? "project_reconstruction_completed" : "step_load_completed");
            if (cancel_->load()) {
                publish(JobPhase::finished, "job_cancelled",
                        JobOutcome::cancelled);
                emit done();
                return;
            }
            if (reconstructing && !decision_log_path_.empty()) {
                attach_decision_log(*session, decision_log_path_, step_path_);
            }

            bool have_analysis_event = false;
            JobPhase last_analysis_phase = JobPhase::loading_geometry;
            std::string last_analysis_target;
            auto last_analysis_time = std::chrono::steady_clock::now();
            const JobProgressCallback analysis_progress =
                [&](const JobProgressEvent& source) {
                    const bool publish_event = mustPublishProgress(
                        source, have_analysis_event, last_analysis_phase,
                        last_analysis_target, last_analysis_time);
                    if (!publish_event) return !cancel_->load();
                    JobProgressEvent event = source;
                    event.job_id = job_id;
                    event.sequence = ++sequence;
                    event.elapsed_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();
                    emit progress(event);
                    have_analysis_event = true;
                    last_analysis_phase = source.phase;
                    last_analysis_target = source.target_id;
                    last_analysis_time = std::chrono::steady_clock::now();
                    return !cancel_->load();
                };
            CommandResult analysis = analyze(
                *session, document_id, ReportIdMode::scoped,
                analysis_progress);
            if (cancel_->load()
                || (!analysis.ok && !analysis.report.diagnostics.empty()
                    && analysis.report.diagnostics.back().code
                        == "job_cancelled")) {
                emit done();
                return;
            }
            auto loaded = std::make_shared<LoadedDocument>();
            loaded->session = std::move(session);
            loaded->document_id = document_id;
            loaded->analysis = std::move(analysis);
            loaded->reconstructed = reconstructing;
            loaded->reconstructed_operations = reconstructed_operations;
            emit documentLoaded(std::move(loaded));
            emit done();
            return;
        }
        if (!session_) throw std::runtime_error("session is unavailable");
        if (kind_ == Kind::agent_context) {
            if (cancel_->load()) {
                emit done();
                return;
            }
            auto result = std::make_shared<AgentContextSnapshot>(
                make_agent_context_snapshot(
                    *session_, document_id_, autonomy_policy_,
                    autonomy_context_options_, conversion_policy_digest_));
            if (!cancel_->load()) emit agentContextCompleted(std::move(result));
            emit done();
            return;
        }
        if (kind_ == Kind::autonomy_operation) {
            if (cancel_->load()) {
                emit done();
                return;
            }
            DecisionAuditContext audit;
            audit.actor = "human";
            audit.proposal_actor = autonomy_item_.proposal.actor;
            audit.proposal_id = autonomy_item_.proposal.id;
            audit.rationale = autonomy_item_.proposal.rationale;
            audit.policy_rule = autonomy_item_.policy_rule;
            audit.approval_mode = "manual";
            set_decision_audit_context(*session_, audit);
            auto result = std::make_shared<AutonomyOperationResult>();
            result->item = autonomy_item_;
            try {
                result->execution = execute_autonomy_operation(
                    *session_, document_id_, result->item, autonomy_policy_);
            } catch (...) {
                clear_decision_audit_context(*session_);
                throw;
            }
            clear_decision_audit_context(*session_);
            result->item.executed = true;
            result->item.execution_ok = result->execution.ok;
            result->item.review_rationale = result->execution.ok
                ? "approved by human reviewer"
                : "approved attempt failed or rolled back";
            if (!result->item.execution_ok && result->item.reason.empty()) {
                result->item.reason =
                    "atomic session mutation failed or rolled back";
            }
            emit autonomyOperationCompleted(std::move(result));
            emit done();
            return;
        }
        bool have_progress_event = false;
        JobPhase last_progress_phase = JobPhase::queued;
        std::string last_progress_target;
        auto last_progress_time = std::chrono::steady_clock::now();
        const JobProgressCallback generic = [this, &have_progress_event,
                &last_progress_phase, &last_progress_target,
                &last_progress_time](const JobProgressEvent& event) {
            if (mustPublishProgress(event, have_progress_event,
                    last_progress_phase, last_progress_target,
                    last_progress_time)) {
                emit progress(event);
                have_progress_event = true;
                last_progress_phase = event.phase;
                last_progress_target = event.target_id;
                last_progress_time = std::chrono::steady_clock::now();
            }
            return !cancel_->load();
        };
        if (kind_ == Kind::fixed_point) {
            const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto result = std::make_shared<FixedPointDecompositionResult>(
                decompose_to_fixed_point(
                    *session_, document_id_, {}, ReportIdMode::scoped,
                    adapt_fixed_point_progress(
                        "fixed-point:" + document_id_ + ":" + std::to_string(tick),
                        document_revision(*session_, document_id_), generic)));
            emit fixedPointCompleted(std::move(result));
        } else if (kind_ == Kind::batch_remove) {
            auto result = std::make_shared<CommandResult>(remove_solids(
                *session_, targets_, ReportIdMode::scoped, generic));
            emit commandCompleted(std::move(result));
        } else if (kind_ == Kind::decomposition_preview) {
            auto result = std::make_shared<DecompositionPreview>(
                collect_decomposition_preview(*session_, candidate_, generic));
            emit previewCompleted(std::move(result));
        } else if (kind_ == Kind::export_step) {
            auto result = std::make_shared<CommandResult>(export_document_step(
                *session_, document_id_, output_path_,
                ReportIdMode::scoped, generic));
            emit commandCompleted(std::move(result));
        } else if (kind_ == Kind::emit_csg) {
            auto result = std::make_shared<CommandResult>(emit_document(
                *session_, document_id_, output_path_,
                ReportIdMode::scoped, generic));
            emit commandCompleted(std::move(result));
        } else {
            auto result = std::make_shared<CommandResult>(analyze(
                *session_, document_id_, ReportIdMode::scoped, generic));
            emit commandCompleted(std::move(result));
        }
    } catch (const std::exception& exception) {
        emit failed(QStringLiteral("job_exception"),
                    QString::fromLocal8Bit(exception.what()));
    } catch (...) {
        emit failed(QStringLiteral("job_exception"),
                    QStringLiteral("Unknown geometry worker failure."));
    }
    emit done();
}

} // namespace csgify
