#include "tipe/candidate_painter.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace tipe::painter {
namespace {

struct LayoutHandle {
  PangoLayout *value{};
  ~LayoutHandle() { if (value) g_object_unref(value); }
};

PangoFontDescription *font(int pixels) {
  auto *d = pango_font_description_new();
  pango_font_description_set_family(d, "Sans");
  pango_font_description_set_absolute_size(
      d, static_cast<double>(std::clamp(pixels, 8, 96)) * PANGO_SCALE);
  return d;
}

struct Measurer {
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(surface);
  LayoutHandle layout{pango_cairo_create_layout(cr)};
  explicit Measurer(int pixels) {
    auto *d = font(pixels);
    pango_layout_set_font_description(layout.value, d);
    pango_font_description_free(d);
    pango_layout_set_single_paragraph_mode(layout.value, true);
  }
  ~Measurer() { cairo_destroy(cr); cairo_surface_destroy(surface); }
  int width(const std::string &text) {
    pango_layout_set_text(layout.value, text.c_str(), static_cast<int>(text.size()));
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout.value, &w, &h);
    return std::max(1, w);
  }
};

void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = std::min({r, w / 2.0, h / 2.0});
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

void chevron(cairo_t *cr, double cx, double cy, double size, double red,
             double green, double blue, double alpha) {
  cairo_save(cr);
  cairo_set_source_rgba(cr, red, green, blue, alpha);
  cairo_set_line_width(cr, std::max(1.0, size / 5.0));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_move_to(cr, cx - size / 2.0, cy - size / 4.0);
  cairo_line_to(cr, cx, cy + size / 4.0);
  cairo_line_to(cr, cx + size / 2.0, cy - size / 4.0);
  cairo_stroke(cr);
  cairo_restore(cr);
}

void draw_text(cairo_t *cr, const std::string &text, const grid::Rect &rect,
               int pixels, bool ellipsize, double red, double green,
               double blue) {
  if (rect.width <= 0 || rect.height <= 0) return;
  LayoutHandle l{pango_cairo_create_layout(cr)};
  pango_layout_set_text(l.value, text.c_str(), static_cast<int>(text.size()));
  auto *d = font(pixels);
  pango_layout_set_font_description(l.value, d);
  pango_font_description_free(d);
  pango_layout_set_single_paragraph_mode(l.value, true);
  pango_layout_set_width(l.value, rect.width * PANGO_SCALE);
  pango_layout_set_height(l.value, rect.height * PANGO_SCALE);
  pango_layout_set_ellipsize(l.value,
      ellipsize ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
  int tw = 0, th = 0;
  pango_layout_get_pixel_size(l.value, &tw, &th);
  cairo_save(cr);
  cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
  cairo_clip(cr);
  cairo_set_source_rgb(cr, red, green, blue);
  cairo_move_to(cr, rect.x, rect.y + std::max(0, (rect.height - th) / 2));
  pango_cairo_show_layout(cr, l.value);
  cairo_restore(cr);
}

void shift_layout(grid::Layout &l, int dy) {
  l.bounds.height += dy;
  for (auto &c : l.cells) {
    c.rect.y += dy;
    c.text_rect.y += dy;
  }
}

Frame make_frame(std::vector<std::string> texts, std::string preedit,
                 bool expanded, int available_width, int available_height,
                 int selected, size_t begin, int font_pixels,
                 double scale_factor) {
  Frame frame;
  frame.candidates = std::move(texts);
  frame.preedit = std::move(preedit);
  frame.expanded = expanded;
  frame.selected = selected;
  frame.font_pixels = std::clamp(font_pixels, 8, 96);
  frame.scale_factor = std::isfinite(scale_factor) && scale_factor > 0.0
                           ? std::clamp(scale_factor, 0.5, 8.0) : 1.0;
  const int aw = std::clamp(available_width, 1, 16384);
  const int ah = std::clamp(available_height, 1, 16384);
  Measurer metrics(frame.font_pixels);
  std::vector<grid::Candidate> measured;
  measured.reserve(frame.candidates.size());
  for (const auto &text : frame.candidates)
    measured.push_back({text, metrics.width(text)});

  const int line = frame.font_pixels + 12;
  const int header = expanded && !frame.preedit.empty() && ah >= 2 * line ? line : 0;
  const int footer = expanded && !frame.candidates.empty() && ah >= header + line + 28
                         ? std::max(18, frame.font_pixels + 2) : 0;
  frame.layout = grid::strip_layout(measured, expanded, aw,
      std::max(1, ah - header - footer), begin, selected, line);
  frame.begin = frame.layout.begin;
  if (header) shift_layout(frame.layout, header);
  frame.width = std::min(aw, std::max(1, frame.layout.bounds.width));
  if (frame.candidates.empty() && !frame.preedit.empty())
    frame.width = std::min(aw, std::max(frame.width, metrics.width(frame.preedit) + 12));
  frame.height = std::min(ah, std::max(1, frame.layout.bounds.height));
  if (header) frame.preedit_rect = {6, 2,
      std::max(0, frame.width - 2 * 6), header - 2};
  if (footer && (frame.layout.has_previous() || frame.layout.has_next())) {
    frame.page_text = std::to_string(frame.layout.begin + 1) + "–" +
                      std::to_string(frame.layout.end) + " / " +
                      std::to_string(frame.layout.total);
    frame.footer_rect = {6, frame.height,
        std::max(0, frame.width - 2 * 6), footer};
    frame.height = std::min(ah, frame.height + footer);
  }
  // An empty candidate list still needs a stable preedit surface.
  if (frame.layout.cells.empty() && !frame.preedit.empty()) {
    frame.height = std::min(ah, line + 6);
    frame.preedit_rect = {std::min(6, aw / 4), 0,
        std::max(1, frame.width - 2 * std::min(6, aw / 4)), frame.height};
    frame.footer_rect = {};
    frame.page_text.clear();
  }
  return frame;
}

} // namespace

Frame measure(std::span<const grid::Candidate> candidates, std::string preedit,
              bool expanded, int available_width, int available_height,
              int selected, size_t begin, int font_pixels,
              double scale_factor) {
  std::vector<std::string> text;
  text.reserve(candidates.size());
  for (const auto &candidate : candidates) text.push_back(candidate.text);
  return make_frame(std::move(text), std::move(preedit), expanded,
                    available_width, available_height, selected, begin,
                    font_pixels, scale_factor);
}

Frame measure(std::span<const std::string> candidates, std::string preedit,
              bool expanded, int available_width, int available_height,
              int selected, size_t begin, int font_pixels,
              double scale_factor) {
  return make_frame({candidates.begin(), candidates.end()}, std::move(preedit),
                    expanded, available_width, available_height, selected,
                    begin, font_pixels, scale_factor);
}

void paint(cairo_t *cr, const Frame &frame) {
  if (!cr) return;
  cairo_save(cr);
  cairo_scale(cr, frame.scale_factor, frame.scale_factor);
  // Dark candidate capsule/panel with a hairline outline, light text and
  // a blue rounded selection. The same
  // renderer is used by GTK, Qt and classicui, so a toolkit switch cannot
  // change the visual language halfway through a composition.
  rounded(cr, 0.5, 0.5, std::max(0, frame.width - 1),
          std::max(0, frame.height - 1), frame.expanded ? 10 : frame.height / 2.0);
  cairo_set_source_rgba(cr, 26.0 / 255.0, 26.0 / 255.0, 28.0 / 255.0, 0.98);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.42, 0.43, 0.46, 0.88);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
  if (!frame.preedit.empty()) {
    draw_text(cr, frame.preedit, frame.preedit_rect, frame.font_pixels,
              true, 0.94, 0.95, 0.97);
  }
  for (const auto &cell : frame.layout.cells) {
    const bool selected = static_cast<int>(cell.candidate_index) == frame.selected;
    if (selected) {
      cairo_set_source_rgb(cr, 10.0 / 255.0, 132.0 / 255.0, 255.0 / 255.0);
      rounded(cr, cell.rect.x, cell.rect.y, cell.rect.width, cell.rect.height,
              frame.expanded ? 6 : cell.rect.height / 2.0);
      cairo_fill(cr);
    }
    grid::Rect label{cell.rect.x + 3, cell.rect.y, std::min(20, cell.text_rect.x - cell.rect.x - 3), cell.rect.height};
    draw_text(cr, cell.label ? std::string(1, cell.label) : std::string{}, label, frame.font_pixels - 3, false,
              selected ? 0.96 : 0.58, selected ? 0.98 : 0.60, selected ? 1.0 : 0.64);
    if (cell.candidate_index < frame.candidates.size())
      draw_text(cr, frame.candidates[cell.candidate_index], cell.text_rect,
                frame.font_pixels, cell.elide,
                selected ? 1.0 : 0.94, selected ? 1.0 : 0.95,
                selected ? 1.0 : 0.97);
  }
  if (!frame.expanded && frame.width >= 28) {
    const double divider_x = frame.width - 29.0;
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.68, 0.69, 0.72, 0.46);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, divider_x, 6.0);
    cairo_line_to(cr, divider_x, std::max(6.0, frame.height - 6.0));
    cairo_stroke(cr);
    cairo_restore(cr);
    chevron(cr, frame.width - 14.0, frame.height / 2.0, 9.0,
            0.70, 0.72, 0.76, 0.82);
  }
  if (!frame.page_text.empty())
    draw_text(cr, frame.page_text, frame.footer_rect,
              frame.font_pixels - 4, true, 0.68, 0.70, 0.74);
  cairo_restore(cr);
}

std::optional<size_t> hit_test(const Frame &frame, int x, int y) {
  return grid::hit_test(frame.layout,
                        static_cast<int>(std::floor(x / frame.scale_factor)),
                        static_cast<int>(std::floor(y / frame.scale_factor)));
}

std::optional<size_t> select_label(const Frame &frame, char label) {
  return grid::select_label(frame.layout, label);
}

} // namespace tipe::painter
