#include "manager.hpp"

#include <signal.h>

#include <cstdio>
#include <thread>

#include "process.hpp"
#include "vrm/log.hpp"
#include "vrm/managed_node.hpp"

namespace vrm {

namespace {
constexpr auto kExitGracePeriod = std::chrono::milliseconds(2000);
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
                if (node.spec->criticality == Criticality::SafetyCritical) {
                    log::error("manager", message);
                } else {
                    log::warn("manager", message);
                }
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
    log::error("manager", node.spec->name + " failed to start: " + reason);
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
            log::warn("manager", spec.name + " skipped: " + node.detail);
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
        log::error("manager", spec.name + " could not be spawned: " + error.what());
        return false;
    }
    node.running = true;
    log::info("manager", "spawned " + spec.name + " (pid " + std::to_string(node.pid) + ", " +
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
    log::info("manager", "starting system '" + manifest_.name + "' with " +
                             std::to_string(nodes_.size()) + " nodes");
    for (auto& node : nodes_) {
        if (g_stop_requested) {
            return true;
        }
        if (!start_node(node) && node.spec->criticality == Criticality::SafetyCritical) {
            log::error("manager", "safety-critical node " + node.spec->name +
                                      " is not available; aborting startup");
            return false;
        }
    }
    log::info("manager", "startup complete");
    print_summary();
    return true;
}

void Manager::supervise(std::chrono::milliseconds duration, std::chrono::milliseconds report_interval) {
    const auto start = std::chrono::steady_clock::now();
    auto next_report = start + report_interval;
    for (auto& node : nodes_) {
        if (node.in_cgroup) sample_resources(node);  // Baseline for CPU usage.
    }

    while (!g_stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (duration.count() > 0 && now - start >= duration) {
            break;
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

void Manager::print_resources() {
    if (!cgroups_) return;
    std::printf("\n%-20s %7s %6s %9s %9s %9s %9s %4s\n", "RESOURCES", "CPU", "LIMIT", "MEMORY", "PEAK",
                "LIMIT", "THROTTLED", "OOM");
    for (auto& node : nodes_) {
        if (!node.in_cgroup) continue;
        if (node.running) sample_resources(node);
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
    log::info("manager", "shutting down");

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
            log::warn("manager", node.spec->name + " did not exit; sending SIGKILL");
            kill(node.pid, SIGKILL);
            while (is_running(node)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    log::info("manager", "all nodes stopped");
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
