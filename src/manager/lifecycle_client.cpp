#include "lifecycle_client.hpp"

#include <thread>

#include "vrm/dds_lifecycle.hpp"
#include "vrm/log.hpp"

namespace vrm {

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kResendInterval = std::chrono::milliseconds(300);
}  // namespace

struct LifecycleClient::Dds {
    dds::domain::DomainParticipant participant{org::eclipse::cyclonedds::domain::default_id()};
    dds::topic::Topic<msg::LifecycleCommand> command_topic{participant, dds_lifecycle::kCommandTopic};
    dds::topic::Topic<msg::LifecycleStatus> status_topic{participant, dds_lifecycle::kStatusTopic};
    dds::pub::Publisher publisher{participant};
    dds::sub::Subscriber subscriber{participant};
    dds::pub::DataWriter<msg::LifecycleCommand> command_writer{
        publisher, command_topic, dds_lifecycle::command_writer_qos(publisher)};
    dds::sub::DataReader<msg::LifecycleStatus> status_reader{
        subscriber, status_topic, dds_lifecycle::status_reader_qos(subscriber)};
};

LifecycleClient::LifecycleClient() : dds_(std::make_unique<Dds>()) {}
LifecycleClient::~LifecycleClient() = default;

void LifecycleClient::poll() {
    for (const auto& sample : dds_->status_reader.take()) {
        if (!sample.info().valid()) {
            continue;
        }
        const auto& data = sample.data();
        NodeStatus next{dds_lifecycle::from_msg(data.state()), data.request_id(), data.success(),
                        data.message(), data.pid()};

        auto& current = latest_[data.node()];
        const bool changed = current.state != next.state || current.pid != next.pid;
        if (changed || (!next.success && next.request_id != current.request_id)) {
            std::string line = std::string(to_string(next.state));
            if (!next.message.empty()) {
                line += " (" + next.message + ")";
            }
            if (next.success) {
                log::info("manager", data.node() + ": " + line);
            } else {
                log::warn("manager", data.node() + ": " + line);
            }
        }
        current = next;
    }
}

std::optional<NodeStatus> LifecycleClient::status(const std::string& node) const {
    const auto it = latest_.find(node);
    if (it == latest_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool LifecycleClient::wait_for_state(const std::string& node, int pid, State state,
                                     std::chrono::milliseconds timeout, const std::function<bool()>& alive) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        poll();
        const auto current = status(node);
        if (current && current->pid == pid && current->state == state) {
            return true;
        }
        if (!alive()) {
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

RequestResult LifecycleClient::request(const std::string& node, Transition transition,
                                       std::chrono::milliseconds timeout, const std::function<bool()>& alive) {
    const std::uint32_t id = next_request_id_++;
    const msg::LifecycleCommand command(node, id, dds_lifecycle::to_msg(transition));

    const auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (std::chrono::steady_clock::now() >= next_send) {
            dds_->command_writer.write(command);
            next_send += kResendInterval;
        }
        poll();
        const auto current = status(node);
        if (current && current->request_id == id && is_primary(current->state)) {
            return {current->success, current->state, current->message};
        }
        if (!alive()) {
            return {false, std::nullopt, "process exited"};
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return {false, std::nullopt, "no reply within " + std::to_string(timeout.count()) + " ms"};
}

}  // namespace vrm
