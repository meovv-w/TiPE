#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tipe {

struct CandidateSnapshotRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CandidateSnapshotAnchor {
    CandidateSnapshotRect rect;
    bool pointerFallback = false;
};

class CandidateCursorFollowTracker {
public:
    bool observe(CandidateSnapshotRect rect, std::string_view preedit, std::size_t preeditCursor);
    void reset();

private:
    enum class Mode {
        Unknown,
        Static,
        Live,
    };

    CandidateSnapshotRect lastRect_;
    std::string lastPreedit_;
    std::size_t lastPreeditCursor_ = 0;
    Mode mode_ = Mode::Unknown;
    bool initialized_ = false;
};

std::string escapeCandidateSnapshotField(std::string_view text);
CandidateSnapshotRect logicalCandidateSnapshotRect(CandidateSnapshotRect rect, double scale);
CandidateSnapshotAnchor candidateSnapshotAnchorFor(std::string_view frontend, CandidateSnapshotRect rect,
                                                    double scale);
std::string buildCandidateSnapshotLine(std::string_view preedit, bool expanded, std::size_t selectedIndex,
                                       CandidateSnapshotRect rect, const std::vector<std::string> &candidates,
                                       std::string_view metadata = {});
std::string clearCandidateSnapshotLine();

} // namespace tipe
