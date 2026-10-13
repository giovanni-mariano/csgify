#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace csgify {

struct ProcessLimits {
    // Zero disables this limit. Linux uses RLIMIT_AS (virtual address space).
    // macOS polls the worker's resident memory and checks exit peak RSS; it
    // permits brief overshoot and does not aggregate descendant memory.
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
    bool memory_limit_exceeded = false;
    // Resident-memory high water mark on macOS; zero when not measured.
    std::uint64_t peak_memory_bytes = 0;
};

ProcessResult run_bounded_process(
    const std::vector<std::string>& arguments,
    const ProcessLimits& limits,
    const std::filesystem::path& working_directory = {});

} // namespace csgify
