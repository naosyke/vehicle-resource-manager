#include "process.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

extern char** environ;

namespace vrm {

std::string resolve_executable(const std::string& executable) {
    if (executable.find('/') != std::string::npos) {
        return executable;
    }
    char self[4096];
    const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        std::string directory(self);
        directory.erase(directory.find_last_of('/'));
        const std::string candidate = directory + "/" + executable;
        struct stat info {};
        if (stat(candidate.c_str(), &info) == 0 && (info.st_mode & S_IXUSR)) {
            return candidate;
        }
    }
    return executable;  // Fall back to PATH lookup in execvp.
}

namespace {

// Between fork() and exec() the child of a multi-threaded process may only
// call async-signal-safe functions (no malloc, no stdio), so these helpers
// use plain system calls and stack buffers.
void write_stderr(const char* text) {
    ssize_t ignored = write(STDERR_FILENO, text, std::strlen(text));
    (void)ignored;
}

bool join_cgroup(const char* procs_path) {
    const int fd = open(procs_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char digits[16];
    int length = 0;
    for (pid_t pid = getpid(); pid > 0; pid /= 10) digits[length++] = static_cast<char>('0' + pid % 10);
    char text[16];
    for (int i = 0; i < length; ++i) text[i] = digits[length - 1 - i];
    const bool ok = write(fd, text, length) == length;
    close(fd);
    return ok;
}

}  // namespace

pid_t spawn_process(const std::string& executable, const std::vector<std::string>& args,
                    const std::string& cgroup_procs, const std::vector<std::string>& extra_env,
                    int rt_priority) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // The environment is prepared before fork(); the child must not allocate.
    std::vector<char*> envp;
    for (char** variable = environ; *variable; ++variable) envp.push_back(*variable);
    for (const auto& variable : extra_env) envp.push_back(const_cast<char*>(variable.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // Own process group, so a Ctrl-C in the terminal reaches only the
        // manager, which then shuts the nodes down in order.
        setpgid(0, 0);
        if (!cgroup_procs.empty() && !join_cgroup(cgroup_procs.c_str())) {
            write_stderr("cannot join cgroup ");
            write_stderr(cgroup_procs.c_str());
            write_stderr("\n");
            _exit(126);
        }
        if (rt_priority > 0) {
            sched_param param{};
            param.sched_priority = rt_priority;
            // On failure the node runs with normal scheduling; the manager
            // notices from the node's heartbeat and warns.
            if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
                write_stderr("cannot set SCHED_FIFO (missing CAP_SYS_NICE?)\n");
            }
        }
        execvpe(argv[0], argv.data(), envp.data());
        write_stderr("cannot execute ");
        write_stderr(argv[0]);
        write_stderr("\n");
        _exit(127);
    }
    return pid;
}

std::optional<ExitInfo> reap_child() {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) {
        return std::nullopt;
    }
    return ExitInfo{pid, status};
}

std::string describe_exit(int status) {
    if (WIFEXITED(status)) {
        return "exited with code " + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "killed by signal " + std::to_string(WTERMSIG(status)) + " (" +
               strsignal(WTERMSIG(status)) + ")";
    }
    return "stopped (status " + std::to_string(status) + ")";
}

}  // namespace vrm
