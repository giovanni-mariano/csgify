#pragma once

#include "csgify/progress.hpp"

#include <QString>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace csgify {

struct RemovalPlanArtifactEntry {
    std::string component_instance_id;
    std::string component_definition_id;
    std::vector<std::string> target_ids;
};

class ViewArtifactCapture {
public:
    bool begin(const QString& job_id);
    bool appendEvent(const JobProgressEvent& event);
    bool capture(const QString& name,
                 const std::function<bool(const QString&)>& dump);
    static std::string removalPlanJson(
        std::size_t base_revision,
        const std::vector<RemovalPlanArtifactEntry>& entries,
        bool human_confirmed);
    bool writeRemovalPlan(std::size_t base_revision,
                          const std::vector<RemovalPlanArtifactEntry>& entries,
                          bool human_confirmed);
    bool writeSummary(const JobProgressEvent& terminal);
    const QString& directory() const { return directory_; }

private:
    QString directory_;
};

} // namespace csgify
