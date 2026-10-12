#include "view_artifact_capture.hpp"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace csgify {

bool ViewArtifactCapture::begin(const QString& job_id) {
    QString safe = job_id;
    for (qsizetype i = 0; i < safe.size(); ++i) {
        const QChar ch = safe.at(i);
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_') safe[i] = '_';
    }
    directory_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/csgify-jobs/") + safe;
    if (!QDir().mkpath(directory_)) return false;
    QFile events(directory_ + QStringLiteral("/events.jsonl"));
    return events.open(QIODevice::WriteOnly | QIODevice::Truncate);
}

bool ViewArtifactCapture::appendEvent(const JobProgressEvent& event) {
    if (directory_.isEmpty()) return false;
    const std::string line = job_progress_event_json(event);
    QFile file(directory_ + QStringLiteral("/events.jsonl"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    if (file.write(line.data(), static_cast<qint64>(line.size()))
            != static_cast<qint64>(line.size())
        || file.write("\n", 1) != 1) {
        return false;
    }
    return file.flush();
}

bool ViewArtifactCapture::capture(
    const QString& name, const std::function<bool(const QString&)>& dump) {
    if (directory_.isEmpty() || !dump) return false;
    const QString target = directory_ + QLatin1Char('/') + name;
    const QString temporary = target + QStringLiteral(".tmp.png");
    QFile::remove(temporary);
    if (!dump(temporary)) return false;
    QFile::remove(target);
    return QFile::rename(temporary, target);
}

std::string ViewArtifactCapture::removalPlanJson(
    std::size_t base_revision,
    const std::vector<RemovalPlanArtifactEntry>& entries,
    bool human_confirmed) {
    QJsonArray components;
    std::size_t target_count = 0;
    for (const RemovalPlanArtifactEntry& entry : entries) {
        QJsonArray targets;
        for (const std::string& target : entry.target_ids) {
            targets.append(QString::fromStdString(target));
            ++target_count;
        }
        QJsonObject component;
        component.insert(QStringLiteral("component_instance_id"),
            QString::fromStdString(entry.component_instance_id));
        component.insert(QStringLiteral("component_definition_id"),
            QString::fromStdString(entry.component_definition_id));
        component.insert(QStringLiteral("resolved_target_ids"), targets);
        components.append(component);
    }
    QJsonObject plan;
    plan.insert(QStringLiteral("schema"),
                QStringLiteral("csgify-removal-plan-v1"));
    plan.insert(QStringLiteral("base_revision"),
                static_cast<qint64>(base_revision));
    plan.insert(QStringLiteral("human_confirmed"), human_confirmed);
    plan.insert(QStringLiteral("component_count"),
                static_cast<qint64>(entries.size()));
    plan.insert(QStringLiteral("resolved_target_count"),
                static_cast<qint64>(target_count));
    plan.insert(QStringLiteral("components"), components);
    const QByteArray encoded = QJsonDocument(plan).toJson(QJsonDocument::Indented);
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
}

bool ViewArtifactCapture::writeRemovalPlan(
    std::size_t base_revision,
    const std::vector<RemovalPlanArtifactEntry>& entries,
    bool human_confirmed) {
    if (directory_.isEmpty()) return false;
    QSaveFile file(directory_ + QStringLiteral("/removal-plan.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const std::string encoded = removalPlanJson(
        base_revision, entries, human_confirmed);
    file.write(encoded.data(), static_cast<qint64>(encoded.size()));
    return file.commit();
}

bool ViewArtifactCapture::writeSummary(const JobProgressEvent& terminal) {
    if (directory_.isEmpty() || terminal.phase != JobPhase::finished) return false;

    QJsonArray diagnostic_values;
    for (const Diagnostic& diagnostic : terminal.diagnostics) {
        const char* severity = diagnostic.severity == DiagnosticSeverity::error
            ? "error" : diagnostic.severity == DiagnosticSeverity::warning
                ? "warning" : "info";
        QJsonObject value;
        value.insert(QStringLiteral("severity"), QString::fromLatin1(severity));
        value.insert(QStringLiteral("code"), QString::fromStdString(diagnostic.code));
        value.insert(QStringLiteral("message"), QString::fromStdString(diagnostic.message));
        value.insert(QStringLiteral("shape_id"), QString::fromStdString(diagnostic.shape_id));
        diagnostic_values.append(value);
    }
    QSaveFile diagnostics_file(directory_ + QStringLiteral("/diagnostics.json"));
    if (!diagnostics_file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QJsonObject diagnostics_root;
    diagnostics_root.insert(QStringLiteral("diagnostics"), diagnostic_values);
    diagnostics_file.write(QJsonDocument(diagnostics_root).toJson(QJsonDocument::Indented));
    if (!diagnostics_file.commit()) return false;

    QJsonArray artifacts;
    const QFileInfoList files = QDir(directory_).entryInfoList(
        QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& info : files) {
        if (info.fileName() == QStringLiteral("summary.json")) continue;
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&input)) continue;
        QJsonObject artifact;
        artifact.insert(QStringLiteral("name"), info.fileName());
        artifact.insert(QStringLiteral("sha256"),
                        QString::fromLatin1(hash.result().toHex()));
        artifact.insert(QStringLiteral("bytes"), info.size());
        artifacts.append(artifact);
    }
    QSaveFile file(directory_ + QStringLiteral("/summary.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QJsonObject summary;
    summary.insert(QStringLiteral("schema"), QStringLiteral("csgify-job-review-v1"));
    summary.insert(QStringLiteral("job_id"), QString::fromStdString(terminal.job_id));
    summary.insert(QStringLiteral("operation"),
                   QString::fromLatin1(job_operation_name(terminal.operation)));
    summary.insert(QStringLiteral("outcome"),
                   QString::fromLatin1(job_outcome_name(terminal.outcome)));
    summary.insert(QStringLiteral("base_revision"),
                   static_cast<qint64>(terminal.base_revision));
    summary.insert(QStringLiteral("final_revision"),
                   static_cast<qint64>(terminal.committed_revision));
    summary.insert(QStringLiteral("elapsed_ms"), terminal.elapsed_ms);
    summary.insert(QStringLiteral("artifacts"), artifacts);
    file.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace csgify
