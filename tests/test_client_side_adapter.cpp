#include "tipe/client_side_candidate_adapter.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  using tipe::client_side::Update;
  Update update;
  update.preedit = "nihao";
  update.candidates = {"你好", "你号"};
  update.candidate_cursor = 99;  // stale after a late page update
  update.layout_hint = 1;
  update.scale_factor = NAN;
  auto frame = tipe::client_side::adapt(update);
  assert(frame.expanded);
  assert(frame.selected == 1);
  assert(frame.scale_factor == 1.0);
  assert(frame.candidates.size() == 2);

  update.candidates.assign(10, "你好");
  update.candidate_cursor = 9;
  frame = tipe::client_side::adapt(update);
  assert(frame.layout.cells.size() == 10);
  assert(frame.layout.rows == 1);
  assert(frame.layout.cells.back().column == 9);
  assert(tipe::painter::select_label(frame, '0') == 9);
  update.candidates[1] = "这是一段很长的候选文本";
  frame = tipe::client_side::adapt(update);
  assert(frame.layout.cells[1].rect.width > frame.layout.cells[0].rect.width);
  assert(frame.layout.cells[2].rect.width == frame.layout.cells[0].rect.width);
  size_t expanded_columns = 0;
  for (const auto &cell : frame.layout.cells)
    expanded_columns = std::max(expanded_columns, cell.column + 1);
  update.layout_hint = 2;
  frame = tipe::client_side::adapt(update);
  assert(frame.layout.rows >= 1 && !frame.layout.cells.empty());
  assert(frame.width <= 768);
  size_t compact_columns = 0;
  for (const auto &cell : frame.layout.cells)
    compact_columns = std::max(compact_columns, cell.column + 1);
  assert(compact_columns == expanded_columns);
  // Compact mode keeps natural per-candidate widths so a phrase longer than
  // three Chinese characters remains readable whenever the strip fits.
  assert(!frame.layout.cells[1].elide);
  const auto &last = frame.layout.cells.back();
  assert(tipe::painter::hit_test(frame, last.rect.x + 1, last.rect.y + 1) ==
         last.candidate_index);
  // Expanding must actually reveal further candidates while retaining every
  // first-row x coordinate, width and text area, including long phrases.
  for (const std::string phrase : {std::string("你好"), std::string("时代发生"), std::string("这是一段很长的候选文本")}) {
    update.preedit.clear();
    update.candidates.assign(40, phrase);
    update.candidate_cursor = 0;
    update.layout_hint = 2;
    auto compact = tipe::client_side::adapt(update);
    update.layout_hint = 1;
    auto panel = tipe::client_side::adapt(update);
    assert(panel.width == compact.width);
    assert(panel.layout.rows > 1);
    assert(panel.layout.cells.size() > compact.layout.cells.size());
    assert(!compact.layout.cells.front().elide);
    for (size_t i = 0; i < compact.layout.cells.size(); ++i) {
      const auto &a = compact.layout.cells[i];
      const auto &b = panel.layout.cells[i];
      assert(a.rect.x == b.rect.x && a.rect.width == b.rect.width);
      assert(a.text_rect.x == b.text_rect.x && a.text_rect.width == b.text_rect.width);
      assert(a.column == b.column && b.row == 0);
    }
    update.candidate_cursor = 39;
    panel = tipe::client_side::adapt(update);
    const auto selected = std::find_if(panel.layout.cells.begin(), panel.layout.cells.end(), [](const auto &c) { return c.candidate_index == 39; });
    assert(selected != panel.layout.cells.end());
    assert(tipe::painter::select_label(panel, '0') == 39);
    assert(tipe::painter::hit_test(panel, selected->rect.x + 1, selected->rect.y + 1) == 39);
    assert(selected->rect.y + selected->rect.height <= panel.height);
  }
  update.candidates.clear();
  update.candidate_cursor = 0;
  update.layout_hint = 0;
  frame = tipe::client_side::adapt(update);
  assert(!frame.expanded);
  assert(frame.selected == -1);
  assert(frame.candidates.empty());
  std::cout << "PASS client-side candidate adapter\n";
}
