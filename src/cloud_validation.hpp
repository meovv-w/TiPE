#pragma once
#include <algorithm>
#include <string_view>

namespace tipe::cloud_detail {
// Strict Unicode scalar decoding: no overlong encodings, surrogates,
// truncation, C0/C1 controls, line separators, or bidi override controls.
inline bool valid_text(std::string_view value, size_t limit, bool allow_empty = false) {
  if (value.size() > limit || (!allow_empty && value.empty())) return false;
  for (size_t i = 0; i < value.size();) {
    const auto lead = static_cast<unsigned char>(value[i++]);
    uint32_t cp{}; unsigned trailing{};
    if (lead < 0x80) cp = lead;
    else if (lead >= 0xc2 && lead <= 0xdf) { cp = lead & 0x1f; trailing = 1; }
    else if (lead >= 0xe0 && lead <= 0xef) { cp = lead & 0x0f; trailing = 2; }
    else if (lead >= 0xf0 && lead <= 0xf4) { cp = lead & 7; trailing = 3; }
    else return false;
    if (i + trailing > value.size()) return false;
    for (unsigned j = 0; j < trailing; ++j) {
      const auto byte = static_cast<unsigned char>(value[i++]);
      if ((byte & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (byte & 0x3f);
    }
    if ((trailing == 1 && cp < 0x80) || (trailing == 2 && cp < 0x800) ||
        (trailing == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 ||
        (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029 ||
        (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) return false;
  }
  return true;
}
inline bool valid_context_token(std::string_view token) {
  return !token.empty() && token.size() <= 64 &&
         std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
inline bool valid_pinyin(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || c == '\''; }) &&
         value.find_first_not_of('\'') != value.npos;
}
} // namespace tipe::cloud_detail
