#pragma once

#include "csgify/project_store.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace csgify {

// Durable consumer for one claimed project job. Engine-local job IDs and
// sequence numbers are normalized to the project identity, including across
// retries. Persistence or ownership failures request safe engine cancellation.
class ProjectJobProgressSink {
public:
    using Clock = std::function<std::int64_t()>;

    ProjectJobProgressSink(ProjectStore& project,
                           const ClaimedProjectJob& claimed,
                           std::int64_t heartbeat_interval_seconds = 5,
                           Clock clock = {});

    bool ready() const { return ready_; }
    bool terminal_seen() const { return terminal_seen_; }
    bool cancellation_seen() const { return cancellation_seen_; }
    std::uint64_t latest_sequence() const { return latest_sequence_; }
    const std::string& last_error() const { return last_error_; }

    // Persists a normalized immutable event, updates the heartbeat only at the
    // configured cadence, then checks the project's cancellation authority.
    bool consume(const JobProgressEvent& event,
                 const std::string& artifact_digest = {});

    JobProgressCallback callback();

    // Publishes and indexes an immutable payload before appending the event
    // that references its digest. This prevents an event from pointing at an
    // artifact that was never committed to the content-addressed store.
    bool publish_bytes(const JobProgressEvent& event,
                       const std::string& kind,
                       const std::string& extension,
                       const std::string& contents,
                       ArtifactRecord* artifact = nullptr);

    // Commits the artifact-linked terminal event and conversion-job state in
    // one transaction. A racing cancellation wins and leaves neither commit.
    bool complete(const JobProgressEvent& terminal_event,
                  const ArtifactRecord& artifact,
                  const std::string& certificate_digest);

private:
    std::int64_t now() const;
    bool heartbeat_if_due(std::int64_t timestamp, bool force);

    ProjectStore* project_ = nullptr;
    std::string job_id_;
    std::string worker_id_;
    std::int64_t heartbeat_interval_seconds_ = 5;
    std::int64_t last_heartbeat_at_ = 0;
    Clock clock_;
    std::uint64_t latest_sequence_ = 0;
    std::unordered_map<int, std::size_t> phase_completed_;
    std::string last_error_;
    bool ready_ = false;
    bool terminal_seen_ = false;
    bool cancellation_seen_ = false;
};

} // namespace csgify
