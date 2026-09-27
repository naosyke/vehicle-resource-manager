// Timing statistics of a periodic task.
//
// For every activation (tick) the node records:
//   release  - when the tick was due
//   start    - when it actually started      latency  = start - release
//   end      - when it finished             response = end - release
// A deadline miss is a response time above the deadline, or a release that
// was skipped because the previous tick overran.
#pragma once

#include <chrono>
#include <cstdint>

namespace vrm {

struct TimingWindow {
    std::uint64_t ticks = 0;
    std::uint64_t misses = 0;
    std::chrono::microseconds max_latency{0};
    std::chrono::microseconds max_response{0};
    std::chrono::microseconds total_response{0};

    std::chrono::microseconds average_response() const {
        return ticks ? total_response / static_cast<std::int64_t>(ticks) : std::chrono::microseconds{0};
    }
};

class DeadlineMonitor {
public:
    using Clock = std::chrono::steady_clock;

    // A deadline of zero means "no deadline": timing is still measured, but
    // nothing counts as a miss (for batch work that just uses spare CPU).
    explicit DeadlineMonitor(std::chrono::microseconds deadline) : deadline_(deadline) {}

    std::chrono::microseconds deadline() const { return deadline_; }
    bool has_deadline() const { return deadline_.count() > 0; }

    void record(Clock::time_point release, Clock::time_point start, Clock::time_point end);

    // Releases skipped because a tick overran; each counts as a miss.
    void record_skipped(std::uint64_t count);

    // Statistics since the last call (for periodic heartbeats).
    TimingWindow take_window();

    // Totals since construction or reset().
    std::uint64_t total_ticks() const { return total_ticks_; }
    std::uint64_t total_misses() const { return total_misses_; }

    void reset();

private:
    std::chrono::microseconds deadline_;
    TimingWindow window_;
    std::uint64_t total_ticks_ = 0;
    std::uint64_t total_misses_ = 0;
};

}  // namespace vrm
