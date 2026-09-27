#include "vrm/managed_node.hpp"

#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <thread>

#include "vrm/dds_lifecycle.hpp"
#include "vrm/log.hpp"

namespace vrm {

std::atomic<bool> g_stop_requested{false};

namespace {
void on_stop_signal(int) { g_stop_requested = true; }
}  // namespace

void install_stop_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

struct ManagedNode::Dds {
    dds::domain::DomainParticipant participant{org::eclipse::cyclonedds::domain::default_id()};
    dds::topic::Topic<msg::LifecycleCommand> command_topic{participant, dds_lifecycle::kCommandTopic};
    dds::topic::Topic<msg::LifecycleStatus> status_topic{participant, dds_lifecycle::kStatusTopic};
    dds::sub::Subscriber subscriber{participant};
    dds::pub::Publisher publisher{participant};
    dds::sub::DataReader<msg::LifecycleCommand> command_reader{
        subscriber, command_topic, dds_lifecycle::command_reader_qos(subscriber)};
    dds::pub::DataWriter<msg::LifecycleStatus> status_writer{
        publisher, status_topic, dds_lifecycle::status_writer_qos(publisher)};
};

ManagedNode::ManagedNode(std::string name, std::chrono::milliseconds tick_period)
    : name_(std::move(name)),
      tick_period_(tick_period),
      machine_(LifecycleCallbacks{
          [this] { return on_configure(); },
          [this] { return on_cleanup(); },
          [this] { return on_activate(); },
          [this] { return on_deactivate(); },
          [this] { return on_shutdown(); },
          [this] { return on_error(); },
      }),
      dds_(std::make_unique<Dds>()) {
    if (const char* id = std::getenv(dds_lifecycle::kManagerIdEnv)) {
        manager_id_ = std::strtoull(id, nullptr, 10);
    }
    // Report every state change, including transition states.
    machine_.set_observer([this](State, State to) {
        if (!is_primary(to)) {
            publish_status(to, current_request_, true, "");
        }
    });
}

ManagedNode::~ManagedNode() = default;

void ManagedNode::publish_status(State state, std::uint32_t request_id, bool success,
                                 const std::string& message) {
    dds_->status_writer.write(msg::LifecycleStatus(manager_id_, name_, dds_lifecycle::to_msg(state), request_id,
                                                   success, message, static_cast<int32_t>(getpid())));
}

void ManagedNode::handle_command(std::uint32_t request_id, Transition transition) {
    if (request_id != 0 && request_id == last_request_) {
        publish_status(machine_.state(), request_id, last_success_, last_message_);
        return;
    }
    current_request_ = request_id;
    const auto result = machine_.trigger(transition);
    if (result.success) {
        log::info(name_, std::string(to_string(transition)) + " -> " + std::string(to_string(result.state)));
    } else {
        log::warn(name_, std::string(to_string(transition)) + " failed: " + result.message);
    }
    publish_status(result.state, request_id, result.success, result.message);
    current_request_ = 0;
    last_request_ = request_id;
    last_success_ = result.success;
    last_message_ = result.message;
}

void ManagedNode::shutdown_gracefully() {
    if (machine_.state() == State::Active) {
        handle_command(0, Transition::Deactivate);
    }
    if (machine_.state() != State::Finalized) {
        handle_command(0, Transition::Shutdown);
    }
}

int ManagedNode::run() {
    publish_status(machine_.state(), 0, true, "started");
    log::info(name_, "started (pid " + std::to_string(getpid()) + "), waiting for commands");

    auto next_tick = std::chrono::steady_clock::now();
    while (machine_.state() != State::Finalized) {
        if (g_stop_requested) {
            log::info(name_, "stop signal received");
            shutdown_gracefully();
            break;
        }

        for (const auto& sample : dds_->command_reader.take()) {
            if (!sample.info().valid() || sample.data().node() != name_) {
                continue;
            }
            // Ignore other systems' managers that happen to use the same node names.
            if (manager_id_ != 0 && sample.data().manager_id() != manager_id_) {
                continue;
            }
            handle_command(sample.data().request_id(), dds_lifecycle::from_msg(sample.data().transition()));
        }

        const auto now = std::chrono::steady_clock::now();
        if (machine_.state() == State::Active && now >= next_tick) {
            on_tick();
            next_tick += tick_period_;
            if (next_tick < now) {
                next_tick = now + tick_period_;  // Skip missed ticks instead of bursting.
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    log::info(name_, "finalized");
    // Give the transient-local status a moment to reach the manager.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

}  // namespace vrm
