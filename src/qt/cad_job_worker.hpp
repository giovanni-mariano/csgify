#pragma once

#include "csgify/agent.hpp"
#include "csgify/session.hpp"
#include "../session_view.hpp"

#include <QObject>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace csgify {

using FixedPointResultPtr = std::shared_ptr<FixedPointDecompositionResult>;
using CommandResultPtr = std::shared_ptr<CommandResult>;
using DecompositionPreviewPtr = std::shared_ptr<DecompositionPreview>;
using AgentContextSnapshotPtr = std::shared_ptr<AgentContextSnapshot>;

struct AutonomyOperationResult {
    AutonomyQueueItem item;
    AutonomyOperationExecution execution;
};
using AutonomyOperationResultPtr = std::shared_ptr<AutonomyOperationResult>;

struct LoadedDocument {
    SessionPtr session;
    DocumentId document_id;
    CommandResult analysis;
    bool reconstructed = false;
    std::size_t reconstructed_operations = 0;
};
using LoadedDocumentPtr = std::shared_ptr<LoadedDocument>;

class CadJobWorker final : public QObject {
    Q_OBJECT
public:
    enum class Kind {
        analyze,
        fixed_point,
        batch_remove,
        decomposition_preview,
        export_step,
        emit_csg,
        autonomy_operation,
        load_document,
        reconstruct_document,
        agent_context,
    };

    CadJobWorker(Session* session, std::string document_id, Kind kind,
                 std::vector<std::string> targets,
                 std::shared_ptr<std::atomic_bool> cancel,
                 DecompositionCandidate candidate = {},
                 std::string output_path = {});
    CadJobWorker(std::string step_path, std::string decision_log_path,
                 ConversionOptions options,
                 std::shared_ptr<std::atomic_bool> cancel);
    CadJobWorker(std::string step_path, std::string decision_log_path,
                 std::string recipe_path, std::string replay_report_path,
                 ConversionOptions options,
                 std::shared_ptr<std::atomic_bool> cancel);
    CadJobWorker(Session* session, std::string document_id,
                 AutonomyQueueItem item, AutonomyPolicy policy,
                 std::shared_ptr<std::atomic_bool> cancel);
    CadJobWorker(Session* session, std::string document_id,
                 AutonomyPolicy policy, AutonomyContextOptions options,
                 std::string conversion_policy_digest,
                 std::shared_ptr<std::atomic_bool> cancel);

public slots:
    void run();

signals:
    void progress(csgify::JobProgressEvent event);
    void fixedPointCompleted(csgify::FixedPointResultPtr result);
    void commandCompleted(csgify::CommandResultPtr result);
    void previewCompleted(csgify::DecompositionPreviewPtr result);
    void documentLoaded(csgify::LoadedDocumentPtr result);
    void autonomyOperationCompleted(csgify::AutonomyOperationResultPtr result);
    void agentContextCompleted(csgify::AgentContextSnapshotPtr result);
    void failed(QString code, QString message);
    void done();

private:
    Session* session_ = nullptr;
    std::string document_id_;
    Kind kind_ = Kind::fixed_point;
    std::vector<std::string> targets_;
    std::shared_ptr<std::atomic_bool> cancel_;
    DecompositionCandidate candidate_;
    std::string output_path_;
    std::string step_path_;
    std::string decision_log_path_;
    std::string recipe_path_;
    std::string replay_report_path_;
    ConversionOptions load_options_;
    AutonomyQueueItem autonomy_item_;
    AutonomyPolicy autonomy_policy_;
    AutonomyContextOptions autonomy_context_options_;
    std::string conversion_policy_digest_;
};

} // namespace csgify

Q_DECLARE_METATYPE(csgify::JobProgressEvent)
Q_DECLARE_METATYPE(csgify::FixedPointResultPtr)
Q_DECLARE_METATYPE(csgify::CommandResultPtr)
Q_DECLARE_METATYPE(csgify::DecompositionPreviewPtr)
Q_DECLARE_METATYPE(csgify::LoadedDocumentPtr)
Q_DECLARE_METATYPE(csgify::AutonomyOperationResultPtr)
Q_DECLARE_METATYPE(csgify::AgentContextSnapshotPtr)
