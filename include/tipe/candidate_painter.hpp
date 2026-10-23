#pragma once

#include "tipe/candidate_grid.hpp"

#include <cairo/cairo.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tipe::painter {

// A measured, ready-to-draw candidate surface. Coordinates are logical
// pixels. The caller can render it on a HiDPI surface by setting
// scale_factor; hit_test uses the same logical coordinates as grid::Layout.
struct Frame {
  grid::Layout layout;
  std::vector<std::string> candidates;
  std::string preedit;
  int width{};
  int height{};
  int font_pixels{16};
  int selected{-1};
  size_t begin{};
  bool expanded{};
  double scale_factor{1.0};
  grid::Rect preedit_rect;
  grid::Rect footer_rect;
  std::string page_text;
};

// Measure UTF-8 candidate text with the installed Pango font and construct
// the exact grid used by paint(). The candidate vector is copied into Frame
// because a popup normally outlives the input-method View that created it.
Frame measure(std::span<const grid::Candidate> candidates,
              std::string preedit, bool expanded, int available_width,
              int available_height, int selected = -1, size_t begin = 0,
              int font_pixels = 16, double scale_factor = 1.0);

Frame measure(std::span<const std::string> candidates, std::string preedit,
              bool expanded, int available_width, int available_height,
              int selected = -1, size_t begin = 0, int font_pixels = 16,
              double scale_factor = 1.0);

// Draws the complete popup (preedit, candidate cells, labels, selection and
// page indicator). cairo_t's current transform is preserved. Text clipping
// and ellipsizing happen inside each grid cell, so long words cannot paint
// over neighboring candidates.
void paint(cairo_t *, const Frame &);

// Mouse coordinates are physical surface pixels; scale_factor is applied
// before delegating to the Layout used for drawing. Protocols delivering
// logical coordinates can call grid::hit_test(frame.layout, x, y) directly.
std::optional<size_t> hit_test(const Frame &, int x, int y);
std::optional<size_t> select_label(const Frame &, char label);

} // namespace tipe::painter
