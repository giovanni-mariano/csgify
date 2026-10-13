#include "csgify/worker.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <signal.h>
#include <new>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string mode = argv[1];
        if (mode == "child-ok") {
            std::cout << "worker stdout\n";
            std::cerr << "worker stderr\n";
            return 0;
        }
        if (mode == "child-sleep") {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return 0;
        }
        if (mode == "child-output") {
            for (int i = 0; i < 20000; ++i) std::cout << "0123456789abcdef";
            return 0;
        }
        if (mode == "child-memory" || mode == "child-memory-quick"
            || mode == "child-memory-small") {
            try {
                const std::size_t bytes = (mode == "child-memory-small" ? 8 : 256)
                    * 1024 * 1024;
                std::vector<char> memory(bytes);
                // Force real, touched pages even in optimized test builds.
                volatile char* pages = memory.data();
                for (std::size_t i = 0; i < bytes; i += 4096) pages[i] = 1;
                if (mode != "child-memory-quick")
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return 0;
            } catch (const std::bad_alloc&) {
                return 42;
            }
        }
        return 2;
    }

    csgify::ProcessLimits normal;
    normal.wall_time_ms = 2000;
    csgify::ProcessResult result =
        csgify::run_bounded_process({argv[0], "child-ok"}, normal);
    assert(result.started);
    assert(result.success);
    assert(result.exit_code == 0);
    assert(result.standard_output == "worker stdout\n");
    assert(result.standard_error == "worker stderr\n");

    csgify::ProcessLimits timeout;
    timeout.wall_time_ms = 100;
    result = csgify::run_bounded_process({argv[0], "child-sleep"}, timeout);
    assert(result.started);
    assert(!result.success);
    assert(result.timed_out);
    assert(result.limit_reason == "wall_time");

    csgify::ProcessLimits output;
    output.wall_time_ms = 2000;
    output.max_captured_output_bytes = 4096;
    result = csgify::run_bounded_process({argv[0], "child-output"}, output);
    assert(!result.success);
    assert(result.output_limit_exceeded);
    assert(result.standard_output.size() <= output.max_captured_output_bytes);
    assert(result.limit_reason == "captured_output");

    csgify::ProcessLimits memory;
    memory.wall_time_ms = 2000;
    memory.memory_bytes = 96 * 1024 * 1024;
    result = csgify::run_bounded_process({argv[0], "child-memory"}, memory);
    assert(result.started);
    assert(!result.success);
    assert(result.launch_error.empty());
    assert(!result.timed_out);
#if defined(__APPLE__)
    assert(result.memory_limit_exceeded);
    assert(result.limit_reason == "memory_bytes");
    assert(result.peak_memory_bytes > memory.memory_bytes);
    assert(result.termination_signal == SIGKILL);
#else
    assert(result.exit_code == 42 || result.termination_signal != 0);
#endif

    result = csgify::run_bounded_process({argv[0], "child-memory-small"}, memory);
    assert(result.success);
    assert(!result.memory_limit_exceeded);
    assert(result.launch_error.empty());

    result = csgify::run_bounded_process({argv[0], "child-memory-quick"}, memory);
    assert(!result.success);
    assert(!result.timed_out);
    assert(result.launch_error.empty());
#if defined(__APPLE__)
    assert(result.memory_limit_exceeded);
    assert(result.limit_reason == "memory_bytes");
#endif

    // Without a memory budget the same allocation must finish successfully.
    result = csgify::run_bounded_process({argv[0], "child-memory-quick"}, normal);
    assert(result.success);
    assert(!result.memory_limit_exceeded);

    csgify::ProcessLimits invalid;
    invalid.memory_bytes = std::numeric_limits<std::uint64_t>::max();
    result = csgify::run_bounded_process({argv[0], "child-ok"}, invalid);
    assert(!result.started);
    assert(!result.success);
    assert(!result.launch_error.empty());
    assert(result.standard_output.empty());

    result = csgify::run_bounded_process({argv[0], "child-ok"}, normal,
                                        "/nonexistent-csgify-worker-directory");
    assert(!result.success);
    assert(result.launch_error.find("chdir:") == 0);
    assert(result.standard_output.empty());

    result = csgify::run_bounded_process({"/nonexistent-csgify-worker"}, normal);
    assert(!result.success);
    assert(result.launch_error.find("exec:") == 0);
    return 0;
}
