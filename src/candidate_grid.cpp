#include "tipe/candidate_grid.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace tipe::grid {
bool Rect::contains(int px, int py) const {
  return px >= x && py >= y && static_cast<int64_t>(px) < static_cast<int64_t>(x) + width &&
         static_cast<int64_t>(py) < static_cast<int64_t>(y) + height;
}

Layout layout(std::span<const Candidate> candidates, const Options& o, size_t begin) {
  Layout out;
  out.begin = out.end = std::min(begin, candidates.size());
  out.total = candidates.size();
  out.expanded = o.expanded;
  if (out.begin == out.total || o.available_width <= 0 || o.available_height <= 0) return out;

  // Bound decoration to the available space, including extremely narrow
  // screens. All arithmetic below stays within the supplied rectangle.
  const int p = std::clamp(o.padding, 0, std::min(o.available_width, o.available_height) / 4);
  const int width = o.available_width - 2 * p;
  const int height = o.available_height - 2 * p;
  const int rh = std::clamp(o.row_height, 1, height);
  const int cg = std::clamp(o.column_gap, 0, width);
  const int rg = std::clamp(o.row_gap, 0, height);
  const int cp = std::clamp(o.cell_padding, 0, width / 4);
  const int lw = std::clamp(o.label_width, 0, width);
  int x = p, y = p, used_width = 0;
  const int64_t right = static_cast<int64_t>(p) + width;
  const int64_t bottom = static_cast<int64_t>(p) + height;
  size_t row = 0, column = 0;
  const size_t columns = o.expanded ? std::min<size_t>(10, o.columns) : 0;
  out.aligned_columns = columns != 0;
  std::vector<int> column_widths(columns, 1);
  const auto desired_width = [&](const Candidate &candidate) {
    int64_t result = std::max<int64_t>(std::max(1, o.minimum_cell_width),
        static_cast<int64_t>(std::max(0, candidate.text_width)) + lw + 2 * cp);
    if (o.maximum_cell_width > 0) result = std::min<int64_t>(result, o.maximum_cell_width);
    // Aligned expanded columns may retain their natural widths when the
    // caller asks them to stretch.  Applying an equal per-column cap here
    // would make a long phrase disappear even though the whole row fits.
    if (columns && !o.stretch_columns) result = std::min<int64_t>(result,
        std::max<int64_t>(1, (width - (columns - 1) * std::min<int64_t>(cg, width / columns)) / columns));
    return static_cast<int>(std::min<int64_t>(width, result));
  };
  if (columns) {
    for (size_t i = out.begin; i < candidates.size() && i - out.begin < o.maximum_candidates; ++i) {
      auto &cw = column_widths[(i - out.begin) % columns];
      cw = std::max(cw, desired_width(candidates[i]));
    }
    if (o.column_widths.size() == columns) {
      column_widths = o.column_widths;
    } else if (o.stretch_columns) {
      // Preserve the natural per-column widths (so a longer phrase remains
      // readable) while using the full compact-strip width.  Distribute any
      // spare space evenly instead of replacing every column with the same
      // width; this keeps the grid three-column and stable without hiding
      // which column needs more room.
      const int gap_total = (columns - 1) * std::min<int>(cg, width / columns);
      const int target = std::max<int>(1, width - gap_total);
      int used = 0;
      for (const int cw : column_widths) used += cw;
      if (used < target) {
        const int extra = target - used;
        const int each = extra / static_cast<int>(columns);
        // Leave at most a couple of pixels of symmetric slack when the
        // remainder cannot be divided equally.  Giving the remainder to an
        // arbitrary column would make otherwise equal outer columns differ
        // by one pixel and makes the grid visibly asymmetrical.
        for (auto &cw : column_widths) cw += each;
      } else if (used > target) {
        // This path is uncommon because desired_width already bounds each
        // column, but retain deterministic fitting for very narrow popups.
        int over = used - target;
        for (auto &cw : column_widths) {
          const int reducible = std::max(0, cw - 1);
          const int cut = std::min(reducible, over);
          cw -= cut;
          over -= cut;
          if (over == 0) break;
        }
      }
    }
  }

  for (size_t i = out.begin; i < candidates.size() && out.cells.size() < o.maximum_candidates; ++i) {
    const int cw = columns ? column_widths[(i - out.begin) % columns]
                           : desired_width(candidates[i]);
    if (x > p && ((columns && column >= columns) || static_cast<int64_t>(x) + cw > right)) {
      if (!o.expanded || static_cast<int64_t>(y) + 2LL * rh + rg > bottom) break;
      x = p;
      // `rh + rg` can overflow a 32-bit int when toolkit metrics are
      // malformed (both values are independently clamped to the available
      // height).  The row-fit check above is already int64_t based, so keep
      // the coordinate update in that domain before converting back.
      y = static_cast<int>(static_cast<int64_t>(y) + rh + static_cast<int64_t>(rg));
      ++row;
      column = 0;
    }
    Cell cell;
    cell.candidate_index = i;
    cell.row = row;
    cell.column = column++;
    cell.rect = {x, y, cw, rh};
    const int prefix = std::min(cw, cp + lw);
    const int right_pad = std::min(cp, cw - prefix);
    cell.text_rect = {x + prefix, y, cw - prefix - right_pad, rh};
    cell.label = out.cells.size() >= 10 ? '\0' : (out.cells.size() == 9 ? '0' : static_cast<char>('1' + out.cells.size()));
    cell.elide = candidates[i].text_width > cell.text_rect.width;
    out.cells.push_back(cell);
    out.end = i + 1;
    used_width = std::max(used_width, x - p + cw);
    // A full-width final cell can leave x beyond the area. int64_t guards
    // the addition; the next candidate starts a new row if one is available.
    x = static_cast<int>(std::min<int64_t>(std::numeric_limits<int>::max(), static_cast<int64_t>(x) + cw + cg));
  }
  if (!out.cells.empty()) {
    out.rows = row + 1;
    // Keep the reported rectangle inside the caller-provided area even when
    // dimensions are close to INT_MAX.  The layout arithmetic above uses
    // int64_t for comparisons, but this final conversion used to overflow
    // and return a negative width/height under fuzzed or hostile metrics.
    const auto bounded = [](int64_t value, int limit) {
      return static_cast<int>(std::clamp<int64_t>(value, 0, limit));
    };
    out.bounds = {0, 0, bounded(static_cast<int64_t>(used_width) + 2LL * p,
                                o.available_width),
                  bounded(static_cast<int64_t>(y) + rh + p, o.available_height)};
  }
  return out;
}

Layout strip_layout(std::span<const Candidate> candidates, bool expanded,
                    int width, int height, size_t begin, int selected,
                    int row_height) {
  constexpr int control = 30;
  Options o;
  o.available_width = std::max(1, width - control);
  o.available_height = std::max(1, height);
  o.row_height = row_height;
  o.minimum_cell_width = 0;
  o.label_width = 12;
  o.cell_padding = 4;
  o.row_gap = 2;
  auto compact = layout(candidates, o, begin);
  if (!expanded || compact.cells.empty() || height < row_height + 12) {
    compact.expanded = expanded;
    compact.bounds.width = std::min(width, compact.bounds.width + control);
    return compact;
  }
  o.expanded = true;
  o.columns = compact.cells.size();
  o.maximum_candidates = 40;
  for (const auto &cell : compact.cells) o.column_widths.push_back(cell.rect.width);
  o.available_width = compact.bounds.width;
  // Build the complete page first. Very long phrases can leave just one
  // column; scroll this page by whole rows instead of hiding the selection.
  o.available_height = 16384;
  auto panel = layout(candidates, o, begin);
  const size_t visible_rows = std::max(1, (height - 2 * o.padding + o.row_gap) /
                                          (row_height + o.row_gap));
  size_t first_row = 0;
  for (const auto &cell : panel.cells)
    if (static_cast<int>(cell.candidate_index) == selected && cell.row >= visible_rows)
      first_row = cell.row - visible_rows + 1;
  const size_t group = selected >= 0 ? (static_cast<size_t>(selected) - std::min(begin, static_cast<size_t>(selected))) / 10 : 0;
  std::erase_if(panel.cells, [&](const Cell &c) {
    return c.row < first_row || c.row >= first_row + visible_rows;
  });
  for (auto &cell : panel.cells) {
    cell.row -= first_row;
    cell.rect.y -= first_row * (row_height + o.row_gap);
    cell.text_rect.y -= first_row * (row_height + o.row_gap);
    const size_t local = cell.candidate_index - begin;
    cell.label = local / 10 == group ? (local % 10 == 9 ? '0' : static_cast<char>('1' + local % 10)) : '\0';
  }
  if (!panel.cells.empty()) {
    panel.begin = panel.cells.front().candidate_index;
    panel.end = panel.cells.back().candidate_index + 1;
    panel.rows = panel.cells.back().row + 1;
    panel.bounds.height = std::min(height, static_cast<int>(2 * o.padding + panel.rows * row_height + (panel.rows - 1) * o.row_gap));
  }
  panel.bounds.width = compact.bounds.width + control;
  return panel;
}

size_t previous_page(std::span<const Candidate> candidates, const Options& options, size_t current_begin) {
  current_begin = std::min(current_begin, candidates.size());
  size_t begin = 0;
  while (begin < current_begin) {
    const auto page = layout(candidates, options, begin);
    if (page.end <= begin || page.end >= current_begin) return begin;
    begin = page.end;
  }
  return 0;
}

Navigation navigate(const Layout& l, size_t current_candidate, Direction direction) {
  if (l.cells.empty()) return {};
  auto it = std::find_if(l.cells.begin(), l.cells.end(), [&](const Cell& c) { return c.candidate_index == current_candidate; });
  if (it == l.cells.end()) return {Action::Select, l.cells.front().candidate_index};
  const auto select = [](const Cell& cell) { return Navigation{Action::Select, cell.candidate_index}; };
  if (direction == Direction::Home) return select(l.cells.front());
  if (direction == Direction::End) return select(l.cells.back());
  if (direction == Direction::Left) {
    if (it != l.cells.begin() && (it - 1)->row == it->row) return select(*(it - 1));
    if (it != l.cells.begin() && it->row > 0) return select(*(it - 1));
    return {l.has_previous() ? Action::PreviousPage : Action::EditPinyin, std::nullopt};
  }
  if (direction == Direction::Right) {
    if (it + 1 != l.cells.end() && (it + 1)->row == it->row) return select(*(it + 1));
    auto next = std::find_if(it + 1, l.cells.end(), [&](const Cell &cell) { return cell.row == it->row + 1; });
    if (next != l.cells.end()) return select(*next);
    return {l.has_next() ? Action::NextPage : Action::Stay, std::nullopt};
  }
  if (direction == Direction::Up && it->row == 0)
    return {l.has_previous() ? Action::PreviousPage : Action::Stay, std::nullopt};
  if (direction == Direction::Down && it->row + 1 == l.rows)
    return {l.has_next() ? Action::NextPage : Action::Stay, std::nullopt};
  const size_t target_row = direction == Direction::Up ? it->row - 1 : it->row + 1;
  if (l.aligned_columns) {
    const Cell *last = nullptr;
    for (const auto &cell : l.cells) {
      if (cell.row != target_row) continue;
      last = &cell;
      if (cell.column == it->column) return select(cell);
    }
    if (last) return select(*last);
  }
  const int64_t center = 2LL * it->rect.x + it->rect.width;
  const Cell* nearest = nullptr;
  int64_t distance = std::numeric_limits<int64_t>::max();
  for (const auto& cell : l.cells) {
    if (cell.row != target_row) continue;
    const int64_t other = 2LL * cell.rect.x + cell.rect.width;
    const int64_t d = center > other ? center - other : other - center;
    if (d < distance) { distance = d; nearest = &cell; }
  }
  return nearest ? select(*nearest) : Navigation{};
}

std::optional<size_t> hit_test(const Layout& l, int x, int y) {
  for (const auto& cell : l.cells) if (cell.rect.contains(x, y)) return cell.candidate_index;
  return std::nullopt;
}

std::optional<size_t> select_label(const Layout& l, char digit) {
  for (const auto& cell : l.cells) if (digit && cell.label == digit) return cell.candidate_index;
  return std::nullopt;
}
} // namespace tipe::grid
