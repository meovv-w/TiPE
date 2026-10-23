#include "tipe/candidate_grid.hpp"

#include <cassert>
#include <iostream>
#include <limits>

using namespace tipe::grid;

int main() {
  std::vector<Candidate> words(25, {"你好", 32});
  Options options;
  options.available_width = 250;
  options.available_height = 130;
  const auto compact = layout(words, options);
  assert(compact.rows == 1 && compact.cells.size() == 3);
  assert(compact.has_next() && !compact.has_previous());

  options.expanded = true;
  const auto expanded = layout(words, options);
  assert(expanded.rows == 3 && expanded.cells.size() == 9);
  assert(expanded.bounds.width <= 250 && expanded.bounds.height <= 130);
  assert(navigate(expanded, 0, Direction::Down).candidate_index == 3);
  assert(navigate(expanded, 3, Direction::Up).candidate_index == 0);
  assert(navigate(expanded, 3, Direction::Right).candidate_index == 4);
  assert(navigate(expanded, 4, Direction::Left).candidate_index == 3);
  assert(navigate(expanded, 0, Direction::Left).action == Action::EditPinyin);
  // Horizontal movement is row-major at a visual row edge.  Right from the
  // last cell of a row enters the first cell of the next row; Left reverses
  // that transition.  This is deliberately distinct from Up/Down's nearest
  // column rule and prevents a wrapped grid from jumping to an unrelated
  // column.
  assert(navigate(expanded, 2, Direction::Right).candidate_index == 3);
  assert(navigate(expanded, 3, Direction::Left).candidate_index == 2);
  assert(navigate(expanded, 5, Direction::Right).candidate_index == 6);
  assert(navigate(expanded, 6, Direction::Left).candidate_index == 5);
  // Reaching the first cell of a page moves to the preceding page before
  // entering pinyin editing.  Only the first page's first cell is the edit
  // boundary, so repeatedly pressing Left cannot prematurely close the grid.
  const auto second = layout(words, options, expanded.end);
  assert(navigate(second, second.begin, Direction::Left).action == Action::PreviousPage);
  assert(navigate(second, second.begin + 1, Direction::Left).action == Action::Select);
  assert(navigate(expanded, expanded.cells.back().candidate_index,
                  Direction::Right).action == Action::NextPage);
  assert(previous_page(words, options, second.begin) == expanded.begin);

  // Variable-width cells must select the nearest visual column, rather than
  // assuming a fixed candidate count per row. This mirrors the painter's
  // measured geometry and guards Up/Down against UTF-8 byte-width estimates.
  std::vector<Candidate> varied_rows{{"甲", 24}, {"特别宽的候选", 150},
                                     {"乙", 24}, {"丙", 24}, {"丁", 24},
                                     {"戊", 24}};
  Options measured = options;
  measured.available_width = 260;
  measured.available_height = 130;
  auto measured_layout = layout(varied_rows, measured);
  assert(measured_layout.rows >= 2);
  const auto from_first_row = navigate(measured_layout, 1, Direction::Down);
  // Candidate 1 is the wide cell in row 0. Its center is nearest to candidate
  // 4 in row 1, so Down must preserve visual alignment rather
  // than simply selecting row 1's first cell.
  assert(from_first_row.candidate_index == 4);
  assert(measured_layout.cells[*from_first_row.candidate_index].row == 1);
  assert(select_label(second, '1') == second.begin);

  options.available_width = 720;
  auto ten = layout(words, options);
  assert(ten.cells.size() == 10 && select_label(ten, '0') == 9);
  assert(!select_label(ten, 'x'));
  const auto tenth = ten.cells.back().rect;
  assert(hit_test(ten, tenth.x + 1, tenth.y + 1) == 9);
  assert(!hit_test(ten, -1, -1));

  words[1] = {"这是一条更宽的候选文本", 250};
  options.available_width = 360;
  auto varied = layout(words, options);
  assert(varied.cells[1].rect.width > varied.cells[0].rect.width);
  assert(varied.cells[2].rect.width == varied.cells[0].rect.width);
  assert(!varied.cells[1].elide);
  words[1].text_width = 5000;
  varied = layout(words, options);
  assert(varied.cells[1].elide);
  assert(varied.cells[1].rect.width <= options.available_width);

  // Area/label/hit invariants across tiny screens, tall screens and huge
  // requested text metrics. Empty or negative areas produce no cells.
  words[2].text_width = std::numeric_limits<int>::max();
  for (int width : {0, 1, 20, 99, 400, 1000}) {
    for (int height : {0, 1, 20, 130, 600}) {
      options.available_width = width;
      options.available_height = height;
      auto result = layout(words, options);
      assert(result.bounds.width <= width && result.bounds.height <= height);
      for (size_t i = 0; i < result.cells.size(); ++i) {
        const auto& c = result.cells[i];
        assert(c.rect.x >= 0 && c.rect.y >= 0);
        assert(c.rect.x + c.rect.width <= width);
        assert(c.rect.y + c.rect.height <= height);
        assert(c.text_rect.width >= 0);
        assert(select_label(result, c.label) == c.candidate_index);
        assert(hit_test(result, c.rect.x, c.rect.y) == c.candidate_index);
      }
    }
  }
  // Dimension values come from toolkit surfaces and should remain safe even
  // when a malformed caller supplies the largest representable integer.
  // Verify the final bounds conversion cannot wrap negative and that every
  // cell remains inside the advertised area.
  options.available_width = std::numeric_limits<int>::max();
  options.available_height = std::numeric_limits<int>::max();
  auto huge_area = layout(words, options);
  assert(huge_area.bounds.width >= 0 && huge_area.bounds.width <= options.available_width);
  assert(huge_area.bounds.height >= 0 && huge_area.bounds.height <= options.available_height);
  for (const auto &cell : huge_area.cells) {
    assert(cell.rect.x >= 0 && cell.rect.y >= 0);
    assert(static_cast<int64_t>(cell.rect.x) + cell.rect.width <= options.available_width);
    assert(static_cast<int64_t>(cell.rect.y) + cell.rect.height <= options.available_height);
  }
  std::cout << "candidate grid layout/navigation/hit tests passed\n";
}
