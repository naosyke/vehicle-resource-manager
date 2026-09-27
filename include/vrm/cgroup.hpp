// Per-node resource isolation with Linux cgroup v2.
//
// Layout, under the manager's own cgroup (the container's cgroup root):
//
//   <root>/                 cgroup.subtree_control: +cpu +memory +cpuset
//   ├── init/               processes that were in <root> (cgroup v2 forbids
//   │                       processes in a group that delegates controllers)
//   └── vrm/                cgroup.subtree_control: +cpu +memory +cpuset
//       ├── brake_control/  cpu.max, memory.max, cpuset.cpus
//       └── infotainment/
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "vrm/manifest.hpp"

namespace vrm {

class CgroupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// --- Pure helpers (no cgroup filesystem needed) ---------------------------

// cpu.max value for a budget in cores, e.g. 0.5 -> "50000 100000".
std::string format_cpu_max(double cores, std::uint32_t period_us = 100000);

// CPU list in cgroup format, e.g. {0, 1, 2, 5} -> "0-2,5".
std::string format_cpu_list(std::vector<int> cpus);
std::vector<int> parse_cpu_list(std::string_view text);

// Parses "key value" lines (cpu.stat, memory.events).
std::map<std::string, std::uint64_t> parse_flat_keyed(std::string_view text);

// "some avg10=0.00 avg60=0.00 avg300=0.00 total=1234" -> 1234 (the "some" line).
std::uint64_t parse_pressure_total(std::string_view text);

// 67108864 -> "64.0Mi"
std::string format_bytes(std::uint64_t bytes);

// --- cgroup v2 management -------------------------------------------------

struct CgroupUsage {
    std::uint64_t cpu_usage_usec = 0;
    std::uint64_t nr_throttled = 0;    // Periods in which cpu.max throttled the group.
    std::uint64_t throttled_usec = 0;
    std::uint64_t memory_current = 0;
    std::uint64_t memory_peak = 0;
    std::uint64_t oom_kills = 0;       // Processes killed for exceeding memory.max.
    // Pressure stall information: total time (us) in which some task of the
    // group was waiting for CPU / memory. Differences give the pressure.
    std::uint64_t cpu_pressure_usec = 0;
    std::uint64_t memory_pressure_usec = 0;
};

class CgroupManager {
public:
    // Sets up <manager's cgroup>/<name> with the cpu, memory and cpuset
    // controllers. Returns nullptr and sets `reason` when cgroups cannot be
    // managed here (e.g. read-only cgroupfs in an unprivileged container).
    static std::unique_ptr<CgroupManager> create(const std::string& name, std::string& reason,
                                                 const std::string& mount = "/sys/fs/cgroup");
    ~CgroupManager();

    CgroupManager(const CgroupManager&) = delete;
    CgroupManager& operator=(const CgroupManager&) = delete;

    // Creates the group for a node and applies its budget. Returns the path
    // of the group's cgroup.procs file, to move the node's process into.
    std::string create_group(const std::string& node, const ResourceBudget& budget);

    CgroupUsage usage(const std::string& node) const;

    // Changes a node's CPU limit at runtime (nullopt = unlimited).
    void set_cpu_max(const std::string& node, std::optional<double> cores);

    // Removes the (empty) group of a node that has exited.
    void remove_group(const std::string& node);

    const std::string& base_path() const { return base_; }
    const std::vector<int>& available_cpus() const { return available_cpus_; }

private:
    explicit CgroupManager(std::string base);

    std::string group_path(const std::string& node) const { return base_ + "/" + node; }

    std::string base_;
    std::vector<int> available_cpus_;
    std::vector<std::string> groups_;
};

}  // namespace vrm
