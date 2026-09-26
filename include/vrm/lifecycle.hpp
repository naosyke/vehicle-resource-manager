// Node lifecycle state machine, modeled on the ROS 2 managed node design
// (https://design.ros2.org/articles/node_lifecycle.html).
//
// Primary states:    Unconfigured, Inactive, Active, Finalized
// Transition states: Configuring, CleaningUp, Activating, Deactivating,
//                    ShuttingDown, ErrorProcessing
//
// A transition runs the matching callback in its transition state and moves
// to the next primary state depending on the callback result.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vrm {

enum class State {
    Unconfigured,
    Inactive,
    Active,
    Finalized,
    Configuring,
    CleaningUp,
    Activating,
    Deactivating,
    ShuttingDown,
    ErrorProcessing,
};

enum class Transition {
    Configure,
    Cleanup,
    Activate,
    Deactivate,
    Shutdown,
};

enum class CallbackResult {
    Success,
    Failure,  // The transition was rejected; return to the previous state.
    Error,    // Something went wrong; go through ErrorProcessing.
};

std::string_view to_string(State state);
std::string_view to_string(Transition transition);
std::string_view to_string(CallbackResult result);
std::optional<Transition> transition_from_string(std::string_view name);

bool is_primary(State state);

struct LifecycleCallbacks {
    std::function<CallbackResult()> on_configure;
    std::function<CallbackResult()> on_cleanup;
    std::function<CallbackResult()> on_activate;
    std::function<CallbackResult()> on_deactivate;
    std::function<CallbackResult()> on_shutdown;
    std::function<CallbackResult()> on_error;
};

struct TransitionResult {
    bool success;         // True when the requested transition completed.
    State state;          // Primary state after the transition.
    std::string message;  // Reason when success is false.
};

class LifecycleStateMachine {
public:
    // Called on every state change, including transition states.
    using Observer = std::function<void(State from, State to)>;

    explicit LifecycleStateMachine(LifecycleCallbacks callbacks = {});

    State state() const { return state_; }

    void set_observer(Observer observer) { observer_ = std::move(observer); }

    // Runs a transition synchronously. Invalid transitions leave the state
    // unchanged and return success = false.
    TransitionResult trigger(Transition transition);

    // The transition state a transition enters from `from`, or nullopt when
    // the transition is not allowed there.
    static std::optional<State> transition_state(State from, Transition transition);

private:
    struct Outcome {
        State on_success;
        State on_failure;
    };

    CallbackResult run_callback(const std::function<CallbackResult()>& callback);
    void set_state(State next);
    TransitionResult process_error(Transition transition);

    LifecycleCallbacks callbacks_;
    Observer observer_;
    State state_ = State::Unconfigured;
};

}  // namespace vrm
