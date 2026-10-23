#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tipe::grid {

struct Rect {
  int x{}, y{}, width{}, height{};
  bool contains(int px, int py) const;
};

// The renderer measures each candidate with its actual font and scale.
// Keeping font measurement outside this module makes layout deterministic
// and avoids dependence on a particular toolkit or display protocol.
struct Candidate {
  std::string text;
  int text_width{};
};

struct Options {
  int available_width{640};
  int available_height{320};
  // Matches the painter's default 16px font line (font + 12px padding).
  int row_height{28};
  int padding{6};
  int column_gap{4};
  int row_gap{4};
  int label_width{18};
  int cell_padding{8};
  int minimum_cell_width{56};
  bool expanded{};
  // Zero keeps free-flow packing. A positive count aligns that many columns,
  // each sized independently from its own candidates (long words do not
  // widen every column).
  size_t columns{};
  // When set, aligned columns fill the available width. This is used by the
  // expanded candidate panel so opening it adds rows without changing the
  // compact strip's outer width.
  bool stretch_columns{};
  size_t maximum_candidates{10};
  std::vector<int> column_widths; // Explicit widths keep expanded columns fixed.
  int maximum_cell_width{};  // zero means bounded only by available_width
};

struct Cell {
  size_t candidate_index{};
  size_t row{}, column{};
  Rect rect;
  Rect text_rect;
  // Labels always restart on the current view: 1..9, then 0.
  char label{'1'};
  bool elide{};
};

struct Layout {
  Rect bounds;
  std::vector<Cell> cells;
  size_t begin{}, end{}, total{};
  size_t rows{};
  bool expanded{};
  bool aligned_columns{};
  bool has_previous() const { return begin != 0; }
  bool has_next() const { return end < total; }
};

// By default at most ten candidates appear in a view. Compact mode uses one row;
// expanded mode packs variable-width cells into as many rows as fit.
Layout layout(std::span<const Candidate>, const Options&, size_t begin = 0);

// Compact strip and expanded panel share the first row's exact geometry.
// Expanded pages expose up to forty candidates; height clipping scrolls whole
// rows to keep selected visible, without changing column widths.
Layout strip_layout(std::span<const Candidate>, bool expanded, int width = 768,
                    int height = 320, size_t begin = 0, int selected = -1,
                    int row_height = 28);

// Replays the same deterministic pagination to find the view immediately
// preceding current_begin. Useful when Left first scrolls the window back.
size_t previous_page(std::span<const Candidate>, const Options&, size_t current_begin);

enum class Direction { Left, Right, Up, Down, Home, End };
enum class Action { Stay, Select, PreviousPage, NextPage, EditPinyin };
struct Navigation {
  Action action{Action::Stay};
  std::optional<size_t> candidate_index;
};

// Left/Right move horizontally in row-major order: at a row edge they cross
// to the first/last cell of the adjacent row. Up/Down preserve horizontal
// position by choosing the nearest cell center in the adjacent row. At the
// left edge, scroll to a previous view before entering pinyin editing at the
// first view.
Navigation navigate(const Layout&, size_t current_candidate, Direction);
std::optional<size_t> hit_test(const Layout&, int x, int y);
std::optional<size_t> select_label(const Layout&, char digit);

} // namespace tipe::grid
