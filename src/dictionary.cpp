#include "dictionary.h"
#include "english_tokens.h"
#include "pinyin_utils.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <charconv>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef TIPE_HAVE_LIBPINYIN
#include <glib.h>
#include <pinyin.h>
#endif

#ifdef TIPE_HAVE_LIBIME
#include <libime/core/historybigram.h>
#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>
#endif

namespace tipe {

namespace {

using FallbackEntryMap = std::unordered_map<std::string, std::vector<std::string>>;

const FallbackEntryMap &coreFallbackEntries() {
    static const FallbackEntryMap entries{
        {"nihao", {"你好", "你号", "拟好"}},
        {"ni", {"你", "呢", "拟", "尼", "逆"}},
        {"hao", {"好", "号", "浩", "耗", "豪"}},
        {"wo", {"我"}},
        {"cao", {"操", "曹", "草", "槽"}},
        {"jixu", {"继续", "积蓄", "急需", "几许"}},
        {"zuo", {"做", "作", "坐", "左", "座"}},
        {"shenglue", {"省略"}},
    };
    return entries;
}

FallbackEntryMap loadTestFallbackEntries(const std::filesystem::path &path) {
    FallbackEntryMap entries;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream row(line);
        std::string key;
        if (!std::getline(row, key, '\t') || key.empty() ||
            !std::all_of(key.begin(), key.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; })) {
            continue;
        }
        std::string value;
        auto &values = entries[key];
        while (std::getline(row, value, '\t')) {
            if (!value.empty() && value.find_first_of("\r\n") == std::string::npos &&
                std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(std::move(value));
            }
        }
        if (values.empty()) {
            entries.erase(key);
        }
    }
    return entries;
}

const FallbackEntryMap &activeFallbackEntries() {
    if (const char *fixturePath = std::getenv("TIPE_TEST_FALLBACK_DICTIONARY"); fixturePath && *fixturePath) {
        static std::string loadedPath;
        static FallbackEntryMap loadedEntries;
        if (loadedPath != fixturePath) {
            loadedPath = fixturePath;
            loadedEntries = loadTestFallbackEntries(loadedPath);
        }
        if (!loadedEntries.empty()) {
            return loadedEntries;
        }
    }
    return coreFallbackEntries();
}

struct FallbackPart {
    std::string_view key;
    std::vector<std::string> values;
};

using PartBuckets = std::array<std::vector<FallbackPart>, 26>;

struct WeightedCandidate {
    std::string text;
    int weight = 0;
    std::size_t sourceOrder = 0;
};

using EntryMap = std::unordered_map<std::string, std::vector<std::string>>;
using SortedEntryRefs = std::vector<std::pair<std::string_view, const std::vector<std::string> *>>;

constexpr std::size_t maxUserDictionaryRows = 4096;
constexpr std::size_t maxUserDictionaryCandidatesPerRow = 32;
constexpr std::size_t maxUserDictionaryPinyinBytes = 128;
constexpr std::size_t maxUserDictionaryCandidateBytes = 256;

std::vector<FallbackPart> fallbackParts(
    const FallbackEntryMap &entries) {
    std::vector<FallbackPart> parts;
    parts.reserve(entries.size() + knownEnglishTokens.size());
    for (const auto &[key, values] : entries) {
        if (!values.empty() && !key.empty()) {
            if (isKnownEnglishToken(key)) {
                parts.push_back({key, {std::string(key)}});
                continue;
            }
            const auto limit = std::min<std::size_t>(values.size(), 2);
            parts.push_back({key, {values.begin(), values.begin() + limit}});
        }
    }
    for (const auto token : knownEnglishTokens) {
        if (entries.find(std::string(token)) == entries.end()) {
            parts.push_back({token, {std::string(token)}});
        }
    }
    return parts;
}

std::vector<FallbackPart> systemParts(const EntryMap &entries) {
    std::vector<FallbackPart> parts;
    parts.reserve(entries.size());
    for (const auto &[key, values] : entries) {
        if (key.empty() || values.empty()) {
            continue;
        }
        const auto limit = std::min<std::size_t>(values.size(), key.size() <= 2 ? 1 : 2);
        parts.push_back({key, {values.begin(), values.begin() + limit}});
    }
    return parts;
}

SortedEntryRefs sortedEntryRefs(const EntryMap &entries) {
    SortedEntryRefs result;
    result.reserve(entries.size());
    for (const auto &[key, values] : entries) {
        if (!key.empty() && !values.empty()) {
            result.emplace_back(key, &values);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });
    return result;
}

void sortPartsByKeyLength(std::vector<FallbackPart> &parts) {
    std::sort(parts.begin(), parts.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.key.size() != rhs.key.size()) {
            return lhs.key.size() > rhs.key.size();
        }
        return lhs.key < rhs.key;
    });
}

PartBuckets bucketPartsByFirstLetter(std::vector<FallbackPart> parts) {
    PartBuckets buckets;
    for (auto &part : parts) {
        if (part.key.empty()) {
            continue;
        }
        const auto first = static_cast<unsigned char>(part.key.front());
        if (first < 'a' || first > 'z') {
            continue;
        }
        buckets[first - 'a'].push_back(std::move(part));
    }
    for (auto &bucket : buckets) {
        sortPartsByKeyLength(bucket);
    }
    return buckets;
}

const PartBuckets &cachedFallbackBuckets() {
    static const PartBuckets coreBuckets = bucketPartsByFirstLetter(fallbackParts(coreFallbackEntries()));
    if (const char *fixturePath = std::getenv("TIPE_TEST_FALLBACK_DICTIONARY"); fixturePath && *fixturePath) {
        static std::string loadedFixturePath;
        static PartBuckets fixtureBuckets;
        if (loadedFixturePath != fixturePath) {
            loadedFixturePath = fixturePath;
            fixtureBuckets = bucketPartsByFirstLetter(fallbackParts(activeFallbackEntries()));
        }
        return fixtureBuckets;
    }
    return coreBuckets;
}

std::vector<std::string> composeCandidatesFromBuckets(std::string_view pinyin, const PartBuckets &buckets,
                                                      std::size_t maxSegments) {
    if (pinyin.empty()) {
        return {};
    }
    std::vector<std::vector<std::string>> best(pinyin.size() + 1);
    std::vector<std::size_t> segmentCounts(pinyin.size() + 1, 0);
    best[0].push_back(std::string{});
    for (std::size_t offset = 0; offset < pinyin.size(); ++offset) {
        if (best[offset].empty()) {
            continue;
        }
        const auto rest = pinyin.substr(offset);
        const auto first = static_cast<unsigned char>(rest.front());
        if (first < 'a' || first > 'z') {
            continue;
        }
        const auto &parts = buckets[first - 'a'];
        for (const auto &part : parts) {
            if (!rest.starts_with(part.key)) {
                continue;
            }
            const auto next = offset + part.key.size();
            const auto nextSegments = segmentCounts[offset] + 1;
            if (nextSegments > maxSegments) {
                continue;
            }
            if (!best[next].empty() && segmentCounts[next] < nextSegments) {
                continue;
            }
            if (best[next].empty() || segmentCounts[next] > nextSegments) {
                best[next].clear();
                segmentCounts[next] = nextSegments;
            }
            for (const auto &prefix : best[offset]) {
                for (const auto &value : part.values) {
                    best[next].push_back(prefix + value);
                    if (best[next].size() >= 8) {
                        break;
                    }
                }
                if (best[next].size() >= 8) {
                    break;
                }
            }
        }
    }
    if (best.back().empty() || segmentCounts.back() < 2) {
        return {};
    }
    return best.back();
}

std::vector<std::string> composeFallbackCandidates(std::string_view pinyin) {
    return composeCandidatesFromBuckets(pinyin, cachedFallbackBuckets(), 16);
}

std::vector<std::string> composeSystemCandidates(std::string_view pinyin, const PartBuckets &buckets) {
    return composeCandidatesFromBuckets(pinyin, buckets, 16);
}

bool containsAsciiLetter(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80 && std::isalpha(ch);
    });
}

std::size_t utf8CodepointCount(std::string_view text);

bool mixedEnglishPrefixIsReady(std::string_view typedPrefix, std::string_view entryKey,
                               const std::vector<std::string> &values) {
    if (!std::any_of(values.begin(), values.end(), [](const std::string &value) {
            return containsAsciiLetter(value);
        })) {
        return true;
    }

    std::optional<std::size_t> firstTokenStart;
    for (const auto token : knownEnglishTokens) {
        const auto tokenStart = entryKey.find(token);
        if (tokenStart == std::string_view::npos) {
            continue;
        }
        if (!firstTokenStart || tokenStart < *firstTokenStart) {
            firstTokenStart = tokenStart;
        }
    }
    if (!firstTokenStart) {
        return true;
    }
    return typedPrefix.size() > *firstTokenStart;
}

bool fallbackPrefixCompletionIsReady(std::string_view typedPrefix, std::string_view entryKey,
                                     const std::vector<std::string> &values) {
    if (!mixedEnglishPrefixIsReady(typedPrefix, entryKey, values)) {
        return false;
    }
    if (typedPrefix.size() >= entryKey.size()) {
        return true;
    }
    if (std::any_of(values.begin(), values.end(), [](const std::string &value) {
            return containsAsciiLetter(value);
        })) {
        return true;
    }
    if (std::any_of(values.begin(), values.end(), [](const std::string &value) {
            return utf8CodepointCount(value) <= 4;
        })) {
        return true;
    }
    const auto remaining = entryKey.size() - typedPrefix.size();
    return remaining <= 4 || typedPrefix.size() >= 12;
}

std::vector<std::string> fallbackLookup(std::string_view pinyin) {
    if (pinyin.empty()) {
        return {};
    }

    const auto &entries = activeFallbackEntries();
    const auto iter = entries.find(std::string(pinyin));
    if (iter != entries.end()) {
        return iter->second;
    }

    std::vector<std::string> candidates;
    std::vector<std::pair<std::string_view, std::vector<std::string>>> prefixMatches;
    for (const auto &[entry, values] : entries) {
        if (entry.starts_with(pinyin)) {
            if (fallbackPrefixCompletionIsReady(pinyin, entry, values)) {
                prefixMatches.emplace_back(entry, values);
            }
        }
    }
    std::sort(prefixMatches.begin(), prefixMatches.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first.size() != rhs.first.size()) {
            return lhs.first.size() < rhs.first.size();
        }
        return lhs.first < rhs.first;
    });
    for (const auto &[entry, values] : prefixMatches) {
        (void)entry;
        candidates.insert(candidates.end(), values.begin(), values.end());
    }
    if (!candidates.empty()) {
        return candidates;
    }

    if (pinyin.size() <= 32) {
        for (auto &composed : composeFallbackCandidates(pinyin)) {
            candidates.push_back(std::move(composed));
        }
    }

    return candidates;
}

std::vector<std::string> exactFallbackLookup(std::string_view pinyin) {
    if (pinyin.empty()) {
        return {};
    }
    if (isKnownEnglishToken(pinyin)) {
        return {std::string(pinyin)};
    }
    const auto &entries = activeFallbackEntries();
    const auto iter = entries.find(std::string(pinyin));
    if (iter == entries.end()) {
        return {};
    }
    return iter->second;
}

std::filesystem::path userDictionaryPath() {
    if (const char *overridePath = std::getenv("TIPE_USER_DICTIONARY")) {
        if (*overridePath) {
            return overridePath;
        }
    }
    if (const char *xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdgDataHome) / "tipe" / "user-dictionary.tsv";
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "tipe" / "user-dictionary.tsv";
    }
    return {};
}

std::filesystem::path libIMEUserHistoryPath() {
    if (const char *overridePath = std::getenv("TIPE_LIBIME_USER_HISTORY")) {
        return *overridePath ? std::filesystem::path(overridePath) : std::filesystem::path{};
    }
    if (const char *xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdgDataHome) / "tipe" / "libime" / "user.history";
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "tipe" / "libime" / "user.history";
    }
    return {};
}

std::vector<std::string> splitTsvLine(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    std::vector<std::string> fields;
    std::string current;
    for (const char ch : line) {
        if (ch == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}

bool isUserDictionaryPinyin(std::string_view pinyin) {
    return !pinyin.empty() && pinyin.size() <= maxUserDictionaryPinyinBytes &&
           std::all_of(pinyin.begin(), pinyin.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

bool isValidUtf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        if (lead < 0x80) {
            ++offset;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            continuationCount = 1;
            codepoint = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            continuationCount = 2;
            codepoint = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            continuationCount = 3;
            codepoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (offset + continuationCount >= text.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuationCount; ++index) {
            const auto continuation = static_cast<unsigned char>(text[offset + index]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        offset += continuationCount + 1;
    }
    return true;
}

bool isUserDictionaryCandidate(std::string_view candidate, bool requireNonAscii = false) {
    if (candidate.empty() || candidate.size() > maxUserDictionaryCandidateBytes || !isValidUtf8(candidate)) {
        return false;
    }
    bool containsNonAscii = false;
    for (const unsigned char ch : candidate) {
        if (ch >= 0x80) {
            containsNonAscii = true;
            continue;
        }
        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return !requireNonAscii || containsNonAscii;
}

struct UserDictionaryLine {
    std::string preservedText;
    std::optional<std::string> pinyin;
    std::vector<std::string> candidates;
};

bool loadUserDictionaryDocument(const std::filesystem::path &path, std::vector<UserDictionaryLine> &lines) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return !error;
    }
    if (error) {
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        return false;
    }
    std::size_t entryRows = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            lines.push_back({std::move(line), std::nullopt, {}});
            continue;
        }
        auto fields = splitTsvLine(line);
        if (fields.size() < 2 || fields.size() > maxUserDictionaryCandidatesPerRow + 1 ||
            !isUserDictionaryPinyin(fields.front()) || ++entryRows > maxUserDictionaryRows) {
            return false;
        }
        std::vector<std::string> candidates;
        candidates.reserve(fields.size() - 1);
        for (std::size_t index = 1; index < fields.size(); ++index) {
            if (!isUserDictionaryCandidate(fields[index]) ||
                std::find(candidates.begin(), candidates.end(), fields[index]) != candidates.end()) {
                return false;
            }
            candidates.push_back(std::move(fields[index]));
        }
        lines.push_back({{}, std::move(fields.front()), std::move(candidates)});
    }
    return input.eof();
}

enum class UserDictionaryUpdate { Failed, Unchanged, Changed };

UserDictionaryUpdate promoteUserDictionaryCandidate(std::vector<UserDictionaryLine> &lines,
                                                    std::string_view pinyin, std::string_view candidate) {
    std::size_t entryRows = 0;
    for (auto &line : lines) {
        if (!line.pinyin) {
            continue;
        }
        ++entryRows;
        if (*line.pinyin != pinyin) {
            continue;
        }
        const auto existing = std::find(line.candidates.begin(), line.candidates.end(), candidate);
        if (existing == line.candidates.begin()) {
            return UserDictionaryUpdate::Unchanged;
        }
        if (existing != line.candidates.end()) {
            std::rotate(line.candidates.begin(), existing, std::next(existing));
            return UserDictionaryUpdate::Changed;
        }
        if (line.candidates.size() >= maxUserDictionaryCandidatesPerRow) {
            return UserDictionaryUpdate::Failed;
        }
        line.candidates.insert(line.candidates.begin(), std::string(candidate));
        return UserDictionaryUpdate::Changed;
    }
    if (entryRows >= maxUserDictionaryRows) {
        return UserDictionaryUpdate::Failed;
    }
    lines.push_back({{}, std::string(pinyin), {std::string(candidate)}});
    return UserDictionaryUpdate::Changed;
}

std::string serializeUserDictionary(const std::vector<UserDictionaryLine> &lines) {
    std::string contents;
    for (const auto &line : lines) {
        if (!line.pinyin) {
            contents.append(line.preservedText);
            contents.push_back('\n');
            continue;
        }
        contents.append(*line.pinyin);
        for (const auto &candidate : line.candidates) {
            contents.push_back('\t');
            contents.append(candidate);
        }
        contents.push_back('\n');
    }
    return contents;
}

class PrivateFileLock {
public:
    explicit PrivateFileLock(const std::filesystem::path &path) {
        const auto lockPath = path.string() + ".lock";
        descriptor_ = ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
        if (descriptor_ >= 0 && ::fchmod(descriptor_, 0600) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        if (descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

    ~PrivateFileLock() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }

    PrivateFileLock(const PrivateFileLock &) = delete;
    PrivateFileLock &operator=(const PrivateFileLock &) = delete;
    bool locked() const { return descriptor_ >= 0; }

private:
    int descriptor_ = -1;
};

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = ::write(descriptor, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool writePrivateFileAtomically(const std::filesystem::path &path, std::string_view contents) {
    static std::atomic<std::uint64_t> temporarySerial{0};
    std::filesystem::path temporaryPath;
    int descriptor = -1;
    for (int attempt = 0; attempt < 8 && descriptor < 0; ++attempt) {
        temporaryPath = path.string() + ".tmp." + std::to_string(::getpid()) + "." +
                        std::to_string(temporarySerial.fetch_add(1, std::memory_order_relaxed));
        descriptor = ::open(temporaryPath.c_str(), O_CREAT | O_EXCL | O_CLOEXEC | O_WRONLY, 0600);
        if (descriptor < 0 && errno != EEXIST) {
            return false;
        }
    }
    if (descriptor < 0) {
        return false;
    }

    const bool wrote = ::fchmod(descriptor, 0600) == 0 && writeAll(descriptor, contents) && ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    if (!wrote || !closed) {
        ::unlink(temporaryPath.c_str());
        return false;
    }
    if (::rename(temporaryPath.c_str(), path.c_str()) != 0) {
        ::unlink(temporaryPath.c_str());
        return false;
    }

    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int parentDescriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (parentDescriptor < 0) {
        return false;
    }
    const bool synced = ::fsync(parentDescriptor) == 0;
    const bool parentClosed = ::close(parentDescriptor) == 0;
    return synced && parentClosed;
}

bool isRimePinyinCode(std::string_view pinyin) {
    if (pinyin.empty()) {
        return false;
    }
    bool sawLetter = false;
    bool lastWasSpace = false;
    for (const unsigned char ch : pinyin) {
        if (ch >= 'a' && ch <= 'z') {
            sawLetter = true;
            lastWasSpace = false;
            continue;
        }
        if (ch == ' ') {
            if (lastWasSpace) {
                return false;
            }
            lastWasSpace = true;
            continue;
        }
        return false;
    }
    return sawLetter && !lastWasSpace;
}

std::string compactRimePinyin(std::string_view pinyin) {
    std::string compact;
    compact.reserve(pinyin.size());
    for (const char ch : pinyin) {
        if (ch != ' ') {
            compact.push_back(ch);
        }
    }
    return compact;
}

std::optional<int> parseWeight(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::filesystem::path> systemDictionaryPaths() {
    if (const char *overridePath = std::getenv("TIPE_SYSTEM_RIME_DICTIONARY")) {
        if (*overridePath) {
            return {overridePath};
        }
    }
    return {"/usr/share/rime-data/pinyin_simp.dict.yaml", "/usr/share/rime-data/luna_pinyin.dict.yaml"};
}

EntryMap finalizeWeightedEntries(std::unordered_map<std::string, std::vector<WeightedCandidate>> weightedEntries) {
    EntryMap entries;
    entries.reserve(weightedEntries.size());
    for (auto &[key, weightedValues] : weightedEntries) {
        std::stable_sort(weightedValues.begin(), weightedValues.end(), [](const auto &lhs, const auto &rhs) {
            if (lhs.weight != rhs.weight) {
                return lhs.weight > rhs.weight;
            }
            return lhs.sourceOrder < rhs.sourceOrder;
        });
        auto &values = entries[key];
        values.reserve(std::min<std::size_t>(weightedValues.size(), 48));
        std::unordered_set<std::string> seen;
        for (auto &candidate : weightedValues) {
            if (seen.insert(candidate.text).second) {
                values.push_back(std::move(candidate.text));
                if (values.size() >= 48) {
                    break;
                }
            }
        }
    }
    return entries;
}

EntryMap loadSystemRimeEntries(const std::vector<std::filesystem::path> &paths) {
    std::unordered_map<std::string, std::vector<WeightedCandidate>> weightedEntries;
    std::size_t sourceOrder = 0;
    for (const auto &path : paths) {
        std::ifstream input(path);
        if (!input) {
            continue;
        }

        bool inBody = false;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!inBody) {
                if (line == "...") {
                    inBody = true;
                }
                continue;
            }
            if (line.empty() || line.front() == '#') {
                continue;
            }

            auto fields = splitTsvLine(line);
            if (fields.size() < 2 || fields.front().empty() || !isRimePinyinCode(fields[1])) {
                continue;
            }
            const auto key = compactRimePinyin(fields[1]);
            if (!isUserDictionaryPinyin(key)) {
                continue;
            }
            const int weight = fields.size() >= 3 ? parseWeight(fields[2]).value_or(1) : 1;
            weightedEntries[key].push_back({std::move(fields.front()), weight, sourceOrder++});
        }
    }
    return finalizeWeightedEntries(std::move(weightedEntries));
}

std::string systemDictionaryCacheKey(const std::vector<std::filesystem::path> &paths) {
    std::string key;
    std::error_code error;
    for (const auto &path : paths) {
        key.append(path.string());
        key.push_back('\t');
        const auto size = std::filesystem::file_size(path, error);
        key.append(error ? "missing" : std::to_string(size));
        error.clear();
        const auto modified = std::filesystem::last_write_time(path, error);
        if (!error) {
            key.push_back('\t');
            key.append(std::to_string(modified.time_since_epoch().count()));
        }
        error.clear();
        key.push_back('\n');
    }
    return key;
}

struct SharedSystemDictionary {
    explicit SharedSystemDictionary(const std::vector<std::filesystem::path> &paths)
        : entries(loadSystemRimeEntries(paths)), candidatePartBuckets(bucketPartsByFirstLetter(systemParts(entries))),
          sortedEntries(sortedEntryRefs(entries)) {}

    EntryMap entries;
    PartBuckets candidatePartBuckets;
    SortedEntryRefs sortedEntries;
};

#ifdef TIPE_HAVE_LIBIME
std::string compactLibIMEPinyin(std::string_view pinyin) {
    std::string compact;
    compact.reserve(pinyin.size());
    for (const char ch : pinyin) {
        if (ch != '\'' && ch != ' ') {
            compact.push_back(ch);
        }
    }
    return compact;
}

std::filesystem::path firstExistingPath(const char *overrideName,
                                        std::initializer_list<std::filesystem::path> defaults) {
    if (const char *overridePath = std::getenv(overrideName); overridePath && *overridePath) {
        return overridePath;
    }
    std::error_code error;
    for (const auto &path : defaults) {
        if (std::filesystem::is_regular_file(path, error)) {
            return path;
        }
        error.clear();
    }
    return {};
}

struct SharedLibIMEBackend {
    struct LookupResult {
        std::vector<std::string> candidates;
        std::vector<std::vector<std::string>> candidateWords;
        double topScore = 0.0;
        double scoreMargin = 0.0;
        double normalizedTopScore = 0.0;
        bool confidentSentence = false;
        bool decisiveSentence = false;
    };

    SharedLibIMEBackend(const std::filesystem::path &dictionaryPath,
                        const std::filesystem::path &languageModelPath,
                        std::filesystem::path userHistoryPath)
        : userHistoryPath(std::move(userHistoryPath)) {
        if (dictionaryPath.empty() || languageModelPath.empty()) {
            return;
        }
        try {
            auto dictionary = std::make_unique<libime::PinyinDictionary>();
            dictionary->load(0, dictionaryPath.c_str(), libime::PinyinDictFormat::Binary);
            auto languageModel = std::make_unique<libime::UserLanguageModel>(languageModelPath.c_str());
            ime = std::make_unique<libime::PinyinIME>(std::move(dictionary), std::move(languageModel));
            // These limit sentence paths and partial word completion, not the final candidate count.
            // Keep one extra path beyond fcitx5-pinyin's default so TiPE can preserve ambiguous segment chains.
            ime->setNBest(3);
            ime->setWordCandidateLimit(15);
            ime->setPartialLongWordLimit(4);
            if (!reloadHistoryFromDisk(true)) {
                ime->model()->history().clear();
            }
        } catch (...) {
            ime.reset();
        }
    }

    LookupResult lookupResult(std::string_view pinyin) const {
        // LibIME's ambiguous segment graph can grow dramatically for a long
        // unsegmentable typo. Keep short typo completion, but let TiPE's
        // bounded correction and system-dictionary paths handle longer noise.
        if (!ime || !shouldRunFullPinyinDecoder(pinyin)) {
            return {};
        }
        {
            std::lock_guard lock(cacheMutex);
            if (const auto iter = cache.find(std::string(pinyin)); iter != cache.end()) {
                return iter->second;
            }
        }
        try {
            std::lock_guard imeLock(imeMutex);
            libime::PinyinContext context(ime.get());
            context.setMaxSentenceLength(64);
            if (!context.type(pinyin)) {
                return {};
            }
            LookupResult result;
            auto &candidates = result.candidates;
            auto &candidateWords = result.candidateWords;
            std::unordered_set<std::string> seen;
            candidates.reserve(std::min<std::size_t>(context.candidates().size(), 24));
            candidateWords.reserve(candidates.capacity());
            for (const auto &candidate : context.candidates()) {
                auto text = candidate.toString();
                if (!text.empty() && seen.insert(text).second) {
                    candidates.push_back(std::move(text));
                    std::vector<std::string> words;
                    words.reserve(candidate.sentence().size());
                    for (const auto *node : candidate.sentence()) {
                        if (node && !node->word().empty()) {
                            words.push_back(node->word());
                        }
                    }
                    candidateWords.push_back(std::move(words));
                }
                if (candidates.size() >= 24) {
                    break;
                }
            }
            if (!context.candidates().empty() && !candidates.empty()) {
                result.topScore = context.candidates().front().score();
                const auto topLength = utf8CodepointCount(candidates.front());
                const bool exactTopPinyin =
                    compactLibIMEPinyin(context.candidateFullPinyin(context.candidates().front())) == pinyin;
                if (topLength > 0) {
                    result.normalizedTopScore = result.topScore / static_cast<double>(topLength);
                }
                if (context.candidates().size() >= 2) {
                    result.scoreMargin = result.topScore - context.candidates()[1].score();
                }
                constexpr double confidentMargin = 0.75;
                constexpr double confidentNormalizedScore = -4.75;
                constexpr double minimumNormalizedScoreForMargin = -6.5;
                const bool strongRelativeLead = result.scoreMargin >= confidentMargin &&
                                                result.normalizedTopScore >= minimumNormalizedScoreForMargin;
                result.confidentSentence = exactTopPinyin && topLength >= 2 &&
                                           !containsAsciiLetter(candidates.front()) &&
                                           (strongRelativeLead ||
                                            result.normalizedTopScore >= confidentNormalizedScore);
                result.decisiveSentence = exactTopPinyin && topLength >= 2 &&
                                          !containsAsciiLetter(candidates.front()) && strongRelativeLead;
            }
            {
                std::lock_guard lock(cacheMutex);
                const std::string key(pinyin);
                if (!cache.contains(key)) {
                    if (cache.size() >= 2048 && !cacheOrder.empty()) {
                        cache.erase(cacheOrder.front());
                        cacheOrder.pop_front();
                    }
                    cacheOrder.push_back(key);
                    cache.emplace(key, result);
                }
            }
            return result;
        } catch (...) {
            return {};
        }
    }

    std::vector<std::string> lookup(std::string_view pinyin) const {
        return lookupResult(pinyin).candidates;
    }

    bool hasConfidentSentence(std::string_view pinyin) const {
        return lookupResult(pinyin).confidentSentence;
    }

    bool hasDecisiveSentence(std::string_view pinyin) const {
        return lookupResult(pinyin).decisiveSentence;
    }

    std::optional<double> normalizedScore(std::string_view pinyin) const {
        const auto result = lookupResult(pinyin);
        if (result.candidates.empty()) {
            return std::nullopt;
        }
        return result.normalizedTopScore;
    }

    bool learnSelection(std::string_view pinyin, std::string_view candidate) {
        const char *disabled = std::getenv("TIPE_DISABLE_LIBIME_LEARNING");
        if (!ime || userHistoryPath.empty() || pinyin.empty() || candidate.empty() ||
            (disabled && std::string_view(disabled) != "0")) {
            return false;
        }

        const auto result = lookupResult(pinyin);
        const auto selected = std::find(result.candidates.begin(), result.candidates.end(), candidate);
        if (selected == result.candidates.end()) {
            return false;
        }
        const auto selectedIndex = static_cast<std::size_t>(std::distance(result.candidates.begin(), selected));
        if (selectedIndex >= result.candidateWords.size() || result.candidateWords[selectedIndex].empty()) {
            return false;
        }

        std::error_code error;
        const auto parent = userHistoryPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return false;
            }
        }
        PrivateFileLock fileLock(userHistoryPath);
        if (!fileLock.locked()) {
            return false;
        }

        bool historyReady = false;
        bool persisted = false;
        {
            std::lock_guard imeLock(imeMutex);
            historyReady = reloadHistoryFromDisk(false);
            if (historyReady) {
                ime->model()->history().add(result.candidateWords[selectedIndex]);
                persisted = saveHistoryToDisk();
            }
        }
        {
            std::lock_guard cacheLock(cacheMutex);
            cache.clear();
            cacheOrder.clear();
        }
        return historyReady && persisted;
    }

private:
    static constexpr std::size_t maxUserHistoryBytes = 4 * 1024 * 1024;

    bool reloadHistoryFromDisk(bool force) {
        if (!ime || userHistoryPath.empty()) {
            return true;
        }
        struct stat status {};
        if (::lstat(userHistoryPath.c_str(), &status) != 0) {
            if (errno != ENOENT) {
                return false;
            }
            if (force || userHistoryMtime) {
                ime->model()->history().clear();
            }
            userHistoryMtime.reset();
            return true;
        }
        if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 ||
            status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maxUserHistoryBytes) {
            return false;
        }
        std::error_code error;
        const auto modifiedTime = std::filesystem::last_write_time(userHistoryPath, error);
        if (error) {
            return false;
        }
        if (!force && userHistoryMtime && *userHistoryMtime == modifiedTime) {
            return true;
        }
        std::ifstream input(userHistoryPath, std::ios::binary);
        if (!input) {
            return false;
        }
        try {
            ime->model()->history().clear();
            ime->model()->load(input);
        } catch (...) {
            ime->model()->history().clear();
            return false;
        }
        if (!input.eof() && input.fail()) {
            ime->model()->history().clear();
            return false;
        }
        userHistoryMtime = modifiedTime;
        return true;
    }

    bool saveHistoryToDisk() {
        std::ostringstream output(std::ios::binary | std::ios::out);
        try {
            ime->model()->save(output);
        } catch (...) {
            return false;
        }
        const auto contents = output.str();
        if (!output || contents.empty() || contents.size() > maxUserHistoryBytes ||
            !writePrivateFileAtomically(userHistoryPath, contents)) {
            return false;
        }
        std::error_code error;
        const auto modifiedTime = std::filesystem::last_write_time(userHistoryPath, error);
        userHistoryMtime = error ? std::nullopt : std::optional{modifiedTime};
        return !error;
    }

    std::unique_ptr<libime::PinyinIME> ime;
    std::filesystem::path userHistoryPath;
    std::optional<std::filesystem::file_time_type> userHistoryMtime;
    mutable std::mutex imeMutex;
    mutable std::mutex cacheMutex;
    mutable std::unordered_map<std::string, LookupResult> cache;
    mutable std::deque<std::string> cacheOrder;
};

std::shared_ptr<SharedLibIMEBackend> sharedLibIMEBackend() {
    const auto dictionaryPath = firstExistingPath("TIPE_LIBIME_DICTIONARY", {"/usr/share/libime/sc.dict"});
    const auto languageModelPath = firstExistingPath(
        "TIPE_LIBIME_LANGUAGE_MODEL", {"/usr/lib64/libime/zh_CN.lm", "/usr/lib/libime/zh_CN.lm"});
    const auto userHistoryPath = libIMEUserHistoryPath();
    const auto key = systemDictionaryCacheKey({dictionaryPath, languageModelPath}) +
                     "user-history\t" + userHistoryPath.string() + '\n';
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::shared_ptr<SharedLibIMEBackend>> cache;
    std::lock_guard lock(cacheMutex);
    if (const auto iter = cache.find(key); iter != cache.end()) {
        return iter->second;
    }
    auto loaded = std::make_shared<SharedLibIMEBackend>(dictionaryPath, languageModelPath, userHistoryPath);
    cache[key] = loaded;
    return loaded;
}
#endif

std::shared_ptr<const SharedSystemDictionary> sharedSystemDictionary() {
    const auto paths = systemDictionaryPaths();
    const auto key = systemDictionaryCacheKey(paths);
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::shared_ptr<const SharedSystemDictionary>> cache;
    std::lock_guard lock(cacheMutex);
    if (const auto iter = cache.find(key); iter != cache.end()) {
        return iter->second;
    }
    auto loaded = std::make_shared<const SharedSystemDictionary>(paths);
    cache[key] = loaded;
    return loaded;
}

EntryMap loadUserEntries(const std::filesystem::path &path) {
    EntryMap entries;
    if (path.empty()) {
        return entries;
    }
    std::ifstream input(path);
    if (!input) {
        return entries;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        auto fields = splitTsvLine(line);
        if (fields.size() < 2 || fields.size() > maxUserDictionaryCandidatesPerRow + 1 ||
            !isUserDictionaryPinyin(fields.front())) {
            continue;
        }
        std::vector<std::string> rowValues;
        bool validRow = true;
        for (std::size_t index = 1; index < fields.size(); ++index) {
            if (!isUserDictionaryCandidate(fields[index]) ||
                std::find(rowValues.begin(), rowValues.end(), fields[index]) != rowValues.end()) {
                validRow = false;
                break;
            }
            rowValues.push_back(std::move(fields[index]));
        }
        if (!validRow || rowValues.empty()) {
            continue;
        }
        if (!entries.contains(fields.front()) && entries.size() >= maxUserDictionaryRows) {
            continue;
        }
        auto &values = entries[fields.front()];
        for (auto &value : rowValues) {
            if (values.size() < maxUserDictionaryCandidatesPerRow &&
                std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(std::move(value));
            }
        }
    }
    return entries;
}

std::vector<std::string> userLookup(
    std::string_view pinyin, const EntryMap &entries) {
    if (pinyin.empty()) {
        return {};
    }

    std::vector<std::string> candidates;
    if (const auto iter = entries.find(std::string(pinyin)); iter != entries.end()) {
        candidates.insert(candidates.end(), iter->second.begin(), iter->second.end());
    }
    for (const auto &[key, values] : entries) {
        if (key != pinyin && std::string_view(key).starts_with(pinyin)) {
            candidates.insert(candidates.end(), values.begin(), values.end());
        }
    }
    return candidates;
}

std::vector<std::string> systemLookup(std::string_view pinyin, const EntryMap &entries,
                                      const PartBuckets &partBuckets, const SortedEntryRefs &sortedEntries,
                                      bool composeCandidates) {
    if (pinyin.empty()) {
        return {};
    }

    std::vector<std::string> candidates;
    if (const auto iter = entries.find(std::string(pinyin)); iter != entries.end()) {
        const auto limit = composeCandidates ? iter->second.size() : std::min<std::size_t>(iter->second.size(), 8);
        candidates.insert(candidates.end(), iter->second.begin(),
                          iter->second.begin() + static_cast<std::ptrdiff_t>(limit));
    }

    if (!composeCandidates) {
        return candidates;
    }

    if (pinyin.size() <= 32) {
        for (auto &candidate : composeSystemCandidates(pinyin, partBuckets)) {
            candidates.push_back(std::move(candidate));
        }
    }

    if (pinyin.size() >= 4) {
        std::vector<std::pair<std::string_view, const std::vector<std::string> *>> prefixMatches;
        auto iter = std::lower_bound(sortedEntries.begin(), sortedEntries.end(), pinyin,
                                     [](const auto &entry, std::string_view value) { return entry.first < value; });
        for (; iter != sortedEntries.end() && iter->first.starts_with(pinyin); ++iter) {
            if (iter->first != pinyin) {
                prefixMatches.push_back(*iter);
            }
        }
        std::sort(prefixMatches.begin(), prefixMatches.end(), [](const auto &lhs, const auto &rhs) {
            if (lhs.first.size() != rhs.first.size()) {
                return lhs.first.size() < rhs.first.size();
            }
            return lhs.first < rhs.first;
        });
        for (const auto &[key, values] : prefixMatches) {
            (void)key;
            if (!values || values->empty()) {
                continue;
            }
            candidates.push_back(values->front());
            if (candidates.size() >= 32) {
                break;
            }
        }
    }
    return candidates;
}

bool isAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80;
    });
}

std::size_t utf8CodepointCount(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xC0) != 0x80;
    }));
}

void stablePromoteKnownPhrases(std::string_view pinyin, std::vector<std::string> &candidates) {
    const auto fallback = fallbackLookup(pinyin);
    if (fallback.empty() || candidates.empty()) {
        return;
    }

    const auto bestFallback = std::find(candidates.begin(), candidates.end(), fallback.front());
    if (bestFallback == candidates.end()) {
        return;
    }

    const auto first = candidates.begin();
    const auto firstLength = utf8CodepointCount(*first);
    const auto fallbackLength = utf8CodepointCount(*bestFallback);
    const bool knownPrefixCompletion = exactFallbackLookup(pinyin).empty();
    if (bestFallback != first && fallbackLength >= 1 &&
        (firstLength <= 1 || pinyin.size() >= 5 || knownPrefixCompletion)) {
        std::rotate(first, bestFallback, bestFallback + 1);
    }
}

void normalizeCandidateOrder(std::string_view pinyin, std::vector<std::string> &candidates, bool promoteKnownPhrases = true) {
    if (candidates.size() < 2) {
        return;
    }

    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const std::string &candidate) {
                         return candidate.empty() || isAscii(candidate);
                     }),
                     candidates.end());

    std::unordered_set<std::string> seen;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&seen](const std::string &candidate) {
                                        return !seen.insert(candidate).second;
                                    }),
                     candidates.end());

    if (promoteKnownPhrases) {
        stablePromoteKnownPhrases(pinyin, candidates);
    }
}

#ifdef TIPE_HAVE_LIBPINYIN
std::filesystem::path userPinyinDir() {
    if (const char *xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdgDataHome) / "tipe" / "libpinyin";
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "tipe" / "libpinyin";
    }
    return std::filesystem::temp_directory_path() / "tipe-libpinyin";
}

struct PinyinContextDeleter {
    void operator()(pinyin_context_t *context) const {
        if (context) {
            pinyin_fini(context);
        }
    }
};

struct PinyinInstanceDeleter {
    void operator()(pinyin_instance_t *instance) const {
        if (instance) {
            pinyin_free_instance(instance);
        }
    }
};
#endif

} // namespace

struct Dictionary::Impl {
    mutable std::filesystem::path userEntriesPath;
    mutable std::optional<std::filesystem::file_time_type> userEntriesModifiedTime;
    mutable EntryMap userEntries;
    std::shared_ptr<const SharedSystemDictionary> systemDictionary;
#ifdef TIPE_HAVE_LIBIME
    std::shared_ptr<SharedLibIMEBackend> libIMEBackend;
#endif
#ifdef TIPE_HAVE_LIBPINYIN
    std::unique_ptr<pinyin_context_t, PinyinContextDeleter> context;
#endif

    Impl() {
        systemDictionary = sharedSystemDictionary();
#ifdef TIPE_HAVE_LIBIME
        libIMEBackend = sharedLibIMEBackend();
#endif
        reloadUserEntriesIfNeeded();
#ifdef TIPE_HAVE_LIBPINYIN
        const auto userDir = userPinyinDir();
        std::error_code error;
        std::filesystem::create_directories(userDir, error);

        static constexpr std::array systemDirs{"/usr/lib64/libpinyin/data", "/usr/lib/libpinyin/data"};
        for (const auto *systemDir : systemDirs) {
            if (!std::filesystem::exists(systemDir)) {
                continue;
            }
            context.reset(pinyin_init(systemDir, userDir.c_str()));
            if (context) {
                pinyin_set_options(context.get(), IS_PINYIN | PINYIN_INCOMPLETE | DYNAMIC_ADJUST);
                break;
            }
        }
#endif
    }

    void reloadUserEntriesIfNeeded() const {
        const auto path = userDictionaryPath();
        std::error_code error;
        std::optional<std::filesystem::file_time_type> modifiedTime;
        if (!path.empty() && std::filesystem::exists(path, error)) {
            modifiedTime = std::filesystem::last_write_time(path, error);
            if (error) {
                modifiedTime.reset();
            }
        }

        if (path == userEntriesPath && modifiedTime == userEntriesModifiedTime) {
            return;
        }

        userEntriesPath = path;
        userEntriesModifiedTime = modifiedTime;
        userEntries = modifiedTime ? loadUserEntries(path) : EntryMap{};
    }

#ifdef TIPE_HAVE_LIBPINYIN
    std::vector<std::string> lookup(std::string_view pinyin) const {
        if (!context || pinyin.empty()) {
            return {};
        }

        std::string query(pinyin);
        std::unique_ptr<pinyin_instance_t, PinyinInstanceDeleter> instance(pinyin_alloc_instance(context.get()));
        if (!instance) {
            return {};
        }

        const auto parsedLength = pinyin_parse_more_full_pinyins(instance.get(), query.c_str());
        if (parsedLength == 0) {
            return {};
        }

        std::vector<std::string> candidates;
        std::unordered_set<std::string> seen;
        auto appendCandidate = [&](const char *candidate) {
            if (!candidate || !*candidate) {
                return;
            }
            std::string value(candidate);
            if (seen.insert(value).second) {
                candidates.push_back(std::move(value));
            }
        };

        if (pinyin_guess_sentence(instance.get())) {
            char *sentence = nullptr;
            if (pinyin_get_sentence(instance.get(), 0, &sentence) && sentence) {
                appendCandidate(sentence);
            }
            g_free(sentence);
        }

        if (pinyin_guess_candidates(instance.get(), 0, SORT_BY_PHRASE_LENGTH_AND_FREQUENCY)) {
            guint count = 0;
            if (pinyin_get_n_candidate(instance.get(), &count)) {
                const auto limit = std::min<guint>(count, 32);
                for (guint index = 0; index < limit; ++index) {
                    lookup_candidate_t *candidate = nullptr;
                    const gchar *candidateString = nullptr;
                    if (pinyin_get_candidate(instance.get(), index, &candidate) &&
                        pinyin_get_candidate_string(instance.get(), candidate, &candidateString)) {
                        appendCandidate(candidateString);
                    }
                }
            }
        }

        return candidates;
    }
#endif
};

Dictionary::Dictionary() : impl_(std::make_unique<Impl>()) {}

Dictionary::~Dictionary() = default;

Dictionary::Dictionary(Dictionary &&) noexcept = default;

Dictionary &Dictionary::operator=(Dictionary &&) noexcept = default;

std::vector<std::string> Dictionary::lookup(std::string_view pinyin) const {
    if (impl_) {
        impl_->reloadUserEntriesIfNeeded();
    }
    auto userCandidates = impl_ ? userLookup(pinyin, impl_->userEntries) : std::vector<std::string>{};
    auto fallbackCandidates = fallbackLookup(pinyin);
    const bool containsEmbeddedEnglish = std::any_of(knownEnglishTokens.begin(), knownEnglishTokens.end(),
                                                     [pinyin](std::string_view token) {
                                                         const auto offset = pinyin.find(token);
                                                         return offset != std::string_view::npos &&
                                                                token.size() != pinyin.size();
                                                     });
#ifdef TIPE_HAVE_LIBIME
    auto libIMECandidates = impl_ && impl_->libIMEBackend && !containsEmbeddedEnglish &&
                                    !isKnownEnglishToken(pinyin)
                                ? impl_->libIMEBackend->lookup(pinyin)
                                : std::vector<std::string>{};
#else
    std::vector<std::string> libIMECandidates;
#endif
    const char *systemDictionaryOverride = std::getenv("TIPE_SYSTEM_RIME_DICTIONARY");
    const bool useLegacyComposition = libIMECandidates.empty() || containsEmbeddedEnglish ||
                                      (systemDictionaryOverride && *systemDictionaryOverride);
    auto systemCandidates = impl_ && impl_->systemDictionary
                                  ? systemLookup(pinyin, impl_->systemDictionary->entries,
                                                 impl_->systemDictionary->candidatePartBuckets,
                                                 impl_->systemDictionary->sortedEntries, useLegacyComposition)
                                  : std::vector<std::string>{};
#ifdef TIPE_HAVE_LIBPINYIN
    auto libpinyinCandidates = impl_ && libIMECandidates.empty() && !containsEmbeddedEnglish &&
                                       !isKnownEnglishToken(pinyin)
                                   ? impl_->lookup(pinyin)
                                   : std::vector<std::string>{};
#else
    std::vector<std::string> libpinyinCandidates;
#endif
    std::vector<std::string> merged;
    merged.reserve(userCandidates.size() + libIMECandidates.size() + libpinyinCandidates.size() +
                   systemCandidates.size() + fallbackCandidates.size());
    std::unordered_set<std::string> seen;
    const auto appendUnique = [&merged, &seen](std::vector<std::string> &source) {
        for (auto &candidate : source) {
            if (seen.insert(candidate).second) {
                merged.push_back(std::move(candidate));
            }
        }
    };
    appendUnique(userCandidates);
    if (containsEmbeddedEnglish) {
        appendUnique(systemCandidates);
    }
    appendUnique(libIMECandidates);
    appendUnique(libpinyinCandidates);
    if (!containsEmbeddedEnglish) {
        appendUnique(systemCandidates);
    }
    appendUnique(fallbackCandidates);
    normalizeCandidateOrder(pinyin, merged, userCandidates.empty());
    return merged;
}

std::vector<std::string> Dictionary::exactUserCandidates(std::string_view pinyin) const {
    if (!impl_ || pinyin.empty()) {
        return {};
    }
    impl_->reloadUserEntriesIfNeeded();
    if (const auto iter = impl_->userEntries.find(std::string(pinyin)); iter != impl_->userEntries.end()) {
        return iter->second;
    }
    return {};
}

std::vector<std::string> Dictionary::exactKnownCandidates(std::string_view pinyin) const {
    if (pinyin.empty()) {
        return {};
    }

    auto candidates = exactUserCandidates(pinyin);
    std::unordered_set<std::string> seen(candidates.begin(), candidates.end());
    if (impl_ && impl_->systemDictionary) {
        if (const auto iter = impl_->systemDictionary->entries.find(std::string(pinyin));
            iter != impl_->systemDictionary->entries.end()) {
            for (const auto &candidate : iter->second) {
                if (seen.insert(candidate).second) {
                    candidates.push_back(candidate);
                }
            }
        }
    }
    for (auto &candidate : exactFallbackLookup(pinyin)) {
        if (seen.insert(candidate).second) {
            candidates.push_back(std::move(candidate));
        }
    }
    normalizeCandidateOrder(pinyin, candidates, false);
    return candidates;
}

bool Dictionary::hasExactKnownPinyin(std::string_view pinyin) const {
    return exactPinyinPriority(pinyin) > 0;
}

bool Dictionary::hasConfidentLanguageModelSentence(std::string_view pinyin) const {
#ifdef TIPE_HAVE_LIBIME
    if (!impl_ || !impl_->libIMEBackend || pinyin.empty() || isKnownEnglishToken(pinyin)) {
        return false;
    }
    const bool containsEmbeddedEnglish = std::any_of(knownEnglishTokens.begin(), knownEnglishTokens.end(),
                                                     [pinyin](std::string_view token) {
                                                         const auto offset = pinyin.find(token);
                                                         return offset != std::string_view::npos &&
                                                                token.size() != pinyin.size();
                                                     });
    return !containsEmbeddedEnglish && impl_->libIMEBackend->hasConfidentSentence(pinyin);
#else
    (void)pinyin;
    return false;
#endif
}

bool Dictionary::hasDecisiveLanguageModelSentence(std::string_view pinyin) const {
#ifdef TIPE_HAVE_LIBIME
    if (!impl_ || !impl_->libIMEBackend || pinyin.empty() || isKnownEnglishToken(pinyin)) {
        return false;
    }
    const bool containsEmbeddedEnglish = std::any_of(knownEnglishTokens.begin(), knownEnglishTokens.end(),
                                                     [pinyin](std::string_view token) {
                                                         const auto offset = pinyin.find(token);
                                                         return offset != std::string_view::npos &&
                                                                token.size() != pinyin.size();
                                                     });
    return !containsEmbeddedEnglish && impl_->libIMEBackend->hasDecisiveSentence(pinyin);
#else
    (void)pinyin;
    return false;
#endif
}

std::optional<double> Dictionary::languageModelNormalizedScore(std::string_view pinyin) const {
#ifdef TIPE_HAVE_LIBIME
    if (impl_ && impl_->libIMEBackend && !pinyin.empty() && !isKnownEnglishToken(pinyin)) {
        return impl_->libIMEBackend->normalizedScore(pinyin);
    }
#else
    (void)pinyin;
#endif
    return std::nullopt;
}

int Dictionary::learnedCorrectionPriority(std::string_view pinyin) const {
    const auto exactPriority = exactPinyinPriority(pinyin);
    if (exactPriority >= 2) {
        return exactPriority + 2;
    }
    if (hasDecisiveLanguageModelSentence(pinyin)) {
        return 3;
    }
    if (hasConfidentLanguageModelSentence(pinyin)) {
        return 2;
    }
    return 0;
}

int Dictionary::exactPinyinPriority(std::string_view pinyin) const {
    if (pinyin.empty()) {
        return 0;
    }
    if (impl_) {
        impl_->reloadUserEntriesIfNeeded();
        if (impl_->userEntries.contains(std::string(pinyin))) {
            return 3;
        }
    }
    if (activeFallbackEntries().contains(std::string(pinyin))) {
        return 2;
    }
    if (impl_ && impl_->systemDictionary && impl_->systemDictionary->entries.contains(std::string(pinyin))) {
        return 2;
    }
    return isKnownEnglishToken(pinyin) || isCompletePinyinSequence(pinyin) ? 1 : 0;
}

bool Dictionary::learnLanguageModelSelection(std::string_view pinyin, std::string_view candidate) {
#ifdef TIPE_HAVE_LIBIME
    if (impl_ && impl_->libIMEBackend && isUserDictionaryPinyin(pinyin) &&
        isUserDictionaryCandidate(candidate, true)) {
        return impl_->libIMEBackend->learnSelection(pinyin, candidate);
    }
#else
    (void)pinyin;
    (void)candidate;
#endif
    return false;
}

bool Dictionary::learnUserEntry(std::string_view pinyin, std::string_view candidate) {
    if (!impl_ || !isUserDictionaryPinyin(pinyin) || !isUserDictionaryCandidate(candidate, true)) {
        return false;
    }
    const auto path = userDictionaryPath();
    if (path.empty()) {
        return false;
    }

    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    PrivateFileLock lock(path);
    if (!lock.locked()) {
        return false;
    }
    std::vector<UserDictionaryLine> lines;
    if (!loadUserDictionaryDocument(path, lines)) {
        return false;
    }
    const auto update = promoteUserDictionaryCandidate(lines, pinyin, candidate);
    if (update == UserDictionaryUpdate::Failed) {
        return false;
    }
    if (update == UserDictionaryUpdate::Changed) {
        if (!writePrivateFileAtomically(path, serializeUserDictionary(lines))) {
            return false;
        }
    } else if (::chmod(path.c_str(), 0600) != 0) {
        return false;
    }

    impl_->userEntriesPath = path;
    impl_->userEntries = loadUserEntries(path);
    const auto modifiedTime = std::filesystem::last_write_time(path, error);
    impl_->userEntriesModifiedTime = error ? std::nullopt : std::optional{modifiedTime};
    return true;
}

} // namespace tipe
