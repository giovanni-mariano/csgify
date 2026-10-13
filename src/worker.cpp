#include "csgify/worker.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace csgify {
namespace {

bool make_pipe(int descriptors[2], std::string* error) {
    if (::pipe(descriptors) == 0) return true;
    *error = std::strerror(errno);
    return false;
}

void close_if_open(int* descriptor) {
    if (*descriptor >= 0) {
        ::close(*descriptor);
        *descriptor = -1;
    }
}

// Only POD data and async-signal-safe operations are used before exec.
struct LaunchError {
    int stage;
    int error;
};

enum LaunchStage { process_group, directory, memory_limit, cpu_limit, file_limit, execute };

void fail_launch(int descriptor, LaunchStage stage) {
    const LaunchError error{stage, errno};
    ssize_t written;
    do {
        written = ::write(descriptor, &error, sizeof(error));
    } while (written < 0 && errno == EINTR);
    _exit(126);
}

bool apply_limit(int resource, std::uint64_t value) {
    if (value == 0) return true;
    struct rlimit limit;
    if (::getrlimit(resource, &limit) != 0) return false;
    // Never raise a tighter limit inherited from the caller.
    limit.rlim_cur = std::min(limit.rlim_cur, static_cast<rlim_t>(value));
    limit.rlim_max = std::min(limit.rlim_max, static_cast<rlim_t>(value));
    return ::setrlimit(resource, &limit) == 0;
}

bool append_pipe(int descriptor,
                 std::string* destination,
                 std::uint64_t* captured,
                 std::uint64_t maximum,
                 bool* exceeded) {
    char buffer[8192];
    bool open = true;
    // Yield to resource monitoring even when the child continuously writes.
    for (std::size_t drained = 0; drained < 65536;) {
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            drained += static_cast<std::size_t>(count);
            const std::uint64_t available = maximum == 0 || *captured >= maximum
                ? (maximum == 0 ? static_cast<std::uint64_t>(count) : 0)
                : std::min<std::uint64_t>(count, maximum - *captured);
            if (available > 0) {
                destination->append(buffer, static_cast<std::size_t>(available));
                *captured += available;
            }
            if (maximum != 0 && available < static_cast<std::uint64_t>(count)) {
                *exceeded = true;
            }
            continue;
        }
        if (count == 0) open = false;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) open = false;
        break;
    }
    return open;
}

} // namespace

ProcessResult run_bounded_process(
    const std::vector<std::string>& arguments,
    const ProcessLimits& limits,
    const std::filesystem::path& working_directory) {
    ProcessResult result;
    if (arguments.empty() || arguments.front().empty()) {
        result.launch_error = "worker argument vector is empty";
        return result;
    }

    for (const auto value : {limits.memory_bytes, limits.cpu_time_seconds,
                             limits.file_size_bytes}) {
        if (value >= static_cast<std::uint64_t>(RLIM_INFINITY)) {
            result.launch_error = "resource limit must be finite (use zero to disable)";
            return result;
        }
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    int launch_pipe[2]{-1, -1};
    const auto close_pipes = [&] {
        for (auto* pipe : {stdout_pipe, stderr_pipe, launch_pipe}) {
            close_if_open(&pipe[0]);
            close_if_open(&pipe[1]);
        }
    };
    if (!make_pipe(stdout_pipe, &result.launch_error)
        || !make_pipe(stderr_pipe, &result.launch_error)
        || !make_pipe(launch_pipe, &result.launch_error)) {
        close_pipes();
        return result;
    }
    if (::fcntl(launch_pipe[1], F_SETFD, FD_CLOEXEC) != 0
        || ::fcntl(launch_pipe[0], F_SETFL, O_NONBLOCK) != 0) {
        result.launch_error = std::strerror(errno);
        close_pipes();
        return result;
    }

    const auto start = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    if (child < 0) {
        result.launch_error = std::strerror(errno);
        close_pipes();
        return result;
    }
    if (child == 0) {
        ::close(launch_pipe[0]);
        if (::setpgid(0, 0) != 0) fail_launch(launch_pipe[1], process_group);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        if (!working_directory.empty()
            && ::chdir(working_directory.c_str()) != 0)
            fail_launch(launch_pipe[1], directory);
#if !defined(__APPLE__)
        if (!apply_limit(RLIMIT_AS, limits.memory_bytes))
            fail_launch(launch_pipe[1], memory_limit);
#endif
        if (!apply_limit(RLIMIT_CPU, limits.cpu_time_seconds))
            fail_launch(launch_pipe[1], cpu_limit);
        if (!apply_limit(RLIMIT_FSIZE, limits.file_size_bytes))
            fail_launch(launch_pipe[1], file_limit);
        ::execvp(argv.front(), argv.data());
        fail_launch(launch_pipe[1], execute);
    }

    result.started = true;
    ::setpgid(child, child);
    close_if_open(&launch_pipe[1]);
    bool launch_open = true;
    close_if_open(&stdout_pipe[1]);
    close_if_open(&stderr_pipe[1]);
    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    bool stdout_open = true;
    bool stderr_open = true;
    bool child_finished = false;
    bool killed = false;
    int wait_status = 0;
    struct rusage usage{};
    std::uint64_t captured = 0;
    while (!child_finished || stdout_open || stderr_open || launch_open) {
        struct pollfd descriptors[3] = {
            {stdout_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
            {stderr_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
            {launch_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
        };
        ::poll(descriptors, 3, 20);
        if (launch_open) {
            LaunchError launch_error{};
            const ssize_t count = ::read(launch_pipe[0], &launch_error,
                                         sizeof(launch_error));
            if (count == sizeof(launch_error)) {
                const char* stages[] = {"setpgid", "chdir", "memory limit", "CPU limit",
                                        "file size limit", "exec"};
                result.launch_error = std::string(stages[launch_error.stage]) + ": "
                    + std::strerror(launch_error.error);
            } else if (count > 0 || (count < 0 && errno != EINTR
                                    && errno != EAGAIN && errno != EWOULDBLOCK)) {
                result.launch_error = "could not read worker launch status";
                ::kill(-child, SIGKILL);
                killed = true;
            }
            if (count >= 0 || !result.launch_error.empty()) {
                launch_open = false;
                close_if_open(&launch_pipe[0]);
            }
        }
        if (stdout_open) {
            stdout_open = append_pipe(stdout_pipe[0], &result.standard_output,
                                      &captured, limits.max_captured_output_bytes,
                                      &result.output_limit_exceeded);
        }
        if (stderr_open) {
            stderr_open = append_pipe(stderr_pipe[0], &result.standard_error,
                                      &captured, limits.max_captured_output_bytes,
                                      &result.output_limit_exceeded);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (!killed && limits.wall_time_ms > 0
            && static_cast<std::uint64_t>(elapsed) > limits.wall_time_ms) {
            result.timed_out = true;
            result.limit_reason = "wall_time";
            ::kill(-child, SIGKILL);
            killed = true;
        } else if (!killed && result.output_limit_exceeded) {
            result.limit_reason = "captured_output";
            ::kill(-child, SIGKILL);
            killed = true;
        }

#if defined(__APPLE__)
        int memory_error = 0;
        if (!child_finished && !killed && !launch_open && limits.memory_bytes > 0
            && result.launch_error.empty()) {
            struct rusage_info_v0 memory{};
            if (::proc_pid_rusage(child, RUSAGE_INFO_V0,
                    reinterpret_cast<rusage_info_t*>(&memory)) == 0) {
                result.peak_memory_bytes = std::max(result.peak_memory_bytes,
                                                    memory.ri_resident_size);
            } else {
                memory_error = errno != 0 ? errno : EIO;
            }
        }
#endif
        if (!child_finished) {
            const pid_t waited = ::wait4(child, &wait_status, WNOHANG, &usage);
            if (waited == child) {
                child_finished = true;
#if defined(__APPLE__)
                // Darwin reports ru_maxrss in bytes. This also catches a child
                // that allocates and exits between monitoring polls.
                result.peak_memory_bytes = std::max(result.peak_memory_bytes,
                    static_cast<std::uint64_t>(usage.ru_maxrss));
#endif
            } else if (waited < 0 && errno != EINTR) {
                result.launch_error = std::string("wait4: ") + std::strerror(errno);
                child_finished = true;
            }
        }
#if defined(__APPLE__)
        if (limits.memory_bytes > 0 && result.launch_error.empty()) {
            if (result.peak_memory_bytes > limits.memory_bytes) {
                result.memory_limit_exceeded = true;
                if (result.limit_reason.empty()) result.limit_reason = "memory_bytes";
                if (!killed) {
                    ::kill(-child, SIGKILL);
                    killed = true;
                }
            } else if (memory_error != 0 && !child_finished) {
                // ESRCH is benign only if wait4 confirmed the child exited.
                result.launch_error = std::string("memory monitoring: ")
                    + std::strerror(memory_error);
                if (result.limit_reason.empty()) result.limit_reason = "memory_monitor";
                ::kill(-child, SIGKILL);
                killed = true;
            }
        }
#endif
        if (child_finished && !stdout_open && !stderr_open && !launch_open) break;
    }
    if (!child_finished) ::waitpid(child, &wait_status, 0);
    close_if_open(&stdout_pipe[0]);
    close_if_open(&stderr_pipe[0]);

    result.wall_time_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    if (WIFEXITED(wait_status)) result.exit_code = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) {
        result.termination_signal = WTERMSIG(wait_status);
        if (result.limit_reason.empty() && result.termination_signal == SIGXCPU)
            result.limit_reason = "cpu_time";
        if (result.limit_reason.empty() && result.termination_signal == SIGXFSZ)
            result.limit_reason = "file_size";
    }
    result.success = result.exit_code == 0 && result.termination_signal == 0
        && !result.timed_out && !result.output_limit_exceeded
        && !result.memory_limit_exceeded && result.launch_error.empty();
    return result;
}

} // namespace csgify
