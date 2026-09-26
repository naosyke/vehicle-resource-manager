#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

#include "vrm/lifecycle.hpp"

using vrm::CallbackResult;
using vrm::LifecycleCallbacks;
using vrm::LifecycleStateMachine;
using vrm::State;
using vrm::Transition;

TEST(Lifecycle, StartsUnconfigured) {
    LifecycleStateMachine machine;
    EXPECT_EQ(machine.state(), State::Unconfigured);
}

TEST(Lifecycle, HappyPathThroughAllPrimaryStates) {
    LifecycleStateMachine machine;
    std::vector<std::pair<State, State>> changes;
    machine.set_observer([&](State from, State to) { changes.emplace_back(from, to); });

    EXPECT_TRUE(machine.trigger(Transition::Configure).success);
    EXPECT_EQ(machine.state(), State::Inactive);
    EXPECT_TRUE(machine.trigger(Transition::Activate).success);
    EXPECT_EQ(machine.state(), State::Active);
    EXPECT_TRUE(machine.trigger(Transition::Deactivate).success);
    EXPECT_EQ(machine.state(), State::Inactive);
    EXPECT_TRUE(machine.trigger(Transition::Cleanup).success);
    EXPECT_EQ(machine.state(), State::Unconfigured);
    EXPECT_TRUE(machine.trigger(Transition::Shutdown).success);
    EXPECT_EQ(machine.state(), State::Finalized);

    // Every transition passes through its transition state.
    const std::vector<std::pair<State, State>> expected = {
        {State::Unconfigured, State::Configuring}, {State::Configuring, State::Inactive},
        {State::Inactive, State::Activating},      {State::Activating, State::Active},
        {State::Active, State::Deactivating},      {State::Deactivating, State::Inactive},
        {State::Inactive, State::CleaningUp},      {State::CleaningUp, State::Unconfigured},
        {State::Unconfigured, State::ShuttingDown}, {State::ShuttingDown, State::Finalized},
    };
    EXPECT_EQ(changes, expected);
}

TEST(Lifecycle, InvalidTransitionIsRejectedWithoutStateChange) {
    LifecycleStateMachine machine;
    int calls = 0;
    machine.set_observer([&](State, State) { ++calls; });

    const auto result = machine.trigger(Transition::Activate);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.state, State::Unconfigured);
    EXPECT_EQ(result.message, "transition 'activate' is not valid in state 'unconfigured'");
    EXPECT_EQ(calls, 0);
}

TEST(Lifecycle, FailureReturnsToPreviousPrimaryState) {
    LifecycleCallbacks callbacks;
    callbacks.on_activate = [] { return CallbackResult::Failure; };
    LifecycleStateMachine machine(callbacks);
    machine.trigger(Transition::Configure);

    const auto result = machine.trigger(Transition::Activate);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.state, State::Inactive);
    EXPECT_EQ(result.message, "on_activate returned failure");
}

TEST(Lifecycle, ErrorGoesThroughErrorProcessingAndRecovers) {
    LifecycleCallbacks callbacks;
    callbacks.on_configure = [] { return CallbackResult::Error; };
    bool error_handled = false;
    callbacks.on_error = [&] {
        error_handled = true;
        return CallbackResult::Success;
    };
    LifecycleStateMachine machine(callbacks);
    std::vector<State> states;
    machine.set_observer([&](State, State to) { states.push_back(to); });

    const auto result = machine.trigger(Transition::Configure);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(error_handled);
    EXPECT_EQ(machine.state(), State::Unconfigured);
    EXPECT_EQ(states, (std::vector<State>{State::Configuring, State::ErrorProcessing, State::Unconfigured}));
}

TEST(Lifecycle, UnrecoverableErrorFinalizes) {
    LifecycleCallbacks callbacks;
    callbacks.on_activate = [] { return CallbackResult::Error; };
    callbacks.on_error = [] { return CallbackResult::Failure; };
    LifecycleStateMachine machine(callbacks);
    machine.trigger(Transition::Configure);

    machine.trigger(Transition::Activate);

    EXPECT_EQ(machine.state(), State::Finalized);
}

TEST(Lifecycle, ExceptionInCallbackIsTreatedAsError) {
    LifecycleCallbacks callbacks;
    callbacks.on_configure = []() -> CallbackResult { throw std::runtime_error("boom"); };
    LifecycleStateMachine machine(callbacks);

    const auto result = machine.trigger(Transition::Configure);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(machine.state(), State::Unconfigured);  // Default on_error recovers.
}

TEST(Lifecycle, ShutdownIsAllowedFromEveryPrimaryStateExceptFinalized) {
    for (const auto steps : {0, 1, 2}) {
        LifecycleStateMachine machine;
        if (steps >= 1) machine.trigger(Transition::Configure);
        if (steps >= 2) machine.trigger(Transition::Activate);

        EXPECT_TRUE(machine.trigger(Transition::Shutdown).success);
        EXPECT_EQ(machine.state(), State::Finalized);
        EXPECT_FALSE(machine.trigger(Transition::Shutdown).success);
    }
}

TEST(Lifecycle, FailedShutdownStillFinalizes) {
    LifecycleCallbacks callbacks;
    callbacks.on_shutdown = [] { return CallbackResult::Failure; };
    LifecycleStateMachine machine(callbacks);

    machine.trigger(Transition::Shutdown);

    EXPECT_EQ(machine.state(), State::Finalized);
}

TEST(Lifecycle, TransitionNamesRoundTrip) {
    for (auto transition : {Transition::Configure, Transition::Cleanup, Transition::Activate,
                            Transition::Deactivate, Transition::Shutdown}) {
        EXPECT_EQ(vrm::transition_from_string(vrm::to_string(transition)), transition);
    }
    EXPECT_FALSE(vrm::transition_from_string("reboot").has_value());
}
