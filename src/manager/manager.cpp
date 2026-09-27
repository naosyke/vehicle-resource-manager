#include "manager.hpp"

#include <signal.h>

#include <cstdio>
#include <sstream>
#include <thread>

#include "process.hpp"
#include "vrm/log.hpp"
#include "vrm/managed_node.hpp"

namespace vrm {

namespace {
constexpr auto kExitGracePeriod = std::chrono::milliseconds(2000);
constexpr std::size_t kMaxEvents = 100;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string json_int_list(const std::vector<int>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + std::to_string(values[i]);
    return out + "]";
}

const char* level_name(int level) {
    return level == 0 ? "info" : level == 1 ? "warn" : "error";
}
}  // namespace

std::string_view to_string(NodeOutcome outcome) {
    switch (outcome) {
        case NodeOutcome::NotStarted: return "not started";
        case NodeOutcome::Active: return "active";
        case NodeOutcome::Failed: return "failed";
        case NodeOutcome::Skipped: return "skipped";
        case NodeOutcome::Exited: return "exited";
        case NodeOutcome::Stopped: return "stopped";
    }
    return "unknown";
}

Manager::Manager(SystemManifest manifest, std::unique_ptr<CgroupManager> cgroups)
    : manifest_(std::move(manifest)), cgroups_(std::move(cgroups)) {
    for (const auto* spec : startup_order(manifest_)) {
        RunningNode node{};
        node.spec = spec;
        nodes_.push_back(node);
    }
    client_.set_state_listener([this](const std::string& node, const NodeStatus& status) {
        std::string message = node + " -> " + std::string(to_string(status.state));
        if (!status.message.empty()) message += " (" + status.message + ")";
        events_.push_back({now_seconds(), status.success ? Level::Info : Level::Warn, node, message});
        while (events_.size() > kMaxEvents) events_.pop_front();
    });
}

void Manager::note(Level level, const std::string& node, const std::string& message) {
    switch (level) {
        case Level::Info: log::info("manager", message); break;
        case Level::Warn: log::warn("manager", message); break;
        case Level::Error: log::error("manager", message); break;
    }
    events_.push_back({now_seconds(), level, node, message});
    while (events_.size() > kMaxEvents) events_.pop_front();
}

Manager::~Manager() { stop(); }

Manager::RunningNode* Manager::find(const std::string& name) {
    for (auto& node : nodes_) {
        if (node.spec->name == name) {
            return &node;
        }
    }
    return nullptr;
}

void Manager::reap_children() {
    while (const auto exit = reap_child()) {
        for (auto& node : nodes_) {
            if (node.pid != exit->pid || !node.running) {
                continue;
            }
            node.running = false;
            auto description = describe_exit(exit->status);
            if (node.in_cgroup) {
                sample_resources(node);
                if (node.usage.oom_kills > 0) {
                    description = "killed by the OOM killer (memory limit " +
                                  format_bytes(*node.spec->resources.memory_bytes) + ")";
                }
            }
            if (node.outcome == NodeOutcome::Active) {
                node.outcome = NodeOutcome::Exited;
                node.detail = description;
                const auto message = node.spec->name + " (" + std::string(to_string(node.spec->criticality)) +
                                     ") " + description + " unexpectedly";
                note(node.spec->criticality == Criticality::SafetyCritical ? Level::Error : Level::Warn,
                     node.spec->name, message);
            }
        }
    }
}

bool Manager::is_running(const RunningNode& node) {
    reap_children();
    return node.running;
}

void Manager::fail_node(RunningNode& node, const std::string& reason) {
    node.outcome = NodeOutcome::Failed;
    node.detail = reason;
    note(Level::Error, node.spec->name, node.spec->name + " failed to start: " + reason);
    if (is_running(node)) {
        const auto alive = [&] { return is_running(node); };
        client_.request(node.spec->name, Transition::Shutdown, manifest_.transition_timeout, alive);
    }
}

bool Manager::start_node(RunningNode& node) {
    const auto& spec = *node.spec;
    for (const auto& dependency : spec.depends_on) {
        const auto* required = find(dependency);
        if (!required || required->outcome != NodeOutcome::Active) {
            node.outcome = NodeOutcome::Skipped;
            node.detail = "dependency '" + dependency + "' is not active";
            note(Level::Warn, spec.name, spec.name + " skipped: " + node.detail);
            return false;
        }
    }

    std::vector<std::string> args = {"--name", spec.name};
    args.insert(args.end(), spec.args.begin(), spec.args.end());
    const auto executable = resolve_executable(spec.executable);
    try {
        std::string cgroup_procs;
        if (cgroups_) {
            cgroup_procs = cgroups_->create_group(spec.name, spec.resources);
            node.in_cgroup = true;
        }
        node.pid = spawn_process(executable, args, cgroup_procs);
    } catch (const std::exception& error) {
        node.outcome = NodeOutcome::Failed;
        node.detail = error.what();
        note(Level::Error, spec.name, spec.name + " could not be spawned: " + error.what());
        return false;
    }
    node.running = true;
    note(Level::Info, spec.name, "spawned " + spec.name + " (pid " + std::to_string(node.pid) + ", " +
                                     std::string(to_string(spec.criticality)) + ")");

    const auto alive = [&] { return is_running(node); };
    if (!client_.wait_for_state(spec.name, node.pid, State::Unconfigured, manifest_.transition_timeout, alive)) {
        fail_node(node, is_running(node) ? "did not come up" : "process exited during startup");
        return false;
    }

    for (const auto transition : {Transition::Configure, Transition::Activate}) {
        const auto result = client_.request(spec.name, transition, manifest_.transition_timeout, alive);
        if (!result.success) {
            fail_node(node, std::string(to_string(transition)) + ": " + result.message);
            return false;
        }
    }

    node.outcome = NodeOutcome::Active;
    return true;
}

bool Manager::start() {
    note(Level::Info, "", "starting system '" + manifest_.name + "' with " + std::to_string(nodes_.size()) + " nodes");
    for (auto& node : nodes_) {
        if (g_stop_requested) {
            return true;
        }
        if (!start_node(node) && node.spec->criticality == Criticality::SafetyCritical) {
            note(Level::Error, node.spec->name,
                 "safety-critical node " + node.spec->name + " is not available; aborting startup");
            return false;
        }
    }
    note(Level::Info, "", "startup complete");
    write_status();
    print_summary();
    return true;
}

void Manager::supervise(std::chrono::milliseconds duration, std::chrono::milliseconds report_interval) {
    const auto start = std::chrono::steady_clock::now();
    auto next_report = start + report_interval;
    for (auto& node : nodes_) {
        if (node.in_cgroup) sample_resources(node);  // Baseline for CPU usage.
    }

    auto next_status = start;
    while (!g_stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (duration.count() > 0 && now - start >= duration) {
            break;
        }
        if (!status_path_.empty() && now >= next_status) {
            write_status();
            next_status += status_interval_;
        }
        if (cgroups_ && report_interval.count() > 0 && now >= next_report) {
            print_resources();
            next_report += report_interval;
        }
        client_.poll();
        reap_children();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Manager::sample_resources(RunningNode& node) {
    const auto now = std::chrono::steady_clock::now();
    const auto usage = cgroups_->usage(node.spec->name);
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - node.sampled_at).count();
    if (node.sampled_at.time_since_epoch().count() > 0 && elapsed_us > 0) {
        node.cpu_percent = 100.0 * static_cast<double>(usage.cpu_usage_usec - node.usage.cpu_usage_usec) /
                           static_cast<double>(elapsed_us);
    }
    node.usage = usage;
    node.sampled_at = now;
}

void Manager::sample_if_stale(RunningNode& node) {
    // Keep at least 500 ms between samples so CPU percentages are stable.
    if (std::chrono::steady_clock::now() - node.sampled_at >= std::chrono::milliseconds(500)) {
        sample_resources(node);
    }
}

void Manager::set_status_file(std::string path, std::chrono::milliseconds interval) {
    status_path_ = std::move(path);
    status_interval_ = interval;
}

void Manager::write_status() {
    if (status_path_.empty()) return;

    std::ostringstream json;
    json << "{\"system\":" << json_string(manifest_.name) << ",\"timestamp\":" << std::fixed << now_seconds()
         << ",\"cgroups\":" << (cgroups_ ? "true" : "false")
         << ",\"available_cpus\":" << json_int_list(cgroups_ ? cgroups_->available_cpus() : std::vector<int>{})
         << ",\"nodes\":[";
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        auto& node = nodes_[i];
        if (node.in_cgroup && node.running) sample_if_stale(node);
        const auto& spec = *node.spec;
        const auto status = client_.status(spec.name);
        const std::string state = status && node.pid != 0 && status->pid == node.pid
                                      ? std::string(to_string(status->state))
                                      : "not_started";
        json << (i ? "," : "") << "{\"name\":" << json_string(spec.name)
             << ",\"criticality\":" << json_string(to_string(spec.criticality))
             << ",\"priority\":" << spec.priority << ",\"pid\":" << node.pid
             << ",\"running\":" << (node.running ? "true" : "false") << ",\"state\":" << json_string(state)
             << ",\"outcome\":" << json_string(to_string(node.outcome))
             << ",\"detail\":" << json_string(node.detail) << ",\"depends_on\":[";
        for (std::size_t d = 0; d < spec.depends_on.size(); ++d) {
            json << (d ? "," : "") << json_string(spec.depends_on[d]);
        }
        json << "],\"budget\":{\"cpu_cores\":";
        if (spec.resources.cpu_cores) json << *spec.resources.cpu_cores; else json << "null";
        json << ",\"memory_bytes\":";
        if (spec.resources.memory_bytes) json << *spec.resources.memory_bytes; else json << "null";
        json << ",\"cpus\":" << json_int_list(spec.resources.cpus) << "}";
        if (node.in_cgroup) {
            json << ",\"usage\":{\"cpu_percent\":" << (node.running ? node.cpu_percent : 0.0)
                 << ",\"memory_bytes\":" << node.usage.memory_current
                 << ",\"memory_peak_bytes\":" << node.usage.memory_peak
                 << ",\"nr_throttled\":" << node.usage.nr_throttled
                 << ",\"throttled_usec\":" << node.usage.throttled_usec
                 << ",\"oom_kills\":" << node.usage.oom_kills << "}";
        } else {
            json << ",\"usage\":null";
        }
        json << "}";
    }
    json << "],\"events\":[";
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const auto& event = events_[i];
        json << (i ? "," : "") << "{\"time\":" << event.time
             << ",\"level\":" << json_string(level_name(static_cast<int>(event.level)))
             << ",\"node\":" << json_string(event.node) << ",\"message\":" << json_string(event.message) << "}";
    }
    json << "]}\n";

    // Write then rename, so readers never see a half-written file.
    const std::string temporary = status_path_ + ".tmp";
    if (FILE* file = std::fopen(temporary.c_str(), "w")) {
        const auto text = json.str();
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        std::rename(temporary.c_str(), status_path_.c_str());
    }
}

void Manager::print_resources() {
    if (!cgroups_) return;
    std::printf("\n%-20s %7s %6s %9s %9s %9s %9s %4s\n", "RESOURCES", "CPU", "LIMIT", "MEMORY", "PEAK",
                "LIMIT", "THROTTLED", "OOM");
    for (auto& node : nodes_) {
        if (!node.in_cgroup) continue;
        if (node.running) sample_if_stale(node);
        const auto& budget = node.spec->resources;
        const std::string cpu_limit =
            budget.cpu_cores ? std::to_string(static_cast<int>(*budget.cpu_cores * 100 + 0.5)) + "%" : "-";
        const std::string memory_limit = budget.memory_bytes ? format_bytes(*budget.memory_bytes) : "-";
        std::printf("%-20s %6.1f%% %6s %9s %9s %9s %9llu %4llu\n", node.spec->name.c_str(),
                    node.running ? node.cpu_percent : 0.0, cpu_limit.c_str(),
                    format_bytes(node.usage.memory_current).c_str(), format_bytes(node.usage.memory_peak).c_str(),
                    memory_limit.c_str(), static_cast<unsigned long long>(node.usage.nr_throttled),
                    static_cast<unsigned long long>(node.usage.oom_kills));
    }
    std::printf("\n");
    std::fflush(stdout);
}

void Manager::stop() {
    bool any_running = false;
    for (auto& node : nodes_) {
        any_running = any_running || is_running(node);
    }
    if (!any_running) {
        return;
    }
    note(Level::Info, "", "shutting down");

    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        auto& node = *it;
        if (!is_running(node)) {
            continue;
        }
        // From here on the process exiting is expected.
        if (node.outcome == NodeOutcome::Active) {
            node.outcome = NodeOutcome::Stopped;
        }
        const auto alive = [&] { return is_running(node); };
        const auto status = client_.status(node.spec->name);
        if (status && status->state == State::Active) {
            client_.request(node.spec->name, Transition::Deactivate, manifest_.transition_timeout, alive);
        }
        client_.request(node.spec->name, Transition::Shutdown, manifest_.transition_timeout, alive);

        // A finalized node exits on its own; escalate if it does not.
        const auto deadline = std::chrono::steady_clock::now() + kExitGracePeriod;
        while (is_running(node) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (is_running(node)) {
            note(Level::Warn, node.spec->name, node.spec->name + " did not exit; sending SIGKILL");
            kill(node.pid, SIGKILL);
            while (is_running(node)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    note(Level::Info, "", "all nodes stopped");
    write_status();
}

void Manager::print_summary() const {
    std::printf("\n%-20s %-17s %4s %7s %-12s %s\n", "NODE", "CRITICALITY", "PRIO", "PID", "OUTCOME", "DETAIL");
    for (const auto& node : nodes_) {
        std::printf("%-20s %-17s %4d %7d %-12s %s\n", node.spec->name.c_str(),
                    std::string(to_string(node.spec->criticality)).c_str(), node.spec->priority,
                    static_cast<int>(node.pid), std::string(to_string(node.outcome)).c_str(),
                    node.detail.c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

}  // namespace vrm
