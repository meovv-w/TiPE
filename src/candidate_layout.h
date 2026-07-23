#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tipe {

inline constexpr std::size_t visualCandidateColumns = 6;
inline constexpr int tipeUIPanelCellWidth = 92;
inline constexpr int tipeUIPanelExpandedCellWidth = 94;
inline constexpr int tipeUIPanelCompactCellMinWidth = 46;
inline constexpr int tipeUIPanelCompactCellGap = 2;
inline constexpr int tipeUIPanelMaxWidth = 596;
inline constexpr int tipeUIPanelMinWidth = 150;
inline constexpr int tipeUIPanelHorizontalPadding = 10;
inline constexpr int tipeUIPanelTopPadding = 7;
inline constexpr int tipeUIPanelBottomPadding = 5;
inline constexpr int tipeUIPanelRowHeight = 28;
inline constexpr int tipeUIPanelPreeditHeight = 27;
inline constexpr int tipeUIPanelPreeditDividerHeight = 1;
inline constexpr int tipeUIPanelMaxExpandedRows = 5;
inline constexpr int tipeUIPopupEdgeFallbackLeftThreshold = 1180;
inline constexpr int tipeUIPopupEdgeFallbackTopThreshold = 620;
inline constexpr int tipeUIStatusPopupWidth = 54;
inline constexpr int tipeUIStatusPopupHeight = 26;
struct VisualCandidateCell {
    std::size_t index = 0;
    std::size_t row = 0;
    std::size_t column = 0;
    std::size_t span = 1;
};

struct VisualCandidateDrawCell {
    VisualCandidateCell cell;
    int x = 0;
    int width = 0;
};

struct CandidateHitRegion {
    std::size_t index = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

inline std::optional<std::size_t> candidateIndexAtPoint(const std::vector<CandidateHitRegion> &regions,
                                                        double x, double y) {
    const auto iter = std::find_if(regions.begin(), regions.end(), [x, y](const auto &region) {
        return x >= region.left && x < region.right && y >= region.top && y < region.bottom;
    });
    return iter == regions.end() ? std::nullopt : std::optional<std::size_t>(iter->index);
}

inline int estimatedCandidateCodepointWidth(unsigned char leadByte) {
    return leadByte < 0x80 ? 8 : 15;
}

inline int estimatedCandidateTextWidth(std::string_view text) {
    int width = 0;
    for (const unsigned char ch : text) {
        if ((ch & 0xC0) == 0x80) {
            continue;
        }
        width += estimatedCandidateCodepointWidth(ch);
    }
    return width;
}

inline std::size_t candidateVisualSpan(const std::string &candidate) {
    const int widthWithLabel = estimatedCandidateTextWidth(candidate) + 38;
    return std::clamp<std::size_t>(
        static_cast<std::size_t>((widthWithLabel + tipeUIPanelCellWidth - 1) / tipeUIPanelCellWidth), 1,
        visualCandidateColumns);
}

inline int tipeUICompactCellWidthFor(const std::vector<std::string> &candidateTexts,
                                     const VisualCandidateCell &cell) {
    const int textWidth = cell.index < candidateTexts.size() ? estimatedCandidateTextWidth(candidateTexts[cell.index]) : 0;
    const int minWidth = static_cast<int>(cell.span) * tipeUIPanelCompactCellMinWidth;
    const int maxWidth = static_cast<int>(cell.span) * tipeUIPanelCellWidth;
    return std::clamp(textWidth + 38, minWidth, maxWidth);
}

inline int tipeUIVisualColumnWidthFor(int panelWidth) {
    const int gapWidth = static_cast<int>(visualCandidateColumns - 1) * tipeUIPanelCompactCellGap;
    const int contentWidth = panelWidth - tipeUIPanelHorizontalPadding * 2 - gapWidth;
    return std::clamp(contentWidth / static_cast<int>(visualCandidateColumns), 44,
                      tipeUIPanelExpandedCellWidth);
}

inline int tipeUIVisualCellX(std::size_t column, int visualColumnWidth) {
    return tipeUIPanelHorizontalPadding +
           static_cast<int>(column) * (visualColumnWidth + tipeUIPanelCompactCellGap);
}

inline int tipeUIVisualCellWidth(std::size_t span, int visualColumnWidth) {
    if (span == 0) {
        return 0;
    }
    return static_cast<int>(span) * visualColumnWidth +
           static_cast<int>(span - 1) * tipeUIPanelCompactCellGap;
}

inline std::vector<VisualCandidateDrawCell>
tipeUIDrawCellsFor(const std::vector<VisualCandidateCell> &visibleCells,
                   const std::vector<std::string> &candidateTexts, int panelWidth, bool expanded) {
    std::vector<VisualCandidateDrawCell> drawCells;
    drawCells.reserve(visibleCells.size());
    const int visualColumnWidth = tipeUIVisualColumnWidthFor(panelWidth);
    if (expanded) {
        for (const auto &cell : visibleCells) {
            drawCells.push_back({cell, tipeUIVisualCellX(cell.column, visualColumnWidth),
                                 tipeUIVisualCellWidth(cell.span, visualColumnWidth)});
        }
        return drawCells;
    }

    std::size_t rowCount = 1;
    for (const auto &cell : visibleCells) {
        rowCount = std::max(rowCount, cell.row + 1);
    }
    std::vector<int> rowOffsets(rowCount, 0);
    for (const auto &cell : visibleCells) {
        auto &rowOffset = rowOffsets[cell.row];
        const int x = tipeUIPanelHorizontalPadding + rowOffset;
        const int width = tipeUICompactCellWidthFor(candidateTexts, cell);
        drawCells.push_back({cell, x, width});
        rowOffset += width + tipeUIPanelCompactCellGap;
    }
    return drawCells;
}

inline std::vector<VisualCandidateCell> visualCandidateCells(const std::vector<std::string> &candidates) {
    std::vector<VisualCandidateCell> cells;
    std::vector<std::size_t> rowColumns;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto span = std::min<std::size_t>(candidateVisualSpan(candidates[index]), visualCandidateColumns);
        std::size_t row = 0;
        for (; row < rowColumns.size(); ++row) {
            if (rowColumns[row] + span <= visualCandidateColumns) {
                break;
            }
        }
        if (row == rowColumns.size()) {
            rowColumns.push_back(0);
        }
        const auto column = rowColumns[row];
        cells.push_back({index, row, column, span});
        rowColumns[row] += span;
    }
    return cells;
}

inline std::vector<VisualCandidateCell> cellsInVisualRow(const std::vector<VisualCandidateCell> &cells,
                                                         std::size_t row) {
    std::vector<VisualCandidateCell> rowCells;
    std::copy_if(cells.begin(), cells.end(), std::back_inserter(rowCells), [row](const auto &cell) {
        return cell.row == row;
    });
    return rowCells;
}

inline std::vector<VisualCandidateCell> collapsedVisualCandidateCells(const std::vector<std::string> &candidates) {
    return cellsInVisualRow(visualCandidateCells(candidates), 0);
}

inline std::optional<VisualCandidateCell> visualCellForIndex(const std::vector<VisualCandidateCell> &cells,
                                                             std::size_t index) {
    const auto iter = std::find_if(cells.begin(), cells.end(), [index](const auto &cell) {
        return cell.index == index;
    });
    if (iter == cells.end()) {
        return std::nullopt;
    }
    return *iter;
}

inline std::size_t visualRowCount(const std::vector<VisualCandidateCell> &cells) {
    if (cells.empty()) {
        return 0;
    }
    return std::max_element(cells.begin(), cells.end(), [](const auto &lhs, const auto &rhs) {
               return lhs.row < rhs.row;
           })->row +
           1;
}

inline std::size_t selectedVisualRow(const std::vector<VisualCandidateCell> &cells, std::size_t selectedIndex) {
    if (const auto cell = visualCellForIndex(cells, selectedIndex)) {
        return cell->row;
    }
    return selectedIndex / visualCandidateColumns;
}

inline std::size_t firstVisibleExpandedRow(std::size_t selectedRow, std::size_t rowCount,
                                           std::size_t maxRows = 5) {
    return rowCount <= maxRows ? 0 : std::min(selectedRow > 1 ? selectedRow - 1 : 0, rowCount - maxRows);
}

inline std::vector<VisualCandidateCell> visibleVisualCellsFor(const std::vector<std::string> &candidateTexts,
                                                              std::size_t selectedIndex, bool expanded,
                                                              std::size_t maxExpandedRows = 5) {
    const auto allCells = visualCandidateCells(candidateTexts);
    std::vector<VisualCandidateCell> result;
    if (!expanded) {
        return collapsedVisualCandidateCells(candidateTexts);
    }

    const auto selectedRow = selectedVisualRow(allCells, selectedIndex);
    const auto rowCount = visualRowCount(allCells);
    const auto firstRow = firstVisibleExpandedRow(selectedRow, rowCount, maxExpandedRows);
    for (const auto &cell : allCells) {
        if (cell.row < firstRow || cell.row >= firstRow + maxExpandedRows) {
            continue;
        }
        auto visibleCell = cell;
        visibleCell.row -= firstRow;
        result.push_back(visibleCell);
    }
    return result;
}

inline std::string shortcutForVisualCell(const std::vector<VisualCandidateCell> &cells,
                                         const VisualCandidateCell &cell, std::size_t selectedRow, bool expanded) {
    if (expanded && cell.row != selectedRow) {
        return "";
    }
    std::size_t shortcut = 1;
    for (const auto &other : cells) {
        if (other.row != cell.row) {
            continue;
        }
        if (other.index == cell.index) {
            return std::to_string(shortcut);
        }
        ++shortcut;
    }
    return "";
}

struct TipeUIPanelMetrics {
    int width = tipeUIPanelMinWidth;
    int height = 0;
    int visibleRows = 0;
    std::size_t occupiedColumns = 0;
};

inline TipeUIPanelMetrics tipeUIPanelMetricsFor(const std::vector<VisualCandidateCell> &visibleCells,
                                                const std::vector<std::string> &candidateTexts, bool expanded,
                                                bool hasPreedit) {
    TipeUIPanelMetrics metrics;
    metrics.visibleRows = visibleCells.empty() ? 0 : 1;
    for (const auto &cell : visibleCells) {
        metrics.occupiedColumns = std::max(metrics.occupiedColumns, cell.column + cell.span);
        metrics.visibleRows = std::max(metrics.visibleRows, static_cast<int>(cell.row + 1));
    }

    if (expanded) {
        metrics.width = tipeUIPanelMaxWidth;
    } else {
        std::vector<int> rowWidths(static_cast<std::size_t>(std::max(1, metrics.visibleRows)), 0);
        for (const auto &cell : visibleCells) {
            auto &rowWidth = rowWidths[cell.row];
            if (rowWidth > 0) {
                rowWidth += tipeUIPanelCompactCellGap;
            }
            rowWidth += tipeUICompactCellWidthFor(candidateTexts, cell);
        }
        const int contentWidth = rowWidths.empty() ? 0 : *std::max_element(rowWidths.begin(), rowWidths.end());
        metrics.width = std::clamp(tipeUIPanelHorizontalPadding * 2 + contentWidth, tipeUIPanelMinWidth,
                                   tipeUIPanelMaxWidth);
    }
    const int preeditHeight = hasPreedit ? tipeUIPanelPreeditHeight + tipeUIPanelPreeditDividerHeight : 0;
    metrics.height = tipeUIPanelTopPadding + preeditHeight + metrics.visibleRows * tipeUIPanelRowHeight +
                     tipeUIPanelBottomPadding;
    return metrics;
}

inline int tipeUIPreeditTextWidthFor(int panelWidth) {
    return std::max(16, panelWidth - 24);
}

inline int tipeUIBufferScaleFor(double inputScale, int preferredScale) {
    const double requestedScale = std::max(inputScale, static_cast<double>(preferredScale));
    return std::clamp(static_cast<int>(std::ceil(requestedScale)), 1, 4);
}

inline bool tipeUIPopupEdgeFallbackNeededForRect(int left, int top, int width, int height,
                                                int leftThreshold = tipeUIPopupEdgeFallbackLeftThreshold,
                                                int topThreshold = tipeUIPopupEdgeFallbackTopThreshold,
                                                int popupWidth = 0, int popupHeight = 0) {
    const bool usable = left > 0 || top > 0 || width > 0 || height > 0;
    if (!usable) {
        return false;
    }
    const int right = left + std::max(0, width) + std::max(0, popupWidth);
    const int bottom = top + std::max(0, height) + std::max(0, popupHeight);
    return left >= leftThreshold || top >= topThreshold ||
           (popupWidth > 0 && right >= leftThreshold) ||
           (popupHeight > 0 && bottom >= topThreshold);
}

} // namespace tipe
