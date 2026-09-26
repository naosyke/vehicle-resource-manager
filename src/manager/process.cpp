#include "process.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

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

pid_t spawn_process(const std::string& executable, const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // Own process group, so a Ctrl-C in the terminal reaches only the
        // manager, which then shuts the nodes down in order.
        setpgid(0, 0);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "cannot execute %s: %s\n", argv[0], std::strerror(errno));
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
