#pragma once

#include "cad_job_worker.hpp"

#include <QObject>
#include <QPointer>

namespace csgify {

class CadJobController final : public QObject {
    Q_OBJECT
public:
    explicit CadJobController(QObject* parent = nullptr);
    ~CadJobController() override;

    bool active() const { return active_; }
    bool startAnalysis(Session& session, const DocumentId& document_id);
    bool startFixedPoint(Session& session, const DocumentId& document_id);
    bool startBatchRemoval(Session& session, const DocumentId& document_id,
                           std::vector<std::string> targets);
    bool startPreview(Session& session, const DocumentId& document_id,
                      DecompositionCandidate candidate);
    bool startStepExport(Session& session, const DocumentId& document_id,
                         std::string output_path);
    bool startEmission(Session& session, const DocumentId& document_id,
                       std::string output_path);
    bool startAutonomyOperation(Session& session,
                                const DocumentId& document_id,
                                AutonomyQueueItem item,
                                AutonomyPolicy policy);
    bool startAgentContext(Session& session,
                           const DocumentId& document_id,
                           AutonomyPolicy policy,
                           AutonomyContextOptions options,
                           std::string conversion_policy_digest = {});
    bool startDocumentLoad(std::string step_path,
                           std::string decision_log_path = {},
                           ConversionOptions options = {});
    bool startDocumentReconstruction(std::string step_path,
                                     std::string decision_log_path,
                                     std::string recipe_path,
                                     std::string replay_report_path,
                                     ConversionOptions options = {});

public slots:
    void cancel();

signals:
    void progress(csgify::JobProgressEvent event);
    void fixedPointCompleted(csgify::FixedPointResultPtr result);
    void commandCompleted(csgify::CommandResultPtr result);
    void previewCompleted(csgify::DecompositionPreviewPtr result);
    void documentLoaded(csgify::LoadedDocumentPtr result);
    void autonomyOperationCompleted(
        csgify::AutonomyOperationResultPtr result);
    void agentContextCompleted(csgify::AgentContextSnapshotPtr result);
    void failed(QString code, QString message);
    void activeChanged(bool active);

private:
    bool start(Session& session, const DocumentId& document_id,
               CadJobWorker::Kind kind, std::vector<std::string> targets,
               DecompositionCandidate candidate = {},
               std::string output_path = {});
    bool startWorker(CadJobWorker* worker);
    bool reserveSession(Session& session);
    void releaseSession();
    void finishThread();

    QPointer<QThread> thread_;
    std::shared_ptr<std::atomic_bool> cancel_;
    bool active_ = false;
    Session* reserved_session_ = nullptr;
};

} // namespace csgify
