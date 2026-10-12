#pragma once

#include "job_progress_model.hpp"

#include <QDockWidget>
#include <QElapsedTimer>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;

namespace csgify {

class JobProgressDock final : public QDockWidget {
    Q_OBJECT
public:
    explicit JobProgressDock(QWidget* parent = nullptr);
    void beginJob();
    void appendEvent(const JobProgressEvent& event);
    const JobProgressModel& model() const { return model_; }
    void setArtifactDirectory(QString path);

signals:
    void cancelRequested();
    void showCurrentRequested();
    void showPlanRequested();
    void fitRequested();
    void openArtifactRequested(QString path);

private slots:
    void refresh();

private:
    JobProgressModel model_;
    QLabel* operation_ = nullptr;
    QLabel* badge_ = nullptr;
    QLabel* counts_ = nullptr;
    QLabel* target_ = nullptr;
    QLabel* timing_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* open_artifact_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QTimer* elapsed_timer_ = nullptr;
    QElapsedTimer wall_elapsed_;
    QString artifact_directory_;
    std::size_t rendered_event_count_ = 0;
};

} // namespace csgify
