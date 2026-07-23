#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace tipe {

inline constexpr std::array<std::string_view, 43> knownEnglishTokens{
    "typescript", "flatpak", "github", "docker", "cursor", "openai", "python", "vscode",
    "wayland",    "cargo",   "cmake",  "codex",  "fcitx",  "linux",  "react",  "bash",
    "javascript", "cargobuild", "cmakebuild", "hyprland", "chatgpt", "ollama", "waybar", "systemd",
    "gnome",      "dbus",       "build",      "json",     "node",    "niri",   "npm",    "rust",
    "vue",        "api",        "gtk",        "gpt4",      "qwen2",   "qwen3",  "ipv4",   "ipv6",
    "git",        "gpt",        "tipe",
};

inline bool isKnownEnglishToken(std::string_view text) {
    return std::find(knownEnglishTokens.begin(), knownEnglishTokens.end(), text) != knownEnglishTokens.end();
}

inline bool isKnownEnglishTokenPrefix(std::string_view text) {
    return std::any_of(knownEnglishTokens.begin(), knownEnglishTokens.end(), [text](const auto token) {
        return token.starts_with(text);
    });
}

inline std::string asciiLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const unsigned char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

inline bool looksLikeEnglishIdentifier(std::string_view preedit) {
    if (preedit.size() < 4) {
        return false;
    }
    const auto lowered = asciiLower(preedit);
    if (isKnownEnglishToken(lowered)) {
        return true;
    }
    if (!std::all_of(preedit.begin(), preedit.end(), [](unsigned char ch) { return std::isalpha(ch); })) {
        return false;
    }
    for (std::size_t index = 0; index < lowered.size(); ++index) {
        const char ch = lowered[index];
        const char next = index + 1 < lowered.size() ? lowered[index + 1] : '\0';
        const char previous = index > 0 ? lowered[index - 1] : '\0';
        if (ch == 'v' && previous != 'l' && previous != 'n') {
            return true;
        }
        if (ch == 'x' && next != 'i' && next != 'u') {
            return true;
        }
        if (ch == 'q' && next != 'i' && next != 'u') {
            return true;
        }
    }
    static constexpr std::string_view markers[] = {
        "ck", "cl", "cr", "ct", "dr", "ea", "ee", "fl", "ft", "gr", "ld", "ll", "lt", "mp", "nd",
        "nt", "oo", "ph", "pl", "pr", "pt", "rb", "rd", "rk", "rn", "rs", "rt", "sk", "sl", "sm",
        "sn", "sp", "ss", "st", "sv", "sw", "th", "tr", "ts", "tw", "xt",
    };
    if (std::any_of(std::begin(markers), std::end(markers), [&lowered](const auto marker) {
        return lowered.find(marker) != std::string::npos;
    })) {
        return true;
    }

    const bool hasAsciiVowel = lowered.find_first_of("aeiouy") != std::string::npos;
    const bool impossiblePinyinEnding =
        std::string_view("bdfjklmpqtvxz").find(lowered.back()) != std::string_view::npos;
    return hasAsciiVowel && impossiblePinyinEnding;
}

} // namespace tipe
