#include "tipe/client_side_candidate_adapter.hpp"

#include <algorithm>
#include <cmath>

namespace tipe::client_side {

tipe::painter::Frame adapt(const Update &update, const Options &options) {
  // Clamp a stale signal cursor so highlight/hit-test always refer to a
  // visible item. Fcitx's D-Bus cursor is page-local (not a global index).
  int selected = update.candidate_cursor;
  if (selected < 0 || update.candidates.empty()) {
    selected = -1;
  } else {
    selected = std::clamp(selected, 0,
                          static_cast<int>(update.candidates.size()) - 1);
  }

  // A vertical hint is Fcitx's request to expand; horizontal/unset remains
  // the compact one-line mode.  `has_previous/has_next` are intentionally not
  // used to synthesize candidates: pagination is handled by the frontend's
  // existing PageUp/PageDown requests through the same D-Bus context.
  const bool expanded = update.layout_hint == 1;
  const double scale = std::isfinite(update.scale_factor) &&
                               update.scale_factor > 0.0
                           ? update.scale_factor
                           : 1.0;
  return tipe::painter::measure(
      update.candidates, update.preedit, expanded, options.width, options.height,
      selected, 0, options.font_pixels, scale);
}

}  // namespace tipe::client_side
