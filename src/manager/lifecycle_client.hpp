// Manager side of the lifecycle protocol: sends LifecycleCommand samples and
// tracks the LifecycleStatus published by each node.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "vrm/lifecycle.hpp"

namespace vrm {

struct NodeStatus {
    State state = State::Unconfigured;
    std::uint32_t request_id = 0;
    bool success = true;
    std::string message;
    int pid = 0;
};

struct RequestResult {
    bool success;
    std::optional<State> state;  // Empty when the node never answered.
    std::string message;
};

class LifecycleClient {
public:
    // Called when a node reports a new state (or a failed request).
    using StateListener = std::function<void(const std::string& node, const NodeStatus& status)>;

    // `manager_id` identifies this manager run; only nodes started with it
    // (VRM_MANAGER_ID) are commanded and tracked.
    explicit LifecycleClient(std::uint64_t manager_id);
    ~LifecycleClient();

    std::uint64_t manager_id() const { return manager_id_; }

    void set_state_listener(StateListener listener) { listener_ = std::move(listener); }

    // Takes new status samples and logs state changes.
    void poll();

    std::optional<NodeStatus> status(const std::string& node) const;

    // Waits until `node` (running as `pid`) reports `state`.
    // `alive` is checked while waiting so a crashed node fails fast.
    bool wait_for_state(const std::string& node, int pid, State state, std::chrono::milliseconds timeout,
                        const std::function<bool()>& alive);

    // Sends a transition and waits for the node's reply. The command is
    // resent periodically, because DDS discovery may not have matched the
    // node's reader yet when the first sample is written.
    RequestResult request(const std::string& node, Transition transition, std::chrono::milliseconds timeout,
                          const std::function<bool()>& alive);

private:
    struct Dds;
    std::unique_ptr<Dds> dds_;
    std::map<std::string, NodeStatus> latest_;
    StateListener listener_;
    std::uint64_t manager_id_;
    std::uint32_t next_request_id_ = 1;
};

}  // namespace vrm
