#include <gtest/gtest.h>

#include "cpu_tracer.hpp"

using vrm::parse_sched_switch;
using vrm::SchedSwitch;

TEST(CpuTracer, ParsesSchedSwitchLine) {
    SchedSwitch event{};
    ASSERT_TRUE(parse_sched_switch(
        "   perception-22      [001] d..2.  4410.303921: sched_switch: prev_comm=perception prev_pid=22 "
        "prev_prio=120 prev_state=R ==> next_comm=brake_control next_pid=14 next_prio=19",
        event));
    EXPECT_EQ(event.cpu, 1);
    EXPECT_DOUBLE_EQ(event.time, 4410.303921);
    EXPECT_EQ(event.next_comm, "brake_control");
}

TEST(CpuTracer, SwitchToIdleHasEmptyName) {
    SchedSwitch event{};
    ASSERT_TRUE(parse_sched_switch(
        " brake_control-14 [000] d..2. 12.5: sched_switch: prev_comm=brake_control prev_pid=14 prev_prio=19 "
        "prev_state=S ==> next_comm=swapper/0 next_pid=0 next_prio=120",
        event));
    EXPECT_EQ(event.cpu, 0);
    EXPECT_EQ(event.next_comm, "");
}

TEST(CpuTracer, IgnoresOtherLines) {
    SchedSwitch event{};
    EXPECT_FALSE(parse_sched_switch("# tracer: nop", event));
    EXPECT_FALSE(parse_sched_switch("  bash-1 [002] d..2. 1.0: sched_wakeup: comm=x pid=2", event));
}
