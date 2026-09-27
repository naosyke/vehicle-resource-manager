// Starts the nodes of a system manifest in order, drives each one to Active
// over DDS, supervises the processes, and shuts everything down in reverse.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "lifecycle_client.hpp"
#include "vrm/cgroup.hpp"
#include "vrm/manifest.hpp"

namespace vrm {

enum class NodeOutcome {
    NotStarted,
    Active,
    Failed,   // Did not reach Active; stopped.
    Skipped,  // A dependency is not Active.
    Exited,   // Process ended unexpectedly.
    Stopped,  // Shut down by the manager.
};

std::string_view to_string(NodeOutcome outcome);

class Manager {
public:
    // Without a CgroupManager, resource budgets are not enforced.
    // `manager_id` identifies this run (random when 0).
    Manager(SystemManifest manifest, std::unique_ptr<CgroupManager> cgroups, std::uint64_t manager_id = 0);
    ~Manager();

    // Returns false when a safety-critical node could not be started.
    bool start();

    // Supervises until a stop signal, or until `duration` elapses when
    // positive. Prints a resource report every `report_interval` (if positive).
    void supervise(std::chrono::milliseconds duration, std::chrono::milliseconds report_interval);

    // Deactivates and shuts down nodes in reverse start order, then makes
    // sure every process has exited.
    void stop();

    void print_summary() const;
    void print_resources();

    // Writes a JSON snapshot of nodes, budgets, usage and recent events to
    // `path` every `interval` while supervising (for the dashboard).
    void set_status_file(std::string path, std::chrono::milliseconds interval);
    void write_status();

private:
    struct RunningNode {
        const NodeSpec* spec;
        pid_t pid = 0;
        bool running = false;
        NodeOutcome outcome = NodeOutcome::NotStarted;
        std::string detail;

        bool in_cgroup = false;
        CgroupUsage usage;  // Latest sample.
        std::chrono::steady_clock::time_point sampled_at;
        double cpu_percent = 0.0;  // 100% = one core, over the last sample interval.
    };

    bool start_node(RunningNode& node);
    void fail_node(RunningNode& node, const std::string& reason);
    void reap_children();
    bool is_running(const RunningNode& node);
    void sample_resources(RunningNode& node);
    void sample_if_stale(RunningNode& node);

    enum class Level { Info, Warn, Error };
    // Logs a message and keeps it in the recent event list.
    void note(Level level, const std::string& node, const std::string& message);

    struct Event {
        double time;  // Unix time in seconds.
        Level level;
        std::string node;
        std::string message;
    };
    RunningNode* find(const std::string& name);

    SystemManifest manifest_;
    std::unique_ptr<CgroupManager> cgroups_;
    LifecycleClient client_;
    std::vector<RunningNode> nodes_;  // In start order.
    std::deque<Event> events_;
    std::string status_path_;
    std::chrono::milliseconds status_interval_{1000};
    bool status_write_failed_ = false;
};

}  // namespace vrm
