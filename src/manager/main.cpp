// vrm_manager: starts and supervises the nodes described in a system manifest.
//
//   vrm_manager config/system.yaml                   run until Ctrl-C
//   vrm_manager config/system.yaml --exit-after 5      run for 5 seconds (demos, CI)
//
// Options:
//   --exit-after SECONDS      stop after this many seconds
//   --report-interval SECONDS resource report interval (default 5, 0 = off)
//   --no-cgroups              do not enforce resource budgets
//   --require-cgroups         exit with code 3 if budgets cannot be enforced
//   --no-rt                   ignore priorities (all nodes SCHED_OTHER), for comparisons
//   --no-arbitration          never throttle, deactivate or stop nodes to protect others
//   --equal-weights           cpu.weight 100 for every node instead of by criticality
//   --status-file PATH        write a JSON status snapshot for the dashboard
//   --status-interval SECONDS status snapshot interval (default 1)
//   --trace-cpus LIST         add a timeline of which node runs on these CPUs (e.g. 0,1)
//                             to the status file (ftrace; needs --privileged)
#include <chrono>
#include <iostream>
#include <string>

#include "manager.hpp"
#include "vrm/cgroup.hpp"
#include "vrm/log.hpp"
#include "vrm/managed_node.hpp"

int main(int argc, char** argv) {
    std::string manifest_path;
    double exit_after_seconds = 0.0;
    double report_interval_seconds = 5.0;
    bool use_cgroups = true;
    bool require_cgroups = false;
    bool realtime = true;
    bool arbitration = true;
    bool criticality_weights = true;
    std::string status_file;
    double status_interval_seconds = 1.0;
    std::string trace_cpus;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--exit-after" && i + 1 < argc) {
            exit_after_seconds = std::stod(argv[++i]);
        } else if (arg == "--report-interval" && i + 1 < argc) {
            report_interval_seconds = std::stod(argv[++i]);
        } else if (arg == "--no-cgroups") {
            use_cgroups = false;
        } else if (arg == "--require-cgroups") {
            require_cgroups = true;
        } else if (arg == "--no-rt") {
            realtime = false;
        } else if (arg == "--no-arbitration") {
            arbitration = false;
        } else if (arg == "--equal-weights") {
            criticality_weights = false;
        } else if (arg == "--status-file" && i + 1 < argc) {
            status_file = argv[++i];
        } else if (arg == "--trace-cpus" && i + 1 < argc) {
            trace_cpus = argv[++i];
        } else if (arg == "--status-interval" && i + 1 < argc) {
            status_interval_seconds = std::stod(argv[++i]);
        } else if (manifest_path.empty() && arg.rfind("--", 0) != 0) {
            manifest_path = arg;
        } else {
            std::cerr << "usage: vrm_manager MANIFEST.yaml [--exit-after S] [--report-interval S] [--no-cgroups] [--require-cgroups] [--no-rt] [--no-arbitration] [--equal-weights] [--status-file PATH] [--status-interval S] [--trace-cpus LIST]\n";
            return 2;
        }
    }
    if (manifest_path.empty()) {
        std::cerr << "usage: vrm_manager MANIFEST.yaml [--exit-after S] [--report-interval S] [--no-cgroups] [--require-cgroups] [--no-rt] [--no-arbitration] [--equal-weights] [--status-file PATH] [--status-interval S] [--trace-cpus LIST]\n";
        return 2;
    }

    vrm::SystemManifest manifest;
    try {
        manifest = vrm::load_manifest(manifest_path);
    } catch (const vrm::ManifestError& error) {
        vrm::log::error("manager", error.what());
        return 2;
    }

    std::unique_ptr<vrm::CgroupManager> cgroups;
    if (use_cgroups) {
        std::string reason;
        cgroups = vrm::CgroupManager::create("vrm", reason);
        if (cgroups) {
            vrm::log::info("manager", "resource budgets enforced with cgroup v2 at " + cgroups->base_path() +
                                          " (CPUs " + vrm::format_cpu_list(cgroups->available_cpus()) + ")");
        } else if (require_cgroups) {
            vrm::log::error("manager", "cannot enforce resource budgets: " + reason);
            return 3;
        } else {
            vrm::log::warn("manager", "resource budgets NOT enforced: " + reason);
        }
    }

    vrm::install_stop_signal_handlers();
    vrm::Manager manager(std::move(manifest), std::move(cgroups), realtime, arbitration, criticality_weights);
    if (!status_file.empty()) {
        manager.set_status_file(status_file,
                                std::chrono::milliseconds(static_cast<long>(status_interval_seconds * 1000)));
    }
    if (!trace_cpus.empty()) {
        std::string reason;
        auto tracer = vrm::CpuTracer::start(vrm::parse_cpu_list(trace_cpus), 2.0, reason);
        if (tracer) {
            vrm::log::info("manager", "tracing which node runs on CPUs " + trace_cpus);
            manager.set_cpu_tracer(std::move(tracer));
        } else {
            vrm::log::warn("manager", "CPU timeline disabled: " + reason);
        }
    }

    if (!manager.start()) {
        manager.stop();
        manager.print_summary();
        return 1;
    }

    manager.supervise(std::chrono::milliseconds(static_cast<long>(exit_after_seconds * 1000)),
                      std::chrono::milliseconds(static_cast<long>(report_interval_seconds * 1000)));
    manager.print_resources();
    manager.print_timing();
    manager.stop();
    manager.print_summary();
    return 0;
}
