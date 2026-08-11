#pragma once

#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tipe {

// Block SIGTERM across fork so a parent cannot terminate the child before it
// replaces fcitx5's inherited signal handler. The child restores the default
// disposition before unblocking; exec then starts with normal process signals.
inline pid_t forkExecChild() {
    sigset_t blocked{};
    sigset_t previous{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    const int blockResult = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (blockResult != 0) {
        errno = blockResult;
        return -1;
    }

    const pid_t child = fork();
    const int forkError = errno;
    if (child == 0) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, nullptr) != 0 ||
            pthread_sigmask(SIG_SETMASK, &previous, nullptr) != 0) {
            _exit(126);
        }
        return 0;
    }

    const int restoreResult = pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (child < 0) {
        errno = forkError;
        return -1;
    }
    if (restoreResult != 0) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        errno = restoreResult;
        return -1;
    }
    return child;
}

} // namespace tipe
