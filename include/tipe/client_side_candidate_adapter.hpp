#pragma once

#include "tipe/candidate_painter.hpp"

#include <string>
#include <vector>

namespace tipe::client_side {

// This is the wire shape used by Fcitx's D-Bus UpdateClientSideUI signal.
// GTK/Qt frontends receive only the visible page, so `candidate_cursor` is a
// page-local index and hasPrev/hasNext are the only pagination evidence.
struct Update {
  std::string preedit;
  std::vector<std::string> candidates;
  int candidate_cursor{-1};
  int layout_hint{};  // FcitxCandidateLayoutHint: 0=unset, 1=vertical
  bool has_previous{};
  bool has_next{};
  double scale_factor{1.0};
};

struct Options {
  // A 768px logical strip keeps ordinary three-character phrases readable
  // at 2x display scale while remaining well inside a desktop work area.
  int width{768};
  int height{320};
  int font_pixels{16};
};

// Converts one client-side update into the shared TiPE grid frame.  The
// adapter intentionally does not create a Wayland connection or an input
// method: the owning GTK/Qt frontend remains responsible for embedding the
// resulting frame in its parent-native popup.  Empty/invalid pages produce a
// hidden frame, and a stale cursor is clamped so a late D-Bus signal cannot
// highlight a nonexistent cell.
tipe::painter::Frame adapt(const Update &, const Options & = {});

}  // namespace tipe::client_side
