#include "csgify/project_job_progress.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace csgify {

namespace {

std::int64_t epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

ProjectJobProgressSink::ProjectJobProgressSink(
    ProjectStore& project,
    const ClaimedProjectJob& claimed,
    std::int64_t heartbeat_interval_seconds,
    Clock clock)
    : project_(&project), job_id_(claimed.spec.job_id),
      worker_id_(claimed.worker_id),
      heartbeat_interval_seconds_(std::max<std::int64_t>(
          1, heartbeat_interval_seconds)),
      clock_(std::move(clock)) {
    if (job_id_.empty() || worker_id_.empty()
        || claimed.state != ProjectJobState::running) {
        last_error_ = "project progress sink requires a claimed running job";
        return;
    }
    latest_sequence_ = project_->latest_job_event_sequence(job_id_, &last_error_);
    ready_ = last_error_.empty();
}

std::int64_t ProjectJobProgressSink::now() const {
    return clock_ ? clock_() : epoch_seconds();
}

bool ProjectJobProgressSink::heartbeat_if_due(std::int64_t timestamp,
                                              bool force) {
    if (!force && last_heartbeat_at_ > 0
        && timestamp - last_heartbeat_at_ < heartbeat_interval_seconds_) {
        return true;
    }
    if (!project_->heartbeat_job(job_id_, worker_id_, timestamp, &last_error_)) {
        ready_ = false;
        return false;
    }
    last_heartbeat_at_ = timestamp;
    return true;
}

bool ProjectJobProgressSink::consume(const JobProgressEvent& source,
                                     const std::string& artifact_digest) {
    if (!ready_) return false;
    if (terminal_seen_) {
        last_error_ = "project job event received after terminal event";
        ready_ = false;
        return false;
    }

    const int phase_key = static_cast<int>(source.phase);
    const auto previous = phase_completed_.find(phase_key);
    if (previous != phase_completed_.end()
        && source.completed < previous->second) {
        last_error_ = "project job progress decreased within a phase";
        ready_ = false;
        return false;
    }

    const std::int64_t timestamp = now();
    if (!heartbeat_if_due(timestamp, source.phase == JobPhase::finished))
        return false;

    JobProgressEvent event = source;
    event.job_id = job_id_;
    event.sequence = ++latest_sequence_;
    if (!project_->append_job_event(event, timestamp, artifact_digest,
                                    &last_error_)) {
        --latest_sequence_;
        ready_ = false;
        return false;
    }
    phase_completed_[phase_key] = source.completed;
    terminal_seen_ = source.phase == JobPhase::finished;

    bool requested = false;
    if (!project_->job_cancellation_requested(
            job_id_, worker_id_, &requested, &last_error_)) {
        ready_ = false;
        return false;
    }
    cancellation_seen_ = cancellation_seen_ || requested;
    return !requested;
}

JobProgressCallback ProjectJobProgressSink::callback() {
    return [this](const JobProgressEvent& event) { return consume(event); };
}

bool ProjectJobProgressSink::publish_bytes(const JobProgressEvent& event,
                                           const std::string& kind,
                                           const std::string& extension,
                                           const std::string& contents,
                                           ArtifactRecord* artifact) {
    if (!ready_) return false;
    ArtifactRecord published;
    if (!project_->artifacts().publish_bytes(
            kind, extension, contents, &published, &last_error_)
        || !project_->record_artifact(published, &last_error_)) {
        ready_ = false;
        return false;
    }
    if (!consume(event, published.digest)) return false;
    if (artifact) *artifact = std::move(published);
    return true;
}

bool ProjectJobProgressSink::complete(
    const JobProgressEvent& source,
    const ArtifactRecord& artifact,
    const std::string& certificate_digest) {
    if (!ready_ || terminal_seen_) return false;
    if (source.phase != JobPhase::finished
        || source.outcome != JobOutcome::committed) {
        last_error_ = "project job completion requires a committed terminal event";
        ready_ = false;
        return false;
    }
    JobProgressEvent event = source;
    event.job_id = job_id_;
    event.sequence = latest_sequence_ + 1;
    event.artifact_id = artifact.digest;
    const std::int64_t timestamp = now();
    if (!project_->complete_job_with_event(
            job_id_, worker_id_, artifact, certificate_digest, event,
            timestamp, &last_error_)) {
        bool requested = false;
        if (project_->job_cancellation_requested(
                job_id_, worker_id_, &requested, nullptr)) {
            cancellation_seen_ = cancellation_seen_ || requested;
        }
        return false;
    }
    latest_sequence_ = event.sequence;
    phase_completed_[static_cast<int>(event.phase)] = event.completed;
    last_heartbeat_at_ = timestamp;
    terminal_seen_ = true;
    return true;
}

} // namespace csgify
