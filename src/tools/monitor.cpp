// vrm_monitor: follows every running resource manager over DDS and shows its
// nodes like `top`. Runs anywhere on the network - another container, another
// machine - since it only needs DDS.
//
//   vrm_monitor               refresh every second until Ctrl-C
//   vrm_monitor --once 2      wait 2 s for data, print one snapshot, exit
//                             (exit code 1 if no manager was seen)
#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <tuple>

#include "vrm/cgroup.hpp"
#include "vrm/dds_telemetry.hpp"
#include "vrm/managed_node.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Seen {
    vrm::msg::NodeStatusReport report;
    Clock::time_point at;
};

std::string ms(unsigned long microseconds) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", microseconds / 1000.0);
    return text;
}

std::string criticality_short(const std::string& criticality) {
    if (criticality == "safety_critical") return "safety";
    if (criticality == "mission_critical") return "mission";
    return "best-eff";
}

void print(const std::map<std::tuple<std::uint64_t, std::string>, Seen>& nodes,
           const std::deque<vrm::msg::SystemEvent>& events, bool clear) {
    if (clear) std::printf("\x1b[H\x1b[2J");
    std::uint64_t current_manager = 0;
    const auto now = Clock::now();
    for (const auto& [key, seen] : nodes) {
        const auto& r = seen.report;
        if (std::get<0>(key) != current_manager) {
            current_manager = std::get<0>(key);
            std::printf("\nSYSTEM %s  (manager %016llx)\n", r.system().c_str(),
                        static_cast<unsigned long long>(current_manager));
            std::printf("%-15s %-8s %-11s %13s %5s %17s %-13s %15s %6s %s\n", "NODE", "CRIT", "STATE", "CPU/LIMIT %",
                        "WAIT", "MEMORY/LIMIT", "SCHED", "RESP/DEADLINE", "MISS", "ARBITRATION");
        }
        const bool stale = now - seen.at > std::chrono::seconds(3);
        char cpu[32];
        if (r.cpu_limit_percent() >= 0) {
            std::snprintf(cpu, sizeof(cpu), "%.1f/%.0f", r.cpu_percent(), r.cpu_limit_percent());
        } else {
            std::snprintf(cpu, sizeof(cpu), "%.1f/-", r.cpu_percent());
        }
        const std::string memory = vrm::format_bytes(r.memory_bytes()) + "/" +
                                   (r.memory_limit_bytes() ? vrm::format_bytes(r.memory_limit_bytes()) : "-");
        const std::string sched = r.sched_policy().empty() ? "-"
                                  : r.sched_priority()     ? r.sched_policy().substr(6) + " " + std::to_string(r.sched_priority())
                                                           : r.sched_policy().substr(6);
        const std::string timing =
            r.period_us() ? ms(r.max_response_us()) + "/" + (r.deadline_us() ? ms(r.deadline_us()) : "-") : "-";
        const std::string state = r.running() ? r.state() : r.outcome();
        std::string arbitration = r.arbitration_state();
        if (r.restarts()) arbitration += (arbitration.empty() ? "" : ", ") + std::string("restarts ") + std::to_string(r.restarts());
        std::printf("%-15s %-8s %-11s %13s %4.0f%% %17s %-13s %15s %6llu %s%s\n", r.node().c_str(),
                    criticality_short(r.criticality()).c_str(), state.c_str(), cpu, r.cpu_pressure() * 100.0,
                    memory.c_str(), sched.c_str(), timing.c_str(), static_cast<unsigned long long>(r.total_misses()),
                    arbitration.c_str(), stale ? "  (stale)" : "");
    }
    if (nodes.empty()) std::printf("waiting for a vrm_manager on this DDS domain...\n");

    std::printf("\nRECENT EVENTS\n");
    for (const auto& event : events) {
        const auto seconds = static_cast<std::time_t>(event.time());
        std::tm local{};
        localtime_r(&seconds, &local);
        char time[16];
        std::strftime(time, sizeof(time), "%H:%M:%S", &local);
        std::printf("%s %-5s %s\n", time, event.level().c_str(), event.message().c_str());
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    double once_after = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once" && i + 1 < argc) {
            once_after = std::stod(argv[++i]);
        } else {
            std::cerr << "usage: vrm_monitor [--once SECONDS]\n";
            return 2;
        }
    }
    vrm::install_stop_signal_handlers();

    dds::domain::DomainParticipant participant(org::eclipse::cyclonedds::domain::default_id());
    dds::sub::Subscriber subscriber(participant);
    dds::topic::Topic<vrm::msg::NodeStatusReport> status_topic(participant, vrm::dds_telemetry::kNodeStatusTopic);
    dds::topic::Topic<vrm::msg::SystemEvent> event_topic(participant, vrm::dds_telemetry::kSystemEventTopic,
                                                         vrm::dds_telemetry::event_topic_qos(participant));
    dds::sub::DataReader<vrm::msg::NodeStatusReport> status_reader(
        subscriber, status_topic, vrm::dds_telemetry::status_reader_qos(subscriber));
    dds::sub::DataReader<vrm::msg::SystemEvent> event_reader(subscriber, event_topic,
                                                            vrm::dds_telemetry::event_reader_qos(subscriber));

    std::map<std::tuple<std::uint64_t, std::string>, Seen> nodes;
    std::deque<vrm::msg::SystemEvent> events;
    const auto start = Clock::now();
    auto next_print = start;

    while (!vrm::g_stop_requested) {
        for (const auto& sample : status_reader.take()) {
            if (!sample.info().valid()) continue;
            nodes[{sample.data().manager_id(), sample.data().node()}] = Seen{sample.data(), Clock::now()};
        }
        for (const auto& sample : event_reader.take()) {
            if (!sample.info().valid()) continue;
            events.push_back(sample.data());
            while (events.size() > 10) events.pop_front();
        }

        const auto now = Clock::now();
        if (once_after >= 0) {
            if (now - start >= std::chrono::duration<double>(once_after)) {
                print(nodes, events, false);
                return nodes.empty() ? 1 : 0;
            }
        } else if (now >= next_print) {
            print(nodes, events, true);
            next_print += std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}
