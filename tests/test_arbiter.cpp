#include <gtest/gtest.h>

#include <vector>

#include "vrm/arbiter.hpp"

using vrm::Arbiter;
using vrm::ArbiterActionType;
using vrm::ArbiterNode;
using vrm::Criticality;

namespace {

ArbiterNode node(const char* name, Criticality criticality, std::vector<int> cpus = {0}) {
    ArbiterNode n;
    n.name = name;
    n.criticality = criticality;
    n.cpus = std::move(cpus);
    n.active = true;
    return n;
}

// brake (safety), perception (mission) and two best-effort hogs on CPU 0.
std::vector<ArbiterNode> system_on_cpu0() {
    auto hog_a = node("map_renderer", Criticality::BestEffort);
    hog_a.cpu_percent = 40;
    auto hog_b = node("infotainment", Criticality::BestEffort);
    hog_b.cpu_percent = 45;
    return {node("brake_control", Criticality::SafetyCritical), node("perception", Criticality::MissionCritical),
            hog_a, hog_b};
}

}  // namespace

TEST(Arbiter, NoActionWithoutInterference) {
    Arbiter arbiter;
    EXPECT_TRUE(arbiter.decide(system_on_cpu0(), 0).empty());
}

TEST(Arbiter, ThrottlesEveryBestEffortNodeBeforeDeactivatingAny) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].missed_deadlines = true;  // perception misses deadlines...

    EXPECT_TRUE(arbiter.decide(nodes, -0.5).empty());  // ...but one round could be a spike.
    auto first = arbiter.decide(nodes, 0);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].type, ArbiterActionType::Throttle);
    EXPECT_EQ(first[0].node, "infotainment");  // Biggest CPU user first.
    EXPECT_EQ(first[0].reason, "perception keeps missing deadlines");

    EXPECT_TRUE(arbiter.decide(nodes, 1.0).empty());  // Waits for the escalation interval.

    auto second = arbiter.decide(nodes, 2.0);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].type, ArbiterActionType::Throttle);
    EXPECT_EQ(second[0].node, "map_renderer");

    auto third = arbiter.decide(nodes, 4.0);
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(third[0].type, ArbiterActionType::Deactivate);
    EXPECT_EQ(third[0].node, "infotainment");
}

TEST(Arbiter, DegradesUpToStopAndNeverTouchesMoreCriticalNodes) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].missed_deadlines = true;

    std::vector<std::pair<ArbiterActionType, std::string>> steps;
    for (int i = 0; i < 20; ++i) {
        for (const auto& action : arbiter.decide(nodes, i * 1.0)) steps.emplace_back(action.type, action.node);
    }

    // Two best-effort nodes x three levels; perception itself and brake are never degraded.
    ASSERT_EQ(steps.size(), 6u);
    EXPECT_EQ(steps.back().first, ArbiterActionType::Stop);
    for (const auto& step : steps) {
        EXPECT_NE(step.second, "perception");
        EXPECT_NE(step.second, "brake_control");
    }
    EXPECT_EQ(arbiter.level("infotainment"), 3);
    EXPECT_EQ(arbiter.level("map_renderer"), 3);
}

TEST(Arbiter, SafetyPressureMayDegradeMissionNodes) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[0].missed_deadlines = true;  // brake misses deadlines.
    nodes.erase(nodes.begin() + 2, nodes.end());  // Only brake and perception left.

    EXPECT_TRUE(arbiter.decide(nodes, 0).empty());  // One round could be a spike.
    const auto actions = arbiter.decide(nodes, 0.5);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].node, "perception");
    EXPECT_EQ(actions[0].reason, "brake_control keeps missing deadlines");
}

TEST(Arbiter, CpuPressureAloneOnlyThrottles) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].cpu_pressure = 0.4;  // Waiting, but meeting its deadlines.

    std::vector<ArbiterActionType> steps;
    for (int i = 0; i < 10; ++i) {
        for (const auto& action : arbiter.decide(nodes, i * 2.0)) steps.push_back(action.type);
    }
    EXPECT_EQ(steps, (std::vector<ArbiterActionType>{ArbiterActionType::Throttle, ArbiterActionType::Throttle}));
}

TEST(Arbiter, CpuPressureOfRealtimeNodeIsIgnored) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[0].realtime = true;
    nodes[0].cpu_pressure = 0.4;  // brake's own threads waiting for each other.
    EXPECT_TRUE(arbiter.decide(nodes, 0).empty());
}

TEST(Arbiter, MissesOfRealtimeNodeNeverDegradeNormalNodes) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[0].realtime = true;  // brake runs SCHED_FIFO...
    nodes[0].missed_deadlines = true;  // ...and still misses (e.g. VM stalls).

    for (int i = 0; i < 10; ++i) EXPECT_TRUE(arbiter.decide(nodes, i * 2.0).empty());

    nodes[3].realtime = true;  // A real-time infotainment on the same CPU could be the cause.
    arbiter.decide(nodes, 30);
    const auto actions = arbiter.decide(nodes, 32);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].node, "infotainment");
}

TEST(Arbiter, PressureBelowThresholdIsIgnored) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].cpu_pressure = 0.15;  // e.g. waiting for the more critical brake.
    EXPECT_TRUE(arbiter.decide(nodes, 0).empty());
}

TEST(Arbiter, RecoveryWaitsLongerAfterRelapse) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].cpu_pressure = 0.4;
    arbiter.decide(nodes, 0);  // Throttle infotainment.

    nodes[1].cpu_pressure = 0.0;
    ASSERT_EQ(arbiter.decide(nodes, 5).size(), 1u);  // Unthrottle after 5 s.
    EXPECT_DOUBLE_EQ(arbiter.recovery_seconds(), 5);

    nodes[1].cpu_pressure = 0.4;  // Interference is back right away.
    ASSERT_EQ(arbiter.decide(nodes, 6).size(), 1u);
    EXPECT_DOUBLE_EQ(arbiter.recovery_seconds(), 10);

    nodes[1].cpu_pressure = 0.0;
    EXPECT_TRUE(arbiter.decide(nodes, 12).empty());   // 6 s calm is no longer enough.
    EXPECT_EQ(arbiter.decide(nodes, 16).size(), 1u);  // 10 s is.
}

TEST(Arbiter, IgnoresNodesOnOtherCpus) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].cpu_pressure = 0.5;  // Throttling is enough to test the CPU overlap.
    nodes[2].cpus = {3};
    nodes[3].cpus = {4, 5};

    EXPECT_TRUE(arbiter.decide(nodes, 0).empty());

    nodes[3].cpus = {};  // Unpinned: may run on CPU 0.
    const auto actions = arbiter.decide(nodes, 10);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].node, "infotainment");
}

TEST(Arbiter, RestoresOneStepAfterCalmPeriod) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[1].missed_deadlines = true;
    arbiter.decide(nodes, -0.5);
    arbiter.decide(nodes, 0);  // Throttle infotainment.
    arbiter.decide(nodes, 2);  // Throttle map_renderer.
    arbiter.decide(nodes, 4);  // Deactivate infotainment.

    nodes[1].missed_deadlines = false;
    EXPECT_TRUE(arbiter.decide(nodes, 8).empty());  // Calm for 4 s only.

    auto first = arbiter.decide(nodes, 9.5);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].type, ArbiterActionType::Reactivate);
    EXPECT_EQ(first[0].node, "infotainment");

    EXPECT_TRUE(arbiter.decide(nodes, 12).empty());
    auto second = arbiter.decide(nodes, 15);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].type, ArbiterActionType::Unthrottle);
    EXPECT_EQ(arbiter.level("infotainment") + arbiter.level("map_renderer"), 1);
}

TEST(Arbiter, StopsNodeBeforeOomButNeverSafetyNodes) {
    Arbiter arbiter;
    auto nodes = system_on_cpu0();
    nodes[0].memory_fraction = 0.95;  // brake
    nodes[3].memory_fraction = 0.93;  // infotainment

    const auto actions = arbiter.decide(nodes, 0);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].type, ArbiterActionType::StopBeforeOom);
    EXPECT_EQ(actions[0].node, "infotainment");
    EXPECT_EQ(actions[0].reason, "memory at 93% of its limit");
    EXPECT_TRUE(arbiter.decide(nodes, 1).empty());  // Only once.
}

TEST(Arbiter, DemotesRealtimeNodeThatKeepsExceedingItsBudget) {
    Arbiter arbiter;
    auto runaway = node("planner", Criticality::MissionCritical);
    runaway.realtime = true;
    runaway.cpu_budget_percent = 30;
    runaway.cpu_percent = 85;

    EXPECT_TRUE(arbiter.decide({runaway}, 0).empty());
    EXPECT_TRUE(arbiter.decide({runaway}, 0.5).empty());
    const auto actions = arbiter.decide({runaway}, 1.0);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions[0].type, ArbiterActionType::DemoteRealtime);
    EXPECT_EQ(actions[0].reason, "SCHED_FIFO node uses 85% CPU, budget 30%");

    runaway.cpu_percent = 32;  // Within budget + 5% tolerance.
    EXPECT_TRUE(arbiter.decide({runaway}, 5).empty());
}

TEST(Arbiter, DisabledDoesNothing) {
    vrm::ArbitrationConfig config;
    config.enabled = false;
    Arbiter arbiter(config);
    auto nodes = system_on_cpu0();
    nodes[1].cpu_pressure = 0.9;
    nodes[1].missed_deadlines = true;
    nodes[3].memory_fraction = 0.99;
    EXPECT_TRUE(arbiter.decide(nodes, 0).empty());
    EXPECT_TRUE(arbiter.decide(nodes, 1).empty());
}
