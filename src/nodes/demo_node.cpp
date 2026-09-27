// Configurable demo node used to model vehicle functions.
//
//   vrm_demo_node --name brake_control --period-ms 10 --work-ms 2
//   vrm_demo_node --name infotainment --memory-mb 64 --fail-on activate
//
// --period-ms  Tick period while Active.
// --work-ms    CPU time burned per tick (busy loop on the thread's CPU clock),
//              to model computation.
// --memory-mb  Memory allocated and touched in on_configure, freed in on_cleanup.
// --leak-mb-per-sec  Memory allocated (and never freed) per second while Active,
//              to model a runaway process.
// --fail-on    Make a transition fail: configure, activate or error-on-configure.
// --deadline-ms  Deadline of each tick, relative to its release (default: the period).
// --no-deadline  Batch work: measure timing but never count deadline misses.
// --crash-after-sec  Abort (SIGABRT) this many seconds after activation.
// --hang-after-sec   Stop responding (endless loop) this many seconds after activation.
#include <time.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "vrm/log.hpp"
#include "vrm/managed_node.hpp"

namespace {

struct Options {
    std::string name;
    int period_ms = 100;
    int work_ms = 0;
    int memory_mb = 0;
    double leak_mb_per_sec = 0.0;
    double deadline_ms = 0.0;
    bool no_deadline = false;
    double crash_after_sec = 0.0;
    double hang_after_sec = 0.0;
    std::string fail_on;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--name") options.name = value();
        else if (arg == "--period-ms") options.period_ms = std::stoi(value());
        else if (arg == "--work-ms") options.work_ms = std::stoi(value());
        else if (arg == "--memory-mb") options.memory_mb = std::stoi(value());
        else if (arg == "--leak-mb-per-sec") options.leak_mb_per_sec = std::stod(value());
        else if (arg == "--fail-on") options.fail_on = value();
        else if (arg == "--deadline-ms") options.deadline_ms = std::stod(value());
        else if (arg == "--no-deadline") options.no_deadline = true;
        else if (arg == "--crash-after-sec") options.crash_after_sec = std::stod(value());
        else if (arg == "--hang-after-sec") options.hang_after_sec = std::stod(value());
        else {
            std::cerr << "unknown option " << arg << "\n";
            std::exit(2);
        }
    }
    if (options.name.empty()) {
        std::cerr << "usage: vrm_demo_node --name NAME [--period-ms N] [--work-ms N] "
                     "[--memory-mb N] [--leak-mb-per-sec N] [--deadline-ms N | --no-deadline]\n"
                     "       [--crash-after-sec N] [--hang-after-sec N] [--fail-on configure|activate|error-on-configure]\n";
        std::exit(2);
    }
    return options;
}

class DemoNode : public vrm::ManagedNode {
public:
    explicit DemoNode(Options options)
        : ManagedNode(options.name, std::chrono::milliseconds(options.period_ms),
                      options.no_deadline ? std::chrono::microseconds{-1}
                                          : std::chrono::microseconds(static_cast<long>(options.deadline_ms * 1000))),
          options_(std::move(options)) {}

protected:
    vrm::CallbackResult on_configure() override {
        if (options_.fail_on == "configure") return vrm::CallbackResult::Failure;
        if (options_.fail_on == "error-on-configure") return vrm::CallbackResult::Error;

        const std::size_t bytes = static_cast<std::size_t>(options_.memory_mb) * 1024 * 1024;
        buffer_.assign(bytes, 0);
        // Touch every page so the memory is actually resident.
        for (std::size_t i = 0; i < buffer_.size(); i += 4096) buffer_[i] = 1;
        return vrm::CallbackResult::Success;
    }

    vrm::CallbackResult on_cleanup() override {
        std::vector<char>().swap(buffer_);
        return vrm::CallbackResult::Success;
    }

    vrm::CallbackResult on_activate() override {
        if (options_.fail_on == "activate") return vrm::CallbackResult::Failure;
        ticks_ = 0;
        activated_at_ = std::chrono::steady_clock::now();
        return vrm::CallbackResult::Success;
    }

    vrm::CallbackResult on_deactivate() override {
        vrm::log::info(name(), "ran " + std::to_string(ticks_) + " ticks while active");
        return vrm::CallbackResult::Success;
    }

    void on_tick() override {
        const double active_for =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - activated_at_).count();
        if (options_.crash_after_sec > 0 && active_for >= options_.crash_after_sec) {
            vrm::log::warn(name(), "simulating a crash");
            std::abort();
        }
        if (options_.hang_after_sec > 0 && active_for >= options_.hang_after_sec) {
            vrm::log::warn(name(), "simulating a hang");
            for (volatile bool hung = true; hung;) {
            }
        }
        // Burn `work_ms` of CPU time (not wall time): like a real computation,
        // the tick takes longer when other tasks get the CPU in between.
        const auto until = thread_cpu_time() + std::chrono::milliseconds(options_.work_ms);
        while (thread_cpu_time() < until) {
        }
        ++ticks_;
        leak();
    }

private:
    static std::chrono::nanoseconds thread_cpu_time() {
        timespec now{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
        return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
    }

    void leak() {
        if (options_.leak_mb_per_sec <= 0.0) return;
        const auto now = std::chrono::steady_clock::now();
        if (last_leak_.time_since_epoch().count() == 0) last_leak_ = now;
        const double seconds = std::chrono::duration<double>(now - last_leak_).count();
        const auto bytes = static_cast<std::size_t>(seconds * options_.leak_mb_per_sec * 1024 * 1024);
        if (bytes < 1024 * 1024) return;
        last_leak_ = now;
        leaked_.emplace_back(bytes, 1);  // Filled with 1s, so the pages are resident.
    }

    Options options_;
    std::vector<std::vector<char>> leaked_;
    std::chrono::steady_clock::time_point last_leak_{};
    std::vector<char> buffer_;
    long ticks_ = 0;
    std::chrono::steady_clock::time_point activated_at_{};
};

}  // namespace

int main(int argc, char** argv) {
    vrm::install_stop_signal_handlers();
    DemoNode node(parse_options(argc, argv));
    return node.run();
}
