#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace tipe {

bool isCompletePinyinSequence(std::string_view text);
std::optional<std::size_t> shortestPinyinSyllableCount(std::string_view text);
bool shouldRunFullPinyinDecoder(std::string_view text);

} // namespace tipe
