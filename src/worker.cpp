#include "csgify/worker.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <thread>

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

void apply_limit(int resource, std::uint64_t value) {
    if (value == 0) return;
    struct rlimit limit;
    limit.rlim_cur = static_cast<rlim_t>(value);
    limit.rlim_max = static_cast<rlim_t>(value);
    ::setrlimit(resource, &limit);
}

bool append_pipe(int descriptor,
                 std::string* destination,
                 std::uint64_t* captured,
                 std::uint64_t maximum,
                 bool* exceeded) {
    char buffer[8192];
    bool open = true;
    while (true) {
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
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

    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (!make_pipe(stdout_pipe, &result.launch_error)
        || !make_pipe(stderr_pipe, &result.launch_error)) {
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[0]);
        close_if_open(&stderr_pipe[1]);
        return result;
    }

    const auto start = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    if (child < 0) {
        result.launch_error = std::strerror(errno);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[0]);
        close_if_open(&stderr_pipe[1]);
        return result;
    }
    if (child == 0) {
        ::setpgid(0, 0);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        if (!working_directory.empty()
            && ::chdir(working_directory.c_str()) != 0) {
            const std::string message = std::string("chdir failed: ")
                + std::strerror(errno) + "\n";
            ::write(STDERR_FILENO, message.data(), message.size());
            _exit(126);
        }
        apply_limit(RLIMIT_AS, limits.memory_bytes);
        apply_limit(RLIMIT_CPU, limits.cpu_time_seconds);
        apply_limit(RLIMIT_FSIZE, limits.file_size_bytes);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        const std::string message = std::string("exec failed: ")
            + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    result.started = true;
    ::setpgid(child, child);
    close_if_open(&stdout_pipe[1]);
    close_if_open(&stderr_pipe[1]);
    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    bool stdout_open = true;
    bool stderr_open = true;
    bool child_finished = false;
    bool killed = false;
    int wait_status = 0;
    std::uint64_t captured = 0;
    while (!child_finished || stdout_open || stderr_open) {
        struct pollfd descriptors[2] = {
            {stdout_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
            {stderr_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
        };
        ::poll(descriptors, 2, 20);
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

        if (!child_finished) {
            const pid_t waited = ::waitpid(child, &wait_status, WNOHANG);
            if (waited == child) child_finished = true;
            else if (waited < 0 && errno != EINTR) child_finished = true;
        }
        if (child_finished && !stdout_open && !stderr_open) break;
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
        && !result.timed_out && !result.output_limit_exceeded;
    return result;
}

} // namespace csgify
