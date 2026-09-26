// System manifest: which nodes run, how critical they are, and the resources
// they are allowed to use. Loaded from YAML (see config/system.yaml).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vrm {

// Ordered from most to least critical. Resource arbitration protects nodes
// higher in this list from interference by nodes lower in it.
enum class Criticality {
    SafetyCritical,   // e.g. braking; must never be starved.
    MissionCritical,  // e.g. perception; needed for the main function.
    BestEffort,       // e.g. infotainment; may be throttled or stopped.
};

std::string_view to_string(Criticality criticality);

struct ResourceBudget {
    std::optional<double> cpu_cores;        // e.g. 0.5 = half a core (cgroup cpu.max).
    std::optional<std::uint64_t> memory_bytes;  // Hard limit (cgroup memory.max).
    std::vector<int> cpus;                  // CPU affinity (cgroup cpuset.cpus).
};

struct NodeSpec {
    std::string name;
    std::string executable;
    std::vector<std::string> args;
    Criticality criticality = Criticality::BestEffort;
    int priority = 0;  // 1-99 = real-time (SCHED_FIFO) priority, 0 = normal scheduling.
    ResourceBudget resources;
    std::vector<std::string> depends_on;
};

struct SystemManifest {
    std::string name;
    std::chrono::milliseconds transition_timeout{3000};
    std::vector<NodeSpec> nodes;

    const NodeSpec* find(std::string_view node_name) const;
};

class ManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

SystemManifest load_manifest(const std::string& path);
SystemManifest parse_manifest(const std::string& yaml_text);

// Parses sizes like "256Mi", "1Gi", "512M" or a plain byte count.
std::uint64_t parse_memory_size(std::string_view text);

// Start order: dependencies first; otherwise more critical nodes first, then
// higher priority, then by name. Throws ManifestError on cycles.
std::vector<const NodeSpec*> startup_order(const SystemManifest& manifest);

}  // namespace vrm
