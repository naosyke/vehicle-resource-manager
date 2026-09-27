#include "cpu_tracer.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace vrm {

namespace {

bool write_text(const std::string& path, const std::string& value) {
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    const bool ok = std::fputs(value.c_str(), file) >= 0;
    return (std::fclose(file) == 0) && ok;
}

std::string cpu_mask(const std::vector<int>& cpus) {
    unsigned long long mask = 0;
    for (const int cpu : cpus) mask |= 1ULL << cpu;
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", mask);
    return text;
}

}  // namespace

bool parse_sched_switch(const std::string& line, SchedSwitch& event) {
    // "  <comm>-<pid>  [001] d..2.  4410.303921: sched_switch: prev_comm=... ==> next_comm=X next_pid=N next_prio=P"
    const auto marker = line.find(": sched_switch:");
    if (marker == std::string::npos) return false;
    const auto open = line.find('[');
    const auto close = line.find(']', open);
    if (open == std::string::npos || close == std::string::npos || close > marker) return false;
    event.cpu = std::atoi(line.c_str() + open + 1);

    auto time_start = line.rfind(' ', marker - 1);
    if (time_start == std::string::npos) return false;
    event.time = std::strtod(line.c_str() + time_start + 1, nullptr);

    const auto next = line.find("next_comm=", marker);
    const auto next_pid = line.find(" next_pid=", next);
    if (next == std::string::npos || next_pid == std::string::npos) return false;
    event.next_comm = line.substr(next + 10, next_pid - next - 10);
    if (event.next_comm.rfind("swapper/", 0) == 0) event.next_comm.clear();  // Idle.
    return true;
}

double CpuTracer::monotonic_now() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

std::unique_ptr<CpuTracer> CpuTracer::start(const std::vector<int>& cpus, double window_seconds,
                                            std::string& reason) {
    std::string tracing = "/sys/kernel/tracing";
    struct stat info {};
    if (stat((tracing + "/trace_pipe").c_str(), &info) != 0) {
        mkdir(tracing.c_str(), 0755);
        if (mount("nodev", tracing.c_str(), "tracefs", 0, nullptr) != 0) {
            reason = std::string("cannot mount tracefs (run the container with --privileged): ") + std::strerror(errno);
            return nullptr;
        }
    }
    const bool ok = write_text(tracing + "/tracing_on", "0") && write_text(tracing + "/trace", "") &&
                    write_text(tracing + "/trace_clock", "mono") &&
                    write_text(tracing + "/tracing_cpumask", cpu_mask(cpus)) &&
                    write_text(tracing + "/events/sched/sched_switch/enable", "1") &&
                    write_text(tracing + "/tracing_on", "1");
    if (!ok) {
        reason = "cannot configure ftrace in " + tracing + ": " + std::strerror(errno);
        return nullptr;
    }
    return std::unique_ptr<CpuTracer>(new CpuTracer(cpus, window_seconds, tracing));
}

CpuTracer::CpuTracer(std::vector<int> cpus, double window_seconds, std::string tracing)
    : cpus_(std::move(cpus)), window_seconds_(window_seconds), tracing_(std::move(tracing)) {
    pipe_fd_ = open((tracing_ + "/trace_pipe").c_str(), O_RDONLY | O_NONBLOCK);
    reader_ = std::thread([this] { read_loop(); });
}

CpuTracer::~CpuTracer() {
    running_ = false;
    if (reader_.joinable()) reader_.join();
    if (pipe_fd_ >= 0) close(pipe_fd_);
    // Leave the kernel as we found it: tracing off, all CPUs, event disabled.
    write_text(tracing_ + "/tracing_on", "0");
    write_text(tracing_ + "/events/sched/sched_switch/enable", "0");
    write_text(tracing_ + "/tracing_cpumask", "ffffffff");
    write_text(tracing_ + "/trace", "");
}

void CpuTracer::read_loop() {
    std::string pending;
    char buffer[65536];
    while (running_ && pipe_fd_ >= 0) {
        pollfd descriptor{pipe_fd_, POLLIN, 0};
        if (poll(&descriptor, 1, 100) <= 0) continue;
        const ssize_t length = read(pipe_fd_, buffer, sizeof(buffer));
        if (length <= 0) continue;
        pending.append(buffer, static_cast<std::size_t>(length));

        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t line_start = 0;
        for (auto newline = pending.find('\n'); newline != std::string::npos;
             newline = pending.find('\n', line_start)) {
            SchedSwitch event{};
            if (parse_sched_switch(pending.substr(line_start, newline - line_start), event)) {
                auto current = current_.find(event.cpu);
                if (current != current_.end()) {
                    auto& closed = segments_[event.cpu];
                    CpuSegment segment = current->second;
                    segment.end = event.time;
                    // Merge back-to-back runs of the same thread name.
                    if (!closed.empty() && closed.back().comm == segment.comm && segment.start - closed.back().end < 1e-4) {
                        closed.back().end = segment.end;
                    } else {
                        closed.push_back(segment);
                    }
                }
                current_[event.cpu] = CpuSegment{event.time, 0.0, event.next_comm};
            }
            line_start = newline + 1;
        }
        pending.erase(0, line_start);

        // Drop what fell out of the window.
        const double oldest = monotonic_now() - window_seconds_;
        for (auto& [cpu, closed] : segments_) {
            std::size_t drop = 0;
            while (drop < closed.size() && closed[drop].end < oldest) ++drop;
            closed.erase(closed.begin(), closed.begin() + static_cast<long>(drop));
        }
    }
}

std::map<int, std::vector<CpuSegment>> CpuTracer::timeline(double now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = segments_;
    for (const auto& [cpu, segment] : current_) {
        auto running = segment;
        running.end = now;
        result[cpu].push_back(running);
    }
    return result;
}

}  // namespace vrm
