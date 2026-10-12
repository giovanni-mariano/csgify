#include "csgify/worker.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
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
        if (mode == "child-memory") {
            try {
                std::vector<char> memory(256 * 1024 * 1024, 1);
                return memory.empty() ? 0 : 99;
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
    assert(result.exit_code == 42 || result.termination_signal != 0);
    return 0;
}
