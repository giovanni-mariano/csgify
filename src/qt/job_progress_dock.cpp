#include "job_progress_dock.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace csgify {
namespace {
QString phase_text(JobPhase phase) {
    return QString::fromLatin1(job_phase_name(phase)).replace(QLatin1Char('_'), QLatin1Char(' '));
}
}

JobProgressDock::JobProgressDock(QWidget* parent)
    : QDockWidget(QStringLiteral("Job progress"), parent) {
    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    auto* heading = new QHBoxLayout;
    operation_ = new QLabel(QStringLiteral("No active job"), body);
    badge_ = new QLabel(QStringLiteral("READY"), body);
    badge_->setStyleSheet(QStringLiteral("padding:3px 7px;border-radius:3px;background:#555;"));
    heading->addWidget(operation_, 1);
    heading->addWidget(badge_);
    layout->addLayout(heading);
    progress_ = new QProgressBar(body);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    layout->addWidget(progress_);
    counts_ = new QLabel(QStringLiteral("Completed: —"), body);
    target_ = new QLabel(QStringLiteral("Current target: —"), body);
    timing_ = new QLabel(QStringLiteral("Elapsed 0:00 · ETA —"), body);
    layout->addWidget(counts_);
    layout->addWidget(target_);
    layout->addWidget(timing_);
    diagnostics_ = new QListWidget(body);
    diagnostics_->setMaximumHeight(130);
    layout->addWidget(diagnostics_);
    auto* actions = new QHBoxLayout;
    cancel_ = new QPushButton(QStringLiteral("Cancel"), body);
    auto* current = new QPushButton(QStringLiteral("Show current"), body);
    auto* plan = new QPushButton(QStringLiteral("Show plan"), body);
    auto* fit = new QPushButton(QStringLiteral("Fit"), body);
    open_artifact_ = new QPushButton(QStringLiteral("Open artifact"), body);
    open_artifact_->setEnabled(false);
    actions->addWidget(cancel_);
    actions->addWidget(current);
    actions->addWidget(plan);
    actions->addWidget(fit);
    actions->addWidget(open_artifact_);
    layout->addLayout(actions);
    setWidget(body);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setSingleShot(true);
    refresh_timer_->setInterval(150);
    connect(refresh_timer_, &QTimer::timeout, this, &JobProgressDock::refresh);
    elapsed_timer_ = new QTimer(this);
    elapsed_timer_->setInterval(1000);
    connect(elapsed_timer_, &QTimer::timeout, this, &JobProgressDock::refresh);
    connect(cancel_, &QPushButton::clicked, this, &JobProgressDock::cancelRequested);
    connect(current, &QPushButton::clicked, this, &JobProgressDock::showCurrentRequested);
    connect(plan, &QPushButton::clicked, this, &JobProgressDock::showPlanRequested);
    connect(fit, &QPushButton::clicked, this, &JobProgressDock::fitRequested);
    connect(open_artifact_, &QPushButton::clicked, this, [this] {
        emit openArtifactRequested(artifact_directory_);
    });
}

void JobProgressDock::beginJob() {
    model_.clear();
    rendered_event_count_ = 0;
    diagnostics_->clear();
    artifact_directory_.clear();
    open_artifact_->setEnabled(false);
    cancel_->setEnabled(true);
    wall_elapsed_.restart();
    elapsed_timer_->start();
    show();
}

void JobProgressDock::appendEvent(const JobProgressEvent& event) {
    if (!model_.append(event)) return;
    if (event.phase == JobPhase::finished || !refresh_timer_->isActive())
        refresh_timer_->start(event.phase == JobPhase::finished ? 0 : 150);
}

void JobProgressDock::setArtifactDirectory(QString path) {
    artifact_directory_ = std::move(path);
    open_artifact_->setEnabled(!artifact_directory_.isEmpty());
}

void JobProgressDock::refresh() {
    const JobProgressEvent* event = model_.latest();
    if (!event) return;
    operation_->setText(QStringLiteral("%1 — %2")
        .arg(QString::fromLatin1(job_operation_name(event->operation)).replace('_', ' '),
             phase_text(event->phase)));
    if (event->phase == JobPhase::finished) {
        badge_->setText(QString::fromLatin1(job_outcome_name(event->outcome)).toUpper());
        const bool certified = event->outcome == JobOutcome::committed;
        badge_->setStyleSheet(certified
            ? QStringLiteral("padding:3px 7px;border-radius:3px;background:#267a3b;")
            : QStringLiteral("padding:3px 7px;border-radius:3px;background:#a45b16;"));
        cancel_->setEnabled(false);
        elapsed_timer_->stop();
    } else {
        badge_->setText(QStringLiteral("PROVISIONAL"));
        badge_->setStyleSheet(QStringLiteral("padding:3px 7px;border-radius:3px;background:#a45b16;"));
    }
    if (event->total_known && event->total > 0) {
        progress_->setRange(0, static_cast<int>(event->total));
        progress_->setValue(static_cast<int>(std::min(event->completed, event->total)));
        counts_->setText(QStringLiteral("Completed: %1 / %2")
            .arg(event->completed).arg(event->total));
    } else {
        progress_->setRange(event->phase == JobPhase::finished ? 0 : 0,
                            event->phase == JobPhase::finished ? 1 : 0);
        if (event->phase == JobPhase::finished) progress_->setValue(1);
        counts_->setText(QStringLiteral("Candidates: %1 · actions: %2 · residuals: %3 · splitters: %4")
            .arg(event->candidates_evaluated).arg(event->actions_applied)
            .arg(event->residual_count).arg(event->splitter_calls));
    }
    target_->setText(QStringLiteral("Current target: %1%2")
        .arg(event->target_id.empty() ? QStringLiteral("—")
                                     : QString::fromStdString(event->target_id),
             event->strategy.empty() ? QString() : QStringLiteral(" · %1").arg(
                 QString::fromStdString(event->strategy))));
    const qint64 wall_seconds = wall_elapsed_.isValid()
        ? wall_elapsed_.elapsed() / 1000 : 0;
    const QString live_elapsed = QStringLiteral("%1:%2")
        .arg(wall_seconds / 60).arg(wall_seconds % 60, 2, 10, QLatin1Char('0'));
    timing_->setText(QStringLiteral("Elapsed %1 · ETA %2 · revision %3 → %4")
        .arg(event->phase == JobPhase::finished ? model_.elapsedText() : live_elapsed,
             model_.etaText())
        .arg(event->base_revision).arg(event->committed_revision));
    const auto& events = model_.events();
    for (; rendered_event_count_ < events.size(); ++rendered_event_count_) {
        for (const Diagnostic& diagnostic
             : events[rendered_event_count_].diagnostics) {
            diagnostics_->addItem(QStringLiteral("%1: %2")
                .arg(QString::fromStdString(diagnostic.code),
                     QString::fromStdString(diagnostic.message)));
        }
    }
    while (diagnostics_->count() > 100) delete diagnostics_->takeItem(0);
}

} // namespace csgify
