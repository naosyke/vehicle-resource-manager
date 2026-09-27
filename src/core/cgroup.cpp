#include "vrm/cgroup.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace vrm {

// --- Pure helpers -----------------------------------------------------------

std::string format_cpu_max(double cores, std::uint32_t period_us) {
    const auto quota = static_cast<std::uint64_t>(std::llround(cores * period_us));
    // The kernel rejects quotas below 1 ms.
    return std::to_string(std::max<std::uint64_t>(quota, 1000)) + " " + std::to_string(period_us);
}

std::string format_cpu_list(std::vector<int> cpus) {
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());

    std::string text;
    for (std::size_t i = 0; i < cpus.size();) {
        std::size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) {
            ++j;
        }
        if (!text.empty()) text += ",";
        text += std::to_string(cpus[i]);
        if (j > i) text += "-" + std::to_string(cpus[j]);
        i = j + 1;
    }
    return text;
}

std::vector<int> parse_cpu_list(std::string_view text) {
    std::vector<int> cpus;
    std::stringstream stream{std::string(text)};
    std::string part;
    while (std::getline(stream, part, ',')) {
        part.erase(std::remove_if(part.begin(), part.end(), ::isspace), part.end());
        if (part.empty()) continue;
        const auto dash = part.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(part));
        } else {
            for (int cpu = std::stoi(part.substr(0, dash)); cpu <= std::stoi(part.substr(dash + 1)); ++cpu) {
                cpus.push_back(cpu);
            }
        }
    }
    return cpus;
}

std::map<std::string, std::uint64_t> parse_flat_keyed(std::string_view text) {
    std::map<std::string, std::uint64_t> values;
    std::stringstream stream{std::string(text)};
    std::string key;
    std::uint64_t value = 0;
    while (stream >> key >> value) {
        values[key] = value;
    }
    return values;
}

std::uint64_t parse_pressure_total(std::string_view text) {
    const auto line_end = text.find('\n');
    const auto some = text.substr(0, line_end);
    if (some.rfind("some", 0) != 0) return 0;
    const auto total = some.find("total=");
    if (total == std::string_view::npos) return 0;
    return std::stoull(std::string(some.substr(total + 6)));
}

std::string format_bytes(std::uint64_t bytes) {
    const char* units[] = {"B", "Ki", "Mi", "Gi"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof(text), unit == 0 ? "%.0f%s" : "%.1f%s", value, units[unit]);
    return text;
}

// --- Filesystem access ------------------------------------------------------

namespace {

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw CgroupError("cannot read " + path + ": " + std::strerror(errno));
    }
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

std::string read_optional(const std::string& path) {
    std::ifstream file(path);
    std::stringstream content;
    if (file) content << file.rdbuf();
    return content.str();
}

// Returns errno (0 on success). cgroup files need a single write() call.
int write_file(const std::string& path, const std::string& value) {
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) return errno;
    const bool written = std::fwrite(value.data(), 1, value.size(), file) == value.size();
    const int write_errno = written ? 0 : errno;
    const bool closed = std::fclose(file) == 0;  // cgroupfs reports errors on flush.
    if (!written) return write_errno;
    return closed ? 0 : errno;
}

void write_or_throw(const std::string& path, const std::string& value) {
    if (const int error = write_file(path, value)) {
        throw CgroupError("cannot write '" + value + "' to " + path + ": " + std::strerror(error));
    }
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

// The manager's own cgroup path from /proc/self/cgroup ("0::/path").
std::string own_cgroup() {
    std::stringstream lines(read_file("/proc/self/cgroup"));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("0::", 0) == 0) return line.substr(3);
    }
    throw CgroupError("not a cgroup v2 system (no '0::' entry in /proc/self/cgroup)");
}

const std::vector<std::string> kControllers = {"cpu", "memory", "cpuset"};

std::string enable_command() {
    std::string command;
    for (const auto& controller : kControllers) command += (command.empty() ? "+" : " +") + controller;
    return command;
}

// Moves every process of `group` into `group`/init so that `group` can
// delegate controllers to its children.
void evacuate(const std::string& group) {
    const std::string init = group + "/init";
    if (mkdir(init.c_str(), 0755) != 0 && errno != EEXIST) {
        throw CgroupError("cannot create " + init + ": " + std::strerror(errno));
    }
    std::stringstream procs(read_file(group + "/cgroup.procs"));
    std::string pid;
    while (procs >> pid) {
        write_file(init + "/cgroup.procs", pid);  // Exited processes fail harmlessly.
    }
}

}  // namespace

// --- CgroupManager ------------------------------------------------------------

std::unique_ptr<CgroupManager> CgroupManager::create(const std::string& name, std::string& reason,
                                                     const std::string& mount) {
    try {
        std::string own = own_cgroup();
        // After an earlier evacuation, processes started from the container
        // (including a later manager run) live in /init; manage the root.
        if (own == "/init") own = "/";
        const std::string parent = mount + (own == "/" ? "" : own);

        if (access((parent + "/cgroup.subtree_control").c_str(), W_OK) != 0) {
            reason = "cgroup filesystem is not writable (run the container with --privileged --cgroupns=private)";
            return nullptr;
        }
        const std::string available = read_file(parent + "/cgroup.controllers");
        for (const auto& controller : kControllers) {
            if (available.find(controller) == std::string::npos) {
                reason = "cgroup controller '" + controller + "' is not available";
                return nullptr;
            }
        }

        int error = write_file(parent + "/cgroup.subtree_control", enable_command());
        if (error == EBUSY) {
            // Only move processes out of the namespace root (a container),
            // never out of a shared cgroup on the host.
            if (own != "/") {
                reason = "cgroup " + own + " has other processes; run the manager in its own cgroup namespace";
                return nullptr;
            }
            evacuate(parent);
            error = write_file(parent + "/cgroup.subtree_control", enable_command());
        }
        if (error) {
            reason = "cannot enable controllers in " + parent + ": " + std::strerror(error);
            return nullptr;
        }

        const std::string base = parent + "/" + name;
        if (mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) {
            reason = "cannot create " + base + ": " + std::strerror(errno);
            return nullptr;
        }
        write_or_throw(base + "/cgroup.subtree_control", enable_command());

        std::unique_ptr<CgroupManager> manager(new CgroupManager(base));
        manager->available_cpus_ = parse_cpu_list(read_file(base + "/cpuset.cpus.effective"));
        return manager;
    } catch (const CgroupError& error) {
        reason = error.what();
        return nullptr;
    }
}

CgroupManager::CgroupManager(std::string base) : base_(std::move(base)) {}

CgroupManager::~CgroupManager() {
    for (const auto& node : std::vector<std::string>(groups_)) {
        remove_group(node);
    }
    rmdir(base_.c_str());
}

std::string CgroupManager::create_group(const std::string& node, const ResourceBudget& budget) {
    const std::string path = group_path(node);
    rmdir(path.c_str());  // Leftover from a previous run, if empty.
    if (mkdir(path.c_str(), 0755) != 0) {
        throw CgroupError("cannot create " + path + ": " + std::strerror(errno));
    }
    groups_.push_back(node);

    if (budget.cpu_cores) {
        write_or_throw(path + "/cpu.max", format_cpu_max(*budget.cpu_cores));
    }
    if (budget.memory_bytes) {
        write_or_throw(path + "/memory.max", std::to_string(*budget.memory_bytes));
        // Without this the kernel could swap instead of enforcing the limit.
        write_file(path + "/memory.swap.max", "0");
    }
    if (!budget.cpus.empty()) {
        const std::set<int> available(available_cpus_.begin(), available_cpus_.end());
        for (const int cpu : budget.cpus) {
            if (!available.count(cpu)) {
                throw CgroupError("CPU " + std::to_string(cpu) + " is not available (available: " +
                                  format_cpu_list(available_cpus_) + ")");
            }
        }
        write_or_throw(path + "/cpuset.cpus", format_cpu_list(budget.cpus));
    }
    return path + "/cgroup.procs";
}

CgroupUsage CgroupManager::usage(const std::string& node) const {
    const std::string path = group_path(node);
    const auto cpu = parse_flat_keyed(read_optional(path + "/cpu.stat"));
    const auto events = parse_flat_keyed(read_optional(path + "/memory.events"));
    const auto number = [](const std::string& text) -> std::uint64_t {
        return text.empty() ? 0 : std::stoull(trim(text));
    };
    const auto value = [](const std::map<std::string, std::uint64_t>& values, const char* key) {
        const auto it = values.find(key);
        return it == values.end() ? 0 : it->second;
    };

    CgroupUsage usage;
    usage.cpu_usage_usec = value(cpu, "usage_usec");
    usage.nr_throttled = value(cpu, "nr_throttled");
    usage.throttled_usec = value(cpu, "throttled_usec");
    usage.memory_current = number(read_optional(path + "/memory.current"));
    usage.memory_peak = number(read_optional(path + "/memory.peak"));
    usage.oom_kills = value(events, "oom_kill");
    usage.cpu_pressure_usec = parse_pressure_total(read_optional(path + "/cpu.pressure"));
    usage.memory_pressure_usec = parse_pressure_total(read_optional(path + "/memory.pressure"));
    return usage;
}

void CgroupManager::set_cpu_max(const std::string& node, std::optional<double> cores) {
    write_or_throw(group_path(node) + "/cpu.max", cores ? format_cpu_max(*cores) : "max 100000");
}

void CgroupManager::remove_group(const std::string& node) {
    const std::string path = group_path(node);
    // The kernel may need a moment after the last process exits.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (rmdir(path.c_str()) == 0 || errno == ENOENT) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    groups_.erase(std::remove(groups_.begin(), groups_.end(), node), groups_.end());
}

}  // namespace vrm
