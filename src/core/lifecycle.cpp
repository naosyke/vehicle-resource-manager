#include "vrm/lifecycle.hpp"

#include <exception>

namespace vrm {

std::string_view to_string(State state) {
    switch (state) {
        case State::Unconfigured: return "unconfigured";
        case State::Inactive: return "inactive";
        case State::Active: return "active";
        case State::Finalized: return "finalized";
        case State::Configuring: return "configuring";
        case State::CleaningUp: return "cleaning_up";
        case State::Activating: return "activating";
        case State::Deactivating: return "deactivating";
        case State::ShuttingDown: return "shutting_down";
        case State::ErrorProcessing: return "error_processing";
    }
    return "unknown";
}

std::string_view to_string(Transition transition) {
    switch (transition) {
        case Transition::Configure: return "configure";
        case Transition::Cleanup: return "cleanup";
        case Transition::Activate: return "activate";
        case Transition::Deactivate: return "deactivate";
        case Transition::Shutdown: return "shutdown";
    }
    return "unknown";
}

std::string_view to_string(CallbackResult result) {
    switch (result) {
        case CallbackResult::Success: return "success";
        case CallbackResult::Failure: return "failure";
        case CallbackResult::Error: return "error";
    }
    return "unknown";
}

std::optional<Transition> transition_from_string(std::string_view name) {
    for (auto transition : {Transition::Configure, Transition::Cleanup, Transition::Activate,
                            Transition::Deactivate, Transition::Shutdown}) {
        if (to_string(transition) == name) {
            return transition;
        }
    }
    return std::nullopt;
}

bool is_primary(State state) {
    return state == State::Unconfigured || state == State::Inactive || state == State::Active ||
           state == State::Finalized;
}

std::optional<State> LifecycleStateMachine::transition_state(State from, Transition transition) {
    switch (transition) {
        case Transition::Configure:
            if (from == State::Unconfigured) return State::Configuring;
            break;
        case Transition::Cleanup:
            if (from == State::Inactive) return State::CleaningUp;
            break;
        case Transition::Activate:
            if (from == State::Inactive) return State::Activating;
            break;
        case Transition::Deactivate:
            if (from == State::Active) return State::Deactivating;
            break;
        case Transition::Shutdown:
            if (from == State::Unconfigured || from == State::Inactive || from == State::Active) {
                return State::ShuttingDown;
            }
            break;
    }
    return std::nullopt;
}

LifecycleStateMachine::LifecycleStateMachine(LifecycleCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

TransitionResult LifecycleStateMachine::trigger(Transition transition) {
    const State from = state_;
    const auto intermediate = transition_state(from, transition);
    if (!intermediate) {
        return {false, state_,
                "transition '" + std::string(to_string(transition)) + "' is not valid in state '" +
                    std::string(to_string(from)) + "'"};
    }

    Outcome outcome{};
    const std::function<CallbackResult()>* callback = nullptr;
    switch (transition) {
        case Transition::Configure:
            outcome = {State::Inactive, State::Unconfigured};
            callback = &callbacks_.on_configure;
            break;
        case Transition::Cleanup:
            outcome = {State::Unconfigured, State::Inactive};
            callback = &callbacks_.on_cleanup;
            break;
        case Transition::Activate:
            outcome = {State::Active, State::Inactive};
            callback = &callbacks_.on_activate;
            break;
        case Transition::Deactivate:
            outcome = {State::Inactive, State::Active};
            callback = &callbacks_.on_deactivate;
            break;
        case Transition::Shutdown:
            // A node that refuses to shut down is finalized anyway.
            outcome = {State::Finalized, State::Finalized};
            callback = &callbacks_.on_shutdown;
            break;
    }

    set_state(*intermediate);
    switch (run_callback(*callback)) {
        case CallbackResult::Success:
            set_state(outcome.on_success);
            return {true, state_, ""};
        case CallbackResult::Failure:
            set_state(outcome.on_failure);
            return {false, state_, "on_" + std::string(to_string(transition)) + " returned failure"};
        case CallbackResult::Error:
            return process_error(transition);
    }
    return {false, state_, "unreachable"};
}

TransitionResult LifecycleStateMachine::process_error(Transition transition) {
    set_state(State::ErrorProcessing);
    const auto recovered = run_callback(callbacks_.on_error) == CallbackResult::Success;
    set_state(recovered ? State::Unconfigured : State::Finalized);
    return {false, state_,
            "on_" + std::string(to_string(transition)) + " raised an error; " +
                (recovered ? "recovered to unconfigured" : "finalized")};
}

CallbackResult LifecycleStateMachine::run_callback(const std::function<CallbackResult()>& callback) {
    if (!callback) {
        return CallbackResult::Success;
    }
    try {
        return callback();
    } catch (const std::exception&) {
        return CallbackResult::Error;
    } catch (...) {
        return CallbackResult::Error;
    }
}

void LifecycleStateMachine::set_state(State next) {
    const State from = state_;
    state_ = next;
    if (observer_) {
        observer_(from, next);
    }
}

}  // namespace vrm
