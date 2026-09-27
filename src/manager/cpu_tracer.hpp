// Records which thread runs on which CPU, from the kernel's sched_switch
// trace events (ftrace), for a timeline of the selected CPUs.
//
// Needs tracefs (a privileged container). Tracing is global to the kernel:
// while a CpuTracer exists it owns /sys/kernel/tracing, and it switches
// tracing off again when destroyed.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrm {

struct CpuSegment {
    double start;      // CLOCK_MONOTONIC seconds.
    double end;
    std::string comm;  // Thread name; "" = idle.
};

// Parses one line of trace_pipe. Returns false for anything but sched_switch.
struct SchedSwitch {
    int cpu;
    double time;
    std::string next_comm;  // "" when the CPU goes idle.
};
bool parse_sched_switch(const std::string& line, SchedSwitch& event);

class CpuTracer {
public:
    static std::unique_ptr<CpuTracer> start(const std::vector<int>& cpus, double window_seconds,
                                            std::string& reason);
    ~CpuTracer();

    CpuTracer(const CpuTracer&) = delete;
    CpuTracer& operator=(const CpuTracer&) = delete;

    const std::vector<int>& cpus() const { return cpus_; }

    // Segments of the last window per CPU, oldest first; the last segment of
    // each CPU is still running and ends at `now`.
    std::map<int, std::vector<CpuSegment>> timeline(double now) const;

    static double monotonic_now();

private:
    CpuTracer(std::vector<int> cpus, double window_seconds, std::string tracing);
    void read_loop();

    std::vector<int> cpus_;
    double window_seconds_;
    std::string tracing_;  // tracefs mount point
    int pipe_fd_ = -1;
    std::atomic<bool> running_{true};
    std::thread reader_;

    mutable std::mutex mutex_;
    std::map<int, std::vector<CpuSegment>> segments_;  // Closed segments.
    std::map<int, CpuSegment> current_;                // Running now (end unset).
};

}  // namespace vrm
