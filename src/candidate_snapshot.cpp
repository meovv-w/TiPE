#include "candidate_snapshot.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tipe {

namespace {

bool sameRect(const CandidateSnapshotRect &left, const CandidateSnapshotRect &right) {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height;
}

bool usableRect(const CandidateSnapshotRect &rect) {
    return rect.height > 0;
}

} // namespace

bool CandidateCursorFollowTracker::observe(CandidateSnapshotRect rect, std::string_view preedit,
                                           std::size_t preeditCursor) {
    if (!usableRect(rect) || preedit.empty()) {
        reset();
        return false;
    }

    if (!initialized_) {
        lastRect_ = rect;
        lastPreedit_ = preedit;
        lastPreeditCursor_ = preeditCursor;
        initialized_ = true;
        return false;
    }

    const bool rectChanged = !sameRect(lastRect_, rect);
    const bool preeditPositionChanged = lastPreedit_ != preedit || lastPreeditCursor_ != preeditCursor;
    if (rectChanged) {
        // Once a client proves that it reports a live caret, never add a synthetic
        // offset for the rest of this composition. This also handles a deferred
        // caret update arriving after the first snapshot for a new key.
        mode_ = Mode::Live;
    } else if (preeditPositionChanged && mode_ != Mode::Live) {
        mode_ = Mode::Static;
    }

    lastRect_ = rect;
    lastPreedit_ = preedit;
    lastPreeditCursor_ = preeditCursor;
    return mode_ == Mode::Static;
}

void CandidateCursorFollowTracker::reset() {
    lastRect_ = {};
    lastPreedit_.clear();
    lastPreeditCursor_ = 0;
    mode_ = Mode::Unknown;
    initialized_ = false;
}

std::string escapeCandidateSnapshotField(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '|':
            escaped += "\\|";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

CandidateSnapshotRect logicalCandidateSnapshotRect(CandidateSnapshotRect rect, double scale) {
    scale = std::max(1.0, scale);
    const auto logical = [scale](int value) { return static_cast<int>(std::lround(value / scale)); };
    return {logical(rect.x), logical(rect.y), logical(rect.width), logical(rect.height)};
}

CandidateSnapshotAnchor candidateSnapshotAnchorFor(std::string_view frontend, CandidateSnapshotRect rect,
                                                    double scale) {
    auto logical = logicalCandidateSnapshotRect(rect, scale);
    if (frontend != "xim" || logical.height > 0) {
        return {logical, false};
    }
    if (logical.x != 0 || logical.y != 0) {
        logical.width = 1;
        logical.height = 22;
        return {logical, false};
    }
    return {logical, true};
}

std::string buildCandidateSnapshotLine(std::string_view preedit, bool expanded, std::size_t selectedIndex,
                                       CandidateSnapshotRect rect, const std::vector<std::string> &candidates,
                                       std::string_view metadata) {
    std::ostringstream line;
    line << preedit << '\t' << (expanded ? '1' : '0') << '\t' << selectedIndex << '\t' << rect.x << '\t' << rect.y
         << '\t' << rect.width << '\t' << rect.height << '\t';
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index != 0) {
            line << '|';
        }
        line << escapeCandidateSnapshotField(candidates[index]);
    }
    if (!metadata.empty()) {
        line << '\t' << metadata;
    }
    line << '\n';
    return line.str();
}

std::string clearCandidateSnapshotLine() { return "\t0\t0\t0\t0\t0\t0\t\n"; }

} // namespace tipe
