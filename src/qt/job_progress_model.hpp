#pragma once

#include "csgify/progress.hpp"

#include <QString>

#include <cstdint>
#include <vector>

namespace csgify {

class JobProgressModel {
public:
    bool append(const JobProgressEvent& event);
    void clear();

    const JobProgressEvent* latest() const;
    const std::vector<JobProgressEvent>& events() const { return events_; }
    QString elapsedText() const;
    QString etaText() const;

private:
    std::vector<JobProgressEvent> events_;
    std::string job_id_;
    std::uint64_t last_sequence_ = 0;
    JobPhase sampled_phase_ = JobPhase::queued;
    std::size_t sampled_completed_ = 0;
    double sampled_elapsed_ms_ = 0.0;
    double ewma_ms_per_item_ = 0.0;
    std::size_t comparable_samples_ = 0;
};

} // namespace csgify
