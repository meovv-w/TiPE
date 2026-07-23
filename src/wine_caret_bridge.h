#pragma once

#include "candidate_snapshot.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace tipe {

struct WineCaretBridgeReply {
    std::uint64_t serial = 0;
    std::optional<CandidateSnapshotRect> rect;
    bool hasImmContext = false;
};

struct WineCaretBridgeRequest {
    bool sent = false;
    pid_t targetPid = -1;
    int rootWidth = 0;
    int rootHeight = 0;
    int outputFd = -1;
};

std::optional<WineCaretBridgeReply> parseWineCaretBridgeReply(std::string_view line);

class WineCaretBridge {
public:
    explicit WineCaretBridge(std::string executablePath);
    ~WineCaretBridge();

    WineCaretBridge(const WineCaretBridge &) = delete;
    WineCaretBridge &operator=(const WineCaretBridge &) = delete;

    WineCaretBridgeRequest request(std::uint64_t serial);
    void stop();
    int outputFd() const { return outputFd_; }

private:
    bool start(std::string prefix, pid_t targetPid);
    bool writeRequest(std::uint64_t serial);

    std::string executablePath_;
    std::string prefix_;
    pid_t targetPid_ = -1;
    pid_t childPid_ = -1;
    int inputFd_ = -1;
    int outputFd_ = -1;
};

} // namespace tipe
