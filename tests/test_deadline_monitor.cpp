#include <gtest/gtest.h>

#include "vrm/deadline_monitor.hpp"

using namespace std::chrono_literals;
using vrm::DeadlineMonitor;

namespace {
const auto t0 = DeadlineMonitor::Clock::time_point{} + 1s;
}

TEST(DeadlineMonitor, MeasuresLatencyAndResponse) {
    DeadlineMonitor monitor(4ms);

    monitor.record(t0, t0 + 100us, t0 + 2100us);
    monitor.record(t0 + 10ms, t0 + 10ms + 300us, t0 + 10ms + 2300us);

    const auto window = monitor.take_window();
    EXPECT_EQ(window.ticks, 2u);
    EXPECT_EQ(window.misses, 0u);
    EXPECT_EQ(window.max_latency, 300us);
    EXPECT_EQ(window.max_response, 2300us);
    EXPECT_EQ(window.average_response(), 2200us);
}

TEST(DeadlineMonitor, ResponseAboveDeadlineIsAMiss) {
    DeadlineMonitor monitor(4ms);

    monitor.record(t0, t0 + 3ms, t0 + 5ms);  // Started late, finished after the deadline.
    monitor.record(t0 + 10ms, t0 + 10ms, t0 + 14ms);  // Exactly on the deadline: not a miss.

    EXPECT_EQ(monitor.take_window().misses, 1u);
    EXPECT_EQ(monitor.total_misses(), 1u);
}

TEST(DeadlineMonitor, SkippedReleasesAreMisses) {
    DeadlineMonitor monitor(10ms);
    monitor.record_skipped(3);
    EXPECT_EQ(monitor.total_misses(), 3u);
    EXPECT_EQ(monitor.total_ticks(), 0u);
}

TEST(DeadlineMonitor, WindowResetsButTotalsAccumulate) {
    DeadlineMonitor monitor(1ms);
    monitor.record(t0, t0, t0 + 2ms);
    monitor.take_window();
    monitor.record(t0 + 10ms, t0 + 10ms, t0 + 10500us);

    const auto window = monitor.take_window();
    EXPECT_EQ(window.ticks, 1u);
    EXPECT_EQ(window.misses, 0u);
    EXPECT_EQ(window.max_response, 500us);
    EXPECT_EQ(monitor.total_ticks(), 2u);
    EXPECT_EQ(monitor.total_misses(), 1u);

    monitor.reset();
    EXPECT_EQ(monitor.total_ticks(), 0u);
}

TEST(DeadlineMonitor, NoDeadlineNeverMisses) {
    DeadlineMonitor monitor(0us);
    monitor.record(t0, t0 + 5ms, t0 + 50ms);
    monitor.record_skipped(4);
    const auto window = monitor.take_window();
    EXPECT_FALSE(monitor.has_deadline());
    EXPECT_EQ(window.ticks, 1u);
    EXPECT_EQ(window.misses, 0u);
    EXPECT_EQ(window.max_response, 50ms);
}

TEST(DeadlineMonitor, EmptyWindow) {
    DeadlineMonitor monitor(1ms);
    const auto window = monitor.take_window();
    EXPECT_EQ(window.ticks, 0u);
    EXPECT_EQ(window.average_response(), 0us);
}
