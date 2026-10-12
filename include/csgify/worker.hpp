#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace csgify {

struct ProcessLimits {
    std::uint64_t memory_bytes = 0;
    std::uint64_t wall_time_ms = 0;
    std::uint64_t cpu_time_seconds = 0;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t max_captured_output_bytes = 1024 * 1024;
};

struct ProcessResult {
    bool started = false;
    bool success = false;
    bool timed_out = false;
    bool output_limit_exceeded = false;
    int exit_code = -1;
    int termination_signal = 0;
    std::uint64_t wall_time_ms = 0;
    std::string standard_output;
    std::string standard_error;
    std::string limit_reason;
    std::string launch_error;
};

ProcessResult run_bounded_process(
    const std::vector<std::string>& arguments,
    const ProcessLimits& limits,
    const std::filesystem::path& working_directory = {});

} // namespace csgify
