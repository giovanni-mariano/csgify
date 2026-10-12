#include "job_progress_model.hpp"

#include <algorithm>
#include <cmath>

namespace csgify {
namespace {
QString duration(double milliseconds) {
    const qint64 seconds = static_cast<qint64>(std::max(0.0, milliseconds) / 1000.0);
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainder = seconds % 60;
    return hours > 0
        ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(remainder, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(minutes).arg(remainder, 2, 10, QLatin1Char('0'));
}
} // namespace

bool JobProgressModel::append(const JobProgressEvent& event) {
    if (!job_id_.empty() && event.job_id != job_id_) return false;
    if (event.sequence <= last_sequence_) return false;
    if (job_id_.empty()) job_id_ = event.job_id;

    if (event.phase != sampled_phase_ || event.completed < sampled_completed_) {
        sampled_phase_ = event.phase;
        sampled_completed_ = event.completed;
        sampled_elapsed_ms_ = event.elapsed_ms;
        ewma_ms_per_item_ = 0.0;
        comparable_samples_ = 0;
    } else if (event.completed > sampled_completed_) {
        const double per_item = (event.elapsed_ms - sampled_elapsed_ms_)
            / static_cast<double>(event.completed - sampled_completed_);
        if (std::isfinite(per_item) && per_item >= 0.0) {
            ewma_ms_per_item_ = comparable_samples_ == 0
                ? per_item : 0.30 * per_item + 0.70 * ewma_ms_per_item_;
            ++comparable_samples_;
        }
        sampled_completed_ = event.completed;
        sampled_elapsed_ms_ = event.elapsed_ms;
    }
    last_sequence_ = event.sequence;
    events_.push_back(event);
    return true;
}

void JobProgressModel::clear() {
    events_.clear();
    job_id_.clear();
    last_sequence_ = 0;
    sampled_phase_ = JobPhase::queued;
    sampled_completed_ = 0;
    sampled_elapsed_ms_ = 0.0;
    ewma_ms_per_item_ = 0.0;
    comparable_samples_ = 0;
}

const JobProgressEvent* JobProgressModel::latest() const {
    return events_.empty() ? nullptr : &events_.back();
}

QString JobProgressModel::elapsedText() const {
    const JobProgressEvent* event = latest();
    return event ? duration(event->elapsed_ms) : QStringLiteral("0:00");
}

QString JobProgressModel::etaText() const {
    const JobProgressEvent* event = latest();
    if (!event || !event->total_known || event->total <= event->completed
        || comparable_samples_ < 5 || ewma_ms_per_item_ <= 0.0)
        return QStringLiteral("—");
    return QStringLiteral("~%1").arg(duration(
        ewma_ms_per_item_ * static_cast<double>(event->total - event->completed)));
}

} // namespace csgify
