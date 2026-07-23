#pragma once

#include <cstdint>

namespace tipe {

inline constexpr std::uint64_t passwordInputCapability = 1ULL << 3;
inline constexpr std::uint64_t sensitiveInputCapability = 1ULL << 36;
inline constexpr std::uint64_t disabledInputMethodCapability = 1ULL << 40;

constexpr bool inputCapabilitiesBlockSupervision(std::uint64_t capabilities) {
    return (capabilities &
            (passwordInputCapability | sensitiveInputCapability | disabledInputMethodCapability)) != 0;
}

} // namespace tipe
