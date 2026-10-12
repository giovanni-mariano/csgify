#include "cad_job_controller.hpp"

#include <QThread>

#include <mutex>
#include <set>

namespace csgify {
namespace {

std::mutex session_reservations_mutex;
std::set<Session*> session_reservations;

} // namespace

CadJobController::CadJobController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<JobProgressEvent>();
    qRegisterMetaType<FixedPointResultPtr>();
    qRegisterMetaType<CommandResultPtr>();
    qRegisterMetaType<DecompositionPreviewPtr>();
    qRegisterMetaType<LoadedDocumentPtr>();
    qRegisterMetaType<AutonomyOperationResultPtr>();
    qRegisterMetaType<AgentContextSnapshotPtr>();
}

CadJobController::~CadJobController() {
    cancel();
    if (thread_ && thread_->isRunning()) {
        thread_->quit();
        thread_->wait();
    }
    releaseSession();
}

bool CadJobController::startFixedPoint(Session& session,
                                       const DocumentId& document_id) {
    return start(session, document_id, CadJobWorker::Kind::fixed_point, {});
}

bool CadJobController::startAnalysis(Session& session,
                                     const DocumentId& document_id) {
    return start(session, document_id, CadJobWorker::Kind::analyze, {});
}

bool CadJobController::startBatchRemoval(Session& session,
                                         const DocumentId& document_id,
                                         std::vector<std::string> targets) {
    return start(session, document_id, CadJobWorker::Kind::batch_remove,
                 std::move(targets));
}

bool CadJobController::startPreview(Session& session,
                                    const DocumentId& document_id,
                                    DecompositionCandidate candidate) {
    return start(session, document_id,
                 CadJobWorker::Kind::decomposition_preview, {},
                 std::move(candidate));
}

bool CadJobController::startStepExport(Session& session,
                                       const DocumentId& document_id,
                                       std::string output_path) {
    return start(session, document_id, CadJobWorker::Kind::export_step, {}, {},
                 std::move(output_path));
}

bool CadJobController::startEmission(Session& session,
                                     const DocumentId& document_id,
                                     std::string output_path) {
    return start(session, document_id, CadJobWorker::Kind::emit_csg, {}, {},
                 std::move(output_path));
}

bool CadJobController::startAutonomyOperation(
    Session& session, const DocumentId& document_id,
    AutonomyQueueItem item, AutonomyPolicy policy) {
    if (active_ || !reserveSession(session)) return false;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const bool started = startWorker(new CadJobWorker(
        &session, document_id, std::move(item), std::move(policy), cancel_));
    if (!started) releaseSession();
    return started;
}

bool CadJobController::startAgentContext(
    Session& session, const DocumentId& document_id,
    AutonomyPolicy policy, AutonomyContextOptions options,
    std::string conversion_policy_digest) {
    if (active_ || !reserveSession(session)) return false;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    const bool started = startWorker(new CadJobWorker(
        &session, document_id, std::move(policy), std::move(options),
        std::move(conversion_policy_digest), cancel_));
    if (!started) releaseSession();
    return started;
}

bool CadJobController::startDocumentLoad(std::string step_path,
                                         std::string decision_log_path,
                                         ConversionOptions options) {
    if (active_) return false;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    return startWorker(new CadJobWorker(
        std::move(step_path), std::move(decision_log_path),
        std::move(options), cancel_));
}

bool CadJobController::startDocumentReconstruction(
    std::string step_path,
    std::string decision_log_path,
    std::string recipe_path,
    std::string replay_report_path,
    ConversionOptions options) {
    if (active_) return false;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    return startWorker(new CadJobWorker(
        std::move(step_path), std::move(decision_log_path),
        std::move(recipe_path), std::move(replay_report_path),
        std::move(options), cancel_));
}

bool CadJobController::start(Session& session, const DocumentId& document_id,
                             CadJobWorker::Kind kind,
                             std::vector<std::string> targets,
                             DecompositionCandidate candidate,
                             std::string output_path) {
    if (active_ || !reserveSession(session)) return false;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    auto* worker = new CadJobWorker(&session, document_id, kind,
                                    std::move(targets), cancel_,
                                    std::move(candidate),
                                    std::move(output_path));
    const bool started = startWorker(worker);
    if (!started) releaseSession();
    return started;
}

bool CadJobController::startWorker(CadJobWorker* worker) {
    if (active_ || !worker) {
        delete worker;
        return false;
    }
    auto* thread = new QThread(this);
    worker->moveToThread(thread);
    thread_ = thread;
    active_ = true;
    emit activeChanged(true);
    connect(thread, &QThread::started, worker, &CadJobWorker::run);
    connect(worker, &CadJobWorker::progress, this, &CadJobController::progress);
    connect(worker, &CadJobWorker::fixedPointCompleted,
            this, &CadJobController::fixedPointCompleted);
    connect(worker, &CadJobWorker::commandCompleted,
            this, &CadJobController::commandCompleted);
    connect(worker, &CadJobWorker::previewCompleted,
            this, &CadJobController::previewCompleted);
    connect(worker, &CadJobWorker::documentLoaded,
            this, &CadJobController::documentLoaded);
    connect(worker, &CadJobWorker::autonomyOperationCompleted,
            this, &CadJobController::autonomyOperationCompleted);
    connect(worker, &CadJobWorker::agentContextCompleted,
            this, &CadJobController::agentContextCompleted);
    connect(worker, &CadJobWorker::failed, this, &CadJobController::failed);
    connect(worker, &CadJobWorker::done, thread, &QThread::quit);
    connect(worker, &CadJobWorker::done, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, &CadJobController::finishThread);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return true;
}

void CadJobController::cancel() {
    if (cancel_) cancel_->store(true);
}

void CadJobController::finishThread() {
    thread_.clear();
    cancel_.reset();
    releaseSession();
    if (active_) {
        active_ = false;
        emit activeChanged(false);
    }
}

bool CadJobController::reserveSession(Session& session) {
    std::lock_guard<std::mutex> lock(session_reservations_mutex);
    if (!session_reservations.insert(&session).second) return false;
    reserved_session_ = &session;
    return true;
}

void CadJobController::releaseSession() {
    if (!reserved_session_) return;
    std::lock_guard<std::mutex> lock(session_reservations_mutex);
    session_reservations.erase(reserved_session_);
    reserved_session_ = nullptr;
}

} // namespace csgify
