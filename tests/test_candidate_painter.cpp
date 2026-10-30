#include "tipe/candidate_painter.hpp"
#include <cassert>
#include <algorithm>
#include <cairo/cairo.h>
#include <filesystem>
#include <iostream>

int main() {
  using tipe::grid::Candidate;
  using tipe::painter::measure;
  std::vector<Candidate> candidates = {
      {"你", 0}, {"你好", 0}, {"这是一个很长的候选文本", 0}, {"测试", 0},
      {"输入法", 0}, {"候选", 0}, {"窗口", 0}, {"布局", 0}, {"中文", 0}, {"拼音", 0},
      {"第十一项", 0}};
  auto compact = measure(candidates, "nihao", false, 480, 100, 0, 0, 16, 1.0);
  assert(!compact.layout.cells.empty());
  assert(compact.layout.rows == 1);
  assert(compact.width > 0 && compact.height > 0);
  assert(compact.preedit_rect.height == 0 && compact.footer_rect.height == 0);
  assert(compact.page_text.empty());
  assert(compact.height <= 16 + 12 + 16);
  // A compact row can fit fewer than its ten numbered candidates. Arrows
  // must scroll it to the selected item without creating additional rows
  // or renumbering that item to an unrelated selection key.
  for (int selected = 0; selected < 10; ++selected) {
    auto moved = measure(candidates, "nihao", false, 160, 100, selected);
    assert(!moved.expanded && moved.layout.rows == 1);
    const auto active = std::find_if(moved.layout.cells.begin(), moved.layout.cells.end(),
        [selected](const auto &cell) { return cell.candidate_index == static_cast<size_t>(selected); });
    assert(active != moved.layout.cells.end());
    const char digit = selected == 9 ? '0' : '1' + selected;
    assert(tipe::painter::select_label(moved, digit) == static_cast<size_t>(selected));
    assert(tipe::painter::hit_test(moved, active->rect.x + 1, active->rect.y + 1) == static_cast<size_t>(selected));
    assert(moved.height == compact.height);
  }
  auto expanded = measure(candidates, "nihao", true, 480, 220, 2, 0, 18, 2.0);
  assert(expanded.layout.cells.size() >= 2);
  assert(expanded.layout.rows >= 2);
  assert(expanded.width <= 480 && expanded.height <= 220);
  const auto &cell = expanded.layout.cells.front();
  assert(tipe::painter::hit_test(expanded, static_cast<int>((cell.rect.x + 1) * expanded.scale_factor),
                                 static_cast<int>((cell.rect.y + 1) * expanded.scale_factor)) == cell.candidate_index);
  assert(tipe::painter::select_label(expanded, '1') == expanded.layout.cells.front().candidate_index);

  std::vector<std::string> empty;
  const auto preedit_only = measure(empty, "suspendedpinyin", false, 480, 100);
  assert(preedit_only.width > 100 && preedit_only.preedit_rect.height > 0);

  const int scale = 2;
  auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                              expanded.width * scale, expanded.height * scale);
  auto *cr = cairo_create(surface);
  tipe::painter::paint(cr, expanded);
  cairo_surface_flush(surface);
  assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
  // The unselected panel is dark; the active cell uses the requested blue.
  const auto *pixels = reinterpret_cast<const unsigned *>(cairo_image_surface_get_data(surface));
  const int stride = cairo_image_surface_get_stride(surface) / 4;
  const unsigned background = pixels[expanded.height * stride + 4 * scale];
  assert(((background >> 16) & 255) < 45 && ((background >> 8) & 255) < 45 && (background & 255) < 45);
  const auto out = std::filesystem::path("/tmp/tipe-candidate-painter.png");
  assert(cairo_surface_write_to_png(surface, out.c_str()) == CAIRO_STATUS_SUCCESS);
  assert(std::filesystem::file_size(out) > 100);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  std::cout << "candidate painter measure/paint/hit tests passed: " << out << "\n";
}
