#include "fork_exec.h"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void inheritedTerminationHandler(int) { _exit(99); }

} // namespace

int main() {
    struct sigaction inherited {};
    inherited.sa_handler = inheritedTerminationHandler;
    sigemptyset(&inherited.sa_mask);
    struct sigaction previous {};
    if (sigaction(SIGTERM, &inherited, &previous) != 0) {
        std::cerr << "failed to install inherited SIGTERM handler\n";
        return 1;
    }

    const pid_t child = tipe::forkExecChild();
    if (child < 0) {
        std::cerr << "forkExecChild failed\n";
        sigaction(SIGTERM, &previous, nullptr);
        return 1;
    }
    if (child == 0) {
        pause();
        _exit(98);
    }

    if (kill(child, SIGTERM) != 0) {
        std::cerr << "failed to terminate forked child\n";
        kill(child, SIGKILL);
    }
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    sigaction(SIGTERM, &previous, nullptr);
    if (waited != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
        std::cerr << "forked child retained the parent's SIGTERM handler\n";
        return 1;
    }
    return 0;
}
