#include "vrm/arbiter.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

namespace vrm {

std::string_view to_string(ArbiterActionType type) {
    switch (type) {
        case ArbiterActionType::LowerWeight: return "lower_weight";
        case ArbiterActionType::RestoreWeight: return "restore_weight";
        case ArbiterActionType::Throttle: return "throttle";
        case ArbiterActionType::Deactivate: return "deactivate";
        case ArbiterActionType::Stop: return "stop";
        case ArbiterActionType::Reactivate: return "reactivate";
        case ArbiterActionType::Unthrottle: return "unthrottle";
        case ArbiterActionType::StopBeforeOom: return "stop_before_oom";
        case ArbiterActionType::DemoteRealtime: return "demote_realtime";
    }
    return "unknown";
}

namespace {

std::string percent(double fraction) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", fraction * 100.0);
    return text;
}

bool is_protected(const ArbiterNode& node) { return node.criticality != Criticality::BestEffort; }

}  // namespace

int Arbiter::level(const std::string& node) const {
    const auto it = levels_.find(node);
    return it == levels_.end() ? 0 : it->second;
}

void Arbiter::forget(const std::string& node) {
    levels_.erase(node);
    rt_overrun_since_.erase(node);
    memory_stopped_.erase(node);
    miss_streak_.erase(node);
}

bool Arbiter::may_share_cpu(const ArbiterNode& a, const ArbiterNode& b) {
    if (a.cpus.empty() || b.cpus.empty()) return true;
    for (const int cpu : a.cpus) {
        if (std::find(b.cpus.begin(), b.cpus.end(), cpu) != b.cpus.end()) return true;
    }
    return false;
}

std::vector<ArbiterAction> Arbiter::decide(const std::vector<ArbiterNode>& nodes, double now) {
    std::vector<ArbiterAction> actions;
    if (!config_.enabled) return actions;

    // 1. Memory: stop non-safety nodes gracefully before the OOM killer does.
    for (const auto& node : nodes) {
        if (!node.active || !node.memory_fraction || memory_stopped_[node.name]) continue;
        if (*node.memory_fraction < config_.memory_stop_fraction) continue;
        if (node.criticality == Criticality::SafetyCritical) continue;  // Never stopped by the arbiter.
        memory_stopped_[node.name] = true;
        levels_[node.name] = 4;
        actions.push_back({ArbiterActionType::StopBeforeOom, node.name,
                           "memory at " + percent(*node.memory_fraction) + " of its limit"});
    }

    // 2. Real-time overrun: cpu.max does not apply to SCHED_FIFO, so demote.
    for (const auto& node : nodes) {
        const bool overrun = node.active && node.realtime && node.cpu_budget_percent &&
                             node.cpu_percent > *node.cpu_budget_percent + 5.0;
        if (!overrun) {
            rt_overrun_since_.erase(node.name);
            continue;
        }
        const auto since = rt_overrun_since_.emplace(node.name, now).first->second;
        if (now - since >= config_.rt_overrun_seconds) {
            rt_overrun_since_.erase(node.name);
            char reason[96];
            std::snprintf(reason, sizeof(reason), "SCHED_FIFO node uses %.0f%% CPU, budget %.0f%%",
                          node.cpu_percent, *node.cpu_budget_percent);
            actions.push_back({ArbiterActionType::DemoteRealtime, node.name, reason});
        }
    }

    // 3. CPU interference with protected nodes.
    for (const auto& node : nodes) {
        miss_streak_[node.name] = node.active && node.missed_deadlines ? miss_streak_[node.name] + 1 : 0;
    }
    std::vector<const ArbiterNode*> missing;   // Persistent deadline misses: full ladder.
    std::vector<const ArbiterNode*> waiting;   // CPU pressure only: throttling only.
    for (const auto& node : nodes) {
        if (!node.active || !is_protected(node)) continue;
        if (miss_streak_[node.name] >= config_.miss_rounds) {
            missing.push_back(&node);
        } else if (!node.realtime && node.cpu_pressure >= config_.cpu_pressure_threshold) {
            // For a SCHED_FIFO node, waiting for CPU can only mean waiting for
            // its own threads or other real-time tasks, which throttling
            // normal tasks cannot help; only its deadline misses count.
            waiting.push_back(&node);
        }
    }

    if (!missing.empty() || !waiting.empty()) {
        last_pressure_ = now;
        if (now - last_escalation_ < config_.escalation_interval) return actions;

        // Victims: active, strictly less critical than a suffering node that
        // they may share a CPU with. Least critical first, then the least
        // degraded (throttle everyone before deactivating anyone), then the
        // lowest priority, then the biggest CPU user.
        const ArbiterNode* victim = nullptr;
        const ArbiterNode* protected_node = nullptr;
        const auto rank = [this](const ArbiterNode* node) {
            return std::make_tuple(-static_cast<int>(node->criticality), level(node->name), node->priority,
                                   -node->cpu_percent);
        };
        const auto consider = [&](const std::vector<const ArbiterNode*>& sufferers, int max_level) {
            for (const auto& candidate : nodes) {
                if (!candidate.active || level(candidate.name) > max_level) continue;
                for (const auto* sufferer : sufferers) {
                    if (candidate.criticality <= sufferer->criticality) continue;
                    if (!may_share_cpu(candidate, *sufferer)) continue;
                    // A SCHED_FIFO node preempts every normal task, so normal
                    // tasks cannot be the cause of its misses (e.g. a VM stall
                    // is); only other real-time nodes can interfere with it.
                    if (sufferer->realtime && !candidate.realtime) continue;
                    if (!victim || rank(&candidate) < rank(victim)) {
                        victim = &candidate;
                        protected_node = sufferer;
                    }
                }
            }
        };
        consider(missing, 3);  // Up to "stop".
        consider(waiting, 0);  // Lower the weight only.
        if (!victim) return actions;

        // Interference right after a restore: wait longer before the next one.
        if (last_recovery_ > last_escalation_ && now - last_recovery_ < 2 * recovery_seconds_) {
            recovery_seconds_ = std::min(recovery_seconds_ * 2, config_.recovery_seconds * 8);
        }

        const int next = level(victim->name) + 1;
        levels_[victim->name] = next;
        last_escalation_ = now;
        const bool misses = miss_streak_[protected_node->name] >= config_.miss_rounds;
        const std::string reason = protected_node->name + (misses ? " keeps missing deadlines"
                                                                  : " waits for CPU " +
                                                                        percent(protected_node->cpu_pressure) +
                                                                        " of the time");
        const auto type = next == 1   ? ArbiterActionType::LowerWeight
                          : next == 2 ? ArbiterActionType::Throttle
                          : next == 3 ? ArbiterActionType::Deactivate
                                      : ArbiterActionType::Stop;
        actions.push_back({type, victim->name, reason});
        return actions;
    }

    // 4. Recovery: calm for a while -> restore the most critical degraded node one step.
    const double calm_since = std::max({last_pressure_, last_escalation_, last_recovery_});
    if (now - calm_since < recovery_seconds_) return actions;

    // Most critical first; among equals the most degraded one, so a
    // deactivated function comes back before a throttled one runs faster.
    const ArbiterNode* restore = nullptr;
    const auto order = [this](const ArbiterNode* node) {
        return std::make_tuple(node->criticality, -level(node->name), -node->priority);
    };
    for (const auto& node : nodes) {
        const int current = level(node.name);
        if (current < 1 || current > 3 || memory_stopped_[node.name]) continue;
        if (!restore || order(&node) < order(restore)) restore = &node;
    }
    if (restore) {
        const int current = level(restore->name);
        levels_[restore->name] = current - 1;
        last_recovery_ = now;
        const auto type = current == 3   ? ArbiterActionType::Reactivate
                          : current == 2 ? ArbiterActionType::Unthrottle
                                         : ArbiterActionType::RestoreWeight;
        actions.push_back({type,
                           restore->name, "no interference for " + std::to_string(static_cast<int>(recovery_seconds_)) + " s"});
    }
    return actions;
}

}  // namespace vrm
