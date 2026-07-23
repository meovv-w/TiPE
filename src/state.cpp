#include "state.h"
#include "candidate_layout.h"
#include "english_tokens.h"
#include "pinyin_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tipe {

namespace {

std::size_t utf8CodepointCount(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xC0) != 0x80;
    }));
}

bool containsAsciiLetter(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isalpha(ch) && ch < 0x80;
    });
}

std::string longestAsciiLetterRun(std::string_view text) {
    std::string best;
    std::string current;
    for (const unsigned char ch : text) {
        if (std::isalpha(ch) && ch < 0x80) {
            current.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }
        if (current.size() > best.size()) {
            best = current;
        }
        current.clear();
    }
    if (current.size() > best.size()) {
        best = current;
    }
    return best;
}

bool prefixCoversMixedEnglishCandidate(std::string_view prefixPinyin, const std::string &candidate,
                                       const std::vector<std::string> &fullCandidates) {
    if (!containsAsciiLetter(candidate)) {
        return false;
    }
    const auto asciiRun = longestAsciiLetterRun(candidate);
    if (asciiRun.size() < 2 || prefixPinyin.find(asciiRun) == std::string_view::npos) {
        return false;
    }
    return !fullCandidates.empty() && fullCandidates.front() != candidate &&
           fullCandidates.front().size() > candidate.size() &&
           std::string_view(fullCandidates.front()).starts_with(candidate);
}

std::optional<std::pair<std::size_t, std::size_t>> mixedEnglishPrefixScanRange(std::string_view preedit,
                                                                               std::string_view asciiRun) {
    if (asciiRun.size() < 2) {
        return std::nullopt;
    }
    const auto tokenStart = preedit.find(asciiRun);
    if (tokenStart == std::string_view::npos) {
        return std::nullopt;
    }
    const auto tokenEnd = tokenStart + asciiRun.size();
    const auto scanEnd = std::min(preedit.size(), tokenEnd + std::size_t{12});
    if (tokenEnd >= preedit.size()) {
        return std::nullopt;
    }
    return std::pair<std::size_t, std::size_t>{tokenEnd, scanEnd};
}

constexpr std::string_view pinyinSyllables[] = {
    "chuang", "shuang", "zhuang", "chang", "cheng", "chong", "chuan", "chuai", "jiang", "liang",
    "niang",  "qiang",  "shang",  "sheng", "shuan",  "shuai", "xiang", "xiong", "zhang", "zheng",
    "zhong",  "zhuan",  "zhuai",  "ang",   "bai",    "ban",   "bang",  "bao",   "bei",   "ben",
    "beng",   "bian",   "biao",   "bie",   "bin",    "bing",  "cai",   "can",   "cang",  "cao",
    "cei",    "cen",    "ceng",   "chai",  "chan",   "chang", "chao",  "che",   "chen",  "cheng",
    "chi",    "chong",  "chou",   "chu",   "chua",   "chuai", "chuan", "chuang", "chui",  "chun",
    "chuo",   "ci",     "cong",   "cou",   "cu",     "cuan",  "cui",   "cun",   "cuo",   "dai",
    "dan",    "dang",   "dao",    "dei",   "den",    "deng",  "di",    "dian",  "diao",  "die",
    "ding",   "diu",    "dong",   "dou",   "du",     "duan",  "dui",   "dun",   "duo",   "eng",
    "er",     "fan",    "fang",   "fei",   "fen",    "feng",  "fo",    "fou",   "fu",    "gai",
    "gan",    "gang",   "gao",    "ge",    "gei",    "gen",   "geng",  "gong",  "gou",   "gu",
    "gua",    "guai",   "guan",   "guang", "gui",    "gun",   "guo",   "hai",   "han",   "hang",
    "hao",    "hei",    "hen",    "heng",  "hong",   "hou",   "hu",    "hua",   "huai",  "huan",
    "huang",  "hui",    "hun",    "huo",   "ji",     "jia",   "jian",  "jiang", "jiao",  "jie",
    "jin",    "jing",   "jiong",  "jiu",   "ju",     "juan",  "jue",   "jun",   "kai",   "kan",
    "kang",   "kao",    "ke",     "kei",   "ken",    "keng",  "kong",  "kou",   "ku",    "kua",
    "kuai",   "kuan",   "kuang",  "kui",   "kun",    "kuo",   "lai",   "lan",   "lang",  "lao",
    "lei",    "leng",   "li",     "lia",   "lian",   "liang", "liao",  "lie",   "lin",   "ling",
    "liu",    "lo",     "long",   "lou",   "lu",     "luan",  "lue",   "lun",   "luo",   "lv",
    "lve",    "mai",    "man",    "mang",  "mao",    "mei",   "men",   "meng",  "mi",    "mian",
    "miao",   "mie",    "min",    "ming",  "miu",    "mo",    "mou",   "mu",    "nai",   "nan",
    "nang",   "nao",    "nei",    "nen",   "neng",   "ni",    "nian",  "niang", "niao",  "nie",
    "nin",    "ning",   "niu",    "nong",  "nou",    "nu",    "nuan",  "nue",   "nuo",   "nv",
    "nve",    "pai",    "pan",    "pang",  "pao",    "pei",   "pen",   "peng",  "pi",    "pian",
    "piao",   "pie",    "pin",    "ping",  "po",     "pou",   "pu",    "qi",    "qia",   "qian",
    "qiang",  "qiao",   "qie",    "qin",   "qing",   "qiong", "qiu",   "qu",    "quan",  "que",
    "qun",    "ran",    "rang",   "rao",   "re",     "ren",   "reng",  "ri",    "rong",  "rou",
    "ru",     "ruan",   "rui",    "run",   "ruo",    "sai",   "san",   "sang",  "sao",   "sei",
    "sen",    "seng",   "sha",    "shai",  "shan",   "shang", "shao",  "she",   "shen",  "sheng",
    "shi",    "shou",   "shu",    "shua",  "shuai",  "shuan", "shuang", "shui", "shun",  "shuo",
    "si",     "song",   "sou",    "su",    "suan",   "sui",   "sun",   "suo",   "tai",   "tan",
    "tang",   "tao",    "tei",    "teng",  "ti",     "tian",  "tiao",  "tie",   "ting",  "tong",
    "tou",    "tu",     "tuan",   "tui",   "tun",    "tuo",   "wai",   "wan",   "wang",  "wei",
    "wen",    "weng",   "wo",     "wu",    "xi",     "xia",   "xian",  "xiang", "xiao",  "xie",
    "xin",    "xing",   "xiong",  "xiu",   "xu",     "xuan",  "xue",   "xun",   "yan",   "yang",
    "yao",    "ye",     "yi",     "yin",   "ying",   "yo",    "yong",  "you",   "yu",    "yuan",
    "yue",    "yun",    "zai",    "zan",   "zang",   "zao",   "zei",   "zen",   "zeng",  "zha",
    "zhai",   "zhan",   "zhang",  "zhao",  "zhe",    "zhei",  "zhen",  "zheng", "zhi",   "zhong",
    "zhou",   "zhu",    "zhua",   "zhuai", "zhuan",  "zhuang", "zhui", "zhun",  "zhuo",  "zi",
    "zong",   "zou",    "zu",     "zuan",  "zui",    "zun",   "zuo",   "ai",    "an",    "ao",
    "ba",     "bo",     "bi",     "bu",    "ca",     "ce",    "ch",    "cu",    "da",    "de",
    "du",     "ei",     "en",     "fa",    "ga",     "ha",    "he",    "ka",    "la",    "le",
    "lo",     "ma",     "me",     "mi",    "na",     "ne",    "ng",    "ou",    "pa",    "po",
    "sa",     "se",     "sh",     "ta",    "te",     "wa",    "ya",    "yo",    "za",    "ze",
    "zh",     "a",      "e",      "m",     "n",      "o",
};

const std::vector<std::string_view> &pinyinUnitsStartingWith(std::string_view text) {
    using Buckets = std::array<std::vector<std::string_view>, 26>;
    static const Buckets buckets = [] {
        Buckets result;
        const auto append = [&result](std::string_view unit) {
            if (unit.empty() || unit.front() < 'a' || unit.front() > 'z') {
                return;
            }
            auto &bucket = result[static_cast<std::size_t>(unit.front() - 'a')];
            if (std::find(bucket.begin(), bucket.end(), unit) == bucket.end()) {
                bucket.push_back(unit);
            }
        };
        for (const auto syllable : pinyinSyllables) {
            append(syllable);
        }
        for (const auto token : knownEnglishTokens) {
            append(token);
        }
        for (auto &bucket : result) {
            std::sort(bucket.begin(), bucket.end(), [](std::string_view lhs, std::string_view rhs) {
                if (lhs.size() != rhs.size()) {
                    return lhs.size() > rhs.size();
                }
                return lhs < rhs;
            });
        }
        return result;
    }();
    static const std::vector<std::string_view> empty;
    if (text.empty() || text.front() < 'a' || text.front() > 'z') {
        return empty;
    }
    return buckets[static_cast<std::size_t>(text.front() - 'a')];
}

std::optional<std::size_t> nextPinyinSyllableLength(std::string_view text, bool allowFullMatch) {
    std::size_t bestLength = 0;
    for (const auto unit : pinyinUnitsStartingWith(text)) {
        if (text.starts_with(unit) && (allowFullMatch || text.size() > unit.size())) {
            bestLength = std::max(bestLength, unit.size());
        }
    }
    if (bestLength == 0) {
        return std::nullopt;
    }
    return bestLength;
}

std::vector<std::size_t> pinyinSyllableLengthOptions(std::string_view text, bool allowFullMatch) {
    std::vector<std::size_t> lengths;
    for (const auto unit : pinyinUnitsStartingWith(text)) {
        if (text.starts_with(unit) && (allowFullMatch || text.size() > unit.size())) {
            lengths.push_back(unit.size());
        }
    }
    std::sort(lengths.begin(), lengths.end());
    lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
    return lengths;
}

std::optional<std::size_t> pinyinSyllableCount(std::string_view text);

constexpr std::size_t maxAmbiguousSplitLength = 96;
constexpr std::size_t maxKnownPrefixScanLength = 48;
constexpr std::size_t maxAutomaticApproximatePrefixLength = 32;
struct PrefixCandidateGroup {
    std::size_t rank = 0;
    std::size_t prefixLength = 0;
    std::vector<std::string> candidates;
};

struct PrefixCandidateEntry {
    std::string text;
    std::size_t prefixLength = 0;
};

std::string splitMemoKey(std::size_t offset, std::size_t depth) {
    return std::to_string(offset) + ":" + std::to_string(depth);
}

void collectPinyinPrefixLengthsForSyllables(std::string_view text, std::size_t syllableCount, std::size_t offset,
                                            std::size_t depth, std::vector<std::size_t> &lengths,
                                            std::unordered_set<std::string> &visited) {
    if (depth == syllableCount) {
        if (offset < text.size() && pinyinSyllableCount(text.substr(offset))) {
            lengths.push_back(offset);
        }
        return;
    }
    if (offset >= text.size()) {
        return;
    }

    const auto key = splitMemoKey(offset, depth);
    if (!visited.insert(key).second) {
        return;
    }

    for (const auto length : pinyinSyllableLengthOptions(text.substr(offset), true)) {
        collectPinyinPrefixLengthsForSyllables(text, syllableCount, offset + length, depth + 1, lengths, visited);
    }
}

std::vector<std::size_t> pinyinPrefixLengthOptionsForSyllables(std::string_view text, std::size_t syllableCount) {
    std::vector<std::size_t> lengths;
    std::unordered_set<std::string> visited;
    collectPinyinPrefixLengthsForSyllables(text, syllableCount, 0, 0, lengths, visited);
    std::sort(lengths.begin(), lengths.end());
    lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
    return lengths;
}

bool canSplitIntoMoreThan(std::string_view text, std::size_t minimumSyllables, std::size_t offset = 0,
                          std::size_t count = 0, std::unordered_set<std::string> *visited = nullptr) {
    if (offset == text.size()) {
        return count > minimumSyllables;
    }
    if (offset > text.size()) {
        return false;
    }
    std::unordered_set<std::string> localVisited;
    if (!visited) {
        visited = &localVisited;
    }
    const auto key = splitMemoKey(offset, count);
    if (!visited->insert(key).second) {
        return false;
    }
    for (const auto length : pinyinSyllableLengthOptions(text.substr(offset), true)) {
        if (canSplitIntoMoreThan(text, minimumSyllables, offset + length, count + 1, visited)) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> pinyinSyllableCount(std::string_view text) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < text.size()) {
        const auto length = nextPinyinSyllableLength(text.substr(offset), true);
        if (!length) {
            return std::nullopt;
        }
        offset += *length;
        ++count;
    }
    return count;
}

std::optional<std::size_t> shortestPinyinSyllableCountImpl(
    std::string_view text, std::size_t offset = 0,
    std::unordered_map<std::size_t, std::optional<std::size_t>> *memo = nullptr) {
    std::unordered_map<std::size_t, std::optional<std::size_t>> localMemo;
    if (!memo) {
        memo = &localMemo;
    }
    if (offset == text.size()) {
        return 0;
    }
    if (const auto iter = memo->find(offset); iter != memo->end()) {
        return iter->second;
    }

    std::optional<std::size_t> best;
    for (const auto length : pinyinSyllableLengthOptions(text.substr(offset), true)) {
        if (offset + length > text.size()) {
            continue;
        }
        if (const auto rest = shortestPinyinSyllableCountImpl(text, offset + length, memo)) {
            const auto count = *rest + 1;
            if (!best || count < *best) {
                best = count;
            }
        }
    }
    (*memo)[offset] = best;
    return best;
}

int correctionTransferPriority(const Dictionary &dictionary, std::string_view original,
                               std::string_view corrected) {
    if (const auto originalSyllables = shortestPinyinSyllableCount(original)) {
        const auto correctedSyllables = shortestPinyinSyllableCount(corrected);
        if (!correctedSyllables || *correctedSyllables != *originalSyllables) {
            return 0;
        }
    }
    return dictionary.learnedCorrectionPriority(corrected);
}

bool canCompletePinyinTail(std::string_view text, std::size_t offset = 0,
                           std::unordered_map<std::size_t, bool> *memo = nullptr) {
    std::unordered_map<std::size_t, bool> localMemo;
    if (!memo) {
        memo = &localMemo;
    }
    if (offset >= text.size()) {
        return true;
    }
    if (const auto iter = memo->find(offset); iter != memo->end()) {
        return iter->second;
    }

    const auto remaining = text.substr(offset);
    const auto matchesTail = [remaining](std::string_view unit) {
        return unit.size() >= remaining.size() && unit.starts_with(remaining);
    };
    const auto &units = pinyinUnitsStartingWith(remaining);
    if (std::any_of(units.begin(), units.end(), matchesTail)) {
        (*memo)[offset] = true;
        return true;
    }

    for (const auto length : pinyinSyllableLengthOptions(remaining, true)) {
        if (length >= remaining.size()) {
            continue;
        }
        if (canCompletePinyinTail(text, offset + length, memo)) {
            (*memo)[offset] = true;
            return true;
        }
    }
    (*memo)[offset] = false;
    return false;
}

bool isSinglePinyinSyllableOrPrefix(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    if (const auto count = shortestPinyinSyllableCount(text); count && *count == 1) {
        return true;
    }
    const auto &units = pinyinUnitsStartingWith(text);
    return std::any_of(units.begin(), units.end(), [text](std::string_view unit) {
        return unit.size() > text.size() && unit.starts_with(text);
    });
}

std::vector<std::size_t> pinyinPrefixLengths(std::string_view text, std::size_t limit) {
    std::vector<std::size_t> lengths;
    std::size_t offset = 0;
    while (offset < text.size() && lengths.size() < limit) {
        const auto length = nextPinyinSyllableLength(text.substr(offset), true);
        if (!length) {
            return lengths;
        }
        offset += *length;
        if (offset < text.size()) {
            lengths.push_back(offset);
        }
    }
    return lengths;
}

struct LocalCorrectionVariant {
    std::string preedit;
    int priority = 0;
};

std::vector<LocalCorrectionVariant> localCorrectionPreeditVariants(std::string_view preedit) {
    std::vector<LocalCorrectionVariant> variants;
    auto addVariant = [&](std::string variant, int priority) {
        const auto syllableCount = pinyinSyllableCount(variant);
        if (variant != preedit && syllableCount &&
            std::find_if(variants.begin(), variants.end(), [&variant](const auto &existing) {
                return existing.preedit == variant;
            }) == variants.end()) {
            variants.push_back({std::move(variant), priority * 10 + static_cast<int>(*syllableCount)});
        }
    };

    if (preedit.size() >= 3 && preedit.size() <= 16) {
        if (preedit.size() > 4 && preedit.ends_with("ng")) {
            addVariant(std::string(preedit.substr(0, preedit.size() - 2)), -3);
        }
        const auto maxTailLength = std::min<std::size_t>(6, preedit.size());
        for (std::size_t tailLength = 2; tailLength <= maxTailLength; ++tailLength) {
            const auto tail = preedit.substr(preedit.size() - tailLength);
            for (const auto syllable : pinyinSyllables) {
                if (tail.size() >= 2 && syllable.size() > tail.size() && std::string_view(syllable).starts_with(tail)) {
                    std::string variant(preedit.substr(0, preedit.size() - tailLength));
                    variant.append(syllable);
                    addVariant(std::move(variant), -static_cast<int>(syllable.size() - tail.size()));
                }
            }
        }
        for (std::size_t offset = 0; offset < preedit.size(); ++offset) {
            std::string variant(preedit);
            variant.erase(variant.begin() + static_cast<std::ptrdiff_t>(offset));
            addVariant(std::move(variant), 0);
            if (variants.size() >= 64) {
                return variants;
            }
        }
        constexpr std::string_view insertionChars = "abcdefghijklmnopqrstuvwxyz";
        for (std::size_t offset = preedit.size() + 1; offset-- > 0;) {
            for (const char ch : insertionChars) {
                std::string variant(preedit);
                variant.insert(variant.begin() + static_cast<std::ptrdiff_t>(offset), ch);
                addVariant(std::move(variant), 1);
                if (variants.size() >= 96) {
                    return variants;
                }
            }
        }
        for (std::size_t offset = 1; offset < preedit.size(); ++offset) {
            std::string variant(preedit);
            std::swap(variant[offset - 1], variant[offset]);
            addVariant(std::move(variant), 2);
            if (variants.size() >= 128) {
                return variants;
            }
        }
    }
    return variants;
}

bool candidateLooksLikeNoisySegmentation(std::string_view preedit, const std::vector<std::string> &candidates) {
    if (candidates.empty()) {
        return true;
    }
    const auto leadingCodepoints = utf8CodepointCount(candidates.front());
    if (leadingCodepoints <= 1) {
        return true;
    }
    if (leadingCodepoints == 2) {
        return false;
    }
    if (preedit.size() >= 9 && !pinyinSyllableCount(preedit)) {
        return true;
    }
    if (leadingCodepoints >= 4) {
        return false;
    }
    return std::all_of(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(
                           std::min<std::size_t>(candidates.size(), 3)),
                       [](const std::string &candidate) {
                           return utf8CodepointCount(candidate) <= 3;
                       });
}

bool candidateLooksLikePhoneticFallback(const std::vector<std::string> &candidates) {
    if (candidates.empty()) {
        return true;
    }
    const auto &candidate = candidates.front();
    return candidate.find("鞥") != std::string::npos || candidate.find("ㄥ") != std::string::npos;
}

bool isAdjacentTransposeCorrection(std::string_view typo, std::string_view corrected) {
    if (typo.size() != corrected.size() || typo.size() < 2) {
        return false;
    }
    std::array<std::size_t, 2> changed{};
    std::size_t changedCount = 0;
    for (std::size_t index = 0; index < typo.size(); ++index) {
        if (typo[index] == corrected[index]) {
            continue;
        }
        if (changedCount >= changed.size()) {
            return false;
        }
        changed[changedCount++] = index;
    }
    return changedCount == 2 && changed[1] == changed[0] + 1 &&
           typo[changed[0]] == corrected[changed[1]] && typo[changed[1]] == corrected[changed[0]];
}

std::vector<std::string> exactLocalCorrectionCandidates(std::string_view preedit, const Dictionary &dictionary,
                                                        const std::vector<std::string> &currentCandidates) {
    if (preedit.size() < 5 || preedit.size() > 16 ||
        !candidateLooksLikeNoisySegmentation(preedit, currentCandidates)) {
        return {};
    }
    auto variants = localCorrectionPreeditVariants(preedit);
    std::stable_sort(variants.begin(), variants.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.priority < rhs.priority;
    });

    std::vector<std::string> result;
    for (const auto &variant : variants) {
        if (variant.priority < 0) {
            continue;
        }
        if (variant.preedit.size() < preedit.size() && preedit.starts_with(variant.preedit) &&
            canCompletePinyinTail(preedit.substr(variant.preedit.size()))) {
            continue;
        }
        if (variant.priority > 18) {
            break;
        }
        auto candidates = dictionary.exactKnownCandidates(variant.preedit);
        for (auto &candidate : candidates) {
            if (candidate.empty() || containsAsciiLetter(candidate) ||
                utf8CodepointCount(candidate) < 2 ||
                std::find(currentCandidates.begin(), currentCandidates.end(), candidate) != currentCandidates.end() ||
                std::find(result.begin(), result.end(), candidate) != result.end()) {
                continue;
            }
            result.push_back(std::move(candidate));
            if (result.size() >= 4) {
                return result;
            }
        }
    }
    return result;
}

std::vector<std::string> singleCharacterCandidates(std::string_view syllable, const Dictionary &dictionary) {
    auto candidates = dictionary.lookup(syllable);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const std::string &candidate) {
                         return containsAsciiLetter(candidate) || utf8CodepointCount(candidate) != 1;
                     }),
                     candidates.end());
    if (candidates.size() > 8) {
        candidates.resize(8);
    }
    return candidates;
}

} // namespace

bool isCompletePinyinSequence(std::string_view text) {
    return !text.empty() && pinyinSyllableCount(text).has_value();
}

std::optional<std::size_t> shortestPinyinSyllableCount(std::string_view text) {
    return shortestPinyinSyllableCountImpl(text);
}

bool shouldRunFullPinyinDecoder(std::string_view text) {
    constexpr std::size_t maxIncompleteDecoderInputLength = 8;
    return !text.empty() &&
           (text.size() <= maxIncompleteDecoderInputLength || shortestPinyinSyllableCount(text).has_value());
}

State::State(Dictionary dictionary, std::filesystem::path preferencePath)
    : dictionary_(std::move(dictionary)), inputModel_(std::move(preferencePath)) {}

const std::string &State::preedit() const { return preedit_; }

const std::vector<std::string> &State::candidates() const { return candidates_; }

std::vector<std::string> State::visibleCandidates() const {
    if (candidatesExpanded_) {
        return candidates_;
    }

    std::vector<std::string> visible;
    for (const auto &cell : collapsedVisualCandidateCells(candidates_)) {
        visible.push_back(candidates_[cell.index]);
    }
    return visible;
}

std::vector<std::string> State::displayCandidates() const {
    return visibleCandidates();
}

const std::vector<InputEvent> &State::recentEvents() const { return inputModel_.recentEvents(); }

const std::vector<std::string> &State::recentCommits() const { return inputModel_.recentCommits(); }

DebugSnapshot State::debugSnapshot() const {
    return {
        preedit_,
        candidates_.size(),
        displayCandidates().size(),
        candidatesExpanded_,
        inputModel_.eventCounts(),
    };
}

bool State::empty() const { return preedit_.empty(); }

bool State::candidatesExpanded() const { return candidatesExpanded_; }

bool State::rawPreeditMode() const { return rawPreeditMode_; }

std::size_t State::preeditCursorIndex() const { return std::min(preeditCursor_, preedit_.size()); }

bool State::preeditCursorAtEnd() const { return preeditCursorIndex() >= preedit_.size(); }

std::size_t State::candidateCursorIndex() const { return candidateCursorIndex_; }

std::size_t State::candidateCount() const { return candidates_.size(); }

std::size_t State::candidateConsumedPrefixLength(std::size_t index) const {
    return candidateConsumedPrefixLengthAt(index);
}

std::string State::candidateSource(std::size_t index) const {
    if (index >= candidateMetadata_.size()) {
        return {};
    }
    return candidateMetadata_[index].source;
}

int State::candidateScore(std::size_t index) const {
    if (index >= candidateMetadata_.size()) {
        return 0;
    }
    return candidateMetadata_[index].score;
}

Action State::inputAscii(char ch) {
    if (!std::isalpha(static_cast<unsigned char>(ch))) {
        return {};
    }

    prepareInputEventRecord();
    const auto normalized = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    inputModel_.record(InputEventType::Letter, std::string_view(&normalized, 1));
    const bool keepRawMode = rawPreeditMode_;
    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    if (preeditCursor_ < preedit_.size()) {
        notePreeditEditBeforeMutation();
    }
    preedit_.insert(preedit_.begin() + static_cast<std::ptrdiff_t>(preeditCursor_), normalized);
    ++preeditCursor_;
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    if (keepRawMode && !preedit_.empty() && (candidates_.empty() || candidates_.front() != preedit_)) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    rawPreeditMode_ = keepRawMode;
    return updateAccepted();
}

Action State::inputUppercaseAscii(char ch) {
    if (ch < 'A' || ch > 'Z') {
        return {};
    }

    prepareInputEventRecord();
    inputModel_.record(InputEventType::Letter, std::string_view(&ch, 1));
    if (preedit_.empty()) {
        return {};
    }

    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    if (preeditCursor_ < preedit_.size()) {
        notePreeditEditBeforeMutation();
    }
    preedit_.insert(preedit_.begin() + static_cast<std::ptrdiff_t>(preeditCursor_), ch);
    ++preeditCursor_;
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    rawPreeditMode_ = true;
    refreshCandidates();
    if (candidates_.empty() || candidates_.front() != preedit_) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    return updateAccepted();
}

Action State::inputAsciiDigit(char ch) {
    if (!std::isdigit(static_cast<unsigned char>(ch)) || preedit_.empty()) {
        return {};
    }

    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    std::string updated = preedit_;
    updated.insert(updated.begin() + static_cast<std::ptrdiff_t>(preeditCursor_), ch);
    const bool rawCandidateActive = preeditCursorAtEnd() && !candidates_.empty() && candidates_.front() == preedit_;
    bool allowed = rawPreeditMode_ || rawCandidateActive || isKnownEnglishToken(updated) || isKnownEnglishTokenPrefix(updated);
    if (!allowed) {
        for (const auto token : knownEnglishTokens) {
            if (updated.size() <= token.size() || !std::string_view(updated).ends_with(token)) {
                continue;
            }
            const auto prefix = std::string_view(updated).substr(0, updated.size() - token.size());
            if (pinyinSyllableCount(prefix)) {
                allowed = true;
                break;
            }
        }
    }
    if (!allowed) {
        return {};
    }

    prepareInputEventRecord();
    inputModel_.record(InputEventType::Digit, std::string_view(&ch, 1));
    if (preeditCursor_ < preedit_.size()) {
        notePreeditEditBeforeMutation();
    }
    preedit_ = std::move(updated);
    ++preeditCursor_;
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    rawPreeditMode_ = rawPreeditMode_ || rawCandidateActive;
    if (rawPreeditMode_ && !preedit_.empty() && (candidates_.empty() || candidates_.front() != preedit_)) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    return updateAccepted();
}

Action State::inputRawTokenSymbol(char ch) {
    const bool tokenSymbol = ch == '-' || ch == '_' || ch == '.' || ch == '/';
    if (!tokenSymbol || preedit_.empty()) {
        return {};
    }

    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    const bool rawCandidateActive = preeditCursorAtEnd() && !candidates_.empty() && candidates_.front() == preedit_;
    if (!rawPreeditMode_ && !rawCandidateActive) {
        return {};
    }

    prepareInputEventRecord();
    inputModel_.record(InputEventType::Symbol, std::string_view(&ch, 1));
    if (preeditCursor_ < preedit_.size()) {
        notePreeditEditBeforeMutation();
    }
    preedit_.insert(preedit_.begin() + static_cast<std::ptrdiff_t>(preeditCursor_), ch);
    ++preeditCursor_;
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    rawPreeditMode_ = true;
    if (!preedit_.empty() && (candidates_.empty() || candidates_.front() != preedit_)) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    return updateAccepted();
}

Action State::backspace() {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::Backspace);
    if (preedit_.empty()) {
        return {};
    }

    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    if (preeditCursor_ == 0) {
        return {ActionType::None, {}, true};
    }
    if (preeditCursor_ < preedit_.size()) {
        notePreeditEditBeforeMutation();
    }
    preedit_.erase(preedit_.begin() + static_cast<std::ptrdiff_t>(preeditCursor_ - 1));
    --preeditCursor_;
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    if (preedit_.empty()) {
        rawPreeditMode_ = false;
    }
    if (rawPreeditMode_ && !preedit_.empty() && (candidates_.empty() || candidates_.front() != preedit_)) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    return updateAccepted();
}

Action State::deleteKey() {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::Delete);
    if (preedit_.empty()) {
        return {};
    }
    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    if (pendingSegmentChain_ && preeditCursor_ >= preedit_.size()) {
        return restorePreedit(pendingSegmentChain_->originalPreedit);
    }
    if (preeditCursor_ >= preedit_.size()) {
        return {ActionType::None, {}, true};
    }
    notePreeditEditBeforeMutation();
    preedit_.erase(preedit_.begin() + static_cast<std::ptrdiff_t>(preeditCursor_));
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    if (preedit_.empty()) {
        rawPreeditMode_ = false;
    }
    if (rawPreeditMode_ && !preedit_.empty() && (candidates_.empty() || candidates_.front() != preedit_)) {
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    return updateAccepted();
}

Action State::observeKey(std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::ObservedKey, keyName);
    return {};
}

Action State::space() {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::Space);
    if (preedit_.empty()) {
        return {};
    }

    return commitCurrentCandidate(true);
}

Action State::punctuation(std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::ObservedKey, keyName);
    if (preedit_.empty()) {
        return {};
    }

    return commitCurrentCandidate(false);
}

Action State::commitCurrentPassthrough(std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::ObservedKey, keyName);
    if (preedit_.empty()) {
        return {};
    }

    auto action = commitCurrentCandidate(false);
    if (!keyName.empty()) {
        action.accepted = true;
        action.passthroughText = std::string(keyName);
    }
    return action;
}

Action State::commitRawPreedit(std::string_view boundaryName) {
    prepareInputEventRecord();
    if (!boundaryName.empty()) {
        inputModel_.record(InputEventType::ObservedKey, boundaryName);
    }
    if (preedit_.empty()) {
        return {};
    }

    const auto text = preedit_;
    learnEditedPreeditCorrection(text);
    inputModel_.recordRawCommit(text);
    captureCompletedSupervision(text, true);
    resetComposition();
    return {ActionType::Commit, text};
}

Action State::enter() {
    prepareInputEventRecord();
    if (preedit_.empty()) {
        inputModel_.record(InputEventType::Enter);
        return {};
    }

    const auto text = preedit_;
    inputModel_.record(InputEventType::Enter);
    learnEditedPreeditCorrection(text);
    inputModel_.recordRawCommit(text, 2);
    captureCompletedSupervision(text, true);
    resetComposition();
    return {ActionType::Commit, text, true};
}

Action State::escape() {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::Escape);
    if (preedit_.empty()) {
        return {};
    }

    captureCompletedSupervision();
    resetComposition();
    return clearAccepted();
}

Action State::select(std::size_t index) {
    if (preedit_.empty()) {
        return {};
    }

    const auto preeditBefore = preedit_;
    auto candidate = candidateAt(index);
    if (!candidate) {
        return {ActionType::None, {}, true};
    }

    return commitCandidate(*candidate, true, 3, false, preeditBefore, candidateConsumedPrefixLengthAt(index));
}

Action State::selectVisibleDigit(std::size_t digitIndex, std::string_view keyName) {
    if (preedit_.empty()) {
        return {};
    }

    if (candidatesExpanded_) {
        const auto cells = visibleVisualCellsFor(candidates_, candidateCursorIndex_, true);
        const auto selectedCell = visualCellForIndex(cells, candidateCursorIndex_);
        const auto rowCells = cellsInVisualRow(cells, selectedCell ? selectedCell->row : 0);
        if (digitIndex >= rowCells.size()) {
            return commitCurrentPassthrough(keyName);
        }
        return select(rowCells[digitIndex].index);
    }

    if (digitIndex >= visibleCandidates().size()) {
        return commitCurrentPassthrough(keyName);
    }
    const auto visibleCells = collapsedVisualCandidateCells(candidates_);
    if (digitIndex >= visibleCells.size()) {
        return commitCurrentPassthrough(keyName);
    }
    return select(visibleCells[digitIndex].index);
}

Action State::cursorMove(std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (preedit_.empty()) {
        return {};
    }

    preeditCursor_ = std::min(preeditCursor_, preedit_.size());
    if (keyName == "Left" || keyName == "KP_Left") {
        if (preeditCursor_ > 0) {
            --preeditCursor_;
            candidatesExpanded_ = false;
            return updateAccepted();
        }
    } else if (keyName == "Right" || keyName == "KP_Right") {
        if (preeditCursor_ < preedit_.size()) {
            ++preeditCursor_;
            candidatesExpanded_ = false;
            return updateAccepted();
        }
    }
    return {ActionType::None, {}, true};
}

Action State::expandCandidates(std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (preedit_.empty()) {
        return {};
    }

    if (candidatesExpanded_) {
        return {ActionType::None, {}, true};
    }

    candidatesExpanded_ = true;
    clampCandidateCursor();
    return updateAccepted();
}

Action State::moveCandidateCursor(int delta, std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (preedit_.empty()) {
        return {};
    }

    if (candidates_.empty()) {
        return {ActionType::None, {}, true};
    }

    candidatesExpanded_ = true;
    if (std::abs(delta) == static_cast<int>(visualCandidateColumns)) {
        const auto cells = visualCandidateCells(candidates_);
        const auto rowCount = visualRowCount(cells);
        const auto selectedCell = visualCellForIndex(cells, candidateCursorIndex_);
        if (rowCount > 0 && selectedCell) {
            const auto currentRowCells = cellsInVisualRow(cells, selectedCell->row);
            const auto currentOrdinal = static_cast<std::size_t>(
                std::distance(currentRowCells.begin(),
                              std::find_if(currentRowCells.begin(), currentRowCells.end(), [this](const auto &cell) {
                                  return cell.index == candidateCursorIndex_;
                              })));
            const auto direction = delta > 0 ? 1 : -1;
            auto targetRow = static_cast<int>(selectedCell->row) + direction;
            targetRow %= static_cast<int>(rowCount);
            if (targetRow < 0) {
                targetRow += static_cast<int>(rowCount);
            }
            const auto targetRowCells = cellsInVisualRow(cells, static_cast<std::size_t>(targetRow));
            if (!targetRowCells.empty()) {
                candidateCursorIndex_ = targetRowCells[std::min(currentOrdinal, targetRowCells.size() - 1)].index;
                return updateAccepted();
            }
        }
    }
    if (std::abs(delta) == 1) {
        auto cells = visualCandidateCells(candidates_);
        std::stable_sort(cells.begin(), cells.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.row != rhs.row ? lhs.row < rhs.row : lhs.column < rhs.column;
        });
        const auto selected = std::find_if(cells.begin(), cells.end(), [this](const auto &cell) {
            return cell.index == candidateCursorIndex_;
        });
        if (selected != cells.end()) {
            const auto current = static_cast<int>(std::distance(cells.begin(), selected));
            auto next = (current + delta) % static_cast<int>(cells.size());
            if (next < 0) {
                next += static_cast<int>(cells.size());
            }
            candidateCursorIndex_ = cells[static_cast<std::size_t>(next)].index;
            return updateAccepted();
        }
    }
    const auto count = static_cast<int>(candidates_.size());
    auto next = static_cast<int>(candidateCursorIndex_) + delta;
    next %= count;
    if (next < 0) {
        next += count;
    }
    candidateCursorIndex_ = static_cast<std::size_t>(next);
    return updateAccepted();
}

Action State::moveCollapsedCandidateCursor(int delta, std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (preedit_.empty()) {
        return {};
    }

    if (candidates_.empty()) {
        return {ActionType::None, {}, true};
    }

    const auto visibleCells = collapsedVisualCandidateCells(candidates_);
    if (visibleCells.empty()) {
        return {ActionType::None, {}, true};
    }
    const auto currentIter = std::find_if(visibleCells.begin(), visibleCells.end(), [this](const auto &cell) {
        return cell.index == candidateCursorIndex_;
    });
    const auto current = currentIter == visibleCells.end()
                             ? std::size_t{0}
                             : static_cast<std::size_t>(std::distance(visibleCells.begin(), currentIter));
    auto next = static_cast<int>(current) + delta;
    next = std::clamp(next, 0, static_cast<int>(visibleCells.size()) - 1);
    candidateCursorIndex_ = visibleCells[static_cast<std::size_t>(next)].index;
    candidatesExpanded_ = false;
    return updateAccepted();
}

Action State::moveCandidateCursorTo(std::size_t index, std::string_view keyName) {
    prepareInputEventRecord();
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (preedit_.empty()) {
        return {};
    }

    if (candidates_.empty()) {
        return {ActionType::None, {}, true};
    }

    candidatesExpanded_ = true;
    candidateCursorIndex_ = std::min(index, candidates_.size() - 1);
    return updateAccepted();
}

Action State::beginExternalModelRerank(bool expandAfterRerank) {
    if (preedit_.empty()) {
        return {};
    }
    prepareInputEventRecord();
    inputModel_.record(InputEventType::AiRerankRequested, preedit_);
    candidatesExpanded_ = expandAfterRerank;
    candidateCursorIndex_ = 0;
    return updateAccepted();
}

Action State::rerankCandidates(std::string_view application, std::string surroundingBefore,
                               std::string surroundingAfter, bool expandAfterRerank, bool allowExternalModel,
                               bool continuousMode) {
    if (preedit_.empty()) {
        return {};
    }

    inputModel_.reloadPreferencesIfChanged(true);
    prepareInputEventRecord();
    inputModel_.record(InputEventType::AiRerankRequested, preedit_);
    const auto previousCorrectionVersion = inputModel_.correctionVersion();
    const auto correctionCandidates = explicitCorrectionCandidates();
    const auto visibleCells = visibleVisualCellsFor(candidates_, candidateCursorIndex_, candidatesExpanded_);
    std::vector<std::size_t> visibleCandidateIndices;
    visibleCandidateIndices.reserve(visibleCells.size());
    const auto selectedRow = selectedVisualRow(visibleCells, candidateCursorIndex_);
    std::vector<std::pair<std::string, std::size_t>> numberedCandidateIndices;
    for (const auto &cell : visibleCells) {
        visibleCandidateIndices.push_back(cell.index);
        const auto shortcut = shortcutForVisualCell(visibleCells, cell, selectedRow, candidatesExpanded_);
        if (!shortcut.empty()) {
            numberedCandidateIndices.emplace_back(shortcut, cell.index);
        }
    }
    std::vector<ModelRequestState::PendingSegment> pendingSegments;
    if (pendingSegmentChain_) {
        pendingSegments.push_back({pendingSegmentChain_->originalPreedit, pendingSegmentChain_->consumedPreedit,
                                   pendingSegmentChain_->committedText, preedit_});
    }
    auto rerankedCandidates = inputModel_.rerankCandidates(
        preedit_, candidates_,
        ModelRequestState{preeditCursorIndex(), candidateCursorIndex_, candidatesExpanded_, continuousMode,
                          modelCandidateMetadata(),
                          std::move(pendingSegments),
                          std::move(visibleCandidateIndices), std::move(numberedCandidateIndices),
                          std::move(surroundingBefore), std::move(surroundingAfter), false, {}},
        application, allowExternalModel);
    return applyRerankedCandidates(std::move(rerankedCandidates), previousCorrectionVersion, correctionCandidates,
                                   expandAfterRerank, allowExternalModel ? "model-rank" : "local-rank");
}

Action State::applyRerankedCandidates(std::vector<std::string> rerankedCandidates,
                                      std::size_t previousCorrectionVersion,
                                      const std::vector<std::string> &correctionCandidates,
                                      bool expandAfterRerank, std::string_view rankSource) {
    ensureCandidateMetadata();
    const auto originalCandidates = candidates_;
    const auto originalMetadata = candidateMetadata_;
    const auto originalFullCorrectionCandidates = fullCorrectionCandidates_;
    if (inputModel_.correctionVersion() != previousCorrectionVersion) {
        refreshCandidates();
        preserveCandidateMetadataFrom(originalCandidates, originalMetadata, rankSource);
    } else {
        fullCorrectionCandidates_.clear();
        candidates_ = std::move(rerankedCandidates);
        preserveCandidateMetadataFrom(originalCandidates, originalMetadata, rankSource);
        for (const auto &candidate : candidates_) {
            if (originalFullCorrectionCandidates.contains(candidate)) {
                fullCorrectionCandidates_.insert(candidate);
            }
        }
        std::size_t correctionInsertIndex = candidates_.empty() ? 0 : 1;
        for (const auto &candidate : correctionCandidates) {
            const auto existing = std::find(candidates_.begin(), candidates_.end(), candidate);
            if (existing != candidates_.end()) {
                const auto index = static_cast<std::size_t>(std::distance(candidates_.begin(), existing));
                if (index == 0) {
                    fullCorrectionCandidates_.insert(candidate);
                    continue;
                }
                eraseCandidate(index);
                if (index < correctionInsertIndex) {
                    --correctionInsertIndex;
                }
            }
            correctionInsertIndex = std::min(correctionInsertIndex, candidates_.size());
            insertCandidate(correctionInsertIndex, candidate, 0, "correction", 950000);
            ++correctionInsertIndex;
            fullCorrectionCandidates_.insert(candidate);
        }
    }
    rawPreeditMode_ = !preedit_.empty() && !candidates_.empty() && candidates_.front() == preedit_;
    candidatesExpanded_ = expandAfterRerank;
    candidateCursorIndex_ = 0;
    finalizeCandidateScores();
    return updateAccepted();
}

std::optional<State::ExternalModelRequest>
State::externalModelRequest(std::string_view application, std::string surroundingBefore,
                            std::string surroundingAfter, bool continuousMode) const {
    if (preedit_.empty() || candidates_.empty()) {
        return std::nullopt;
    }

    const auto visibleCells = visibleVisualCellsFor(candidates_, candidateCursorIndex_, candidatesExpanded_);
    std::vector<std::size_t> visibleCandidateIndices;
    visibleCandidateIndices.reserve(visibleCells.size());
    const auto selectedRow = selectedVisualRow(visibleCells, candidateCursorIndex_);
    std::vector<std::pair<std::string, std::size_t>> numberedCandidateIndices;
    for (const auto &cell : visibleCells) {
        visibleCandidateIndices.push_back(cell.index);
        const auto shortcut = shortcutForVisualCell(visibleCells, cell, selectedRow, candidatesExpanded_);
        if (!shortcut.empty()) {
            numberedCandidateIndices.emplace_back(shortcut, cell.index);
        }
    }
    std::vector<ModelRequestState::PendingSegment> pendingSegments;
    if (pendingSegmentChain_) {
        pendingSegments.push_back({pendingSegmentChain_->originalPreedit, pendingSegmentChain_->consumedPreedit,
                                   pendingSegmentChain_->committedText, preedit_});
    }
    ModelRequestState requestState{preeditCursorIndex(), candidateCursorIndex_, candidatesExpanded_, continuousMode,
                                   modelCandidateMetadata(), std::move(pendingSegments),
                                   std::move(visibleCandidateIndices), std::move(numberedCandidateIndices),
                                   std::move(surroundingBefore), std::move(surroundingAfter), false, {}};
    auto requestModel = inputModel_;
    requestModel.record(InputEventType::AiRerankRequested, preedit_);
    ExternalModelRequest request;
    request.preedit = preedit_;
    request.candidates = candidates_;
    request.state = requestState;
    request.payload = requestModel.modelRequest(preedit_, candidates_, requestState, application);
    armExternalModelRequest(request);
    return request;
}

void State::armExternalModelRequest(ExternalModelRequest &request) const {
    request.expectedPreedit = preedit_;
    request.expectedCandidates = candidates_;
    request.expectedPreeditCursor = preeditCursorIndex();
    request.expectedCandidateCursor = candidateCursorIndex_;
    request.expectedCandidatesExpanded = candidatesExpanded_;
}

Action State::applyExternalModelResponse(const ExternalModelRequest &request, std::string_view output,
                                         bool expandAfterRerank) {
    if (output.empty() || preedit_ != request.expectedPreedit || candidates_ != request.expectedCandidates ||
        preeditCursorIndex() != request.expectedPreeditCursor ||
        candidateCursorIndex_ != request.expectedCandidateCursor ||
        candidatesExpanded_ != request.expectedCandidatesExpanded) {
        return {};
    }

    const auto previousCorrectionVersion = inputModel_.correctionVersion();
    const auto correctionCandidates = explicitCorrectionCandidates();
    auto rerankedCandidates =
        inputModel_.applyExternalModelOutput(request.preedit, request.candidates, request.state, output);
    if (rerankedCandidates.empty()) {
        return {};
    }
    return applyRerankedCandidates(std::move(rerankedCandidates), previousCorrectionVersion, correctionCandidates,
                                   expandAfterRerank, "model-rank");
}

std::string State::modelRequestSnapshot(std::string_view application, std::string surroundingBefore,
                                        std::string surroundingAfter, bool continuousMode) const {
    std::vector<ModelRequestState::PendingSegment> pendingSegments;
    if (pendingSegmentChain_) {
        pendingSegments.push_back({pendingSegmentChain_->originalPreedit, pendingSegmentChain_->consumedPreedit,
                                   pendingSegmentChain_->committedText, preedit_});
    }
    return buildModelRequestSnapshot(preedit_, candidates_, preeditCursorIndex(), candidateCursorIndex_,
                                     candidatesExpanded_, modelCandidateMetadata(), std::move(pendingSegments),
                                     application, std::move(surroundingBefore), std::move(surroundingAfter),
                                     continuousMode);
}

std::optional<State::CompletedSupervisionRequest>
State::completedSupervisionRequest(std::string_view application, std::string surroundingBefore,
                                   std::string surroundingAfter, bool continuousMode) const {
    if (!completedSupervision_) {
        return std::nullopt;
    }
    const auto &completed = *completedSupervision_;
    return CompletedSupervisionRequest{
        completed.preedit,
        completed.candidates.size(),
        completed.candidatesExpanded,
        buildModelRequestSnapshot(completed.preedit, completed.candidates, completed.preeditCursor,
                                  completed.candidateCursor, completed.candidatesExpanded,
                                  completed.candidateMetadata, completed.pendingSegments, application,
                                  std::move(surroundingBefore), std::move(surroundingAfter), continuousMode),
    };
}

void State::clearCompletedSupervisionRequest() { completedSupervision_.reset(); }

std::string State::buildModelRequestSnapshot(
    std::string_view preedit, const std::vector<std::string> &candidates, std::size_t preeditCursor,
    std::size_t candidateCursor, bool candidatesExpanded,
    std::vector<ModelRequestState::CandidateMetadata> candidateMetadata,
    std::vector<ModelRequestState::PendingSegment> pendingSegments, std::string_view application,
    std::string surroundingBefore, std::string surroundingAfter, bool continuousMode) const {
    const auto visibleCells = visibleVisualCellsFor(candidates, candidateCursor, candidatesExpanded);
    std::vector<std::size_t> visibleCandidateIndices;
    visibleCandidateIndices.reserve(visibleCells.size());
    const auto selectedRow = selectedVisualRow(visibleCells, candidateCursor);
    std::vector<std::pair<std::string, std::size_t>> numberedCandidateIndices;
    for (const auto &cell : visibleCells) {
        visibleCandidateIndices.push_back(cell.index);
        const auto shortcut = shortcutForVisualCell(visibleCells, cell, selectedRow, candidatesExpanded);
        if (!shortcut.empty()) {
            numberedCandidateIndices.emplace_back(shortcut, cell.index);
        }
    }
    return inputModel_.modelRequest(preedit, candidates,
                                    ModelRequestState{preeditCursor, candidateCursor, candidatesExpanded,
                                                      continuousMode,
                                                      std::move(candidateMetadata),
                                                      std::move(pendingSegments),
                                                      std::move(visibleCandidateIndices),
                                                      std::move(numberedCandidateIndices),
                                                      std::move(surroundingBefore), std::move(surroundingAfter),
                                                      false, {}},
                                    application);
}

State::RestorableSnapshot State::restorableSnapshot() const {
    RestorableSnapshot snapshot;
    snapshot.inputModel = inputModel_;
    snapshot.preedit = preedit_;
    snapshot.preeditCursor = preeditCursor_;
    snapshot.editedOriginalPreedit = editedOriginalPreedit_;
    if (pendingSegmentChain_) {
        snapshot.pendingSegmentChain = RestorableSnapshot::PendingSegmentChain{
            pendingSegmentChain_->originalPreedit,
            pendingSegmentChain_->consumedPreedit,
            pendingSegmentChain_->committedText,
        };
    }
    snapshot.candidatesExpanded = candidatesExpanded_;
    snapshot.candidateCursorIndex = candidateCursorIndex_;
    snapshot.rawPreeditMode = rawPreeditMode_;
    return snapshot;
}

State::SessionContext State::sessionContext() const { return inputModel_.sessionContext(); }

void State::restoreSessionContext(SessionContext context) {
    inputModel_.restoreSessionContext(std::move(context));
}

Action State::restoreSnapshot(const RestorableSnapshot &snapshot) {
    inputModel_ = snapshot.inputModel;
    preedit_ = snapshot.preedit;
    preeditCursor_ = std::min(snapshot.preeditCursor, preedit_.size());
    editedOriginalPreedit_ = snapshot.editedOriginalPreedit;
    if (snapshot.pendingSegmentChain) {
        pendingSegmentChain_ = PendingSegmentChain{
            snapshot.pendingSegmentChain->originalPreedit,
            snapshot.pendingSegmentChain->consumedPreedit,
            snapshot.pendingSegmentChain->committedText,
        };
    } else {
        pendingSegmentChain_.reset();
    }
    candidatesExpanded_ = snapshot.candidatesExpanded;
    candidateCursorIndex_ = snapshot.candidateCursorIndex;
    rawPreeditMode_ = snapshot.rawPreeditMode;
    clearEventsBeforeNextInput_ = false;
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    clampCandidateCursor();
    return updateAccepted();
}

Action State::restorePreedit(std::string preedit, std::string_view keyName) {
    prepareInputEventRecord();
    if (!keyName.empty()) {
        inputModel_.record(InputEventType::Delete, keyName);
    }
    preedit_ = std::move(preedit);
    preeditCursor_ = preedit_.size();
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    if (pendingSegmentChain_) {
        inputModel_.undoLatestRecentCommit(pendingSegmentChain_->committedText);
    }
    pendingSegmentChain_.reset();
    fullCorrectionCandidates_.clear();
    refreshCandidates();
    rawPreeditMode_ = false;
    clearEventsBeforeNextInput_ = false;
    return updateAccepted();
}

void State::resetComposition() {
    preedit_.clear();
    preeditCursor_ = 0;
    candidates_.clear();
    candidateMetadata_.clear();
    editedOriginalPreedit_.reset();
    pendingSegmentChain_.reset();
    fullCorrectionCandidates_.clear();
    candidatesExpanded_ = false;
    candidateCursorIndex_ = 0;
    rawPreeditMode_ = false;
    clearEventsBeforeNextInput_ = true;
}

void State::reset() {
    resetComposition();
    completedSupervision_.reset();
}

Action State::updateAccepted() { return {ActionType::Update, {}, true}; }

Action State::clearAccepted() { return {ActionType::Clear, {}, true}; }

Action State::commitCurrentCandidate(bool accepted, std::size_t selectionWeight) {
    const auto preeditBefore = preedit_;
    clampCandidateCursor();
    const bool rawCommit = candidates_.empty() ||
                           (candidateCursorIndex_ == 0 &&
                            (rawPreeditMode_ || inputModel_.shouldPreferRaw(preedit_, candidates_)));
    const auto text = rawCommit ? preedit_ : candidates_[candidateCursorIndex_];
    if (!rawCommit && selectionWeight == 1) {
        if (accepted && candidateCursorIndex_ > 0) {
            selectionWeight = 3;
        } else if (candidateCursorIndex_ == 0) {
            selectionWeight = 0;
        }
    }
    const auto consumedPrefixLength = rawCommit ? 0 : candidateConsumedPrefixLengthAt(candidateCursorIndex_);
    return commitCandidate(text, accepted, selectionWeight, rawCommit, preeditBefore, consumedPrefixLength);
}

void State::notePreeditEditBeforeMutation() {
    if (!editedOriginalPreedit_ && !preedit_.empty()) {
        editedOriginalPreedit_ = preedit_;
    }
}

void State::learnEditedPreeditCorrection(std::string_view correctedPreedit) {
    if (editedOriginalPreedit_ && *editedOriginalPreedit_ != correctedPreedit) {
        inputModel_.recordCorrection(*editedOriginalPreedit_, correctedPreedit, 2);
    }
}

void State::prepareInputEventRecord() {
    if (preedit_.empty() && clearEventsBeforeNextInput_) {
        completedSupervision_.reset();
        inputModel_.clear();
        clearEventsBeforeNextInput_ = false;
    }
}

Action State::commitCandidate(std::string candidate, bool accepted, std::size_t selectionWeight, bool rawCommit,
                              std::string preeditBefore, std::size_t consumedPrefixLength) {
    captureCompletedSupervision(candidate, rawCommit);
    if (rawCommit) {
        learnEditedPreeditCorrection(preedit_);
        inputModel_.recordRawCommit(preedit_);
    } else if (fullCorrectionCandidates_.contains(candidate)) {
        learnEditedPreeditCorrection(preedit_);
        inputModel_.recordCandidateSelection(preedit_, candidate, selectionWeight);
        dictionary_.learnLanguageModelSelection(preedit_, candidate);
        resetComposition();
        return {ActionType::Commit, std::move(candidate), accepted, false, std::move(preeditBefore), preedit_};
    } else if (consumedPrefixLength > 0 && consumedPrefixLength < preedit_.size()) {
        commitPartialCandidate(candidate, consumedPrefixLength, selectionWeight);
        preeditCursor_ = preedit_.size();
        candidatesExpanded_ = false;
        candidateCursorIndex_ = 0;
        refreshCandidates();
        if (candidates_.empty()) {
            resetComposition();
            return {ActionType::Commit, std::move(candidate), accepted, false, std::move(preeditBefore), preedit_};
        }
        return {ActionType::Commit, std::move(candidate), accepted, true, std::move(preeditBefore), preedit_};
    } else if (const auto prefixLength = partialCommitPrefixLength(candidate)) {
        commitPartialCandidate(candidate, *prefixLength, selectionWeight);
        preeditCursor_ = preedit_.size();
        candidatesExpanded_ = false;
        candidateCursorIndex_ = 0;
        refreshCandidates();
        if (candidates_.empty()) {
            resetComposition();
            return {ActionType::Commit, std::move(candidate), accepted, false, std::move(preeditBefore), preedit_};
        }
        return {ActionType::Commit, std::move(candidate), accepted, true, std::move(preeditBefore), preedit_};
    } else if (selectionWeight > 1 &&
               (approximatePartialCommitPrefixLength(candidate, false) ||
                shouldPreserveUnresolvedPartialCandidate(candidate))) {
        editedOriginalPreedit_.reset();
        if (const auto prefixLength = approximatePartialCommitPrefixLength(candidate, false);
            prefixLength && *prefixLength < preedit_.size()) {
            commitPartialCandidate(candidate, *prefixLength, selectionWeight);
        } else {
            pendingSegmentChain_.reset();
            inputModel_.recordCandidateSelection(preedit_, candidate, selectionWeight);
        }
        preeditCursor_ = preedit_.size();
        candidatesExpanded_ = false;
        candidateCursorIndex_ = 0;
        refreshCandidates();
        if (candidates_.empty()) {
            resetComposition();
            return {ActionType::Commit, std::move(candidate), accepted, false, std::move(preeditBefore), preedit_};
        }
        return {ActionType::Commit, std::move(candidate), accepted, true, std::move(preeditBefore), preedit_};
    } else {
        learnEditedPreeditCorrection(preedit_);
        learnPendingSegmentChain(preedit_, candidate);
        inputModel_.recordCandidateSelection(preedit_, candidate, selectionWeight);
        dictionary_.learnLanguageModelSelection(preedit_, candidate);
    }
    resetComposition();
    return {ActionType::Commit, std::move(candidate), accepted, false, std::move(preeditBefore), preedit_};
}

void State::captureCompletedSupervision(std::optional<std::string_view> confirmedCandidate, bool rawCommit) {
    CompletedSupervision completed;
    completed.preedit = preedit_;
    completed.candidates = candidates_;
    completed.preeditCursor = preeditCursorIndex();
    completed.candidateCursor = candidateCursorIndex_;
    completed.candidatesExpanded = candidatesExpanded_;
    completed.candidateMetadata = modelCandidateMetadata();
    if (pendingSegmentChain_) {
        completed.pendingSegments.push_back({pendingSegmentChain_->originalPreedit,
                                             pendingSegmentChain_->consumedPreedit,
                                             pendingSegmentChain_->committedText, preedit_});
    }
    if (confirmedCandidate && !confirmedCandidate->empty()) {
        const auto candidate = std::string(*confirmedCandidate);
        const auto iter = std::find(completed.candidates.begin(), completed.candidates.end(), candidate);
        if (iter == completed.candidates.end()) {
            completed.candidates.insert(completed.candidates.begin(), candidate);
            completed.candidateMetadata.insert(completed.candidateMetadata.begin(),
                                               {0, rawCommit ? "raw" : "selected", rawCommit ? 900000 : 0});
            completed.candidateCursor = 0;
        } else {
            completed.candidateCursor =
                static_cast<std::size_t>(std::distance(completed.candidates.begin(), iter));
        }
    }
    completedSupervision_ = std::move(completed);
}

void State::commitPartialCandidate(std::string &candidate, std::size_t prefixLength, std::size_t selectionWeight) {
    editedOriginalPreedit_.reset();
    prefixLength = std::min(prefixLength, preedit_.size());
    const auto consumedPreedit = std::string(std::string_view(preedit_).substr(0, prefixLength));
    if (pendingSegmentChain_) {
        pendingSegmentChain_->consumedPreedit += consumedPreedit;
        pendingSegmentChain_->committedText += candidate;
    } else {
        pendingSegmentChain_ = PendingSegmentChain{preedit_, consumedPreedit, candidate};
    }
    inputModel_.recordCandidateSelection(std::string_view(preedit_).substr(0, prefixLength), candidate,
                                         selectionWeight);
    dictionary_.learnLanguageModelSelection(consumedPreedit, candidate);
    preedit_.erase(0, prefixLength);
}

std::optional<std::string> State::candidateAt(std::size_t index) const {
    if (index >= candidates_.size()) {
        return std::nullopt;
    }
    return candidates_[index];
}

std::size_t State::candidateConsumedPrefixLengthAt(std::size_t index) const {
    if (index >= candidateMetadata_.size()) {
        return 0;
    }
    return candidateMetadata_[index].consumedPrefixLength;
}

void State::clearCandidateMetadata() {
    candidateMetadata_.assign(candidates_.size(), {});
}

void State::setCandidateMetadataDefaults(std::string source, int baseScore) {
    candidateMetadata_.clear();
    candidateMetadata_.reserve(candidates_.size());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        candidateMetadata_.push_back(
            CandidateMetadata{0, source, baseScore - static_cast<int>(std::min<std::size_t>(index, 1000))});
    }
}

void State::ensureCandidateMetadata() {
    candidateMetadata_.resize(candidates_.size());
}

void State::insertCandidate(std::size_t index, std::string candidate, std::size_t consumedPrefixLength,
                            std::string source, int score) {
    index = std::min(index, candidates_.size());
    ensureCandidateMetadata();
    candidates_.insert(candidates_.begin() + static_cast<std::ptrdiff_t>(index), std::move(candidate));
    candidateMetadata_.insert(candidateMetadata_.begin() + static_cast<std::ptrdiff_t>(index),
                              CandidateMetadata{consumedPrefixLength, std::move(source), score});
}

void State::eraseCandidate(std::size_t index) {
    if (index >= candidates_.size()) {
        return;
    }
    ensureCandidateMetadata();
    candidates_.erase(candidates_.begin() + static_cast<std::ptrdiff_t>(index));
    candidateMetadata_.erase(candidateMetadata_.begin() + static_cast<std::ptrdiff_t>(index));
}

void State::finalizeCandidateScores() {
    ensureCandidateMetadata();
    for (std::size_t index = 0; index < candidateMetadata_.size(); ++index) {
        candidateMetadata_[index].score = 1000000 - static_cast<int>(std::min<std::size_t>(index, 1000000));
    }
}

void State::preserveCandidateMetadataFrom(const std::vector<std::string> &originalCandidates,
                                          const std::vector<CandidateMetadata> &originalMetadata,
                                          std::string_view sourcePrefix) {
    std::vector<bool> metadataUsed(originalCandidates.size(), false);
    ensureCandidateMetadata();
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        auto &metadata = candidateMetadata_[index];
        if (metadata.source.empty()) {
            metadata.source = std::string(sourcePrefix);
        } else if (!sourcePrefix.empty() && metadata.source.rfind(std::string(sourcePrefix) + ":", 0) != 0) {
            metadata.source = std::string(sourcePrefix) + ":" + metadata.source;
        }
        for (std::size_t originalIndex = 0; originalIndex < originalCandidates.size(); ++originalIndex) {
            if (metadataUsed[originalIndex] || originalCandidates[originalIndex] != candidates_[index]) {
                continue;
            }
            metadataUsed[originalIndex] = true;
            if (originalIndex < originalMetadata.size()) {
                const auto &original = originalMetadata[originalIndex];
                if (metadata.consumedPrefixLength == 0 && original.consumedPrefixLength > 0) {
                    metadata.consumedPrefixLength = original.consumedPrefixLength;
                }
                if (!original.source.empty()) {
                    metadata.source = sourcePrefix.empty() ? original.source : std::string(sourcePrefix) + ":" + original.source;
                }
            }
            break;
        }
    }
}

std::optional<std::size_t> State::partialCommitPrefixLength(const std::string &candidate) const {
    if (preedit_.empty()) {
        return std::nullopt;
    }

    if (!candidates_.empty() && candidates_.front() == candidate) {
        const auto fullCandidates = dictionary_.lookup(preedit_);
        if (!fullCandidates.empty() && fullCandidates.front() == candidate) {
            const auto totalSyllables = shortestPinyinSyllableCount(preedit_);
            if (!totalSyllables || utf8CodepointCount(candidate) >= *totalSyllables) {
                return std::nullopt;
            }
        }
    }

    if (prefixCoversMixedEnglishCandidate(preedit_, candidate, candidates_)) {
        std::optional<std::size_t> mixedPrefixLength;
        if (const auto range = mixedEnglishPrefixScanRange(preedit_, longestAsciiLetterRun(candidate))) {
            for (std::size_t prefixLength = range->first; prefixLength < range->second; ++prefixLength) {
                const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
                if (!prefixCoversMixedEnglishCandidate(prefixPinyin, candidate, candidates_)) {
                    continue;
                }
                const auto prefixCandidates = dictionary_.lookup(prefixPinyin);
                if (std::find(prefixCandidates.begin(), prefixCandidates.end(), candidate) != prefixCandidates.end()) {
                    mixedPrefixLength = prefixLength;
                }
            }
        }
        if (mixedPrefixLength) {
            return mixedPrefixLength;
        }
    }

    if (preedit_.size() <= maxKnownPrefixScanLength) {
        std::optional<std::size_t> longestKnownPrefixLength;
        for (std::size_t prefixLength = 1; prefixLength < preedit_.size(); ++prefixLength) {
            const auto remaining = std::string_view(preedit_).substr(prefixLength);
            if (!pinyinSyllableCount(remaining)) {
                continue;
            }
            auto prefixCandidates = dictionary_.exactKnownCandidates(std::string_view(preedit_).substr(0, prefixLength));
            if (std::find(prefixCandidates.begin(), prefixCandidates.end(), candidate) == prefixCandidates.end()) {
                continue;
            }
            const bool leadingTextOfFullCandidate =
                !candidates_.empty() && candidates_.front().size() > candidate.size() &&
                std::string_view(candidates_.front()).starts_with(candidate);
            if (leadingTextOfFullCandidate || utf8CodepointCount(candidate) > 1) {
                longestKnownPrefixLength = prefixLength;
            }
        }
        if (longestKnownPrefixLength) {
            return longestKnownPrefixLength;
        }
    }

    const auto candidateSyllables = utf8CodepointCount(candidate);
    const auto totalSyllables = shortestPinyinSyllableCount(preedit_);
    if (!totalSyllables || *totalSyllables <= 1) {
        return std::nullopt;
    }
    const auto fullCandidates = dictionary_.lookup(preedit_);
    if (candidateSyllables >= *totalSyllables &&
        std::find(fullCandidates.begin(), fullCandidates.end(), candidate) != fullCandidates.end()) {
        return std::nullopt;
    }

    auto candidateMatchesPrefix = [this, &candidate](std::size_t syllables, std::size_t prefixLength) {
        const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
        if (shortestPinyinSyllableCount(prefixPinyin) != syllables) {
            return false;
        }
        const bool mixedEnglishPrefix = prefixCoversMixedEnglishCandidate(prefixPinyin, candidate, candidates_);
        if (utf8CodepointCount(candidate) != syllables && !mixedEnglishPrefix) {
            return false;
        }
        auto prefixCandidates =
            syllables == 1 ? singleCharacterCandidates(prefixPinyin, dictionary_) : std::vector<std::string>{};
        auto dictionaryCandidates = dictionary_.exactKnownCandidates(prefixPinyin);
        prefixCandidates.insert(prefixCandidates.end(), std::make_move_iterator(dictionaryCandidates.begin()),
                                std::make_move_iterator(dictionaryCandidates.end()));
        return std::find(prefixCandidates.begin(), prefixCandidates.end(), candidate) != prefixCandidates.end();
    };

    if (preedit_.size() <= maxAmbiguousSplitLength) {
        if (candidateSyllables > 0 && canSplitIntoMoreThan(preedit_, candidateSyllables)) {
            std::optional<std::size_t> longestMatchingPrefix;
            for (const auto prefixLength : pinyinPrefixLengthOptionsForSyllables(preedit_, candidateSyllables)) {
                if (candidateMatchesPrefix(candidateSyllables, prefixLength)) {
                    longestMatchingPrefix = prefixLength;
                }
            }
            if (longestMatchingPrefix) {
                return longestMatchingPrefix;
            }
            const auto candidateIsLeadingText =
                !candidates_.empty() && candidates_.front() != candidate &&
                std::any_of(candidates_.begin(), candidates_.end(), [&candidate](const std::string &other) {
                    return other.size() > candidate.size() && std::string_view(other).starts_with(candidate);
                });
            if (candidateIsLeadingText) {
                const auto prefixLengths = pinyinPrefixLengthOptionsForSyllables(preedit_, candidateSyllables);
                for (auto iter = prefixLengths.rbegin(); iter != prefixLengths.rend(); ++iter) {
                    const auto prefixPinyin = std::string_view(preedit_).substr(0, *iter);
                    if (shortestPinyinSyllableCount(prefixPinyin) == candidateSyllables) {
                        return *iter;
                    }
                }
            }
        }

        for (std::size_t syllables = 1; syllables < *totalSyllables; ++syllables) {
            for (const auto prefixLength : pinyinPrefixLengthOptionsForSyllables(preedit_, syllables)) {
                if (candidateMatchesPrefix(syllables, prefixLength)) {
                    return prefixLength;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> State::approximatePartialCommitPrefixLength(const std::string &candidate,
                                                                       bool requireCandidateMatch) const {
    if (preedit_.size() <= 1 || candidate.empty() || containsAsciiLetter(candidate)) {
        return std::nullopt;
    }

    const auto chunks = utf8CodepointCount(candidate);
    if (chunks == 0) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        if (offset >= preedit_.size()) {
            return std::nullopt;
        }
        if (const auto length = nextPinyinSyllableLength(std::string_view(preedit_).substr(offset), true)) {
            offset += *length;
            continue;
        }
        if (chunk == 0) {
            return std::nullopt;
        }
        const auto ch = static_cast<unsigned char>(preedit_[offset]);
        if (!std::isalpha(ch)) {
            return std::nullopt;
        }
        ++offset;
    }

    if (offset == 0 || offset >= preedit_.size()) {
        return std::nullopt;
    }
    if (!requireCandidateMatch) {
        const auto fullCandidates = dictionary_.lookup(preedit_);
        if (!candidates_.empty() && candidates_.front() == candidate && !fullCandidates.empty() &&
            fullCandidates.front() == candidate) {
            return offset;
        }
    }
    if (requireCandidateMatch) {
        const auto prefixPinyin = std::string_view(preedit_).substr(0, offset);
        auto prefixCandidates =
            chunks == 1 ? singleCharacterCandidates(prefixPinyin, dictionary_) : std::vector<std::string>{};
        auto exactCandidates = dictionary_.exactKnownCandidates(prefixPinyin);
        prefixCandidates.insert(prefixCandidates.end(), std::make_move_iterator(exactCandidates.begin()),
                                std::make_move_iterator(exactCandidates.end()));
        if (std::find(prefixCandidates.begin(), prefixCandidates.end(), candidate) == prefixCandidates.end()) {
            return std::nullopt;
        }
    }
    return offset;
}

bool State::shouldPreserveUnresolvedPartialCandidate(const std::string &candidate) const {
    if (preedit_.size() <= 1 || candidate.empty() || containsAsciiLetter(candidate)) {
        return false;
    }

    const auto candidateSyllables = utf8CodepointCount(candidate);
    if (candidateSyllables == 0) {
        return false;
    }

    if (!partialCommitPrefixLength(candidate) && !approximatePartialCommitPrefixLength(candidate, false)) {
        return false;
    }

    const auto fullCandidates = dictionary_.lookup(preedit_);
    const bool exactFullCandidate =
        std::find(fullCandidates.begin(), fullCandidates.end(), candidate) != fullCandidates.end();
    if (exactFullCandidate && !candidates_.empty() && candidates_.front() == candidate && candidateSyllables > 1) {
        return false;
    }
    if (std::find(fullCandidates.begin(), fullCandidates.end(), candidate) != fullCandidates.end()) {
        if (const auto totalSyllables = shortestPinyinSyllableCount(preedit_);
            totalSyllables && candidateSyllables >= *totalSyllables) {
            return false;
        }
    }

    return candidateSyllables < preedit_.size();
}

std::optional<std::string> State::correctedPreeditForCandidate(std::string_view typedPreedit,
                                                               const std::string &candidate) const {
    const auto exactCandidates = dictionary_.exactKnownCandidates(typedPreedit);
    if (std::find(exactCandidates.begin(), exactCandidates.end(), candidate) != exactCandidates.end()) {
        return std::string(typedPreedit);
    }

    if (typedPreedit.empty() || typedPreedit.size() > 8) {
        return std::nullopt;
    }
    for (const auto syllable : pinyinSyllables) {
        const std::string_view syllableView(syllable);
        if (!syllableView.starts_with(typedPreedit) || syllableView == typedPreedit) {
            continue;
        }
        const auto candidates = dictionary_.exactKnownCandidates(syllableView);
        if (std::find(candidates.begin(), candidates.end(), candidate) != candidates.end()) {
            return std::string(syllableView);
        }
    }

    const auto lookupCandidates = dictionary_.lookup(typedPreedit);
    if (std::find(lookupCandidates.begin(), lookupCandidates.end(), candidate) != lookupCandidates.end()) {
        return std::string(typedPreedit);
    }
    return std::nullopt;
}

void State::learnPendingSegmentChain(std::string_view currentPreedit, const std::string &candidate) {
    if (!pendingSegmentChain_) {
        return;
    }
    const auto correctedRemaining = correctedPreeditForCandidate(currentPreedit, candidate);
    if (!correctedRemaining) {
        pendingSegmentChain_.reset();
        return;
    }

    const auto correctedFullPreedit = pendingSegmentChain_->consumedPreedit + *correctedRemaining;
    const auto combinedCandidate = pendingSegmentChain_->committedText + candidate;
    if (correctedFullPreedit != pendingSegmentChain_->originalPreedit) {
        inputModel_.recordCorrection(pendingSegmentChain_->originalPreedit, correctedFullPreedit, 2);
    }
    const auto chainEvidence = inputModel_.recordSegmentChain(
        {pendingSegmentChain_->originalPreedit, pendingSegmentChain_->consumedPreedit,
         pendingSegmentChain_->committedText, std::string(currentPreedit), correctedFullPreedit, combinedCandidate});
    inputModel_.recordCandidatePreference(pendingSegmentChain_->originalPreedit, combinedCandidate, 2);
    if (chainEvidence >= 2) {
        dictionary_.learnUserEntry(correctedFullPreedit, combinedCandidate);
    }
    pendingSegmentChain_.reset();
}

std::vector<std::string> State::explicitCorrectionCandidates() const {
    struct CorrectionCandidate {
        std::string text;
        int priority = 0;
        std::size_t variantOrder = 0;
        bool exactKnown = false;
    };
    std::vector<CorrectionCandidate> correctionCandidates;
    const auto currentlyVisible = visibleCandidates();
    const bool exactPreeditHasCandidates = !dictionary_.exactKnownCandidates(preedit_).empty();
    for (const auto &learnedCorrection : inputModel_.learnedCorrections(preedit_, [this](std::string_view correction) {
             return correctionTransferPriority(dictionary_, preedit_, correction);
         })) {
        const auto learnedCandidates = dictionary_.lookup(learnedCorrection);
        if (std::any_of(learnedCandidates.begin(), learnedCandidates.end(), [this](const auto &candidate) {
                return !candidates_.empty() && candidates_.front() == candidate;
            })) {
            return {};
        }
    }
    auto variants = localCorrectionPreeditVariants(preedit_);
    std::stable_sort(variants.begin(), variants.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.priority < rhs.priority;
    });
    if (!candidates_.empty()) {
        for (const auto &variant : variants) {
            if (exactPreeditHasCandidates && variant.priority >= 0) {
                continue;
            }
            const auto variantExactCandidates = dictionary_.exactKnownCandidates(variant.preedit);
            if (std::find(variantExactCandidates.begin(), variantExactCandidates.end(), candidates_.front()) !=
                variantExactCandidates.end()) {
                return {};
            }
        }
    }
    std::size_t variantOrder = 0;
    for (const auto &variant : variants) {
        if (exactPreeditHasCandidates && variant.priority >= 0) {
            ++variantOrder;
            continue;
        }
        auto candidates = dictionary_.exactKnownCandidates(variant.preedit);
        const bool exactKnownVariant = !candidates.empty();
        const std::size_t maxCandidatesForVariant = exactKnownVariant ? 1 : 2;
        if (!exactKnownVariant) {
            candidates = dictionary_.lookup(variant.preedit);
        }
        std::size_t emittedForVariant = 0;
        for (auto &candidate : candidates) {
            if (candidate.empty() || containsAsciiLetter(candidate) ||
                std::find(currentlyVisible.begin(), currentlyVisible.end(), candidate) != currentlyVisible.end() ||
                std::find_if(correctionCandidates.begin(), correctionCandidates.end(), [&candidate](const auto &existing) {
                    return existing.text == candidate;
                }) != correctionCandidates.end()) {
                continue;
            }
            correctionCandidates.push_back({std::move(candidate), variant.priority, variantOrder, exactKnownVariant});
            if (++emittedForVariant >= maxCandidatesForVariant || correctionCandidates.size() >= 24) {
                break;
            }
        }
        ++variantOrder;
    }
    std::stable_sort(correctionCandidates.begin(), correctionCandidates.end(),
                     [](const auto &lhs, const auto &rhs) {
                         if (lhs.exactKnown != rhs.exactKnown) {
                             return lhs.exactKnown;
                         }
                         if (lhs.priority != rhs.priority) {
                             return lhs.priority < rhs.priority;
                         }
                         const auto lhsCodepoints = utf8CodepointCount(lhs.text);
                         const auto rhsCodepoints = utf8CodepointCount(rhs.text);
                         if (lhsCodepoints != rhsCodepoints) {
                             return lhsCodepoints > rhsCodepoints;
                         }
                         return lhs.variantOrder < rhs.variantOrder;
                     });
    if (correctionCandidates.size() > 6) {
        correctionCandidates.resize(6);
    }
    std::vector<std::string> result;
    result.reserve(correctionCandidates.size());
    for (auto &candidate : correctionCandidates) {
        result.push_back(std::move(candidate.text));
    }
    return result;
}

std::vector<ModelRequestState::CandidateMetadata> State::modelCandidateMetadata() const {
    std::vector<ModelRequestState::CandidateMetadata> metadata;
    metadata.reserve(candidates_.size());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        if (index < candidateMetadata_.size()) {
            metadata.push_back({candidateMetadata_[index].consumedPrefixLength, candidateMetadata_[index].source,
                                candidateMetadata_[index].score});
        } else {
            metadata.push_back({});
        }
    }
    return metadata;
}

void State::refreshCandidates() {
    inputModel_.reloadPreferencesIfChanged();
    fullCorrectionCandidates_.clear();
    candidateMetadata_.clear();
    candidates_ = dictionary_.lookup(preedit_);
    setCandidateMetadataDefaults("lookup", 500000);
    if (const auto syllables = shortestPinyinSyllableCount(preedit_)) {
        for (auto &candidate : inputModel_.learnedCandidatePreferences(preedit_)) {
            if (containsAsciiLetter(candidate) || utf8CodepointCount(candidate) != *syllables ||
                std::find(candidates_.begin(), candidates_.end(), candidate) != candidates_.end()) {
                continue;
            }
            insertCandidate(candidates_.size(), std::move(candidate), 0, "learned-preference", 550000);
        }
    }
    const auto exactUserCandidates = dictionary_.exactUserCandidates(preedit_);
    const auto exactKnownCandidates = dictionary_.exactKnownCandidates(preedit_);
    auto hasReliableSegmentedCandidate = [this] {
        if (preedit_.size() > maxKnownPrefixScanLength || candidates_.empty()) {
            return false;
        }
        for (const auto prefixLength : pinyinPrefixLengths(preedit_, 12)) {
            const auto remaining = std::string_view(preedit_).substr(prefixLength);
            if (!pinyinSyllableCount(remaining)) {
                continue;
            }
            auto prefixCandidates = dictionary_.exactKnownCandidates(std::string_view(preedit_).substr(0, prefixLength));
            auto suffixCandidates = dictionary_.exactKnownCandidates(remaining);
            if (prefixCandidates.empty() || suffixCandidates.empty()) {
                continue;
            }
            const auto prefixLimit = std::min<std::size_t>(prefixCandidates.size(), 4);
            const auto suffixLimit = std::min<std::size_t>(suffixCandidates.size(), 4);
            for (std::size_t prefixIndex = 0; prefixIndex < prefixLimit; ++prefixIndex) {
                for (std::size_t suffixIndex = 0; suffixIndex < suffixLimit; ++suffixIndex) {
                    if (utf8CodepointCount(prefixCandidates[prefixIndex]) <= 1 &&
                        utf8CodepointCount(suffixCandidates[suffixIndex]) <= 1) {
                        continue;
                    }
                    const auto combined = prefixCandidates[prefixIndex] + suffixCandidates[suffixIndex];
                    if (std::find(candidates_.begin(), candidates_.end(), combined) != candidates_.end()) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    std::vector<std::string> correctionCandidates;
    std::vector<std::string> leadingCorrectionCandidates;
    std::unordered_set<std::string> fullCorrectionCandidateTexts;
    auto learnedCorrections = inputModel_.learnedCorrections(preedit_, [this](std::string_view correction) {
        return correctionTransferPriority(dictionary_, preedit_, correction);
    });
    const bool hasExactLearnedCorrection = inputModel_.hasExactLearnedCorrection(preedit_);
    if (!hasExactLearnedCorrection && !learnedCorrections.empty() &&
        (inputModel_.hasNonRawCandidatePreferenceEvidence(preedit_) ||
         dictionary_.hasConfidentLanguageModelSentence(preedit_))) {
        learnedCorrections.clear();
    }
    const bool hasLearnedTransposeCorrection =
        std::any_of(learnedCorrections.begin(), learnedCorrections.end(), [this](const std::string &correction) {
            return isAdjacentTransposeCorrection(preedit_, correction);
        });
    const bool promoteLearnedTranspose = hasLearnedTransposeCorrection;
    const bool correctionBaseAllowed = exactKnownCandidates.empty() && !hasReliableSegmentedCandidate();
    const bool hasActiveHabitSuggestions = exactKnownCandidates.empty() && inputModel_.hasActiveKeyHabits() &&
                                            !learnedCorrections.empty();
    const bool allowBroadCorrectionLookup =
        hasActiveHabitSuggestions || promoteLearnedTranspose ||
        (correctionBaseAllowed &&
         (candidateLooksLikeNoisySegmentation(preedit_, candidates_) || hasExactLearnedCorrection));
    auto localCorrections =
        correctionBaseAllowed && allowBroadCorrectionLookup && !hasExactLearnedCorrection
            ? exactLocalCorrectionCandidates(preedit_, dictionary_, candidates_)
            : std::vector<std::string>{};
    if ((correctionBaseAllowed || promoteLearnedTranspose || hasActiveHabitSuggestions) && localCorrections.empty()) {
        for (const auto &correction : learnedCorrections) {
            auto correctedCandidates = dictionary_.exactKnownCandidates(correction);
            if (!correctedCandidates.empty()) {
                fullCorrectionCandidateTexts.insert(correctedCandidates.begin(), correctedCandidates.end());
            }
            if (allowBroadCorrectionLookup) {
                auto lookupCandidates = dictionary_.lookup(correction);
                if (!hasExactLearnedCorrection) {
                    const auto correctionSyllables = shortestPinyinSyllableCount(correction);
                    lookupCandidates.erase(
                        std::remove_if(lookupCandidates.begin(), lookupCandidates.end(),
                                       [correctionSyllables](const std::string &candidate) {
                                           return !correctionSyllables || containsAsciiLetter(candidate) ||
                                                  utf8CodepointCount(candidate) != *correctionSyllables;
                                       }),
                        lookupCandidates.end());
                    if (lookupCandidates.size() > 4) {
                        lookupCandidates.resize(4);
                    }
                }
                correctedCandidates.insert(correctedCandidates.end(), std::make_move_iterator(lookupCandidates.begin()),
                                           std::make_move_iterator(lookupCandidates.end()));
            }
            if (!hasExactLearnedCorrection && !hasActiveHabitSuggestions && !promoteLearnedTranspose &&
                std::any_of(correctedCandidates.begin(), correctedCandidates.end(),
                            [this](const std::string &correctedCandidate) {
                                return std::any_of(candidates_.begin(), candidates_.end(),
                                                   [&correctedCandidate](const std::string &currentCandidate) {
                                                       return utf8CodepointCount(currentCandidate) >= 2 &&
                                                              correctedCandidate.size() > currentCandidate.size() &&
                                                              std::string_view(correctedCandidate).find(currentCandidate) !=
                                                                  std::string_view::npos;
                                                   });
                            })) {
                continue;
            }
            if (hasExactLearnedCorrection && fullCorrectionCandidateTexts.empty() && !correctedCandidates.empty()) {
                fullCorrectionCandidateTexts.insert(correctedCandidates.front());
            }
            correctionCandidates.insert(correctionCandidates.end(),
                                        std::make_move_iterator(correctedCandidates.begin()),
                                        std::make_move_iterator(correctedCandidates.end()));
            if (hasActiveHabitSuggestions && !correctedCandidates.empty()) {
                break;
            }
        }
    }
    if (!correctionCandidates.empty()) {
        std::vector<std::string> merged;
        merged.reserve(correctionCandidates.size() + candidates_.size());
        auto appendUnique = [&merged](std::string candidate) {
            if (std::find(merged.begin(), merged.end(), candidate) == merged.end()) {
                merged.push_back(std::move(candidate));
            }
        };
        const bool learnedSuffixCompletion = std::any_of(
            learnedCorrections.begin(), learnedCorrections.end(), [this](const std::string &correction) {
                return correction.size() > preedit_.size() && correction.size() <= preedit_.size() + 2 &&
                       std::string_view(correction).starts_with(preedit_);
            });
        const bool promoteBroadCorrections = !hasExactLearnedCorrection &&
                                             (candidateLooksLikePhoneticFallback(candidates_) ||
                                              learnedSuffixCompletion || promoteLearnedTranspose ||
                                              (inputModel_.hasActiveKeyHabits() && !learnedCorrections.empty()));
        if (hasExactLearnedCorrection || promoteBroadCorrections) {
            for (auto &candidate : correctionCandidates) {
                if (std::find(merged.begin(), merged.end(), candidate) == merged.end()) {
                    if (fullCorrectionCandidateTexts.contains(candidate)) {
                        fullCorrectionCandidates_.insert(candidate);
                        if (hasExactLearnedCorrection || promoteBroadCorrections) {
                            leadingCorrectionCandidates.push_back(candidate);
                        }
                    }
                    merged.push_back(std::move(candidate));
                }
            }
            for (auto &candidate : candidates_) {
                appendUnique(std::move(candidate));
            }
        } else {
            for (auto &candidate : candidates_) {
                appendUnique(std::move(candidate));
            }
            for (auto &candidate : correctionCandidates) {
                if (fullCorrectionCandidateTexts.contains(candidate)) {
                    fullCorrectionCandidates_.insert(candidate);
                }
                appendUnique(std::move(candidate));
            }
        }
        candidates_ = std::move(merged);
        setCandidateMetadataDefaults("correction-merge", 650000);
    }
    // Ranking must see prefix-consumption metadata before it can promote a learned candidate.
    // Limit the early pass to candidates with active evidence; the complete annotation pass
    // later in the pipeline remains the only cost for ordinary input.
    annotateLearnedPreferenceCandidateMetadata();
    {
        const auto originalCandidates = candidates_;
        const auto originalMetadata = candidateMetadata_;
        ModelRequestState rankingState;
        rankingState.candidateMetadata = modelCandidateMetadata();
        candidates_ = inputModel_.applyLearnedPreferences(preedit_, candidates_, std::move(rankingState));
        setCandidateMetadataDefaults("learned-rank", 600000);
        preserveCandidateMetadataFrom(originalCandidates, originalMetadata, "learned-rank");
    }
    std::size_t localLeadingCorrectionCount = 0;
    if (!localCorrections.empty()) {
        for (auto iter = localCorrections.rbegin(); iter != localCorrections.rend(); ++iter) {
            for (;;) {
                const auto existing = std::find(candidates_.begin(), candidates_.end(), *iter);
                if (existing == candidates_.end()) {
                    break;
                }
                eraseCandidate(static_cast<std::size_t>(std::distance(candidates_.begin(), existing)));
            }
            insertCandidate(0, *iter, 0, "local-correction", 850000);
            fullCorrectionCandidates_.insert(*iter);
            ++localLeadingCorrectionCount;
        }
        for (std::size_t index = localLeadingCorrectionCount; index < candidates_.size();) {
            const bool extendsLocalCorrection = std::any_of(localCorrections.begin(), localCorrections.end(),
                                                            [&candidate = candidates_[index]](const auto &correction) {
                                                                return candidate.size() > correction.size() &&
                                                                       std::string_view(candidate).starts_with(correction);
                                                            });
            if (utf8CodepointCount(candidates_[index]) > 1 && !extendsLocalCorrection &&
                std::find(exactUserCandidates.begin(), exactUserCandidates.end(), candidates_[index]) ==
                    exactUserCandidates.end()) {
                eraseCandidate(index);
            } else {
                ++index;
            }
        }
    }
    if (!leadingCorrectionCandidates.empty()) {
        for (std::size_t index = localLeadingCorrectionCount; index < candidates_.size();) {
            if (fullCorrectionCandidates_.contains(candidates_[index])) {
                eraseCandidate(index);
            } else {
                ++index;
            }
        }
        for (auto iter = leadingCorrectionCandidates.rbegin(); iter != leadingCorrectionCandidates.rend(); ++iter) {
            if (std::find(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(
                              std::min(localLeadingCorrectionCount, candidates_.size())),
                          *iter) != candidates_.begin() + static_cast<std::ptrdiff_t>(
                              std::min(localLeadingCorrectionCount, candidates_.size()))) {
                continue;
            }
            insertCandidate(localLeadingCorrectionCount, *iter, 0, "correction", 950000);
        }
    }
    const char *systemDictionaryOverride = std::getenv("TIPE_SYSTEM_RIME_DICTIONARY");
    const bool exactKnownCanOverrideLanguageModel =
        !exactUserCandidates.empty() || (systemDictionaryOverride && *systemDictionaryOverride);
    if (fullCorrectionCandidates_.empty() && !candidates_.empty() && !exactKnownCandidates.empty() &&
        exactKnownCanOverrideLanguageModel &&
        std::find(exactKnownCandidates.begin(), exactKnownCandidates.end(), candidates_.front()) ==
            exactKnownCandidates.end()) {
        const auto preferredExact =
            std::find(candidates_.begin(), candidates_.end(), exactKnownCandidates.front());
        if (preferredExact != candidates_.end()) {
            const auto index = static_cast<std::size_t>(std::distance(candidates_.begin(), preferredExact));
            auto candidate = candidates_[index];
            const auto consumedPrefixLength = candidateConsumedPrefixLengthAt(index);
            eraseCandidate(index);
            insertCandidate(0, std::move(candidate), consumedPrefixLength, "exact", 800000);
        }
    }
    const auto learnedSegmentCandidates = inputModel_.learnedSegmentCandidates(preedit_);
    for (auto iter = learnedSegmentCandidates.rbegin(); iter != learnedSegmentCandidates.rend(); ++iter) {
        for (std::size_t index = 0; index < candidates_.size();) {
            if (candidates_[index] == *iter) {
                eraseCandidate(index);
            } else {
                ++index;
            }
        }
        insertCandidate(0, *iter, 0, "learned-segment-chain", 980000);
    }
    std::size_t protectedLeadingCandidates = learnedSegmentCandidates.size();
    while (protectedLeadingCandidates < candidates_.size() &&
           protectedLeadingCandidates < candidateMetadata_.size() &&
           (candidateMetadata_[protectedLeadingCandidates].source == "local-correction" ||
            candidateMetadata_[protectedLeadingCandidates].source == "correction")) {
        ++protectedLeadingCandidates;
    }
    for (const auto &candidate : exactUserCandidates) {
        if (protectedLeadingCandidates >= candidates_.size() || candidates_[protectedLeadingCandidates] != candidate) {
            break;
        }
        ++protectedLeadingCandidates;
    }
    insertPrefixCandidates(protectedLeadingCandidates);
    annotatePartialCandidateMetadata(protectedLeadingCandidates);
    promotePrefixContinuationCandidates(protectedLeadingCandidates);
    promoteSingleSyllableCandidates(protectedLeadingCandidates);
    promotePendingSegmentChainCandidates(protectedLeadingCandidates);
    demoteDivergentLongCandidates(protectedLeadingCandidates);
    const bool shouldOfferRawCandidate =
        fullCorrectionCandidates_.empty() && looksLikeEnglishIdentifier(preedit_) &&
        !canCompletePinyinTail(preedit_) &&
        std::find(candidates_.begin(), candidates_.end(), preedit_) == candidates_.end();
    if (shouldOfferRawCandidate) {
        const std::size_t insertIndex =
            std::min(candidates_.size(), std::max<std::size_t>(protectedLeadingCandidates, candidates_.empty() ? 0 : 1));
        insertCandidate(insertIndex, preedit_, 0, "raw-offer", 550000);
    }
    if (fullCorrectionCandidates_.empty() && inputModel_.shouldPreferRaw(preedit_, candidates_)) {
        for (std::size_t index = 0; index < candidates_.size();) {
            if (candidates_[index] == preedit_) {
                eraseCandidate(index);
            } else {
                ++index;
            }
        }
        insertCandidate(0, preedit_, 0, "raw", 900000);
    }
    finalizeCandidateScores();
    clampCandidateCursor();
}

void State::insertPrefixCandidates(std::size_t protectedLeadingCandidates) {
    const auto prefixLengths = pinyinPrefixLengths(preedit_, 12);
    const bool compactLongExactPrefixes =
        preedit_.size() > 10 && !dictionary_.exactKnownCandidates(preedit_).empty();
    const bool preeditCanComplete = canCompletePinyinTail(preedit_);

    auto isLeadingTextOfBestFullCandidate = [this](const std::string &candidate) {
        return !candidates_.empty() && candidates_.front().size() > candidate.size() &&
               std::string_view(candidates_.front()).starts_with(candidate);
    };

    std::vector<PrefixCandidateGroup> prefixCandidateGroups;
    std::vector<PrefixCandidateGroup> deferredPrefixCandidateGroups;
    std::size_t syllableCount = 0;
    for (const auto prefixLength : prefixLengths) {
        ++syllableCount;
        const auto remaining = std::string_view(preedit_).substr(prefixLength);
        const bool noisyRecoveryPrefix = !preeditCanComplete && syllableCount <= 2;
        if (!canCompletePinyinTail(remaining) && !(compactLongExactPrefixes && syllableCount <= 2) &&
            !noisyRecoveryPrefix) {
            continue;
        }
        const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
        std::vector<std::string> candidatesForPrefix;
        if (syllableCount == 1) {
            candidatesForPrefix = singleCharacterCandidates(prefixPinyin, dictionary_);
        }
        auto dictionaryCandidates = dictionary_.exactKnownCandidates(prefixPinyin);
        candidatesForPrefix.insert(candidatesForPrefix.end(), std::make_move_iterator(dictionaryCandidates.begin()),
                                   std::make_move_iterator(dictionaryCandidates.end()));
        if (compactLongExactPrefixes && syllableCount == 2) {
            auto backendPrefixCandidates = dictionary_.lookup(prefixPinyin);
            std::size_t added = 0;
            for (auto &candidate : backendPrefixCandidates) {
                if (candidate.empty() || containsAsciiLetter(candidate) ||
                    utf8CodepointCount(candidate) != syllableCount ||
                    std::find(candidatesForPrefix.begin(), candidatesForPrefix.end(), candidate) !=
                        candidatesForPrefix.end()) {
                    continue;
                }
                candidatesForPrefix.push_back(std::move(candidate));
                if (++added >= 4) {
                    break;
                }
            }
        }
        candidatesForPrefix.erase(std::remove_if(candidatesForPrefix.begin(), candidatesForPrefix.end(),
                                                 [syllableCount, prefixPinyin, &isLeadingTextOfBestFullCandidate,
                                                  compactLongExactPrefixes, this](const std::string &candidate) {
                                                     const bool mixedEnglishPrefix =
                                                         prefixCoversMixedEnglishCandidate(prefixPinyin, candidate, candidates_);
                                                     const bool compactAlternativePrefix =
                                                         compactLongExactPrefixes && syllableCount == 2 &&
                                                         utf8CodepointCount(candidate) == syllableCount;
                                                     const bool exactSyllablePrefix =
                                                         utf8CodepointCount(candidate) == syllableCount;
                                                     return candidate.empty() ||
                                                            (utf8CodepointCount(candidate) != syllableCount &&
                                                             !mixedEnglishPrefix) ||
                                                            (syllableCount > 1 && !compactAlternativePrefix &&
                                                             !exactSyllablePrefix &&
                                                             !isLeadingTextOfBestFullCandidate(candidate));
                                                 }),
                                  candidatesForPrefix.end());
        if (compactLongExactPrefixes && syllableCount == 1 && candidatesForPrefix.size() > 2) {
            candidatesForPrefix.resize(2);
        }
        if (!candidatesForPrefix.empty()) {
            prefixCandidateGroups.push_back({syllableCount, prefixLength, std::move(candidatesForPrefix)});
        }
    }

    if (preedit_.size() <= maxKnownPrefixScanLength) {
        for (std::size_t prefixLength = 1; prefixLength < preedit_.size(); ++prefixLength) {
            const auto remaining = std::string_view(preedit_).substr(prefixLength);
            if (!canCompletePinyinTail(remaining)) {
                continue;
            }
            const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
            auto candidatesForPrefix = dictionary_.exactKnownCandidates(prefixPinyin);
            auto deferredCandidates = candidatesForPrefix;
            deferredCandidates.erase(
                std::remove_if(deferredCandidates.begin(), deferredCandidates.end(), [](const std::string &candidate) {
                    return candidate.empty() || containsAsciiLetter(candidate);
                }),
                deferredCandidates.end());
            if (!deferredCandidates.empty()) {
                deferredPrefixCandidateGroups.push_back(
                    {0, prefixLength, std::move(deferredCandidates)});
            }
            candidatesForPrefix.erase(std::remove_if(candidatesForPrefix.begin(), candidatesForPrefix.end(),
                                                     [&isLeadingTextOfBestFullCandidate](const std::string &candidate) {
                                                         return candidate.empty() ||
                                                                !isLeadingTextOfBestFullCandidate(candidate);
                                                     }),
                                      candidatesForPrefix.end());
            if (!candidatesForPrefix.empty()) {
                prefixCandidateGroups.push_back({500 + prefixLength, prefixLength, std::move(candidatesForPrefix)});
            }
        }
    }

    std::vector<std::size_t> mixedPrefixLengths;
    for (const auto &candidate : candidates_) {
        if (const auto range = mixedEnglishPrefixScanRange(preedit_, longestAsciiLetterRun(candidate))) {
            for (std::size_t prefixLength = range->first; prefixLength < range->second; ++prefixLength) {
                if (std::find(mixedPrefixLengths.begin(), mixedPrefixLengths.end(), prefixLength) ==
                    mixedPrefixLengths.end()) {
                    mixedPrefixLengths.push_back(prefixLength);
                }
            }
        }
    }
    std::sort(mixedPrefixLengths.begin(), mixedPrefixLengths.end());
    for (const auto prefixLength : mixedPrefixLengths) {
        const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
        auto candidatesForPrefix = dictionary_.exactKnownCandidates(prefixPinyin);
        candidatesForPrefix.erase(std::remove_if(candidatesForPrefix.begin(), candidatesForPrefix.end(),
                                                 [prefixPinyin, this](const std::string &candidate) {
                                                     return !prefixCoversMixedEnglishCandidate(prefixPinyin, candidate,
                                                                                               candidates_);
                                                 }),
                                  candidatesForPrefix.end());
        if (!candidatesForPrefix.empty()) {
            prefixCandidateGroups.push_back({1000 + prefixLength, prefixLength, std::move(candidatesForPrefix)});
        }
    }

    std::stable_sort(prefixCandidateGroups.begin(), prefixCandidateGroups.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.rank > rhs.rank;
    });

    std::vector<PrefixCandidateEntry> prefixCandidates;
    auto appendUnique = [&prefixCandidates](std::string candidate, std::size_t prefixLength) {
        if (std::find_if(prefixCandidates.begin(), prefixCandidates.end(), [&candidate](const auto &entry) {
                return entry.text == candidate;
            }) == prefixCandidates.end()) {
            prefixCandidates.push_back({std::move(candidate), prefixLength});
        }
    };
    for (const auto &group : prefixCandidateGroups) {
        appendUnique(group.candidates.front(), group.prefixLength);
    }
    std::stable_sort(deferredPrefixCandidateGroups.begin(), deferredPrefixCandidateGroups.end(),
                     [](const auto &lhs, const auto &rhs) {
                         return lhs.prefixLength > rhs.prefixLength;
                     });
    prefixCandidateGroups.insert(prefixCandidateGroups.end(),
                                 std::make_move_iterator(deferredPrefixCandidateGroups.begin()),
                                 std::make_move_iterator(deferredPrefixCandidateGroups.end()));

    protectedLeadingCandidates = std::min(protectedLeadingCandidates, candidates_.size());
    const std::size_t defaultInsertionOffset = candidates_.empty() ? 0 : 1;
    std::size_t insertIndex = std::max(defaultInsertionOffset, protectedLeadingCandidates);
    std::size_t inserted = 0;
    for (auto &candidate : prefixCandidates) {
        const auto prefixLength = candidate.prefixLength;
        if (const auto existing = std::find(candidates_.begin(), candidates_.end(), candidate.text);
            existing != candidates_.end()) {
            const auto existingIndex = static_cast<std::size_t>(std::distance(candidates_.begin(), existing));
            if (existingIndex < protectedLeadingCandidates || existingIndex < insertIndex) {
                if (existingIndex > 0 && existingIndex >= protectedLeadingCandidates && prefixLength > 0 &&
                    prefixLength < preedit_.size()) {
                    ensureCandidateMetadata();
                    auto &metadata = candidateMetadata_[existingIndex];
                    if (metadata.consumedPrefixLength == 0) {
                        metadata.consumedPrefixLength = prefixLength;
                    }
                    if (metadata.source.rfind("prefix", 0) != 0) {
                        metadata.source = metadata.source.empty() ? "prefix-existing" : "prefix-existing:" + metadata.source;
                    }
                }
                continue;
            }
            eraseCandidate(existingIndex);
            if (existingIndex < insertIndex) {
                --insertIndex;
            }
        }
        insertCandidate(insertIndex, std::move(candidate.text),
                        prefixLength > 0 && prefixLength < preedit_.size() ? prefixLength : 0, "prefix",
                        700000 - static_cast<int>(std::min<std::size_t>(inserted, 1000)));
        ++insertIndex;
        const std::size_t insertLimit = compactLongExactPrefixes ? 6 : 8;
        if (++inserted >= insertLimit) {
            break;
        }
    }

    std::size_t deferredAlternatives = 0;
    for (const auto &group : prefixCandidateGroups) {
        const auto limit = std::min<std::size_t>(group.candidates.size(), 4);
        for (std::size_t index = 0; index < limit; ++index) {
            const auto &candidate = group.candidates[index];
            if (candidate.empty() || std::find(candidates_.begin(), candidates_.end(), candidate) != candidates_.end()) {
                continue;
            }
            insertCandidate(candidates_.size(), candidate,
                            group.prefixLength > 0 && group.prefixLength < preedit_.size()
                                ? group.prefixLength
                                : 0,
                            "prefix-alternative", 650000 - static_cast<int>(deferredAlternatives));
            if (++deferredAlternatives >= 6) {
                return;
            }
        }
    }
}

void State::promoteSingleSyllableCandidates(std::size_t protectedLeadingCandidates) {
    if (candidates_.size() < 2 || isKnownEnglishToken(preedit_)) {
        return;
    }

    if (!isSinglePinyinSyllableOrPrefix(preedit_)) {
        return;
    }

    protectedLeadingCandidates = std::min(protectedLeadingCandidates, candidates_.size());
    ensureCandidateMetadata();
    std::vector<std::pair<std::string, CandidateMetadata>> tail;
    tail.reserve(candidates_.size() - protectedLeadingCandidates);
    for (std::size_t index = protectedLeadingCandidates; index < candidates_.size(); ++index) {
        tail.emplace_back(std::move(candidates_[index]), std::move(candidateMetadata_[index]));
    }
    const auto singleEnd = std::stable_partition(tail.begin(), tail.end(), [](const auto &candidate) {
        return utf8CodepointCount(candidate.first) == 1;
    });
    const bool changed = singleEnd != tail.begin();
    candidates_.resize(protectedLeadingCandidates);
    candidateMetadata_.resize(protectedLeadingCandidates);
    for (auto &candidate : tail) {
        candidates_.push_back(std::move(candidate.first));
        candidateMetadata_.push_back(std::move(candidate.second));
    }
    if (changed) {
        candidateCursorIndex_ = 0;
    }
}

void State::promotePrefixContinuationCandidates(std::size_t protectedLeadingCandidates) {
    if (preedit_.size() < 4 || candidates_.size() < 2) {
        return;
    }
    if (dictionary_.hasDecisiveLanguageModelSentence(preedit_)) {
        return;
    }
    if (const auto syllableCount = shortestPinyinSyllableCount(preedit_);
        syllableCount && utf8CodepointCount(candidates_.front()) == *syllableCount) {
        return;
    }
    if (!dictionary_.exactKnownCandidates(preedit_).empty()) {
        return;
    }

    struct PrefixText {
        std::string text;
        std::size_t pinyinLength = 0;
    };
    std::vector<PrefixText> prefixes;
    for (std::size_t prefixLength = 2; prefixLength + 1 <= preedit_.size(); ++prefixLength) {
        const auto remainingLength = preedit_.size() - prefixLength;
        if (remainingLength > 3) {
            continue;
        }
        const auto remainingPinyin = std::string_view(preedit_).substr(prefixLength);
        if (remainingLength == 3 && !pinyinSyllableCount(remainingPinyin)) {
            continue;
        }
        const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
        const auto syllableCount = pinyinSyllableCount(prefixPinyin);
        if (!syllableCount || *syllableCount < 2) {
            continue;
        }
        auto prefixCandidates = dictionary_.exactKnownCandidates(prefixPinyin);
        const auto limit = std::min<std::size_t>(prefixCandidates.size(), 4);
        for (std::size_t index = 0; index < limit; ++index) {
            auto &prefix = prefixCandidates[index];
            if (prefix.empty() || utf8CodepointCount(prefix) < 2) {
                continue;
            }
            if (std::find_if(prefixes.begin(), prefixes.end(), [&prefix](const auto &existing) {
                    return existing.text == prefix;
                }) == prefixes.end()) {
                prefixes.push_back({std::move(prefix), prefixLength});
            }
        }
    }
    if (prefixes.empty()) {
        return;
    }

    if (std::any_of(prefixes.begin(), prefixes.end(), [this](const auto &prefix) {
            return std::string_view(candidates_.front()).starts_with(prefix.text);
        })) {
        return;
    }

    const auto exactKnownCandidates = dictionary_.exactKnownCandidates(preedit_);
    if (!exactKnownCandidates.empty() && !candidates_.empty() && candidates_.front() == exactKnownCandidates.front()) {
        return;
    }

    protectedLeadingCandidates = std::min(protectedLeadingCandidates, candidates_.size());
    auto promotablePrefix = [&](const std::string &candidate) -> const PrefixText * {
        const auto prefix = std::find_if(prefixes.begin(), prefixes.end(), [&candidate, this](const auto &prefix) {
            if (candidate.size() <= prefix.text.size() || !std::string_view(candidate).starts_with(prefix.text)) {
                return false;
            }
            const auto remainingPinyin = std::string_view(preedit_).substr(prefix.pinyinLength);
            if (pinyinSyllableCount(remainingPinyin) != 1) {
                return true;
            }
            const auto suffix = std::string_view(candidate).substr(prefix.text.size());
            const auto commonTailCandidates = singleCharacterCandidates(remainingPinyin, dictionary_);
            return std::find(commonTailCandidates.begin(), commonTailCandidates.end(), suffix) !=
                   commonTailCandidates.end();
        });
        return prefix == prefixes.end() ? nullptr : &*prefix;
    };
    const auto firstPromotable = std::find_if(candidates_.begin() + static_cast<std::ptrdiff_t>(protectedLeadingCandidates),
                                              candidates_.end(), [&](const auto &candidate) {
                                                  return promotablePrefix(candidate) != nullptr;
                                              });
    if (firstPromotable == candidates_.end()) {
        return;
    }

    const auto index = static_cast<std::size_t>(std::distance(candidates_.begin(), firstPromotable));
    auto candidate = candidates_[index];
    ensureCandidateMetadata();
    auto metadata = candidateMetadata_[index];
    eraseCandidate(index);
    metadata.consumedPrefixLength = preedit_.size();
    metadata.source = metadata.source.empty() ? "prefix-continuation" : "prefix-continuation:" + metadata.source;
    insertCandidate(protectedLeadingCandidates, std::move(candidate), metadata.consumedPrefixLength,
                    std::move(metadata.source), 780000);
    candidateCursorIndex_ = protectedLeadingCandidates;
}

void State::annotateLearnedPreferenceCandidateMetadata() {
    if (preedit_.size() <= 1 || candidates_.size() < 2) {
        return;
    }

    ensureCandidateMetadata();
    for (std::size_t index = 1; index < candidates_.size(); ++index) {
        if (!inputModel_.hasActiveCandidatePreference(preedit_, candidates_[index])) {
            continue;
        }
        annotatePartialCandidateMetadataAt(index);
    }
}

void State::annotatePartialCandidateMetadata(std::size_t protectedLeadingCandidates) {
    if (preedit_.size() <= 1 || candidates_.empty()) {
        return;
    }

    ensureCandidateMetadata();
    const auto startIndex = std::max<std::size_t>(1, std::min(protectedLeadingCandidates, candidates_.size()));
    for (std::size_t index = startIndex; index < candidates_.size(); ++index) {
        annotatePartialCandidateMetadataAt(index);
    }
}

void State::annotatePartialCandidateMetadataAt(std::size_t index) {
    if (index >= candidates_.size() || index >= candidateMetadata_.size()) {
        return;
    }
    auto &metadata = candidateMetadata_[index];
    if (metadata.consumedPrefixLength > 0 || fullCorrectionCandidates_.contains(candidates_[index])) {
        return;
    }
    auto prefixLength = partialCommitPrefixLength(candidates_[index]);
    if (!prefixLength && preedit_.size() <= maxAutomaticApproximatePrefixLength) {
        prefixLength = approximatePartialCommitPrefixLength(candidates_[index]);
    }
    if (!prefixLength || *prefixLength == 0 || *prefixLength >= preedit_.size()) {
        return;
    }
    metadata.consumedPrefixLength = *prefixLength;
    if (metadata.source.rfind("prefix", 0) != 0 && metadata.source.rfind("partial", 0) != 0) {
        metadata.source = metadata.source.empty() ? "partial" : "partial:" + metadata.source;
    }
}

void State::promotePendingSegmentChainCandidates(std::size_t protectedLeadingCandidates) {
    if (!pendingSegmentChain_ || preedit_.empty() || candidates_.empty()) {
        return;
    }

    std::vector<std::pair<std::string, CandidateMetadata>> promoted;
    ensureCandidateMetadata();
    const auto fullPreedit = pendingSegmentChain_->consumedPreedit + preedit_;
    auto fullCandidates = inputModel_.learnedSegmentCandidates(pendingSegmentChain_->originalPreedit);
    if (fullPreedit != pendingSegmentChain_->originalPreedit) {
        for (auto &candidate : inputModel_.learnedSegmentCandidates(fullPreedit)) {
            if (std::find(fullCandidates.begin(), fullCandidates.end(), candidate) == fullCandidates.end()) {
                fullCandidates.push_back(std::move(candidate));
            }
        }
    }
    for (auto &candidate : dictionary_.exactKnownCandidates(fullPreedit)) {
        if (std::find(fullCandidates.begin(), fullCandidates.end(), candidate) == fullCandidates.end()) {
            fullCandidates.push_back(std::move(candidate));
        }
    }
    for (const auto &fullCandidate : fullCandidates) {
        if (!std::string_view(fullCandidate).starts_with(pendingSegmentChain_->committedText) ||
            fullCandidate.size() <= pendingSegmentChain_->committedText.size()) {
            continue;
        }
        auto suffix = fullCandidate.substr(pendingSegmentChain_->committedText.size());
        const auto existing = std::find(candidates_.begin(), candidates_.end(), suffix);
        CandidateMetadata metadata{0, "pending-segment-exact", 820000};
        if (existing != candidates_.end()) {
            metadata = candidateMetadata_[static_cast<std::size_t>(std::distance(candidates_.begin(), existing))];
        }
        if (std::find_if(promoted.begin(), promoted.end(), [&suffix](const auto &entry) {
                return entry.first == suffix;
            }) == promoted.end()) {
            promoted.emplace_back(std::move(suffix), std::move(metadata));
        }
    }
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const auto &candidate = candidates_[index];
        const auto correctedRemaining = correctedPreeditForCandidate(preedit_, candidate);
        if (!correctedRemaining) {
            continue;
        }
        const auto correctedFullPreedit = pendingSegmentChain_->consumedPreedit + *correctedRemaining;
        const auto combinedCandidate = pendingSegmentChain_->committedText + candidate;
        const auto fullCandidates = dictionary_.exactKnownCandidates(correctedFullPreedit);
        const auto alreadyPromoted = std::find_if(promoted.begin(), promoted.end(), [&candidate](const auto &entry) {
            return entry.first == candidate;
        });
        if (std::find(fullCandidates.begin(), fullCandidates.end(), combinedCandidate) != fullCandidates.end() &&
            alreadyPromoted == promoted.end()) {
            promoted.emplace_back(candidate, candidateMetadata_[index]);
        }
    }
    if (promoted.empty()) {
        return;
    }

    protectedLeadingCandidates = std::min(protectedLeadingCandidates, candidates_.size());
    for (const auto &candidate : promoted) {
        for (std::size_t index = 0; index < candidates_.size();) {
            if (candidates_[index] == candidate.first) {
                eraseCandidate(index);
            } else {
                ++index;
            }
        }
    }
    std::size_t insertIndex = protectedLeadingCandidates;
    for (auto &candidate : promoted) {
        insertCandidate(insertIndex, std::move(candidate.first), candidate.second.consumedPrefixLength,
                        std::move(candidate.second.source), candidate.second.score);
        ++insertIndex;
    }
    candidateCursorIndex_ = protectedLeadingCandidates;
}

void State::demoteDivergentLongCandidates(std::size_t protectedLeadingCandidates) {
    if (candidates_.size() < 3 || candidates_.front().empty()) {
        return;
    }
    if (dictionary_.hasDecisiveLanguageModelSentence(preedit_)) {
        return;
    }

    const auto bestLength = utf8CodepointCount(candidates_.front());
    if (bestLength < 6) {
        return;
    }

    const auto exactKnownCandidates = dictionary_.exactKnownCandidates(preedit_);
    if (std::find(exactKnownCandidates.begin(), exactKnownCandidates.end(), candidates_.front()) !=
        exactKnownCandidates.end()) {
        return;
    }

    std::vector<std::string> strongKnownPrefixes;
    std::vector<std::string> decoderAnchorPrefixes;
    std::size_t prefixSyllables = 0;
    for (const auto prefixLength : pinyinPrefixLengths(preedit_, 12)) {
        ++prefixSyllables;
        if (prefixLength >= preedit_.size()) {
            continue;
        }
        const auto remaining = std::string_view(preedit_).substr(prefixLength);
        if (!pinyinSyllableCount(remaining)) {
            continue;
        }
        const auto prefixPinyin = std::string_view(preedit_).substr(0, prefixLength);
        for (auto &prefix : dictionary_.exactKnownCandidates(prefixPinyin)) {
            if (utf8CodepointCount(prefix) < 4 ||
                std::find(strongKnownPrefixes.begin(), strongKnownPrefixes.end(), prefix) !=
                    strongKnownPrefixes.end()) {
                continue;
            }
            strongKnownPrefixes.push_back(std::move(prefix));
        }
        if (!decoderAnchorPrefixes.empty() || prefixSyllables < 2) {
            continue;
        }
        auto decodedPrefixes = dictionary_.lookup(prefixPinyin);
        const auto limit = std::min<std::size_t>(decodedPrefixes.size(), 4);
        for (std::size_t index = 0; index < limit; ++index) {
            auto &prefix = decodedPrefixes[index];
            if (utf8CodepointCount(prefix) != prefixSyllables ||
                std::find(decoderAnchorPrefixes.begin(), decoderAnchorPrefixes.end(), prefix) !=
                    decoderAnchorPrefixes.end()) {
                continue;
            }
            decoderAnchorPrefixes.push_back(std::move(prefix));
        }
    }
    if (strongKnownPrefixes.empty() && decoderAnchorPrefixes.empty()) {
        return;
    }

    protectedLeadingCandidates = std::min(protectedLeadingCandidates, candidates_.size());
    const auto demotionBegin = protectedLeadingCandidates;
    ensureCandidateMetadata();
    std::vector<std::pair<std::string, CandidateMetadata>> tail;
    tail.reserve(candidates_.size() - demotionBegin);
    for (std::size_t index = demotionBegin; index < candidates_.size(); ++index) {
        tail.emplace_back(std::move(candidates_[index]), std::move(candidateMetadata_[index]));
    }
    const auto strongPrefixLength = [&strongKnownPrefixes](const auto &candidate) {
        std::size_t best = 0;
        for (const auto &prefix : strongKnownPrefixes) {
            if (std::string_view(candidate.first).starts_with(prefix)) {
                best = std::max(best, utf8CodepointCount(prefix));
            }
        }
        return best;
    };
    const auto decoderPrefixRank = [&decoderAnchorPrefixes](const auto &candidate) {
        for (std::size_t index = 0; index < decoderAnchorPrefixes.size(); ++index) {
            if (std::string_view(candidate.first).starts_with(decoderAnchorPrefixes[index])) {
                return index;
            }
        }
        return decoderAnchorPrefixes.size();
    };
    const auto category = [&](const auto &candidate) {
        if (utf8CodepointCount(candidate.first) < 6) {
            return 2;
        }
        if (strongPrefixLength(candidate) > 0) {
            return 0;
        }
        return decoderPrefixRank(candidate) < decoderAnchorPrefixes.size() ? 1 : 3;
    };
    std::stable_sort(tail.begin(), tail.end(), [&](const auto &lhs, const auto &rhs) {
        const int lhsCategory = category(lhs);
        const int rhsCategory = category(rhs);
        if (lhsCategory != rhsCategory) {
            return lhsCategory < rhsCategory;
        }
        if (lhsCategory == 0) {
            return strongPrefixLength(lhs) > strongPrefixLength(rhs);
        }
        if (lhsCategory == 1) {
            return decoderPrefixRank(lhs) < decoderPrefixRank(rhs);
        }
        return false;
    });
    candidates_.resize(demotionBegin);
    candidateMetadata_.resize(demotionBegin);
    for (auto &candidate : tail) {
        candidates_.push_back(std::move(candidate.first));
        candidateMetadata_.push_back(std::move(candidate.second));
    }
}

void State::clampCandidateCursor() {
    if (candidates_.empty()) {
        candidateCursorIndex_ = 0;
    } else if (candidateCursorIndex_ >= candidates_.size()) {
        candidateCursorIndex_ = candidates_.size() - 1;
    }
}

} // namespace tipe
