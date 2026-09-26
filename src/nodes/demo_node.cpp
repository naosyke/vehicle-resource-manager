// Configurable demo node used to model vehicle functions.
//
//   vrm_demo_node --name brake_control --period-ms 10 --work-ms 2
//   vrm_demo_node --name infotainment --memory-mb 64 --fail-on activate
//
// --period-ms  Tick period while Active.
// --work-ms    CPU time burned per tick (busy loop), to model computation.
// --memory-mb  Memory allocated and touched in on_configure, freed in on_cleanup.
// --fail-on    Make a transition fail: configure, activate or error-on-configure.
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
        else if (arg == "--fail-on") options.fail_on = value();
        else {
            std::cerr << "unknown option " << arg << "\n";
            std::exit(2);
        }
    }
    if (options.name.empty()) {
        std::cerr << "usage: vrm_demo_node --name NAME [--period-ms N] [--work-ms N] "
                     "[--memory-mb N] [--fail-on configure|activate|error-on-configure]\n";
        std::exit(2);
    }
    return options;
}

class DemoNode : public vrm::ManagedNode {
public:
    explicit DemoNode(Options options)
        : ManagedNode(options.name, std::chrono::milliseconds(options.period_ms)),
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
        return vrm::CallbackResult::Success;
    }

    vrm::CallbackResult on_deactivate() override {
        vrm::log::info(name(), "ran " + std::to_string(ticks_) + " ticks while active");
        return vrm::CallbackResult::Success;
    }

    void on_tick() override {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.work_ms);
        while (std::chrono::steady_clock::now() < until) {
            // Busy loop to consume CPU like a real computation would.
        }
        ++ticks_;
    }

private:
    Options options_;
    std::vector<char> buffer_;
    long ticks_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    vrm::install_stop_signal_handlers();
    DemoNode node(parse_options(argc, argv));
    return node.run();
}
