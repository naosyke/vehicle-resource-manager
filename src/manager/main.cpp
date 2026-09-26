// vrm_manager: starts and supervises the nodes described in a system manifest.
//
//   vrm_manager config/system.yaml               run until Ctrl-C
//   vrm_manager config/system.yaml --exit-after 5  run for 5 seconds (demos, CI)
#include <chrono>
#include <iostream>
#include <string>

#include "manager.hpp"
#include "vrm/log.hpp"
#include "vrm/managed_node.hpp"

int main(int argc, char** argv) {
    std::string manifest_path;
    double exit_after_seconds = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--exit-after" && i + 1 < argc) {
            exit_after_seconds = std::stod(argv[++i]);
        } else if (manifest_path.empty() && arg.rfind("--", 0) != 0) {
            manifest_path = arg;
        } else {
            std::cerr << "usage: vrm_manager MANIFEST.yaml [--exit-after SECONDS]\n";
            return 2;
        }
    }
    if (manifest_path.empty()) {
        std::cerr << "usage: vrm_manager MANIFEST.yaml [--exit-after SECONDS]\n";
        return 2;
    }

    vrm::SystemManifest manifest;
    try {
        manifest = vrm::load_manifest(manifest_path);
    } catch (const vrm::ManifestError& error) {
        vrm::log::error("manager", error.what());
        return 2;
    }

    vrm::install_stop_signal_handlers();
    vrm::Manager manager(std::move(manifest));

    if (!manager.start()) {
        manager.stop();
        manager.print_summary();
        return 1;
    }

    manager.supervise(std::chrono::milliseconds(static_cast<long>(exit_after_seconds * 1000)));
    manager.stop();
    manager.print_summary();
    return 0;
}
