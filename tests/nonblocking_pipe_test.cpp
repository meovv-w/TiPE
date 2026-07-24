#include "nonblocking_pipe.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

[[noreturn]] void fail(const char *message) {
    std::cerr << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    int descriptors[2]{-1, -1};
    if (::pipe(descriptors) != 0) {
        fail("cannot create test pipe");
    }
    if (!tipe::setFileDescriptorNonblocking(descriptors[1])) {
        fail("cannot make test pipe nonblocking");
    }
    if (tipe::writeNonblockingMessage(descriptors[1], "snapshot\n") !=
        tipe::NonblockingWriteResult::Complete) {
        fail("empty pipe should accept a complete snapshot");
    }

    const std::array<char, 4096> fill{};
    for (;;) {
        const auto written = ::write(descriptors[1], fill.data(), fill.size());
        if (written > 0) {
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        fail("unexpected result while filling nonblocking pipe");
    }
    if (tipe::writeNonblockingMessage(descriptors[1], "latest\n") !=
        tipe::NonblockingWriteResult::WouldBlock) {
        fail("full pipe should report backpressure without waiting");
    }

    ::close(descriptors[0]);
    ::close(descriptors[1]);
    std::cout << "nonblocking pipe ok\n";
    return 0;
}
