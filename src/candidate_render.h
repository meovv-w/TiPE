#pragma once

#include "candidate_layout.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tipe {

struct CandidatePanelRenderResult {
    std::vector<CandidateHitRegion> hitRegions;
    int maxDrawRight = 0;
    bool boundsOk = true;
};

inline void candidateRoundedRect(cairo_t *cr, double x, double y, double width, double height, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI / 2.0, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI / 2.0);
    cairo_arc(cr, x + radius, y + height - radius, radius, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

inline PangoFontDescription *candidateFont(int size, bool bold) {
    auto *font = pango_font_description_from_string(
        bold ? "MiSans, Noto Sans CJK SC, Sans Bold" : "MiSans, Noto Sans CJK SC, Sans");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    return font;
}

inline int candidateTextWidth(std::string_view text, int size, bool bold) {
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto *cr = cairo_create(surface);
    auto *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
    auto *font = candidateFont(size, bold);
    pango_layout_set_font_description(layout, font);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    pango_font_description_free(font);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return width;
}

inline void drawCandidateText(cairo_t *cr, std::string_view text, double x, double y, double width, int size,
                              bool bold, double red, double green, double blue) {
    auto *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
    auto *font = candidateFont(size, bold);
    pango_layout_set_font_description(layout, font);
    if (width > 0) {
        pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
        pango_layout_set_single_paragraph_mode(layout, TRUE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    }
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_save(cr);
    if (width > 0) {
        cairo_rectangle(cr, x, y, width, size + 8);
        cairo_clip(cr);
    }
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
}

inline std::vector<CandidateHitRegion>
candidatePanelHitRegions(const std::vector<VisualCandidateCell> &cells,
                         const std::vector<std::string> &candidateTexts, int width, bool expanded,
                         bool hasPreedit) {
    const auto drawCells = tipeUIDrawCellsFor(cells, candidateTexts, width, expanded);
    int y = tipeUIPanelTopPadding;
    if (hasPreedit) {
        y += tipeUIPanelPreeditHeight + tipeUIPanelPreeditDividerHeight;
    }
    std::vector<CandidateHitRegion> regions;
    regions.reserve(drawCells.size());
    for (const auto &drawCell : drawCells) {
        const int itemY = y + static_cast<int>(drawCell.cell.row) * tipeUIPanelRowHeight;
        regions.push_back({drawCell.cell.index, drawCell.x, itemY, drawCell.x + drawCell.width,
                           itemY + tipeUIPanelRowHeight});
    }
    return regions;
}

inline CandidatePanelRenderResult
renderCandidatePanel(cairo_t *cr, int width, int height, std::string_view preedit, int preeditCursor,
                     const std::vector<std::string> &candidateTexts, std::size_t selectedIndex, bool expanded,
                     bool continuous, std::optional<std::size_t> hoveredCandidate = std::nullopt) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    candidateRoundedRect(cr, 0.5, 0.5, width - 1, height - 1, 8);
    cairo_set_source_rgba(cr, 0.105, 0.105, 0.115, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (continuous) {
        cairo_arc(cr, width - 12.5, 12.5, 3.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.04, 0.52, 1.0, 0.95);
        cairo_fill(cr);
    }

    int y = tipeUIPanelTopPadding;
    if (!preedit.empty()) {
        const int preeditTextWidth = std::max(20, tipeUIPreeditTextWidthFor(width) - (continuous ? 20 : 0));
        drawCandidateText(cr, preedit, 12, y + 1, preeditTextWidth, 15, true, 0.90, 0.90, 0.93);
        const auto cursor = std::clamp(preeditCursor, 0, static_cast<int>(preedit.size()));
        const auto prefix = preedit.substr(0, static_cast<std::size_t>(cursor));
        const int cursorX = std::min(12 + candidateTextWidth(prefix, 15, true), width - 13);
        cairo_set_source_rgba(cr, 0.92, 0.93, 0.96, 0.92);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cursorX + 0.5, y + 3);
        cairo_line_to(cr, cursorX + 0.5, y + 20);
        cairo_stroke(cr);
        y += tipeUIPanelPreeditHeight;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, 0, y + 0.5);
        cairo_line_to(cr, width, y + 0.5);
        cairo_stroke(cr);
        y += tipeUIPanelPreeditDividerHeight;
    }

    const auto cells = visibleVisualCellsFor(candidateTexts, selectedIndex, expanded, tipeUIPanelMaxExpandedRows);
    const auto selectedRow = selectedVisualRow(cells, selectedIndex);
    const auto drawCells = tipeUIDrawCellsFor(cells, candidateTexts, width, expanded);
    CandidatePanelRenderResult result;
    result.hitRegions.reserve(drawCells.size());
    for (const auto &drawCell : drawCells) {
        const auto &cell = drawCell.cell;
        const int x = drawCell.x;
        const int itemWidth = drawCell.width;
        const int itemY = y + static_cast<int>(cell.row) * tipeUIPanelRowHeight;
        result.hitRegions.push_back({cell.index, x, itemY, x + itemWidth, itemY + tipeUIPanelRowHeight});
        const bool selected = cell.index == selectedIndex;
        const bool hovered = hoveredCandidate == cell.index;
        result.maxDrawRight = std::max(result.maxDrawRight, x + itemWidth);
        if (x < tipeUIPanelHorizontalPadding || x + itemWidth > width - tipeUIPanelHorizontalPadding) {
            result.boundsOk = false;
        }
        if (selected || hovered) {
            candidateRoundedRect(cr, x, itemY + 1, itemWidth, 23, 5);
            if (selected) {
                cairo_set_source_rgb(cr, 0.04, 0.52, 1.0);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
            }
            cairo_fill(cr);
        }
        const auto label = shortcutForVisualCell(cells, cell, selectedRow, expanded);
        const auto &text = candidateTexts[cell.index];
        drawCandidateText(cr, label, x + 7, itemY + 5, -1, 13, true, selected ? 1.0 : 0.62,
                          selected ? 1.0 : 0.65, selected ? 1.0 : 0.69);
        drawCandidateText(cr, text, x + 22, itemY + 2, std::max(16, itemWidth - 26), 16, true,
                          selected ? 1.0 : 0.96, selected ? 1.0 : 0.96, selected ? 1.0 : 0.97);
    }
    cairo_restore(cr);
    return result;
}

inline void renderCandidateStatus(cairo_t *cr, int width, int height, std::string_view statusText) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    candidateRoundedRect(cr, 0.5, 0.5, width - 1, height - 1, 8);
    cairo_set_source_rgba(cr, 0.105, 0.105, 0.115, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    constexpr int statusFontSize = 13;
    const int textWidth = candidateTextWidth(statusText, statusFontSize, true);
    const double textX = std::max(8.0, (static_cast<double>(width) - textWidth) / 2.0);
    drawCandidateText(cr, statusText, textX, 4, width - textX - 6, statusFontSize, true, 0.92, 0.93, 0.96);
    cairo_restore(cr);
}

} // namespace tipe
