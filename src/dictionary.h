#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>

namespace tipe {

class Dictionary {
public:
    Dictionary();
    ~Dictionary();
    Dictionary(Dictionary &&) noexcept;
    Dictionary &operator=(Dictionary &&) noexcept;
    Dictionary(const Dictionary &) = delete;
    Dictionary &operator=(const Dictionary &) = delete;

    std::vector<std::string> lookup(std::string_view pinyin) const;
    std::vector<std::string> exactUserCandidates(std::string_view pinyin) const;
    std::vector<std::string> exactKnownCandidates(std::string_view pinyin) const;
    bool hasExactKnownPinyin(std::string_view pinyin) const;
    bool hasConfidentLanguageModelSentence(std::string_view pinyin) const;
    bool hasDecisiveLanguageModelSentence(std::string_view pinyin) const;
    std::optional<double> languageModelNormalizedScore(std::string_view pinyin) const;
    int learnedCorrectionPriority(std::string_view pinyin) const;
    int exactPinyinPriority(std::string_view pinyin) const;
    bool learnLanguageModelSelection(std::string_view pinyin, std::string_view candidate);
    bool learnUserEntry(std::string_view pinyin, std::string_view candidate);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tipe
