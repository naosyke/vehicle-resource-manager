// Starts the nodes of a system manifest in order, drives each one to Active
// over DDS, supervises the processes, and shuts everything down in reverse.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <string>
#include <vector>

#include "lifecycle_client.hpp"
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
    explicit Manager(SystemManifest manifest);
    ~Manager();

    // Returns false when a safety-critical node could not be started.
    bool start();

    // Supervises until a stop signal, or until `duration` elapses when positive.
    void supervise(std::chrono::milliseconds duration);

    // Deactivates and shuts down nodes in reverse start order, then makes
    // sure every process has exited.
    void stop();

    void print_summary() const;

private:
    struct RunningNode {
        const NodeSpec* spec;
        pid_t pid = 0;
        bool running = false;
        NodeOutcome outcome = NodeOutcome::NotStarted;
        std::string detail;
    };

    bool start_node(RunningNode& node);
    void fail_node(RunningNode& node, const std::string& reason);
    void reap_children();
    bool is_running(const RunningNode& node);
    RunningNode* find(const std::string& name);

    SystemManifest manifest_;
    LifecycleClient client_;
    std::vector<RunningNode> nodes_;  // In start order.
};

}  // namespace vrm
