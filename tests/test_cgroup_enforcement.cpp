// Verifies that budgets are really enforced by the kernel. Run in a
// container with a writable cgroup filesystem:
//   docker run --rm --privileged --cgroupns=private ... ctest
// Without it, every test is skipped.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "vrm/cgroup.hpp"

namespace {

class CgroupEnforcement : public ::testing::Test {
protected:
    void SetUp() override {
        std::string reason;
        cgroups_ = vrm::CgroupManager::create("vrm-test", reason);
        if (!cgroups_) GTEST_SKIP() << reason;
    }

    // Runs `sh -c command` inside the node's cgroup and waits for it.
    int run_in_group(const std::string& procs, const std::string& command, int timeout_seconds) {
        const pid_t pid = fork();
        if (pid == 0) {
            std::ofstream(procs) << getpid() << std::flush;
            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return status;
    }

    std::unique_ptr<vrm::CgroupManager> cgroups_;
};

}  // namespace

TEST_F(CgroupEnforcement, MemoryLimitKillsProcessThatExceedsIt) {
    vrm::ResourceBudget budget;
    budget.memory_bytes = 32ull * 1024 * 1024;
    const auto procs = cgroups_->create_group("hog", budget);

    // tail keeps reading /dev/zero into memory because it never sees a newline.
    // exec, so the killed process is the one we wait for (not a parent shell).
    const int status = run_in_group(procs, "exec tail /dev/zero", 20);

    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    const auto usage = cgroups_->usage("hog");
    EXPECT_GE(usage.oom_kills, 1u);
    EXPECT_LE(usage.memory_peak, 32ull * 1024 * 1024);
    cgroups_->remove_group("hog");
}

TEST_F(CgroupEnforcement, CpuLimitThrottlesBusyLoop) {
    vrm::ResourceBudget budget;
    budget.cpu_cores = 0.2;
    const auto procs = cgroups_->create_group("spinner", budget);

    const auto start = std::chrono::steady_clock::now();
    run_in_group(procs, "timeout 1 sh -c 'while :; do :; done'", 5);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const auto usage = cgroups_->usage("spinner");
    const double cpu_seconds = usage.cpu_usage_usec / 1e6;
    // A busy loop would use ~1 s of CPU; the budget allows 20 % of that.
    EXPECT_LT(cpu_seconds, 0.2 * wall + 0.05);
    EXPECT_GT(cpu_seconds, 0.1);
    EXPECT_GT(usage.nr_throttled, 0u);
    cgroups_->remove_group("spinner");
}

TEST_F(CgroupEnforcement, CpusetPinsProcessToCpu) {
    vrm::ResourceBudget budget;
    budget.cpus = {0};
    const auto procs = cgroups_->create_group("pinned", budget);
    const std::string output = "/tmp/vrm_cpuset_test";

    run_in_group(procs, "grep Cpus_allowed_list /proc/self/status > " + output, 5);

    std::ifstream file(output);
    std::string line;
    std::getline(file, line);
    EXPECT_EQ(line, "Cpus_allowed_list:\t0");
    cgroups_->remove_group("pinned");
}

TEST_F(CgroupEnforcement, UnavailableCpuIsRejected) {
    vrm::ResourceBudget budget;
    budget.cpus = {999};
    EXPECT_THROW(cgroups_->create_group("impossible", budget), vrm::CgroupError);
    cgroups_->remove_group("impossible");
}
