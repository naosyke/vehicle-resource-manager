#include "vrm/deadline_monitor.hpp"

#include <algorithm>

namespace vrm {

void DeadlineMonitor::record(Clock::time_point release, Clock::time_point start, Clock::time_point end) {
    using std::chrono::duration_cast;
    using std::chrono::microseconds;

    const auto latency = std::max(microseconds{0}, duration_cast<microseconds>(start - release));
    const auto response = std::max(microseconds{0}, duration_cast<microseconds>(end - release));

    ++window_.ticks;
    ++total_ticks_;
    window_.max_latency = std::max(window_.max_latency, latency);
    window_.max_response = std::max(window_.max_response, response);
    window_.total_response += response;
    if (has_deadline() && response > deadline_) {
        ++window_.misses;
        ++total_misses_;
    }
}

void DeadlineMonitor::record_skipped(std::uint64_t count) {
    if (!has_deadline()) return;
    window_.misses += count;
    total_misses_ += count;
}

TimingWindow DeadlineMonitor::take_window() {
    const TimingWindow window = window_;
    window_ = TimingWindow{};
    return window;
}

void DeadlineMonitor::reset() {
    window_ = TimingWindow{};
    total_ticks_ = 0;
    total_misses_ = 0;
}

}  // namespace vrm
