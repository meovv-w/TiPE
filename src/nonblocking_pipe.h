#pragma once

#include <cerrno>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

namespace tipe {

enum class NonblockingWriteResult {
    Complete,
    WouldBlock,
    Failed,
};

inline bool setFileDescriptorNonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Candidate snapshots are independent newline-delimited messages. A partial
// write would corrupt that framing, so callers must discard the channel and
// create a fresh one instead of completing the message later.
inline NonblockingWriteResult writeNonblockingMessage(int descriptor, std::string_view message) {
    if (descriptor < 0) {
        return NonblockingWriteResult::Failed;
    }
    if (message.empty()) {
        return NonblockingWriteResult::Complete;
    }

    for (;;) {
        const auto written = ::write(descriptor, message.data(), message.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return NonblockingWriteResult::WouldBlock;
        }
        if (written == static_cast<ssize_t>(message.size())) {
            return NonblockingWriteResult::Complete;
        }
        return NonblockingWriteResult::Failed;
    }
}

} // namespace tipe
