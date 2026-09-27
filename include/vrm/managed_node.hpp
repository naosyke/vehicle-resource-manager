// Base class for a node whose lifecycle is driven over DDS by the manager.
//
// Subclasses override the on_* callbacks and on_tick(), which runs
// periodically while the node is Active. run() blocks until the node is
// finalized or receives SIGINT / SIGTERM.
//
// While running, the node publishes a heartbeat every 500 ms with its
// scheduling policy and the timing of its ticks (latency, response time and
// deadline misses), so the manager can supervise it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "vrm/deadline_monitor.hpp"
#include "vrm/lifecycle.hpp"

namespace vrm {

class ManagedNode {
public:
    // `deadline` is relative to each tick's release; zero means the period,
    // a negative value means no deadline (misses are not counted).
    ManagedNode(std::string name, std::chrono::microseconds tick_period,
                std::chrono::microseconds deadline = std::chrono::microseconds{0});
    virtual ~ManagedNode();

    ManagedNode(const ManagedNode&) = delete;
    ManagedNode& operator=(const ManagedNode&) = delete;

    const std::string& name() const { return name_; }
    State state() const { return machine_.state(); }

    // Returns the process exit code.
    int run();

protected:
    virtual CallbackResult on_configure() { return CallbackResult::Success; }
    virtual CallbackResult on_cleanup() { return CallbackResult::Success; }
    virtual CallbackResult on_activate() { return CallbackResult::Success; }
    virtual CallbackResult on_deactivate() { return CallbackResult::Success; }
    virtual CallbackResult on_shutdown() { return CallbackResult::Success; }
    virtual CallbackResult on_error() { return CallbackResult::Success; }

    // Periodic work while Active.
    virtual void on_tick() {}

private:
    struct Dds;

    void handle_command(std::uint32_t request_id, Transition transition);
    void publish_status(State state, std::uint32_t request_id, bool success, const std::string& message);
    void shutdown_gracefully();
    void publish_heartbeat();
    void run_tick(std::chrono::steady_clock::time_point& next_tick);

    std::string name_;
    std::uint64_t manager_id_ = 0;  // From VRM_MANAGER_ID; 0 accepts any manager.
    std::chrono::microseconds tick_period_;
    DeadlineMonitor monitor_;
    std::uint64_t heartbeat_sequence_ = 0;
    LifecycleStateMachine machine_;
    std::unique_ptr<Dds> dds_;
    std::uint32_t current_request_ = 0;

    // The manager resends a command until it sees a reply, so remember the
    // last one handled and answer duplicates without running it again.
    std::uint32_t last_request_ = 0;
    bool last_success_ = true;
    std::string last_message_;
};

// Set by SIGINT / SIGTERM; checked by ManagedNode::run().
extern std::atomic<bool> g_stop_requested;
void install_stop_signal_handlers();

}  // namespace vrm
