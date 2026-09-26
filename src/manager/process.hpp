// Child process management for node executables.
#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

namespace vrm {

// Resolves a bare executable name ("vrm_demo_node") against the manager's
// own directory first, so nodes built next to the manager are found.
std::string resolve_executable(const std::string& executable);

// Starts `executable args...` in its own process group. When `cgroup_procs`
// is set, the child moves itself into that cgroup before exec, so the
// resource limits apply from its first instruction. Throws on failure.
pid_t spawn_process(const std::string& executable, const std::vector<std::string>& args,
                    const std::string& cgroup_procs = "");

struct ExitInfo {
    pid_t pid;
    int status;  // Raw waitpid status.
};

// Reaps one exited child without blocking, if any.
std::optional<ExitInfo> reap_child();

// "exited with code 1", "killed by signal 9 (Killed)", ...
std::string describe_exit(int status);

}  // namespace vrm
