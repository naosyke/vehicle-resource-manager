#include "vrm/manifest.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace vrm {

std::string_view to_string(Criticality criticality) {
    switch (criticality) {
        case Criticality::SafetyCritical: return "safety_critical";
        case Criticality::MissionCritical: return "mission_critical";
        case Criticality::BestEffort: return "best_effort";
    }
    return "unknown";
}

std::string_view to_string(RestartPolicy policy) {
    return policy == RestartPolicy::OnFailure ? "on-failure" : "never";
}

RestartPolicy default_restart_policy(Criticality criticality) {
    return criticality == Criticality::BestEffort ? RestartPolicy::Never : RestartPolicy::OnFailure;
}

const NodeSpec* SystemManifest::find(std::string_view node_name) const {
    for (const auto& node : nodes) {
        if (node.name == node_name) {
            return &node;
        }
    }
    return nullptr;
}

namespace {

Criticality parse_criticality(const std::string& text, const std::string& node) {
    for (auto criticality :
         {Criticality::SafetyCritical, Criticality::MissionCritical, Criticality::BestEffort}) {
        if (to_string(criticality) == text) {
            return criticality;
        }
    }
    throw ManifestError("node '" + node + "': unknown criticality '" + text +
                        "' (expected safety_critical, mission_critical or best_effort)");
}

std::vector<std::string> string_list(const YAML::Node& node) {
    std::vector<std::string> values;
    if (node) {
        for (const auto& item : node) {
            values.push_back(item.as<std::string>());
        }
    }
    return values;
}

NodeSpec parse_node(const YAML::Node& yaml) {
    NodeSpec node;
    if (!yaml["name"]) {
        throw ManifestError("every node needs a name");
    }
    node.name = yaml["name"].as<std::string>();
    if (!yaml["executable"]) {
        throw ManifestError("node '" + node.name + "': executable is required");
    }
    node.executable = yaml["executable"].as<std::string>();
    node.args = string_list(yaml["args"]);
    node.depends_on = string_list(yaml["depends_on"]);

    if (yaml["criticality"]) {
        node.criticality = parse_criticality(yaml["criticality"].as<std::string>(), node.name);
    }
    node.restart = default_restart_policy(node.criticality);
    if (yaml["restart"]) {
        const auto policy = yaml["restart"].as<std::string>();
        if (policy == "never") {
            node.restart = RestartPolicy::Never;
        } else if (policy == "on-failure") {
            node.restart = RestartPolicy::OnFailure;
        } else {
            throw ManifestError("node '" + node.name + "': restart must be 'never' or 'on-failure'");
        }
    }
    if (yaml["max_restarts"]) {
        node.max_restarts = yaml["max_restarts"].as<int>();
        if (node.max_restarts < 0) {
            throw ManifestError("node '" + node.name + "': max_restarts must not be negative");
        }
    }
    if (yaml["priority"]) {
        node.priority = yaml["priority"].as<int>();
        if (node.priority < 0 || node.priority > 99) {
            throw ManifestError("node '" + node.name + "': priority must be 0-99");
        }
    }

    if (const auto resources = yaml["resources"]) {
        if (resources["cpu"]) {
            node.resources.cpu_cores = resources["cpu"].as<double>();
            if (*node.resources.cpu_cores <= 0.0) {
                throw ManifestError("node '" + node.name + "': cpu must be positive");
            }
        }
        if (resources["memory"]) {
            node.resources.memory_bytes = parse_memory_size(resources["memory"].as<std::string>());
        }
        if (resources["cpus"]) {
            for (const auto& cpu : resources["cpus"]) {
                node.resources.cpus.push_back(cpu.as<int>());
            }
        }
    }
    return node;
}

void validate(const SystemManifest& manifest) {
    std::set<std::string> names;
    for (const auto& node : manifest.nodes) {
        if (!names.insert(node.name).second) {
            throw ManifestError("duplicate node name '" + node.name + "'");
        }
    }
    for (const auto& node : manifest.nodes) {
        for (const auto& dependency : node.depends_on) {
            if (!names.count(dependency)) {
                throw ManifestError("node '" + node.name + "' depends on unknown node '" +
                                    dependency + "'");
            }
        }
    }
}

}  // namespace

std::uint64_t parse_memory_size(std::string_view text) {
    std::size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) {
        ++digits;
    }
    if (digits == 0) {
        throw ManifestError("invalid memory size '" + std::string(text) + "'");
    }
    const std::uint64_t value = std::stoull(std::string(text.substr(0, digits)));
    const std::string unit(text.substr(digits));

    static const std::map<std::string, std::uint64_t> units = {
        {"", 1},
        {"K", 1000ULL},
        {"M", 1000ULL * 1000},
        {"G", 1000ULL * 1000 * 1000},
        {"Ki", 1024ULL},
        {"Mi", 1024ULL * 1024},
        {"Gi", 1024ULL * 1024 * 1024},
    };
    const auto it = units.find(unit);
    if (it == units.end()) {
        throw ManifestError("invalid memory unit '" + unit + "' in '" + std::string(text) + "'");
    }
    return value * it->second;
}

SystemManifest parse_manifest(const std::string& yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& error) {
        throw ManifestError(std::string("invalid YAML: ") + error.what());
    }

    SystemManifest manifest;
    try {
        manifest.name = root["system"] ? root["system"].as<std::string>() : "system";
        if (root["transition_timeout_ms"]) {
            manifest.transition_timeout =
                std::chrono::milliseconds(root["transition_timeout_ms"].as<int>());
        }
        if (const auto arbitration = root["arbitration"]) {
            auto& config = manifest.arbitration;
            const auto read = [&](const char* key, double& value, double min, double max) {
                if (!arbitration[key]) return;
                value = arbitration[key].as<double>();
                if (value < min || value > max) {
                    throw ManifestError(std::string("arbitration.") + key + " must be between " +
                                        std::to_string(min) + " and " + std::to_string(max));
                }
            };
            if (arbitration["enabled"]) config.enabled = arbitration["enabled"].as<bool>();
            read("cpu_pressure_threshold", config.cpu_pressure_threshold, 0.01, 1.0);
            read("escalation_interval_s", config.escalation_interval, 0.1, 3600);
            read("recovery_s", config.recovery_seconds, 0.1, 3600);
            read("throttle_cpu", config.throttle_cpu_cores, 0.01, 1024);
            read("memory_stop_fraction", config.memory_stop_fraction, 0.1, 1.0);
            read("rt_overrun_s", config.rt_overrun_seconds, 0.1, 3600);
            if (arbitration["miss_rounds"]) {
                config.miss_rounds = arbitration["miss_rounds"].as<int>();
                if (config.miss_rounds < 1) throw ManifestError("arbitration.miss_rounds must be at least 1");
            }
        }
        if (root["heartbeat_timeout_ms"]) {
            manifest.heartbeat_timeout = std::chrono::milliseconds(root["heartbeat_timeout_ms"].as<int>());
        }
        if (!root["nodes"] || !root["nodes"].IsSequence()) {
            throw ManifestError("'nodes' must be a list");
        }
        for (const auto& node : root["nodes"]) {
            manifest.nodes.push_back(parse_node(node));
        }
    } catch (const YAML::Exception& error) {
        throw ManifestError(std::string("invalid manifest: ") + error.what());
    }

    validate(manifest);
    return manifest;
}

SystemManifest load_manifest(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
        throw ManifestError("cannot read manifest '" + path + "': " + error.what());
    }
    return parse_manifest(YAML::Dump(root));
}

std::vector<const NodeSpec*> startup_order(const SystemManifest& manifest) {
    // Kahn's algorithm; among ready nodes pick the most important first.
    const auto rank = [](const NodeSpec* node) {
        return std::make_tuple(static_cast<int>(node->criticality), -node->priority, node->name);
    };

    std::map<std::string, int> pending_dependencies;
    for (const auto& node : manifest.nodes) {
        pending_dependencies[node.name] = static_cast<int>(node.depends_on.size());
    }

    std::vector<const NodeSpec*> order;
    std::set<std::string> started;
    while (order.size() < manifest.nodes.size()) {
        const NodeSpec* next = nullptr;
        for (const auto& node : manifest.nodes) {
            if (started.count(node.name) || pending_dependencies[node.name] > 0) {
                continue;
            }
            if (!next || rank(&node) < rank(next)) {
                next = &node;
            }
        }
        if (!next) {
            throw ManifestError("dependency cycle between nodes");
        }
        order.push_back(next);
        started.insert(next->name);
        for (const auto& node : manifest.nodes) {
            if (std::count(node.depends_on.begin(), node.depends_on.end(), next->name)) {
                --pending_dependencies[node.name];
            }
        }
    }
    return order;
}

}  // namespace vrm
