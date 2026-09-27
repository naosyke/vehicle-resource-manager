// Resource arbitration policy: decides how to protect critical nodes when
// resources get tight. Pure logic - the manager feeds it measurements and
// carries out the actions it returns.
//
// CPU interference: a less critical node that can run on the same CPUs as a
// protected (safety- or mission-critical) node is degraded one step at a time:
//
//   level 0 normal
//   -> 1 yielding    (cpu.weight lowered: still uses idle CPU, but gives way)
//   -> 2 throttled   (cpu.max: capped even when the CPU is idle)
//   -> 3 deactivated (lifecycle inactive, process alive)
//   -> 4 stopped
//
// - A protected node waiting for CPU (PSI) above the threshold allows only
//   lowering the weight, which costs nothing when the CPU is idle.
// - A protected node missing deadlines in several consecutive rounds (actual
//   harm) allows the whole ladder. A single spike is ignored.
// - A real-time (SCHED_FIFO) node can only be disturbed by other real-time
//   nodes, so its misses never degrade normal nodes.
//
// After a calm period the most critical degraded node is restored one step
// (yielding / throttled / deactivated nodes; stopped nodes stay stopped). If the
// interference comes back right after a restore, the calm period doubles.
//
// Memory: a non-safety node close to its memory limit is stopped gracefully
// before the kernel's OOM killer has to kill it.
//
// Real-time overrun: a SCHED_FIFO node above its CPU budget is demoted to
// normal scheduling, where cgroup cpu.max applies again.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vrm/manifest.hpp"

namespace vrm {

// What the arbiter needs to know about one node at a point in time.
struct ArbiterNode {
    std::string name;
    Criticality criticality = Criticality::BestEffort;
    int priority = 0;
    std::vector<int> cpus;  // Empty: may run on any CPU.
    bool active = false;    // Running and in the Active (or arbiter-deactivated) state.
    double cpu_percent = 0.0;
    std::optional<double> cpu_budget_percent;
    std::optional<double> memory_fraction;  // memory.current / memory.max
    double cpu_pressure = 0.0;              // Share of time waiting for CPU since the last sample.
    bool missed_deadlines = false;          // Deadline misses since the last sample.
    bool realtime = false;                  // Currently SCHED_FIFO / SCHED_RR.
};

enum class ArbiterActionType {
    LowerWeight,       // Set cpu.weight to lowered_cpu_weight.
    RestoreWeight,     // Restore the node's own cpu.weight.
    Throttle,          // Set cpu.max to throttle_cpu_cores.
    Deactivate,        // Lifecycle deactivate; the process stays alive.
    Stop,              // Lifecycle shutdown.
    Reactivate,        // Undo Deactivate.
    Unthrottle,        // Restore the node's own cpu.max.
    StopBeforeOom,     // Graceful shutdown before the memory limit is hit.
    DemoteRealtime,    // SCHED_FIFO -> SCHED_OTHER.
};

std::string_view to_string(ArbiterActionType type);

struct ArbiterAction {
    ArbiterActionType type;
    std::string node;
    std::string reason;
};

class Arbiter {
public:
    explicit Arbiter(ArbitrationConfig config = {}) : config_(config), recovery_seconds_(config.recovery_seconds) {}

    // Current calm period before a restore step, in seconds.
    double recovery_seconds() const { return recovery_seconds_; }

    const ArbitrationConfig& config() const { return config_; }

    // `now` is in seconds on any monotonic clock.
    std::vector<ArbiterAction> decide(const std::vector<ArbiterNode>& nodes, double now);

    // Degradation level of a node (0 = normal .. 4 = stopped).
    int level(const std::string& node) const;

    // The node was restarted or exited: forget its level and history.
    void forget(const std::string& node);

private:
    static bool may_share_cpu(const ArbiterNode& a, const ArbiterNode& b);

    ArbitrationConfig config_;
    std::map<std::string, int> levels_;
    std::map<std::string, double> rt_overrun_since_;
    std::map<std::string, bool> memory_stopped_;
    std::map<std::string, int> miss_streak_;
    double recovery_seconds_ = 0;  // Current calm period (grows on relapse).
    double last_escalation_ = -1e9;
    double last_pressure_ = -1e9;
    double last_recovery_ = -1e9;
};

}  // namespace vrm
