#include "input_model.h"
#include "english_tokens.h"
#include "pinyin_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cerrno>
#include <iterator>
#include <limits>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

extern char **environ;

namespace tipe {

namespace {

constexpr std::string_view correctionRecordPrefix = "__correction__";
constexpr std::string_view correctionPatternRecordPrefix = "__correction_pattern__";
constexpr std::string_view keyHabitRecordPrefix = "__key_habit__";
constexpr std::string_view segmentChainRecordPrefix = "__segment_chain__";
constexpr std::string_view rawTokenRecordPrefix = "__raw_token__";
constexpr std::size_t maxSavedPreferenceRows = 2048;
constexpr std::size_t maxSavedRawTokenRows = 512;
constexpr std::size_t maxSavedCorrectionRows = 512;
constexpr std::size_t maxSavedCorrectionPatternRows = 512;
constexpr std::size_t maxSavedKeyHabitRows = 128;
constexpr std::size_t maxSavedSegmentChainRows = 512;
constexpr std::size_t maxSavedLearningCount = 1000000;
constexpr std::size_t candidatePreferenceActivationCount = 2;
constexpr std::size_t rawPreferenceActivationCount = 3;
constexpr std::size_t correctionPatternActivationCount = 2;
constexpr std::size_t keyHabitActivationCount = 3;
constexpr std::size_t maxLearnedCorrectionCandidates = 12;

enum class LearnedEditKind { Missing, Extra, Replace, Transpose };

std::optional<LearnedEditKind> learnedEditKind(std::string_view value) {
    if (value == "missing") {
        return LearnedEditKind::Missing;
    }
    if (value == "extra") {
        return LearnedEditKind::Extra;
    }
    if (value == "replace") {
        return LearnedEditKind::Replace;
    }
    if (value == "transpose") {
        return LearnedEditKind::Transpose;
    }
    return std::nullopt;
}

std::size_t correctionPatternActivationFor(LearnedEditKind kind) {
    switch (kind) {
    case LearnedEditKind::Missing:
    case LearnedEditKind::Transpose:
        return correctionPatternActivationCount;
    case LearnedEditKind::Extra:
        return 3;
    case LearnedEditKind::Replace:
        return 4;
    }
    return maxSavedLearningCount;
}

std::size_t keyHabitActivationFor(LearnedEditKind kind) {
    switch (kind) {
    case LearnedEditKind::Transpose:
        return keyHabitActivationCount;
    case LearnedEditKind::Missing:
        return 5;
    case LearnedEditKind::Extra:
    case LearnedEditKind::Replace:
        return 6;
    }
    return maxSavedLearningCount;
}

bool validLearnedEdit(LearnedEditKind kind, std::string_view typed, std::string_view replacement) {
    const auto asciiAlnum = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isalnum(ch); });
    };
    if (!asciiAlnum(typed) || !asciiAlnum(replacement) || typed == replacement) {
        return false;
    }
    switch (kind) {
    case LearnedEditKind::Missing:
        return typed.empty() && replacement.size() == 1;
    case LearnedEditKind::Extra:
        return typed.size() == 1 && replacement.empty();
    case LearnedEditKind::Replace:
        return typed.size() == 1 && replacement.size() == 1;
    case LearnedEditKind::Transpose:
        return typed.size() == 2 && replacement.size() == 2;
    }
    return false;
}

std::uint64_t contextFingerprintPart(std::string_view text, std::uint64_t seed) {
    constexpr std::uint64_t fnvPrime = 1099511628211ULL;
    constexpr std::string_view domain = "TiPE-context-v1\0";
    auto value = seed;
    const auto append = [&value](std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            value ^= byte;
            value *= fnvPrime;
        }
    };
    append(domain);
    append(text);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

std::string contextFingerprint(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    const std::array parts = {
        contextFingerprintPart(text, 14695981039346656037ULL),
        contextFingerprintPart(text, 0x84222325cbf29ce4ULL),
    };
    std::string result = "v1:";
    result.reserve(35);
    for (const auto part : parts) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            result.push_back(hex[(part >> shift) & 0xf]);
        }
    }
    return result;
}

std::size_t preferenceActivationCount(std::string_view preedit, std::string_view candidate) {
    return candidate == preedit ? rawPreferenceActivationCount : candidatePreferenceActivationCount;
}

class PreferenceFileLock {
public:
    explicit PreferenceFileLock(const std::filesystem::path &preferencePath) {
        const auto lockPath = preferencePath.string() + ".lock";
        descriptor_ = ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
        if (descriptor_ >= 0 && ::fchmod(descriptor_, 0600) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        if (descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

    ~PreferenceFileLock() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }

    PreferenceFileLock(const PreferenceFileLock &) = delete;
    PreferenceFileLock &operator=(const PreferenceFileLock &) = delete;
    bool locked() const { return descriptor_ >= 0; }

private:
    int descriptor_ = -1;
};

void addBoundedCount(std::size_t &count, std::size_t increment) {
    const auto boundedCount = std::min(count, maxSavedLearningCount);
    count = boundedCount + std::min(increment, maxSavedLearningCount - boundedCount);
}

template <typename Map>
void mergeLocalDeltas(Map &target, const Map &local, const Map &baseline) {
    for (const auto &[key, value] : local) {
        const auto baselineIter = baseline.find(key);
        const auto baselineValue = baselineIter == baseline.end() ? std::size_t{0} : baselineIter->second;
        if (value > baselineValue) {
            addBoundedCount(target[key], value - baselineValue);
        }
    }
}

template <typename Map>
void retainStrongestCounts(Map &counts, std::size_t limit) {
    if (counts.size() <= limit) {
        return;
    }
    std::vector<std::pair<std::string, std::size_t>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    rows.resize(limit);
    counts.clear();
    counts.reserve(rows.size());
    for (auto &row : rows) {
        counts.emplace(std::move(row.first), row.second);
    }
}

std::optional<std::size_t> parseCount(std::string_view text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        return std::nullopt;
    }
    std::size_t count = 0;
    std::istringstream stream{std::string(text)};
    stream >> count;
    if (!stream || count == 0 || count > maxSavedLearningCount) {
        return std::nullopt;
    }
    return count;
}

std::optional<std::size_t> parseBoundedIndex(std::string_view text, std::size_t maximum) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        return std::nullopt;
    }
    std::size_t value = 0;
    std::istringstream stream{std::string(text)};
    stream >> value;
    if (!stream || value > maximum) {
        return std::nullopt;
    }
    return value;
}

bool isSafeStoredText(std::string_view text) {
    return !text.empty() && text.find_first_of("\t\r\n") == std::string_view::npos;
}

bool looksLikeLearnedRawIdentifier(std::string_view preedit);

std::size_t utf8CodepointCount(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xC0) != 0x80;
    }));
}

bool canApplyLearnedCandidatePreference(std::string_view preedit, std::string_view candidate,
                                        const std::vector<std::string> &candidates) {
    if (preedit.empty() || candidate.empty()) {
        return false;
    }
    if (candidate == preedit) {
        return looksLikeLearnedRawIdentifier(preedit);
    }

    const auto preeditLength = preedit.size();
    const auto candidateLength = utf8CodepointCount(candidate);
    if (preeditLength <= 1 && candidateLength > 1) {
        return false;
    }
    if (preeditLength <= 2 && candidateLength > preeditLength + 1) {
        return false;
    }
    if (!candidates.empty() && utf8CodepointCount(candidates.front()) == 1 && candidateLength > preeditLength) {
        return false;
    }
    return true;
}

std::vector<std::string> splitTabFields(std::string_view line);

std::size_t learnedSegmentContinuationBoost(const std::vector<std::string> &recentCommits,
                                            const std::unordered_map<std::string, std::size_t> &segmentChainCounts,
                                            std::string_view preedit,
                                            std::string_view candidate) {
    if (recentCommits.empty() || segmentChainCounts.empty() || preedit.empty() || candidate.empty()) {
        return 0;
    }

    const auto &lastCommit = recentCommits.back();
    const auto combined = lastCommit + std::string(candidate);
    std::size_t strongest = 0;
    for (const auto &[key, count] : segmentChainCounts) {
        if (count == 0 || !key.starts_with(segmentChainRecordPrefix)) {
            continue;
        }
        const auto fields = splitTabFields(key);
        if (fields.size() != 7) {
            continue;
        }
        if (fields[3] == lastCommit && fields[4] == preedit && fields[6] == combined) {
            strongest = std::max(strongest, count);
        }
    }
    return strongest * 3000;
}

int modelTimeoutSeconds() {
    const char *value = std::getenv("TIPE_MODEL_TIMEOUT_SECONDS");
    if (!value || !*value) {
        return 2;
    }

    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return 2;
    }
    return std::clamp<long>(parsed, 1, 30);
}

bool isSafeModelCommand(std::string_view command) {
    if (command.empty()) {
        return false;
    }
    return std::all_of(command.begin(), command.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-' || ch == ' ' || ch == ':' ||
               ch == '=' || ch == '+';
    });
}

std::string_view inputEventName(InputEventType type) {
    switch (type) {
    case InputEventType::Letter:
        return "letter";
    case InputEventType::Digit:
        return "digit";
    case InputEventType::Symbol:
        return "symbol";
    case InputEventType::Backspace:
        return "backspace";
    case InputEventType::Delete:
        return "delete";
    case InputEventType::Space:
        return "space";
    case InputEventType::Enter:
        return "enter";
    case InputEventType::Escape:
        return "escape";
    case InputEventType::ObservedKey:
        return "observed";
    case InputEventType::CandidateSelected:
        return "candidate-selected";
    case InputEventType::RawCommitted:
        return "raw-committed";
    case InputEventType::CursorMove:
        return "cursor-move";
    case InputEventType::AiRerankRequested:
        return "rerank-requested";
    }
    return "unknown";
}

std::vector<std::pair<std::string_view, std::size_t>> eventKindCounts(const std::vector<InputEvent> &events) {
    std::vector<std::pair<std::string_view, std::size_t>> counts;
    for (const auto &event : events) {
        const auto name = inputEventName(event.type);
        auto iter = std::find_if(counts.begin(), counts.end(), [name](const auto &item) {
            return item.first == name;
        });
        if (iter == counts.end()) {
            counts.emplace_back(name, 1);
        } else {
            ++iter->second;
        }
    }
    return counts;
}

bool isLeadingPrefixCandidate(const std::vector<std::string> &candidates, std::string_view candidate) {
    return !candidate.empty() && std::any_of(candidates.begin(), candidates.end(), [candidate](const auto &other) {
        return other.size() > candidate.size() && std::string_view(other).starts_with(candidate);
    });
}

bool isPartialCandidateForRequest(std::string_view preedit, const std::vector<std::string> &candidates,
                                  const ModelRequestState &state, std::string_view candidate) {
    if (state.candidateMetadata.size() == candidates.size()) {
        const auto iter = std::find(candidates.begin(), candidates.end(), candidate);
        if (iter != candidates.end()) {
            const auto index = static_cast<std::size_t>(std::distance(candidates.begin(), iter));
            const auto consumed = state.candidateMetadata[index].consumedPrefixLength;
            return consumed > 0 && consumed < preedit.size();
        }
    }
    return isLeadingPrefixCandidate(candidates, candidate);
}

bool looksLikeLearnedRawIdentifier(std::string_view preedit) {
    if (looksLikeEnglishIdentifier(preedit)) {
        return true;
    }
    if (preedit.size() < 2 || !std::all_of(preedit.begin(), preedit.end(), [](unsigned char ch) {
            return std::isalpha(ch);
        })) {
        return false;
    }
    if (preedit.size() >= 4 && !isCompletePinyinSequence(preedit)) {
        return true;
    }
    const auto lowered = asciiLower(preedit);
    if (lowered == "er" || lowered == "lv" || lowered == "nv" || lowered == "lve" || lowered == "nve") {
        return false;
    }
    const char last = lowered.back();
    if (last == 'g') {
        return lowered.size() < 2 || lowered[lowered.size() - 2] != 'n';
    }
    return std::string_view("bcdfhjklmpqrstvwxyz").find(last) != std::string_view::npos;
}

std::vector<std::string> splitModelCommand(std::string_view command) {
    std::vector<std::string> tokens;
    std::istringstream stream{std::string(command)};
    std::string token;
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool isEnvironmentAssignment(std::string_view token) {
    const auto delimiter = token.find('=');
    return delimiter != std::string_view::npos && delimiter > 0 && delimiter + 1 < token.size() &&
           std::all_of(token.begin(), token.begin() + static_cast<std::ptrdiff_t>(delimiter), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_';
           }) &&
           !std::isdigit(static_cast<unsigned char>(token.front()));
}

std::string escapedEventText(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string segmentChainRecordKey(const SegmentChain &chain) {
    std::string key(segmentChainRecordPrefix);
    key.push_back('\t');
    key.append(chain.originalPreedit);
    key.push_back('\t');
    key.append(chain.consumedPreedit);
    key.push_back('\t');
    key.append(chain.committedText);
    key.push_back('\t');
    key.append(chain.remainingPreedit);
    key.push_back('\t');
    key.append(chain.correctedFullPreedit);
    key.push_back('\t');
    key.append(chain.combinedCandidate);
    return key;
}

struct CorrectionRequestRow {
    std::string key;
    std::size_t count = 0;
    int priority = 2;
};

std::optional<std::pair<std::string_view, std::string_view>> parseCorrectionRecordKey(std::string_view key) {
    if (!key.starts_with(correctionRecordPrefix)) {
        return std::nullopt;
    }
    const auto typoStart = key.find('\t');
    const auto correctionStart = typoStart == std::string::npos ? std::string::npos : key.find('\t', typoStart + 1);
    if (typoStart == std::string::npos || correctionStart == std::string::npos ||
        key.find('\t', correctionStart + 1) != std::string::npos) {
        return std::nullopt;
    }
    return std::pair{key.substr(typoStart + 1, correctionStart - typoStart - 1), key.substr(correctionStart + 1)};
}

std::vector<std::string> splitTabFields(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto delimiter = line.find('\t', begin);
        const auto end = delimiter == std::string_view::npos ? line.size() : delimiter;
        fields.emplace_back(line.substr(begin, end - begin));
        if (delimiter == std::string_view::npos) {
            break;
        }
        begin = delimiter + 1;
    }
    return fields;
}

std::string modelRequestPayload(std::string_view preedit, const std::vector<std::string> &candidates,
                                std::string_view application,
                                ModelRequestState state,
                                const std::vector<InputEvent> &events,
                                const std::vector<InputEvent> &correctionEvents,
                                const std::vector<std::string> &recentCommits,
                                const std::vector<SegmentChain> &recentSegmentChains,
                                const std::unordered_map<std::string, std::size_t> &selectedCounts,
                                const std::unordered_map<std::string, std::size_t> &correctionCounts,
                                const std::unordered_map<std::string, std::size_t> &segmentChainCounts) {
    std::ostringstream input;
    const bool hasActivePreedit = !preedit.empty() && !state.forcePassThrough;
    input << "protocol\t1\n";
    input << "preedit\t" << preedit << '\n';
    if (!application.empty()) {
        input << "application\t" << escapedEventText(application) << '\n';
    }
    if (!state.surroundingBefore.empty()) {
        input << "surrounding_before\t" << escapedEventText(state.surroundingBefore) << '\n';
    }
    if (!state.surroundingAfter.empty()) {
        input << "surrounding_after\t" << escapedEventText(state.surroundingAfter) << '\n';
    }
    input << "surrounding_features";
    if (!state.surroundingBefore.empty()) {
        input << "\tbefore:" << contextFingerprint(state.surroundingBefore);
    }
    if (!state.surroundingAfter.empty()) {
        input << "\tafter:" << contextFingerprint(state.surroundingAfter);
    }
    input << '\n';
    input << "candidates";
    for (const auto &candidate : candidates) {
        input << '\t' << candidate;
    }
    input << '\n';
    const auto metadataCount = std::min(candidates.size(), state.candidateMetadata.size());
    for (std::size_t index = 0; index < metadataCount; ++index) {
        const auto &metadata = state.candidateMetadata[index];
        input << "candidate_metadata\t" << index << "\tconsumed_prefix\t" << metadata.consumedPrefixLength
              << "\tsource\t" << escapedEventText(metadata.source) << "\tscore\t" << metadata.score << '\n';
    }
    input << "state\tpreedit_cursor\t" << state.preeditCursor << "\tcandidate_cursor\t" << state.candidateCursor
          << "\texpanded\t" << (state.candidatesExpanded ? 1 : 0) << '\n';
    input << "runtime_state\tcontinuous\t" << (state.continuousMode ? 1 : 0);
    if (!state.inputMode.empty()) {
        input << "\tinput_mode\t" << escapedEventText(state.inputMode);
    }
    input << '\n';
    input << "supervision_state\tmode\t" << (hasActivePreedit ? "active-preedit" : "pass-through-only")
          << "\tactive_preedit\t" << (hasActivePreedit ? 1 : 0)
          << "\trecent_events\t" << events.size()
          << "\tcorrection_events\t" << correctionEvents.size() << '\n';
    if (state.candidateCursor < candidates.size()) {
        input << "selected_candidate\t" << state.candidateCursor << '\t' << candidates[state.candidateCursor] << '\n';
    }
    input << "visible_candidates";
    for (const auto index : state.visibleCandidateIndices) {
        if (index < candidates.size()) {
            input << '\t' << index << ':' << candidates[index];
        }
    }
    input << '\n';
    input << "numbered_candidates";
    for (const auto &[shortcut, index] : state.numberedCandidateIndices) {
        if (!shortcut.empty() && index < candidates.size()) {
            input << '\t' << shortcut << ':' << index << ':' << candidates[index];
        }
    }
    input << '\n';
    input << "events";
    for (const auto &event : events) {
        input << '\t' << inputEventName(event.type) << ':' << escapedEventText(event.text);
    }
    input << '\n';
    input << "event_counts";
    for (const auto &[kind, count] : eventKindCounts(events)) {
        input << '\t' << kind << ':' << count;
    }
    input << '\n';
    input << "correction_events";
    for (const auto &event : correctionEvents) {
        input << '\t' << inputEventName(event.type) << ':' << escapedEventText(event.text);
    }
    input << '\n';
    input << "correction_event_counts";
    for (const auto &[kind, count] : eventKindCounts(correctionEvents)) {
        input << '\t' << kind << ':' << count;
    }
    input << '\n';
    input << "context";
    for (const auto &commit : recentCommits) {
        input << '\t' << escapedEventText(commit);
    }
    input << '\n';
    input << "context_features";
    for (const auto &commit : recentCommits) {
        input << '\t' << contextFingerprint(commit);
    }
    input << '\n';
    std::unordered_set<std::string> emittedSegmentChains;
    const auto emitSegmentChain = [&](const SegmentChain &chain) {
        if (!emittedSegmentChains.insert(segmentChainRecordKey(chain)).second) {
            return;
        }
        input << "segment_chain"
              << '\t' << escapedEventText(chain.originalPreedit)
              << '\t' << escapedEventText(chain.consumedPreedit)
              << '\t' << escapedEventText(chain.committedText)
              << '\t' << escapedEventText(chain.remainingPreedit)
              << '\t' << escapedEventText(chain.correctedFullPreedit)
              << '\t' << escapedEventText(chain.combinedCandidate)
              << '\n';
    };
    if (hasActivePreedit) {
        for (const auto &chain : recentSegmentChains) {
            emitSegmentChain(chain);
        }
    }
    if (hasActivePreedit) {
        for (const auto &segment : state.pendingSegments) {
            if (segment.originalPreedit.empty() || segment.consumedPreedit.empty() || segment.committedText.empty()) {
                continue;
            }
            input << "pending_segment"
                  << '\t' << escapedEventText(segment.originalPreedit)
                  << '\t' << escapedEventText(segment.consumedPreedit)
                  << '\t' << escapedEventText(segment.committedText)
                  << '\t' << escapedEventText(segment.remainingPreedit)
                  << '\n';
        }
    }
    std::vector<std::pair<std::size_t, std::string>> storedSegmentChainRows;
    if (hasActivePreedit) {
        for (const auto &[key, count] : segmentChainCounts) {
            if (count == 0 || !key.starts_with(segmentChainRecordPrefix)) {
                continue;
            }
            const auto fields = splitTabFields(key);
            if (fields.size() != 7) {
                continue;
            }
            const bool matchesOriginalPreedit = fields[1] == preedit;
            const bool matchesCurrentContinuation = !recentCommits.empty() && fields[3] == recentCommits.back() &&
                                                    fields[4] == preedit;
            if (matchesOriginalPreedit || matchesCurrentContinuation) {
                storedSegmentChainRows.emplace_back(count, key);
            }
        }
    }
    std::stable_sort(storedSegmentChainRows.begin(), storedSegmentChainRows.end(),
                     [](const auto &lhs, const auto &rhs) {
                         if (lhs.first != rhs.first) {
                             return lhs.first > rhs.first;
                         }
                         return lhs.second < rhs.second;
                     });
    std::size_t emittedStoredSegmentChains = 0;
    for (const auto &[count, key] : storedSegmentChainRows) {
        (void)count;
        if (emittedStoredSegmentChains >= 16) {
            break;
        }
        std::vector<std::string_view> fields;
        std::size_t begin = 0;
        while (begin <= key.size()) {
            const auto delimiter = key.find('\t', begin);
            const auto end = delimiter == std::string::npos ? key.size() : delimiter;
            fields.push_back(std::string_view(key).substr(begin, end - begin));
            if (delimiter == std::string::npos) {
                break;
            }
            begin = delimiter + 1;
        }
        if (fields.size() != 7) {
            continue;
        }
        emitSegmentChain({std::string(fields[1]), std::string(fields[2]), std::string(fields[3]),
                          std::string(fields[4]), std::string(fields[5]), std::string(fields[6])});
        ++emittedStoredSegmentChains;
    }

    const std::string preferencePrefix = std::string(preedit) + '\t';
    std::vector<std::pair<std::string, std::size_t>> preferenceRows;
    if (hasActivePreedit) {
        for (const auto &[key, count] : selectedCounts) {
            if (count == 0 || !key.starts_with(preferencePrefix)) {
                continue;
            }
            const auto candidate = key.substr(preferencePrefix.size());
            if (!isSafeStoredText(candidate)) {
                continue;
            }
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end() ||
                !canApplyLearnedCandidatePreference(preedit, candidate, candidates) ||
                isPartialCandidateForRequest(preedit, candidates, state, candidate)) {
                continue;
            }
            if (candidate == preedit && !looksLikeLearnedRawIdentifier(preedit)) {
                continue;
            }
            const auto activationCount = preferenceActivationCount(preedit, candidate);
            if (count < activationCount) {
                continue;
            }
            preferenceRows.emplace_back(candidate, count);
        }
    }
    std::sort(preferenceRows.begin(), preferenceRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    std::size_t emittedPreferences = 0;
    for (const auto &[candidate, count] : preferenceRows) {
        if (emittedPreferences >= 16) {
            break;
        }
        input << "preference\t" << preedit << '\t' << candidate << '\t' << count << '\n';
        ++emittedPreferences;
    }

    std::vector<CorrectionRequestRow> correctionRows;
    if (hasActivePreedit) {
        correctionRows.reserve(correctionCounts.size());
        for (const auto &[key, count] : correctionCounts) {
            const auto parsed = parseCorrectionRecordKey(key);
            if (!parsed) {
                continue;
            }
            const auto &[typo, corrected] = *parsed;
            int priority = 2;
            if (typo == preedit) {
                priority = 0;
            } else if (corrected == preedit) {
                priority = 1;
            }
            correctionRows.push_back({key, count, priority});
        }
    }
    std::stable_sort(correctionRows.begin(), correctionRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.priority != rhs.priority) {
            return lhs.priority < rhs.priority;
        }
        if (lhs.count != rhs.count) {
            return lhs.count > rhs.count;
        }
        return lhs.key < rhs.key;
    });
    std::size_t emittedCorrections = 0;
    for (const auto &[key, count, priority] : correctionRows) {
        (void)priority;
        if (emittedCorrections >= 32) {
            break;
        }
        const auto parsed = parseCorrectionRecordKey(key);
        if (!parsed) {
            continue;
        }
        const auto &[typo, corrected] = *parsed;
        input << "correction\t" << typo << '\t' << corrected << '\t' << count << '\n';
        ++emittedCorrections;
    }
    return input.str();
}

bool reapModelProcess(pid_t pid) {
    for (;;) {
        const pid_t result = waitpid(pid, nullptr, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            return true;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

std::optional<int> waitModelProcessUntil(pid_t pid, std::chrono::steady_clock::time_point deadline) {
    int status = 0;
    for (;;) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return status;
        }
        if (result < 0 && errno == ECHILD) {
            return 0;
        }
        if (result < 0 && errno != EINTR) {
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        usleep(10000);
    }
}

bool modelProcessOwnsGroup(pid_t pid) {
    return pid > 0 && getpgid(pid) == pid;
}

void signalModelProcess(pid_t pid, int signal) {
    if (modelProcessOwnsGroup(pid)) {
        kill(-pid, signal);
    } else {
        kill(pid, signal);
    }
}

void terminateModelProcess(pid_t pid) {
    const bool processGroup = modelProcessOwnsGroup(pid);
    signalModelProcess(pid, SIGTERM);
    bool reaped = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (reapModelProcess(pid)) {
            reaped = true;
            break;
        }
        usleep(10000);
    }
    if (processGroup && (kill(-pid, 0) == 0 || errno == EPERM)) {
        kill(-pid, SIGKILL);
    }
    if (!reaped && !reapModelProcess(pid)) {
        signalModelProcess(pid, SIGKILL);
        while (!reapModelProcess(pid)) {
            usleep(10000);
        }
    }
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool movePipeDescriptorsAboveStdio(int (&pipeFds)[2]) {
    for (int &descriptor : pipeFds) {
        if (descriptor > STDERR_FILENO) {
            continue;
        }
        const int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) {
            return false;
        }
        close(descriptor);
        descriptor = replacement;
    }
    return true;
}

std::optional<timeval> remainingTimeout(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::nullopt;
    }
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(remainingMs / 1000);
    timeout.tv_usec = static_cast<long>((remainingMs % 1000) * 1000);
    return timeout;
}

bool writePayloadUntil(int fd, const std::string &payload, std::chrono::steady_clock::time_point deadline) {
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t result = write(fd, payload.data() + written, payload.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto timeout = remainingTimeout(deadline);
            if (!timeout) {
                return false;
            }
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(fd, &writeSet);
            const int ready = select(fd + 1, nullptr, &writeSet, nullptr, &*timeout);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::string> runModelCommand(std::string_view command, const std::string &payload) {
    if (!isSafeModelCommand(command)) {
        return std::nullopt;
    }

    auto tokens = splitModelCommand(command);
    std::vector<std::string> environmentAssignments;
    while (!tokens.empty() && isEnvironmentAssignment(tokens.front())) {
        environmentAssignments.push_back(std::move(tokens.front()));
        tokens.erase(tokens.begin());
    }
    if (tokens.empty()) {
        return std::nullopt;
    }

    int stdinPipe[2]{};
    int stdoutPipe[2]{};
    if (pipe2(stdinPipe, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    if (pipe2(stdoutPipe, O_CLOEXEC) != 0) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        return std::nullopt;
    }
    if (!movePipeDescriptorsAboveStdio(stdinPipe) || !movePipeDescriptorsAboveStdio(stdoutPipe)) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return std::nullopt;
    }

    std::vector<char *> argv;
    argv.reserve(tokens.size() + 1);
    for (auto &token : tokens) {
        argv.push_back(token.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> childEnvironment;
    for (char **entry = environ; entry && *entry; ++entry) {
        childEnvironment.emplace_back(*entry);
    }
    for (const auto &assignment : environmentAssignments) {
        const auto delimiter = assignment.find('=');
        const auto key = assignment.substr(0, delimiter + 1);
        const auto existing = std::find_if(childEnvironment.begin(), childEnvironment.end(), [&](const auto &row) {
            return row.starts_with(key);
        });
        if (existing == childEnvironment.end()) {
            childEnvironment.push_back(assignment);
        } else {
            *existing = assignment;
        }
    }
    std::vector<char *> envp;
    envp.reserve(childEnvironment.size() + 1);
    for (auto &entry : childEnvironment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fileActions;
    posix_spawnattr_t attributes;
    const bool fileActionsReady = posix_spawn_file_actions_init(&fileActions) == 0;
    const bool attributesReady = posix_spawnattr_init(&attributes) == 0;
    bool spawnSetupReady = fileActionsReady && attributesReady;
    if (spawnSetupReady) {
        spawnSetupReady = posix_spawn_file_actions_adddup2(&fileActions, stdinPipe[0], STDIN_FILENO) == 0 &&
                          posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDOUT_FILENO) == 0 &&
                          posix_spawn_file_actions_addclose(&fileActions, stdinPipe[1]) == 0 &&
                          posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[0]) == 0 &&
                          posix_spawn_file_actions_addclose(&fileActions, stdinPipe[0]) == 0 &&
                          posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[1]) == 0 &&
                          posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0 &&
                          posix_spawnattr_setpgroup(&attributes, 0) == 0;
    }

    pid_t pid = -1;
    const int spawnResult = spawnSetupReady
                                ? posix_spawnp(&pid, argv.front(), &fileActions, &attributes, argv.data(), envp.data())
                                : EINVAL;
    if (fileActionsReady) {
        posix_spawn_file_actions_destroy(&fileActions);
    }
    if (attributesReady) {
        posix_spawnattr_destroy(&attributes);
    }
    if (spawnResult != 0) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return std::nullopt;
    }

    setpgid(pid, pid);
    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(modelTimeoutSeconds());
    if (!setNonBlocking(stdinPipe[1]) || !setNonBlocking(stdoutPipe[0])) {
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        terminateModelProcess(pid);
        return std::nullopt;
    }

    if (!writePayloadUntil(stdinPipe[1], payload, deadline)) {
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        terminateModelProcess(pid);
        return std::nullopt;
    }
    close(stdinPipe[1]);

    std::string output;
    bool timedOut = false;
    bool stdoutOpen = true;
    while (stdoutOpen) {
        auto timeout = remainingTimeout(deadline);
        if (!timeout) {
            timedOut = true;
            break;
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(stdoutPipe[0], &readSet);
        const int ready = select(stdoutPipe[0] + 1, &readSet, nullptr, nullptr, &*timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            timedOut = ready == 0;
            break;
        }
        char buffer[4096];
        const ssize_t result = read(stdoutPipe[0], buffer, sizeof(buffer));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (result <= 0) {
            stdoutOpen = false;
            break;
        }
        output.append(buffer, static_cast<std::size_t>(result));
        if (output.size() > 65536) {
            timedOut = true;
            break;
        }
    }
    close(stdoutPipe[0]);

    if (timedOut) {
        terminateModelProcess(pid);
        return std::nullopt;
    }

    const auto status = waitModelProcessUntil(pid, deadline);
    if (!status) {
        terminateModelProcess(pid);
        return std::nullopt;
    }
    if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
        return std::nullopt;
    }
    return output;
}

} // namespace

std::optional<std::string> configuredModelCommand() {
    const char *command = std::getenv("TIPE_MODEL_COMMAND");
    if (!command || !*command) {
        command = std::getenv("TIPE_RERANK_COMMAND");
    }
    if (!command || !*command) {
        return std::nullopt;
    }
    return std::string(command);
}

std::optional<std::string> invokeModelCommand(std::string_view command, const std::string &payload) {
    return runModelCommand(command, payload);
}

InputModel::InputModel(std::filesystem::path preferencePath)
    : preferencePath_(preferencePath.empty() ? defaultPreferencePath() : std::move(preferencePath)) {
    loadPreferences();
}

void InputModel::record(InputEventType type, std::string_view text) {
    if (events_.size() == maxEvents_) {
        events_.erase(events_.begin());
    }
    events_.push_back({type, std::string(text)});
    recordCorrectionEvent(type, text);
}

void InputModel::recordCandidateSelection(std::string_view preedit, std::string_view candidate, std::size_t weight) {
    if (preedit.empty() || candidate.empty() || !isSafeStoredText(preedit) || !isSafeStoredText(candidate)) {
        record(InputEventType::CandidateSelected, candidate);
        return;
    }
    reloadPreferencesIfChanged(true);
    const auto correctionVersionBeforeLearning = correctionVersion_;
    learnCorrectionFromRecentEvents(preedit);
    record(InputEventType::CandidateSelected, candidate);
    recordRecentCommit(candidate);
    bool preferencesChanged = correctionVersion_ != correctionVersionBeforeLearning;
    std::optional<std::pair<std::string, std::string>> explicitCandidatePromotion;
    if (weight > 0 && canApplyLearnedCandidatePreference(preedit, candidate, {})) {
        addBoundedCount(selectedCounts_[preferenceKey(preedit, candidate)], weight);
        if (weight >= candidatePreferenceActivationCount) {
            explicitCandidatePromotion.emplace(preedit, candidate);
            promoteExplicitCandidatePreference(preedit, candidate);
        }
        preferencesChanged = true;
    }
    if (preferencesChanged) {
        savePreferences(std::move(explicitCandidatePromotion));
    }
}

void InputModel::recordCandidatePreference(std::string_view preedit, std::string_view candidate, std::size_t weight) {
    if (preedit.empty() || candidate.empty() || weight == 0 || !isSafeStoredText(preedit) ||
        !isSafeStoredText(candidate)) {
        return;
    }
    reloadPreferencesIfChanged(true);
    if (canApplyLearnedCandidatePreference(preedit, candidate, {})) {
        addBoundedCount(selectedCounts_[preferenceKey(preedit, candidate)], weight);
        savePreferences();
    }
}

void InputModel::recordRawCommit(std::string_view preedit, std::size_t weight) {
    if (preedit.empty() || !isSafeStoredText(preedit)) {
        record(InputEventType::RawCommitted, preedit);
        return;
    }
    reloadPreferencesIfChanged(true);
    const auto correctionVersionBeforeLearning = correctionVersion_;
    learnCorrectionFromRecentEvents(preedit);
    record(InputEventType::RawCommitted, preedit);
    recordRecentCommit(preedit);
    bool preferencesChanged = correctionVersion_ != correctionVersionBeforeLearning;
    if (weight > 0 && looksLikeLearnedRawIdentifier(preedit)) {
        addBoundedCount(selectedCounts_[rawPreferenceKey(preedit)], weight);
        preferencesChanged = true;
    }
    if (preferencesChanged) {
        savePreferences();
    }
}

void InputModel::recordCorrection(std::string_view typo, std::string_view correctedPreedit, std::size_t weight) {
    if (typo.empty() || correctedPreedit.empty() || weight == 0 || !isSafeStoredText(typo) ||
        !isSafeStoredText(correctedPreedit) || !isPlausibleCorrection(typo, correctedPreedit)) {
        return;
    }
    reloadPreferencesIfChanged(true);
    addBoundedCount(correctionCounts_[correctionKey(typo, correctedPreedit)], weight);
    ++correctionVersion_;
    savePreferences();
}

std::size_t InputModel::recordSegmentChain(SegmentChain chain) {
    if (!isPlausibleSegmentChain(chain)) {
        return 0;
    }
    reloadPreferencesIfChanged(true);
    if (recentSegmentChains_.size() == maxRecentSegmentChains_) {
        recentSegmentChains_.erase(recentSegmentChains_.begin());
    }
    const auto key = segmentChainRecordKey(chain);
    addBoundedCount(segmentChainCounts_[key], 1);
    recentSegmentChains_.push_back(std::move(chain));
    savePreferences();
    const auto learned = segmentChainCounts_.find(key);
    return learned == segmentChainCounts_.end() ? 0 : learned->second;
}

void InputModel::undoLatestRecentCommit(std::string_view text) {
    if (text.empty() || recentCommits_.empty()) {
        return;
    }
    const auto iter = std::find(recentCommits_.rbegin(), recentCommits_.rend(), text);
    if (iter != recentCommits_.rend()) {
        recentCommits_.erase(std::next(iter).base());
    }
}

const std::vector<InputEvent> &InputModel::recentEvents() const { return events_; }

const std::vector<std::string> &InputModel::recentCommits() const { return recentCommits_; }

const std::vector<SegmentChain> &InputModel::recentSegmentChains() const { return recentSegmentChains_; }

InputSessionContext InputModel::sessionContext() const {
    return {correctionEvents_, recentCommits_, recentSegmentChains_};
}

void InputModel::restoreSessionContext(InputSessionContext context) {
    const auto retainTail = [](auto &values, std::size_t limit) {
        if (values.size() > limit) {
            values.erase(values.begin(), values.end() - static_cast<std::ptrdiff_t>(limit));
        }
    };
    retainTail(context.correctionEvents, maxCorrectionEvents_);
    retainTail(context.recentCommits, maxRecentCommits_);
    retainTail(context.recentSegmentChains, maxRecentSegmentChains_);
    correctionEvents_ = std::move(context.correctionEvents);
    recentCommits_ = std::move(context.recentCommits);
    recentSegmentChains_ = std::move(context.recentSegmentChains);
}

InputEventCounts InputModel::eventCounts() const {
    InputEventCounts counts;
    for (const auto &event : events_) {
        switch (event.type) {
        case InputEventType::Letter:
            ++counts.letters;
            break;
        case InputEventType::Digit:
            ++counts.digits;
            break;
        case InputEventType::Symbol:
            ++counts.symbols;
            break;
        case InputEventType::Backspace:
            ++counts.backspaces;
            break;
        case InputEventType::Delete:
            ++counts.deletes;
            break;
        case InputEventType::Space:
            ++counts.spaces;
            break;
        case InputEventType::Enter:
            ++counts.enters;
            break;
        case InputEventType::Escape:
            ++counts.escapes;
            break;
        case InputEventType::ObservedKey:
            ++counts.observedKeys;
            break;
        case InputEventType::RawCommitted:
            ++counts.rawCommits;
            break;
        case InputEventType::CandidateSelected:
            ++counts.candidateSelections;
            break;
        case InputEventType::CursorMove:
            ++counts.cursorMoves;
            break;
        case InputEventType::AiRerankRequested:
            ++counts.rerankRequests;
            break;
        }
    }
    return counts;
}

void InputModel::clear() { events_.clear(); }

void InputModel::clearCorrectionEvents() { correctionEvents_.clear(); }

void InputModel::reloadPreferencesIfChanged(bool force) {
    constexpr auto checkInterval = std::chrono::milliseconds(250);
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < nextPreferenceReloadCheck_) {
        return;
    }
    nextPreferenceReloadCheck_ = now + checkInterval;
    std::error_code error;
    const auto currentMtime = std::filesystem::last_write_time(preferencePath_, error);
    if (error) {
        if (preferenceMtime_) {
            selectedCounts_.clear();
            rawTokenCounts_.clear();
            correctionCounts_.clear();
            correctionPatternCounts_.clear();
            keyHabitCounts_.clear();
            segmentChainCounts_.clear();
            persistedSelectedCounts_.clear();
            persistedRawTokenCounts_.clear();
            persistedCorrectionCounts_.clear();
            persistedCorrectionPatternCounts_.clear();
            persistedKeyHabitCounts_.clear();
            persistedSegmentChainCounts_.clear();
            preferenceMtime_.reset();
            ++correctionVersion_;
        }
        return;
    }
    if (preferenceMtime_ && *preferenceMtime_ == currentMtime) {
        return;
    }
    loadPreferences();
}

bool InputModel::shouldPreferRaw(std::string_view preedit, const std::vector<std::string> &candidates) const {
    if (preedit.size() < 2) {
        return false;
    }

    if (isKnownEnglishToken(preedit)) {
        return true;
    }

    const bool asciiLettersOnly = std::all_of(preedit.begin(), preedit.end(), [](unsigned char ch) {
        return std::isalpha(ch);
    });
    if (!asciiLettersOnly) {
        return false;
    }

    const auto lowered = asciiLower(preedit);
    const auto supervisedRawToken = rawTokenCounts_.find(lowered);
    const bool supervisedRawActive =
        supervisedRawToken != rawTokenCounts_.end() && supervisedRawToken->second >= rawPreferenceActivationCount;
    const auto rawPreference = selectedCounts_.find(rawPreferenceKey(preedit));
    const bool ordinaryRawActive =
        rawPreference != selectedCounts_.end() && rawPreference->second >= rawPreferenceActivationCount &&
        looksLikeLearnedRawIdentifier(preedit);
    if (!supervisedRawActive && !ordinaryRawActive) {
        return false;
    }

    std::size_t strongestCandidatePreference = 0;
    for (const auto &candidate : candidates) {
        if (candidate == preedit) {
            continue;
        }
        if (const auto iter = selectedCounts_.find(preferenceKey(preedit, candidate)); iter != selectedCounts_.end()) {
            strongestCandidatePreference = std::max(strongestCandidatePreference, iter->second);
        }
    }
    const auto rawCount = std::max(
        supervisedRawActive ? supervisedRawToken->second : 0U,
        ordinaryRawActive ? rawPreference->second : 0U
    );
    return rawCount >= strongestCandidatePreference;
}

bool InputModel::hasActiveCandidatePreference(std::string_view preedit, std::string_view candidate) const {
    const auto iter = selectedCounts_.find(preferenceKey(preedit, candidate));
    return iter != selectedCounts_.end() && iter->second >= preferenceActivationCount(preedit, candidate);
}

bool InputModel::hasNonRawCandidatePreferenceEvidence(std::string_view preedit) const {
    if (preedit.empty()) {
        return false;
    }
    const std::string prefix = std::string(preedit) + '\t';
    return std::any_of(selectedCounts_.begin(), selectedCounts_.end(), [&](const auto &entry) {
        if (!entry.first.starts_with(prefix)) {
            return false;
        }
        const auto candidate = std::string_view(entry.first).substr(prefix.size());
        return candidate != preedit && entry.second > 0;
    });
}

std::size_t InputModel::correctionVersion() const { return correctionVersion_; }

bool InputModel::hasExactLearnedCorrection(std::string_view preedit) const {
    const std::string needle = correctionKey(preedit, "");
    std::size_t strongestCount = 0;
    std::size_t strongestMatches = 0;
    for (const auto &[key, count] : correctionCounts_) {
        if (count < 2 || !key.starts_with(needle)) {
            continue;
        }
        if (count > strongestCount) {
            strongestCount = count;
            strongestMatches = 1;
        } else if (count == strongestCount) {
            ++strongestMatches;
        }
    }
    return strongestCount >= 2 && strongestMatches == 1;
}

bool InputModel::hasActiveKeyHabits() const {
    return std::any_of(keyHabitCounts_.begin(), keyHabitCounts_.end(), [](const auto &entry) {
        const auto fields = splitTabFields(entry.first);
        if (fields.size() != 4 || fields[0] != keyHabitRecordPrefix) {
            return false;
        }
        const auto kind = learnedEditKind(fields[1]);
        return kind && entry.second >= keyHabitActivationFor(*kind);
    });
}

std::vector<std::string> InputModel::learnedCandidatePreferences(std::string_view preedit) const {
    if (preedit.empty()) {
        return {};
    }

    const std::string prefix = std::string(preedit) + '\t';
    std::vector<std::pair<std::string, std::size_t>> matches;
    for (const auto &[key, count] : selectedCounts_) {
        if (!key.starts_with(prefix)) {
            continue;
        }
        const auto candidate = key.substr(prefix.size());
        if (candidate == preedit || count < preferenceActivationCount(preedit, candidate) ||
            !canApplyLearnedCandidatePreference(preedit, candidate, {})) {
            continue;
        }
        matches.emplace_back(candidate, count);
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (matches.size() > 8) {
        matches.resize(8);
    }

    std::vector<std::string> result;
    result.reserve(matches.size());
    for (auto &match : matches) {
        result.push_back(std::move(match.first));
    }
    return result;
}

std::vector<std::string>
InputModel::learnedCorrections(std::string_view preedit,
                               const std::function<int(std::string_view)> &generatedCorrectionPriority) const {
    std::vector<std::pair<std::size_t, std::string>> matches;
    std::unordered_map<std::string, std::size_t> generatedScores;
    const std::string needle = correctionKey(preedit, "");
    for (const auto &[key, count] : correctionCounts_) {
        if (count < 2 || !key.starts_with(needle)) {
            continue;
        }
        matches.emplace_back(count, key.substr(needle.size()));
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first > rhs.first;
    });

    std::vector<std::string> corrections;
    corrections.reserve(maxLearnedCorrectionCandidates);
    if (!matches.empty()) {
        if (matches.size() == 1 || matches[0].first > matches[1].first) {
            corrections.push_back(std::move(matches[0].second));
        }
        return corrections;
    }
    const auto learnedRaw = selectedCounts_.find(rawPreferenceKey(preedit));
    if (learnedRaw != selectedCounts_.end() && learnedRaw->second > 0 &&
        looksLikeLearnedRawIdentifier(preedit)) {
        return corrections;
    }
    if (preedit.size() < 4 || preedit.size() > 16 || isKnownEnglishToken(preedit) ||
        looksLikeEnglishIdentifier(preedit)) {
        return corrections;
    }
    const auto scoredEvidence = [&generatedCorrectionPriority, preedit](
                                    std::string_view corrected,
                                    std::size_t evidence) -> std::optional<std::size_t> {
        if (!generatedCorrectionPriority) {
            return evidence;
        }
        const int priority = generatedCorrectionPriority(corrected);
        if (priority <= 0) {
            return std::nullopt;
        }
        constexpr std::size_t priorityStride = maxSavedLearningCount * 8;
        const bool suffixRepair = corrected.size() > preedit.size() && corrected.size() <= preedit.size() + 2 &&
                                  corrected.starts_with(preedit);
        const auto suffixBonus = suffixRepair ? maxSavedLearningCount * 2 : 0;
        return static_cast<std::size_t>(priority) * priorityStride + suffixBonus + evidence;
    };

    struct Pattern {
        enum class Kind { Missing, Extra, Replace, Transpose };
        Kind kind;
        std::string text;
        std::size_t position = 0;
        std::size_t count = 0;
        bool relativeToEnd = false;
    };

    std::vector<Pattern> patterns;
    const auto addPattern = [&patterns](Pattern::Kind kind, std::string text, std::size_t position,
                                        std::size_t count, bool relativeToEnd = false) {
        for (auto &pattern : patterns) {
            if (pattern.kind == kind && pattern.text == text && pattern.position == position &&
                pattern.relativeToEnd == relativeToEnd) {
                pattern.count += count;
                return;
            }
        }
        patterns.push_back({kind, std::move(text), position, count, relativeToEnd});
    };
    const auto mergePublishedPattern = [&patterns](Pattern::Kind kind, std::string text, std::size_t position,
                                                   std::size_t count, bool relativeToEnd) {
        for (auto &pattern : patterns) {
            if (pattern.kind == kind && pattern.text == text && pattern.position == position &&
                pattern.relativeToEnd == relativeToEnd) {
                pattern.count = std::max(pattern.count, count);
                return;
            }
        }
        patterns.push_back({kind, std::move(text), position, count, relativeToEnd});
    };

    // Exact typo pairs remain immediately useful, but broad patterns are published by
    // click-triggered TiP training once an edit habit crosses its safety threshold.
    // Falling back to pair aggregation keeps old preference files compatible.
    if (correctionPatternCounts_.empty() && keyHabitCounts_.empty()) {
        for (const auto &[key, count] : correctionCounts_) {
            if (count == 0) {
                continue;
            }
        const auto typoStart = key.find('\t');
        const auto correctionStart = typoStart == std::string::npos ? std::string::npos : key.find('\t', typoStart + 1);
        if (typoStart == std::string::npos || correctionStart == std::string::npos) {
            continue;
        }
        const auto typo = std::string_view(key).substr(typoStart + 1, correctionStart - typoStart - 1);
        const auto corrected = std::string_view(key).substr(correctionStart + 1);
        if (!isPlausibleCorrection(typo, corrected)) {
            continue;
        }
        if (corrected.size() == typo.size() + 1) {
            for (std::size_t index = 0; index < corrected.size(); ++index) {
                std::string repaired;
                repaired.reserve(corrected.size() - 1);
                repaired.append(corrected.substr(0, index));
                repaired.append(corrected.substr(index + 1));
                if (repaired == typo) {
                    const auto offsetFromTypoEnd = typo.size() - index;
                    if (offsetFromTypoEnd <= 2) {
                        addPattern(Pattern::Kind::Missing, std::string(corrected.substr(index, 1)),
                                   offsetFromTypoEnd, count, true);
                    } else {
                        addPattern(Pattern::Kind::Missing, std::string(corrected.substr(index, 1)), index, count);
                    }
                    break;
                }
            }
        } else if (typo.size() == corrected.size() + 1) {
            for (std::size_t index = 0; index < typo.size(); ++index) {
                std::string repaired;
                repaired.reserve(typo.size() - 1);
                repaired.append(typo.substr(0, index));
                repaired.append(typo.substr(index + 1));
                if (repaired == corrected) {
                    const auto offsetFromTypoEnd = typo.size() - index - 1;
                    if (offsetFromTypoEnd <= 1) {
                        addPattern(Pattern::Kind::Extra, std::string(typo.substr(index, 1)),
                                   offsetFromTypoEnd, count, true);
                    } else {
                        addPattern(Pattern::Kind::Extra, std::string(typo.substr(index, 1)), index, count);
                    }
                    break;
                }
            }
        } else if (typo.size() == corrected.size()) {
            std::vector<std::size_t> changedIndices;
            for (std::size_t index = 0; index < typo.size(); ++index) {
                if (typo[index] == corrected[index]) {
                    continue;
                }
                changedIndices.push_back(index);
                if (changedIndices.size() > 2) {
                    break;
                }
            }
            if (changedIndices.size() == 1) {
                const auto changedIndex = changedIndices.front();
                std::string replacement;
                replacement.push_back(typo[changedIndex]);
                replacement.push_back('\t');
                replacement.push_back(corrected[changedIndex]);
                const auto offsetFromTypoEnd = typo.size() - changedIndex - 1;
                if (offsetFromTypoEnd <= 1) {
                    std::string relativeReplacement;
                    relativeReplacement.push_back(typo[changedIndex]);
                    relativeReplacement.push_back('\t');
                    relativeReplacement.push_back(corrected[changedIndex]);
                    addPattern(Pattern::Kind::Replace, std::move(relativeReplacement), offsetFromTypoEnd, count, true);
                } else {
                    addPattern(Pattern::Kind::Replace, std::move(replacement), changedIndex, count);
                }
            } else if (changedIndices.size() == 2 && changedIndices[1] == changedIndices[0] + 1) {
                const auto changedIndex = changedIndices.front();
                if (typo[changedIndex] == corrected[changedIndex + 1] &&
                    typo[changedIndex + 1] == corrected[changedIndex]) {
                    std::string replacement;
                    replacement.append(typo.substr(changedIndex, 2));
                    replacement.push_back('\t');
                    replacement.append(corrected.substr(changedIndex, 2));
                    const auto offsetFromTypoEnd = typo.size() - changedIndex - 2;
                    if (offsetFromTypoEnd <= 1) {
                        addPattern(Pattern::Kind::Transpose, std::move(replacement), offsetFromTypoEnd, count, true);
                    } else {
                        addPattern(Pattern::Kind::Transpose, std::move(replacement), changedIndex, count);
                    }
                }
            }
        }
        }
    }

    for (const auto &[key, count] : correctionPatternCounts_) {
        const auto fields = splitTabFields(key);
        if (fields.size() != 6 || fields[0] != correctionPatternRecordPrefix) {
            continue;
        }
        const auto editKind = learnedEditKind(fields[1]);
        const auto position = parseBoundedIndex(fields[4], 63);
        if (!editKind || !position || (fields[5] != "0" && fields[5] != "1") ||
            !validLearnedEdit(*editKind, fields[2], fields[3]) ||
            count < correctionPatternActivationFor(*editKind)) {
            continue;
        }
        Pattern::Kind patternKind = Pattern::Kind::Missing;
        switch (*editKind) {
        case LearnedEditKind::Missing:
            patternKind = Pattern::Kind::Missing;
            break;
        case LearnedEditKind::Extra:
            patternKind = Pattern::Kind::Extra;
            break;
        case LearnedEditKind::Replace:
            patternKind = Pattern::Kind::Replace;
            break;
        case LearnedEditKind::Transpose:
            patternKind = Pattern::Kind::Transpose;
            break;
        }
        std::string text = *editKind == LearnedEditKind::Missing ? fields[3] : fields[2];
        if (*editKind == LearnedEditKind::Replace || *editKind == LearnedEditKind::Transpose) {
            text.push_back('\t');
            text.append(fields[3]);
        }
        mergePublishedPattern(patternKind, std::move(text), *position, count, fields[5] == "1");
    }

    std::stable_sort(patterns.begin(), patterns.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.count != rhs.count) {
            return lhs.count > rhs.count;
        }
        if (lhs.position != rhs.position) {
            return lhs.position < rhs.position;
        }
        if (lhs.relativeToEnd != rhs.relativeToEnd) {
            return !lhs.relativeToEnd;
        }
        return lhs.text < rhs.text;
    });

    for (const auto &pattern : patterns) {
        LearnedEditKind editKind = LearnedEditKind::Missing;
        switch (pattern.kind) {
        case Pattern::Kind::Missing:
            editKind = LearnedEditKind::Missing;
            break;
        case Pattern::Kind::Extra:
            editKind = LearnedEditKind::Extra;
            break;
        case Pattern::Kind::Replace:
            editKind = LearnedEditKind::Replace;
            break;
        case Pattern::Kind::Transpose:
            editKind = LearnedEditKind::Transpose;
            break;
        }
        if (pattern.count < correctionPatternActivationFor(editKind)) {
            continue;
        }
        std::string corrected;
        std::size_t position = pattern.position;
        if (pattern.relativeToEnd) {
            switch (pattern.kind) {
            case Pattern::Kind::Missing:
                if (pattern.position > preedit.size()) {
                    continue;
                }
                position = preedit.size() - pattern.position;
                break;
            case Pattern::Kind::Extra:
            case Pattern::Kind::Replace:
            case Pattern::Kind::Transpose: {
                const auto delimiter = pattern.text.find('\t');
                const auto width = delimiter == std::string::npos ? 1 : delimiter;
                if (pattern.position + width > preedit.size()) {
                    continue;
                }
                position = preedit.size() - pattern.position - width;
                break;
            }
            }
        }
        switch (pattern.kind) {
        case Pattern::Kind::Missing:
            if (position > preedit.size() ||
                (position < preedit.size() && preedit.substr(position, pattern.text.size()) == pattern.text)) {
                continue;
            }
            corrected.reserve(preedit.size() + pattern.text.size());
            corrected.append(preedit.substr(0, position));
            corrected.append(pattern.text);
            corrected.append(preedit.substr(position));
            break;
        case Pattern::Kind::Extra:
            if (position >= preedit.size() ||
                preedit.substr(position, pattern.text.size()) != pattern.text) {
                continue;
            }
            corrected.reserve(preedit.size() - pattern.text.size());
            corrected.append(preedit.substr(0, position));
            corrected.append(preedit.substr(position + pattern.text.size()));
            break;
        case Pattern::Kind::Replace:
        case Pattern::Kind::Transpose: {
            const auto delimiter = pattern.text.find('\t');
            if (delimiter == std::string::npos || delimiter + 1 >= pattern.text.size() ||
                position + delimiter > preedit.size() ||
                preedit.substr(position, delimiter) != std::string_view(pattern.text).substr(0, delimiter)) {
                continue;
            }
            const auto replacement = std::string_view(pattern.text).substr(delimiter + 1);
            corrected.reserve(preedit.size() - delimiter + replacement.size());
            corrected.append(preedit.substr(0, position));
            corrected.append(replacement);
            corrected.append(preedit.substr(position + delimiter));
            break;
        }
        }
        if (isPlausibleCorrection(preedit, corrected)) {
            const auto scoreValue = scoredEvidence(corrected, pattern.count * 4);
            if (!scoreValue) {
                continue;
            }
            auto &score = generatedScores[std::move(corrected)];
            score = std::max(score, *scoreValue);
        }
    }

    struct Habit {
        LearnedEditKind kind = LearnedEditKind::Missing;
        std::string typed;
        std::string replacement;
        std::size_t count = 0;
    };
    std::vector<Habit> habits;
    habits.reserve(keyHabitCounts_.size());
    for (const auto &[key, count] : keyHabitCounts_) {
        const auto fields = splitTabFields(key);
        if (fields.size() != 4 || fields[0] != keyHabitRecordPrefix) {
            continue;
        }
        const auto kind = learnedEditKind(fields[1]);
        if (!kind || !validLearnedEdit(*kind, fields[2], fields[3]) ||
            count < keyHabitActivationFor(*kind)) {
            continue;
        }
        habits.push_back({*kind, fields[2], fields[3], count});
    }
    std::stable_sort(habits.begin(), habits.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.count != rhs.count) {
            return lhs.count > rhs.count;
        }
        if (lhs.kind != rhs.kind) {
            return lhs.kind < rhs.kind;
        }
        if (lhs.typed != rhs.typed) {
            return lhs.typed < rhs.typed;
        }
        return lhs.replacement < rhs.replacement;
    });
    if (habits.size() > 8) {
        habits.resize(8);
    }

    const auto applyHabit = [](std::string_view input, const Habit &habit) {
        std::vector<std::string> results;
        if (habit.kind == LearnedEditKind::Missing) {
            results.reserve(input.size() + 1);
            for (std::size_t offset = 0; offset <= input.size(); ++offset) {
                const auto position = input.size() - offset;
                if (position < input.size() && input.substr(position, habit.replacement.size()) == habit.replacement) {
                    continue;
                }
                std::string corrected;
                corrected.reserve(input.size() + habit.replacement.size());
                corrected.append(input.substr(0, position));
                corrected.append(habit.replacement);
                corrected.append(input.substr(position));
                results.push_back(std::move(corrected));
            }
            return results;
        }
        if (habit.typed.empty() || input.size() < habit.typed.size()) {
            return results;
        }
        for (std::size_t position = 0; position + habit.typed.size() <= input.size(); ++position) {
            if (input.substr(position, habit.typed.size()) != habit.typed) {
                continue;
            }
            std::string corrected;
            corrected.reserve(input.size() - habit.typed.size() + habit.replacement.size());
            corrected.append(input.substr(0, position));
            corrected.append(habit.replacement);
            corrected.append(input.substr(position + habit.typed.size()));
            results.push_back(std::move(corrected));
        }
        return results;
    };

    struct ScoredCorrection {
        std::string text;
        std::size_t score = 0;
    };
    std::unordered_map<std::string, std::size_t> oneEditScores;
    for (const auto &habit : habits) {
        for (auto corrected : applyHabit(preedit, habit)) {
            if (!isPlausibleCorrection(preedit, corrected)) {
                continue;
            }
            auto &score = oneEditScores[std::move(corrected)];
            score = std::max(score, habit.count);
        }
    }
    const auto rankCorrections = [](const auto &scores) {
        std::vector<ScoredCorrection> ranked;
        ranked.reserve(scores.size());
        for (const auto &[text, score] : scores) {
            ranked.push_back({text, score});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.text < rhs.text;
        });
        return ranked;
    };
    auto oneEdit = rankCorrections(oneEditScores);
    if (oneEdit.size() > 128) {
        oneEdit.resize(128);
    }
    for (const auto &candidate : oneEdit) {
        const auto scoreValue = scoredEvidence(candidate.text, candidate.score);
        if (scoreValue) {
            auto &score = generatedScores[candidate.text];
            score = std::max(score, *scoreValue);
        }
    }

    // Combining two habits is useful for sentence-length input, but on a short
    // token it tends to turn one real omission into an over-corrected phrase.
    constexpr std::size_t minTwoEditPreeditLength = 8;
    if (habits.size() >= 2 && preedit.size() >= minTwoEditPreeditLength) {
        std::unordered_map<std::string, std::size_t> twoEditScores;
        constexpr std::size_t maxAcceptedPerHabitPair = 8;
        for (std::size_t firstHabit = 0; firstHabit < habits.size(); ++firstHabit) {
            if (habits[firstHabit].kind != LearnedEditKind::Missing) {
                continue;
            }
            const auto firstResults = applyHabit(preedit, habits[firstHabit]);
            for (std::size_t secondHabit = firstHabit + 1; secondHabit < habits.size(); ++secondHabit) {
                if (habits[secondHabit].kind != LearnedEditKind::Missing) {
                    continue;
                }
                std::size_t acceptedForPair = 0;
                for (const auto &firstResult : firstResults) {
                    for (auto corrected : applyHabit(firstResult, habits[secondHabit])) {
                        if (corrected == preedit || !isPlausibleCorrection(preedit, corrected)) {
                            continue;
                        }
                        const auto combinedEvidence = habits[firstHabit].count + habits[secondHabit].count;
                        const auto evidence = combinedEvidence > 2 ? combinedEvidence - 2 : combinedEvidence;
                        const auto scoreValue = scoredEvidence(corrected, evidence);
                        if (!scoreValue) {
                            continue;
                        }
                        const bool inserted = !twoEditScores.contains(corrected);
                        auto &score = twoEditScores[std::move(corrected)];
                        score = std::max(score, *scoreValue);
                        if (inserted && ++acceptedForPair >= maxAcceptedPerHabitPair) {
                            break;
                        }
                    }
                    if (acceptedForPair >= maxAcceptedPerHabitPair) {
                        break;
                    }
                }
            }
        }
        for (const auto &candidate : rankCorrections(twoEditScores)) {
            auto &score = generatedScores[candidate.text];
            score = std::max(score, candidate.score);
        }
    }
    for (const auto &candidate : rankCorrections(generatedScores)) {
        if (corrections.size() >= maxLearnedCorrectionCandidates) {
            break;
        }
        corrections.push_back(candidate.text);
    }
    return corrections;
}

std::vector<std::string> InputModel::applyLearnedPreferences(std::string_view preedit,
                                                             const std::vector<std::string> &candidates,
                                                             ModelRequestState state) const {
    return localRerankCandidates(preedit, candidates, state);
}

std::vector<std::string> InputModel::learnedSegmentCandidates(std::string_view preedit) const {
    if (preedit.empty()) {
        return {};
    }

    const auto hasSupportingPreference = [this](std::string_view originalPreedit,
                                                std::string_view combinedCandidate) {
        const std::string prefix = std::string(originalPreedit) + '\t';
        return std::any_of(selectedCounts_.begin(), selectedCounts_.end(), [&](const auto &entry) {
            if (!entry.first.starts_with(prefix)) {
                return false;
            }
            const auto candidate = std::string_view(entry.first).substr(prefix.size());
            return candidate != originalPreedit &&
                   entry.second >= preferenceActivationCount(originalPreedit, candidate) &&
                   combinedCandidate.starts_with(candidate);
        });
    };
    std::unordered_map<std::string, std::size_t> candidateCounts;
    for (const auto &[key, count] : segmentChainCounts_) {
        if (count == 0 || !key.starts_with(segmentChainRecordPrefix)) {
            continue;
        }
        const auto fields = splitTabFields(key);
        if (fields.size() != 7 || (fields[1] != preedit && fields[5] != preedit) || fields[6].empty()) {
            continue;
        }
        if (count < 2 && !hasSupportingPreference(fields[1], fields[6])) {
            continue;
        }
        auto &strongest = candidateCounts[fields[6]];
        strongest = std::max(strongest, count);
    }

    std::vector<std::pair<std::string, std::size_t>> ranked(candidateCounts.begin(), candidateCounts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    if (ranked.size() > 8) {
        ranked.resize(8);
    }

    std::vector<std::string> result;
    result.reserve(ranked.size());
    for (auto &entry : ranked) {
        result.push_back(std::move(entry.first));
    }
    return result;
}

std::string InputModel::modelRequest(std::string_view preedit, const std::vector<std::string> &candidates,
                                     ModelRequestState state, std::string_view application) const {
    return modelRequestPayload(preedit, candidates, application, std::move(state), events_, correctionEvents_,
                               recentCommits_, recentSegmentChains_, selectedCounts_, correctionCounts_,
                               segmentChainCounts_);
}

std::vector<std::string> InputModel::rerankCandidates(std::string_view preedit,
                                                      const std::vector<std::string> &candidates,
                                                      ModelRequestState state,
                                                      std::string_view application,
                                                      bool allowExternalModel) {
    if (allowExternalModel) {
        if (auto external = externalRerankCandidates(preedit, candidates, state, application); !external.empty()) {
            return external;
        }
    }
    return localRerankCandidates(preedit, candidates, state, application);
}

std::vector<std::string> InputModel::localRerankCandidates(std::string_view preedit,
                                                           const std::vector<std::string> &candidates,
                                                           const ModelRequestState &state,
                                                           std::string_view application) const {
    (void)application;
    auto ranked = candidates;
    if (ranked.size() < 2) {
        return ranked;
    }

    const auto score = [&](const std::string &candidate, std::size_t originalIndex) {
        std::size_t value = 1000 - std::min<std::size_t>(originalIndex, 999);
        const bool prefixOnlyCandidate = isPartialCandidateForRequest(preedit, ranked, state, candidate);
        if (!prefixOnlyCandidate) {
            if (canApplyLearnedCandidatePreference(preedit, candidate, ranked)) {
                if (const auto iter = selectedCounts_.find(preferenceKey(preedit, candidate));
                    iter != selectedCounts_.end() &&
                    iter->second >= preferenceActivationCount(preedit, candidate)) {
                    value += iter->second * 2000;
                }
            }
        }
        value += learnedSegmentContinuationBoost(recentCommits_, segmentChainCounts_, preedit, candidate);
        return value;
    };

    std::vector<std::size_t> originalIndex(ranked.size());
    for (std::size_t index = 0; index < originalIndex.size(); ++index) {
        originalIndex[index] = index;
    }

    std::stable_sort(originalIndex.begin(), originalIndex.end(), [&](std::size_t lhs, std::size_t rhs) {
        return score(ranked[lhs], lhs) > score(ranked[rhs], rhs);
    });

    std::vector<std::string> sorted;
    sorted.reserve(ranked.size());
    for (const auto index : originalIndex) {
        sorted.push_back(ranked[index]);
    }
    return sorted;
}

std::vector<std::string> InputModel::externalRerankCandidates(std::string_view preedit,
                                                              const std::vector<std::string> &candidates,
                                                              ModelRequestState state,
                                                              std::string_view application) {
    const auto command = configuredModelCommand();
    if (!command) {
        return {};
    }

    const auto output = invokeModelCommand(
        *command, modelRequestPayload(preedit, candidates, application, state, events_, correctionEvents_,
                                      recentCommits_, recentSegmentChains_, selectedCounts_, correctionCounts_,
                                      segmentChainCounts_));
    if (!output) {
        return {};
    }

    return applyExternalModelOutput(preedit, candidates, std::move(state), *output);
}

std::vector<std::string> InputModel::applyExternalModelOutput(std::string_view preedit,
                                                              const std::vector<std::string> &candidates,
                                                              ModelRequestState state,
                                                              std::string_view output) {

    std::unordered_set<std::string> allowed(candidates.begin(), candidates.end());
    std::unordered_set<std::string> emitted;
    std::vector<std::string> reranked;
    std::string line;
    bool learnedExternalCorrection = false;
    bool learnedExternalCandidatePreference = false;
    bool learnedExternalSegmentChain = false;
    std::size_t acceptedCandidateRank = 0;
    const auto candidateAllowed = [&](const std::string &candidate) {
        return allowed.contains(candidate) ||
               (candidate == preedit && looksLikeLearnedRawIdentifier(preedit));
    };
    const auto correctionRelevantToRequest = [&](const std::string &typo, const std::string &correction) {
        return !preedit.empty() && (typo == preedit || correction == preedit);
    };
    const auto pendingSegmentChainAllowed = [&](const SegmentChain &chain) {
        if (chain.remainingPreedit != preedit || !chain.combinedCandidate.starts_with(chain.committedText)) {
            return false;
        }
        const auto suffixCandidate = chain.combinedCandidate.substr(chain.committedText.size());
        if (suffixCandidate.empty() || !candidateAllowed(suffixCandidate)) {
            return false;
        }
        if (state.candidateCursor == 0 || state.candidateCursor >= candidates.size() ||
            candidates[state.candidateCursor] != suffixCandidate) {
            return false;
        }
        return std::any_of(state.pendingSegments.begin(), state.pendingSegments.end(), [&](const auto &segment) {
            return segment.originalPreedit == chain.originalPreedit &&
                   segment.consumedPreedit == chain.consumedPreedit &&
                   segment.committedText == chain.committedText &&
                   segment.remainingPreedit == chain.remainingPreedit;
        });
    };
    const auto rememberExternalCandidate = [&](const std::string &candidate) {
        if (acceptedCandidateRank != 1) {
            return;
        }
        if (!isSafeStoredText(preedit) || !isSafeStoredText(candidate)) {
            return;
        }
        if (isPartialCandidateForRequest(preedit, candidates, state, candidate)) {
            return;
        }
        if (!candidates.empty() && candidates.front() == candidate && candidate != preedit) {
            return;
        }
        if (!canApplyLearnedCandidatePreference(preedit, candidate, candidates)) {
            return;
        }
        const auto key = preferenceKey(preedit, candidate);
        if (const auto existing = selectedCounts_.find(key);
            existing != selectedCounts_.end() &&
            existing->second >= preferenceActivationCount(preedit, candidate)) {
            return;
        }
        const auto weight = candidate == preedit ? std::size_t{3} : std::size_t{2};
        addBoundedCount(selectedCounts_[key], weight);
        learnedExternalCandidatePreference = true;
    };
    const auto promoteAcceptedCandidate = [&](const std::string &candidate) {
        if (candidateAllowed(candidate) && emitted.insert(candidate).second) {
            reranked.push_back(candidate);
            ++acceptedCandidateRank;
            return true;
        }
        return false;
    };
    std::istringstream outputStream{std::string(output)};
    while (std::getline(outputStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto fields = splitTabFields(line);
        if (!fields.empty()) {
            if (fields[0] == "candidate" && fields.size() == 2) {
                const auto &candidate = fields[1];
                if (promoteAcceptedCandidate(candidate)) {
                    rememberExternalCandidate(candidate);
                }
                continue;
            }
            if (fields[0] == "correction" && fields.size() == 3) {
                const auto &typo = fields[1];
                const auto &correction = fields[2];
                const auto key = correctionKey(typo, correction);
                if (isSafeStoredText(typo) && isSafeStoredText(correction) && isPlausibleCorrection(typo, correction) &&
                    correctionRelevantToRequest(typo, correction) && !correctionCounts_.contains(key)) {
                    addBoundedCount(correctionCounts_[key], 2);
                    ++correctionVersion_;
                    learnedExternalCorrection = true;
                }
                continue;
            }
            if (fields[0] == "preference" && (fields.size() == 3 || fields.size() == 4)) {
                const auto &learnedPreedit = fields[1];
                const auto &candidate = fields[2];
                const auto weight = fields.size() == 4 ? parseCount(fields[3]).value_or(0) : std::size_t{2};
                if (weight > 0 && learnedPreedit == preedit && candidateAllowed(candidate) &&
                    !isPartialCandidateForRequest(preedit, candidates, state, candidate) &&
                    canApplyLearnedCandidatePreference(learnedPreedit, candidate, candidates) &&
                    isSafeStoredText(learnedPreedit) && isSafeStoredText(candidate)) {
                    const auto key = candidate == learnedPreedit && looksLikeLearnedRawIdentifier(learnedPreedit)
                                         ? rawPreferenceKey(learnedPreedit)
                                         : preferenceKey(learnedPreedit, candidate);
                    const auto existing = selectedCounts_.find(key);
                    const bool activeExisting = existing != selectedCounts_.end() &&
                                                existing->second >=
                                                    preferenceActivationCount(learnedPreedit, candidate);
                    if (!activeExisting) {
                        const auto clampedWeight = std::min<std::size_t>(weight, 20);
                        addBoundedCount(selectedCounts_[key], clampedWeight);
                        learnedExternalCandidatePreference = true;
                    }
                    promoteAcceptedCandidate(candidate);
                }
                continue;
            }
            if (fields[0] == "segment_chain" && (fields.size() == 7 || fields.size() == 8)) {
                SegmentChain chain{fields[1], fields[2], fields[3], fields[4], fields[5], fields[6]};
                const auto weight = fields.size() == 8 ? parseCount(fields[7]).value_or(0) : std::size_t{1};
                const bool currentPreeditChain = chain.originalPreedit == preedit && candidateAllowed(chain.combinedCandidate);
                const bool pendingContinuationChain = pendingSegmentChainAllowed(chain);
                if (weight > 0 && (currentPreeditChain || pendingContinuationChain) && isPlausibleSegmentChain(chain)) {
                    const auto key = segmentChainRecordKey(chain);
                    if (!segmentChainCounts_.contains(key)) {
                        const auto clampedWeight = std::min<std::size_t>(weight, 20);
                        addBoundedCount(segmentChainCounts_[key], clampedWeight);
                        addBoundedCount(selectedCounts_[preferenceKey(chain.originalPreedit, chain.combinedCandidate)],
                                        clampedWeight);
                        if (chain.correctedFullPreedit != chain.originalPreedit) {
                            const auto correctionKey = InputModel::correctionKey(chain.originalPreedit, chain.correctedFullPreedit);
                            if (!correctionCounts_.contains(correctionKey)) {
                                addBoundedCount(correctionCounts_[correctionKey], 2);
                                ++correctionVersion_;
                                learnedExternalCorrection = true;
                            }
                        }
                        learnedExternalCandidatePreference = true;
                        learnedExternalSegmentChain = true;
                    }
                    if (currentPreeditChain) {
                        promoteAcceptedCandidate(chain.combinedCandidate);
                    }
                }
                continue;
            }
        }
        if (promoteAcceptedCandidate(line)) {
            rememberExternalCandidate(line);
        }
    }
    if (learnedExternalCorrection || learnedExternalCandidatePreference || learnedExternalSegmentChain) {
        savePreferences();
    }

    for (const auto &candidate : candidates) {
        if (emitted.insert(candidate).second) {
            reranked.push_back(candidate);
        }
    }
    const bool addedRawPreedit = emitted.contains(std::string(preedit)) &&
                                 std::find(candidates.begin(), candidates.end(), preedit) == candidates.end();
    const auto expectedSize = candidates.size() + (addedRawPreedit ? 1 : 0);
    return reranked.size() == expectedSize ? reranked : std::vector<std::string>{};
}

void InputModel::learnCorrectionFromRecentEvents(std::string_view correctedPreedit) {
    const auto typo = recentFullyErasedInputBefore(correctedPreedit);
    if (!typo || !isSafeStoredText(*typo) || !isSafeStoredText(correctedPreedit) ||
        !isPlausibleCorrection(*typo, correctedPreedit)) {
        return;
    }
    addBoundedCount(correctionCounts_[correctionKey(*typo, correctedPreedit)], 1);
    ++correctionVersion_;
}

void InputModel::recordCorrectionEvent(InputEventType type, std::string_view text) {
    switch (type) {
    case InputEventType::Letter:
    case InputEventType::Digit:
    case InputEventType::Symbol:
    case InputEventType::Backspace:
    case InputEventType::Delete:
    case InputEventType::Space:
    case InputEventType::Enter:
    case InputEventType::ObservedKey:
    case InputEventType::CandidateSelected:
    case InputEventType::RawCommitted:
    case InputEventType::CursorMove:
    case InputEventType::AiRerankRequested:
    case InputEventType::Escape:
        if (correctionEvents_.size() == maxCorrectionEvents_) {
            correctionEvents_.erase(correctionEvents_.begin());
        }
        correctionEvents_.push_back({type, std::string(text)});
        break;
    }
}

void InputModel::recordRecentCommit(std::string_view text) {
    if (!isSafeStoredText(text)) {
        return;
    }
    if (recentCommits_.size() == maxRecentCommits_) {
        recentCommits_.erase(recentCommits_.begin());
    }
    recentCommits_.push_back(std::string(text));
}

std::optional<std::string> InputModel::recentFullyErasedInputBefore(std::string_view correctedPreedit) const {
    std::string current;
    std::size_t cursor = 0;
    std::string erasedOriginal;
    std::optional<std::string> lastFullyErased;
    std::optional<std::string> lastEditedOriginal;
    std::optional<std::string> inPreeditEditOriginal;
    bool erasing = false;

    const auto rememberInPreeditEdit = [&]() {
        if (!current.empty() && !inPreeditEditOriginal) {
            inPreeditEditOriginal = current;
        }
    };

    for (const auto &event : correctionEvents_) {
        switch (event.type) {
        case InputEventType::Letter:
        case InputEventType::Digit:
        case InputEventType::Symbol:
            if (erasing && !erasedOriginal.empty()) {
                lastEditedOriginal = erasedOriginal;
            }
            erasing = false;
            cursor = std::min(cursor, current.size());
            if (cursor < current.size()) {
                rememberInPreeditEdit();
            }
            current.insert(cursor, event.text);
            cursor += event.text.size();
            break;
        case InputEventType::Backspace:
            cursor = std::min(cursor, current.size());
            if (current.empty() || cursor == 0) {
                erasing = false;
                erasedOriginal.clear();
                break;
            }
            if (cursor < current.size()) {
                rememberInPreeditEdit();
            }
            if (!erasing) {
                erasing = true;
                erasedOriginal = current;
            }
            current.erase(cursor - 1, 1);
            --cursor;
            if (current.empty() && !erasedOriginal.empty()) {
                lastFullyErased = erasedOriginal;
            }
            break;
        case InputEventType::Delete:
            cursor = std::min(cursor, current.size());
            if (current.empty() || cursor >= current.size()) {
                break;
            }
            rememberInPreeditEdit();
            if (!erasing) {
                erasing = true;
                erasedOriginal = current;
            }
            current.erase(cursor, 1);
            if (current.empty() && !erasedOriginal.empty()) {
                lastFullyErased = erasedOriginal;
            }
            break;
        case InputEventType::CursorMove:
            if (event.text == "Left" || event.text == "KP_Left") {
                cursor = cursor == 0 ? 0 : cursor - 1;
            } else if (event.text == "Right" || event.text == "KP_Right") {
                cursor = std::min(cursor + 1, current.size());
            }
            erasing = false;
            erasedOriginal.clear();
            break;
        case InputEventType::CandidateSelected:
        case InputEventType::RawCommitted:
        case InputEventType::Escape:
            current.clear();
            cursor = 0;
            erasedOriginal.clear();
            lastFullyErased.reset();
            lastEditedOriginal.reset();
            inPreeditEditOriginal.reset();
            erasing = false;
            break;
        case InputEventType::Space:
        case InputEventType::Enter:
        case InputEventType::ObservedKey:
        case InputEventType::AiRerankRequested:
            break;
        }
    }

    if (current == correctedPreedit) {
        if (lastFullyErased && isPlausibleCorrection(*lastFullyErased, correctedPreedit)) {
            return lastFullyErased;
        }
        if (lastEditedOriginal && isPlausibleCorrection(*lastEditedOriginal, correctedPreedit)) {
            return lastEditedOriginal;
        }
        if (inPreeditEditOriginal && isPlausibleCorrection(*inPreeditEditOriginal, correctedPreedit)) {
            return inPreeditEditOriginal;
        }
    }
    return std::nullopt;
}

std::filesystem::path InputModel::defaultPreferencePath() {
    if (const char *xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdgDataHome) / "tipe" / "candidate-preferences.tsv";
    }
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "tipe" / "candidate-preferences.tsv";
    }
    return std::filesystem::temp_directory_path() / "tipe-candidate-preferences.tsv";
}

void InputModel::loadPreferences() {
    selectedCounts_.clear();
    rawTokenCounts_.clear();
    correctionCounts_.clear();
    correctionPatternCounts_.clear();
    keyHabitCounts_.clear();
    segmentChainCounts_.clear();
    persistedSelectedCounts_.clear();
    persistedRawTokenCounts_.clear();
    persistedCorrectionCounts_.clear();
    persistedCorrectionPatternCounts_.clear();
    persistedKeyHabitCounts_.clear();
    persistedSegmentChainCounts_.clear();
    ++correctionVersion_;
    std::ifstream input(preferencePath_);
    if (!input) {
        preferenceMtime_.reset();
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto firstDelimiter = line.find('\t');
        if (firstDelimiter == std::string::npos || firstDelimiter == 0) {
            continue;
        }

        if (line.substr(0, firstDelimiter) == segmentChainRecordPrefix) {
            std::vector<std::string_view> fields;
            std::size_t begin = 0;
            while (begin <= line.size()) {
                const auto delimiter = line.find('\t', begin);
                const auto end = delimiter == std::string::npos ? line.size() : delimiter;
                fields.push_back(std::string_view(line).substr(begin, end - begin));
                if (delimiter == std::string::npos) {
                    break;
                }
                begin = delimiter + 1;
            }
            if (fields.size() != 8) {
                continue;
            }
            const auto count = parseCount(fields[7]);
            if (!count) {
                continue;
            }
            SegmentChain chain{std::string(fields[1]), std::string(fields[2]), std::string(fields[3]),
                               std::string(fields[4]), std::string(fields[5]), std::string(fields[6])};
            if (!isPlausibleSegmentChain(chain)) {
                continue;
            }
            segmentChainCounts_[segmentChainRecordKey(chain)] = *count;
            continue;
        }

        if (line.substr(0, firstDelimiter) == correctionRecordPrefix) {
            const auto secondDelimiter = line.find('\t', firstDelimiter + 1);
            const auto thirdDelimiter = secondDelimiter == std::string::npos
                                            ? std::string::npos
                                            : line.find('\t', secondDelimiter + 1);
            if (secondDelimiter == std::string::npos || thirdDelimiter == std::string::npos) {
                continue;
            }
            const auto count = parseCount(std::string_view(line).substr(thirdDelimiter + 1));
            if (!count) {
                continue;
            }
            const auto typo = line.substr(firstDelimiter + 1, secondDelimiter - firstDelimiter - 1);
            const auto correction = line.substr(secondDelimiter + 1, thirdDelimiter - secondDelimiter - 1);
            if (!isSafeStoredText(typo) || !isSafeStoredText(correction) ||
                !isPlausibleCorrection(typo, correction)) {
                continue;
            }
            correctionCounts_[correctionKey(typo, correction)] = *count;
            continue;
        }

        if (line.substr(0, firstDelimiter) == correctionPatternRecordPrefix) {
            const auto fields = splitTabFields(line);
            if (fields.size() != 7) {
                continue;
            }
            const auto kind = learnedEditKind(fields[1]);
            const auto position = parseBoundedIndex(fields[4], 63);
            const auto count = parseCount(fields[6]);
            if (!kind || !validLearnedEdit(*kind, fields[2], fields[3]) || !position ||
                (fields[5] != "0" && fields[5] != "1") || !count) {
                continue;
            }
            correctionPatternCounts_[line.substr(0, line.rfind('\t'))] = *count;
            continue;
        }

        if (line.substr(0, firstDelimiter) == keyHabitRecordPrefix) {
            const auto fields = splitTabFields(line);
            if (fields.size() != 5) {
                continue;
            }
            const auto kind = learnedEditKind(fields[1]);
            const auto count = parseCount(fields[4]);
            if (!kind || !validLearnedEdit(*kind, fields[2], fields[3]) || !count) {
                continue;
            }
            keyHabitCounts_[line.substr(0, line.rfind('\t'))] = *count;
            continue;
        }

        if (line.substr(0, firstDelimiter) == rawTokenRecordPrefix) {
            const auto secondDelimiter = line.find('\t', firstDelimiter + 1);
            if (secondDelimiter == std::string::npos || line.find('\t', secondDelimiter + 1) != std::string::npos) {
                continue;
            }
            const auto token = line.substr(firstDelimiter + 1, secondDelimiter - firstDelimiter - 1);
            const auto count = parseCount(std::string_view(line).substr(secondDelimiter + 1));
            if (!count || token.size() < 2 || token != asciiLower(token) ||
                !std::all_of(token.begin(), token.end(), [](unsigned char ch) { return std::isalpha(ch); })) {
                continue;
            }
            rawTokenCounts_[token] = *count;
            continue;
        }

        const auto secondDelimiter = line.find('\t', firstDelimiter + 1);
        const auto thirdDelimiter =
            secondDelimiter == std::string::npos ? std::string::npos : line.find('\t', secondDelimiter + 1);

        std::optional<std::size_t> count;
        std::string key;
        if (secondDelimiter == std::string::npos) {
            key = line.substr(0, firstDelimiter);
            count = parseCount(std::string_view(line).substr(firstDelimiter + 1));
            if (!isSafeStoredText(key)) {
                continue;
            }
        } else if (thirdDelimiter == std::string::npos) {
            const auto preedit = line.substr(0, firstDelimiter);
            const auto candidate = line.substr(firstDelimiter + 1, secondDelimiter - firstDelimiter - 1);
            if (!isSafeStoredText(preedit) || !isSafeStoredText(candidate)) {
                continue;
            }
            key = preferenceKey(preedit, candidate);
            count = parseCount(std::string_view(line).substr(secondDelimiter + 1));
        } else {
            continue;
        }
        if (count) {
            selectedCounts_[key] = *count;
        }
    }
    retainStrongestCounts(selectedCounts_, maxSavedPreferenceRows);
    retainStrongestCounts(rawTokenCounts_, maxSavedRawTokenRows);
    retainStrongestCounts(correctionCounts_, maxSavedCorrectionRows);
    retainStrongestCounts(correctionPatternCounts_, maxSavedCorrectionPatternRows);
    retainStrongestCounts(keyHabitCounts_, maxSavedKeyHabitRows);
    retainStrongestCounts(segmentChainCounts_, maxSavedSegmentChainRows);
    std::error_code error;
    const auto currentMtime = std::filesystem::last_write_time(preferencePath_, error);
    preferenceMtime_ = error ? std::nullopt : std::optional{currentMtime};
    persistedSelectedCounts_ = selectedCounts_;
    persistedRawTokenCounts_ = rawTokenCounts_;
    persistedCorrectionCounts_ = correctionCounts_;
    persistedCorrectionPatternCounts_ = correctionPatternCounts_;
    persistedKeyHabitCounts_ = keyHabitCounts_;
    persistedSegmentChainCounts_ = segmentChainCounts_;
}

void InputModel::promoteExplicitCandidatePreference(std::string_view preedit, std::string_view candidate) {
    const auto selectedKey = preferenceKey(preedit, candidate);
    const std::string competingPrefix = std::string(preedit) + '\t';
    std::size_t strongestCompetitor = 0;
    for (const auto &[key, count] : selectedCounts_) {
        if (key != selectedKey && key.starts_with(competingPrefix)) {
            strongestCompetitor = std::max(strongestCompetitor, count);
        }
    }

    auto &selectedCount = selectedCounts_[selectedKey];
    if (selectedCount > strongestCompetitor) {
        return;
    }
    if (strongestCompetitor < maxSavedLearningCount) {
        selectedCount = strongestCompetitor + 1;
        return;
    }

    selectedCount = maxSavedLearningCount;
    for (auto &[key, count] : selectedCounts_) {
        if (key != selectedKey && key.starts_with(competingPrefix) && count == maxSavedLearningCount) {
            count = maxSavedLearningCount - 1;
        }
    }
}

void InputModel::savePreferences(
    std::optional<std::pair<std::string, std::string>> explicitCandidatePromotion) {
    std::error_code error;
    std::filesystem::create_directories(preferencePath_.parent_path(), error);
    PreferenceFileLock lock(preferencePath_);
    if (!lock.locked()) {
        return;
    }

    const auto localSelected = selectedCounts_;
    const auto localRawTokens = rawTokenCounts_;
    const auto localCorrections = correctionCounts_;
    const auto localCorrectionPatterns = correctionPatternCounts_;
    const auto localKeyHabits = keyHabitCounts_;
    const auto localSegmentChains = segmentChainCounts_;
    const auto baselineSelected = persistedSelectedCounts_;
    const auto baselineRawTokens = persistedRawTokenCounts_;
    const auto baselineCorrections = persistedCorrectionCounts_;
    const auto baselineCorrectionPatterns = persistedCorrectionPatternCounts_;
    const auto baselineKeyHabits = persistedKeyHabitCounts_;
    const auto baselineSegmentChains = persistedSegmentChainCounts_;
    const auto diskMtime = std::filesystem::last_write_time(preferencePath_, error);
    const bool diskChanged = error ? preferenceMtime_.has_value() : !preferenceMtime_ || diskMtime != *preferenceMtime_;
    if (diskChanged) {
        if (error) {
            selectedCounts_.clear();
            rawTokenCounts_.clear();
            correctionCounts_.clear();
            correctionPatternCounts_.clear();
            keyHabitCounts_.clear();
            segmentChainCounts_.clear();
            persistedSelectedCounts_.clear();
            persistedRawTokenCounts_.clear();
            persistedCorrectionCounts_.clear();
            persistedCorrectionPatternCounts_.clear();
            persistedKeyHabitCounts_.clear();
            persistedSegmentChainCounts_.clear();
            preferenceMtime_.reset();
            ++correctionVersion_;
        } else {
            loadPreferences();
        }
        mergeLocalDeltas(selectedCounts_, localSelected, baselineSelected);
        mergeLocalDeltas(rawTokenCounts_, localRawTokens, baselineRawTokens);
        mergeLocalDeltas(correctionCounts_, localCorrections, baselineCorrections);
        mergeLocalDeltas(correctionPatternCounts_, localCorrectionPatterns, baselineCorrectionPatterns);
        mergeLocalDeltas(keyHabitCounts_, localKeyHabits, baselineKeyHabits);
        mergeLocalDeltas(segmentChainCounts_, localSegmentChains, baselineSegmentChains);
    }
    if (explicitCandidatePromotion) {
        promoteExplicitCandidatePreference(explicitCandidatePromotion->first, explicitCandidatePromotion->second);
    }
    retainStrongestCounts(selectedCounts_, maxSavedPreferenceRows);
    retainStrongestCounts(rawTokenCounts_, maxSavedRawTokenRows);
    retainStrongestCounts(correctionCounts_, maxSavedCorrectionRows);
    retainStrongestCounts(correctionPatternCounts_, maxSavedCorrectionPatternRows);
    retainStrongestCounts(keyHabitCounts_, maxSavedKeyHabitRows);
    retainStrongestCounts(segmentChainCounts_, maxSavedSegmentChainRows);

    const auto temporaryPath = preferencePath_.string() + ".tmp." + std::to_string(::getpid());
    const int temporaryDescriptor = ::open(temporaryPath.c_str(), O_CREAT | O_CLOEXEC | O_TRUNC | O_WRONLY, 0600);
    if (temporaryDescriptor < 0) {
        return;
    }
    const bool temporaryPrivate = ::fchmod(temporaryDescriptor, 0600) == 0;
    const bool temporaryClosed = ::close(temporaryDescriptor) == 0;
    if (!temporaryPrivate || !temporaryClosed) {
        std::filesystem::remove(temporaryPath);
        return;
    }
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) {
        std::filesystem::remove(temporaryPath);
        return;
    }

    std::vector<std::pair<std::string, std::size_t>> selectedRows(selectedCounts_.begin(), selectedCounts_.end());
    std::sort(selectedRows.begin(), selectedRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    std::size_t emittedSelected = 0;
    for (const auto &[key, count] : selectedRows) {
        if (emittedSelected >= maxSavedPreferenceRows) {
            break;
        }
        output << key << '\t' << count << '\n';
        ++emittedSelected;
    }

    std::vector<std::pair<std::string, std::size_t>> rawTokenRows(rawTokenCounts_.begin(), rawTokenCounts_.end());
    std::sort(rawTokenRows.begin(), rawTokenRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    std::size_t emittedRawTokens = 0;
    for (const auto &[token, count] : rawTokenRows) {
        if (emittedRawTokens >= maxSavedRawTokenRows) {
            break;
        }
        output << rawTokenRecordPrefix << '\t' << token << '\t' << count << '\n';
        ++emittedRawTokens;
    }

    std::vector<std::pair<std::string, std::size_t>> correctionRows(correctionCounts_.begin(), correctionCounts_.end());
    std::sort(correctionRows.begin(), correctionRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    std::size_t emittedCorrections = 0;
    for (const auto &[key, count] : correctionRows) {
        if (emittedCorrections >= maxSavedCorrectionRows) {
            break;
        }
        const auto typoStart = key.find('\t');
        const auto correctionStart = typoStart == std::string::npos ? std::string::npos : key.find('\t', typoStart + 1);
        if (typoStart == std::string::npos || correctionStart == std::string::npos) {
            continue;
        }
        output << correctionRecordPrefix << '\t' << key.substr(typoStart + 1, correctionStart - typoStart - 1)
               << '\t' << key.substr(correctionStart + 1) << '\t' << count << '\n';
        ++emittedCorrections;
    }
    const auto emitSerializedRows = [&output](const auto &counts, std::size_t limit) {
        std::vector<std::pair<std::string, std::size_t>> rows(counts.begin(), counts.end());
        std::sort(rows.begin(), rows.end(), [](const auto &lhs, const auto &rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        });
        const auto rowCount = std::min(limit, rows.size());
        for (std::size_t index = 0; index < rowCount; ++index) {
            output << rows[index].first << '\t' << rows[index].second << '\n';
        }
    };
    emitSerializedRows(correctionPatternCounts_, maxSavedCorrectionPatternRows);
    emitSerializedRows(keyHabitCounts_, maxSavedKeyHabitRows);
    std::vector<std::pair<std::string, std::size_t>> segmentChainRows(segmentChainCounts_.begin(),
                                                                      segmentChainCounts_.end());
    std::sort(segmentChainRows.begin(), segmentChainRows.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    });
    std::size_t emittedSegmentChains = 0;
    for (const auto &[key, count] : segmentChainRows) {
        if (emittedSegmentChains >= maxSavedSegmentChainRows) {
            break;
        }
        output << key << '\t' << count << '\n';
        ++emittedSegmentChains;
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporaryPath);
        return;
    }
    std::filesystem::permissions(temporaryPath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
        std::filesystem::remove(temporaryPath);
        return;
    }
    error.clear();
    std::filesystem::rename(temporaryPath, preferencePath_, error);
    if (error) {
        std::filesystem::remove(temporaryPath);
        return;
    }
    const auto currentMtime = std::filesystem::last_write_time(preferencePath_, error);
    preferenceMtime_ = error ? std::nullopt : std::optional{currentMtime};
    persistedSelectedCounts_ = selectedCounts_;
    persistedRawTokenCounts_ = rawTokenCounts_;
    persistedCorrectionCounts_ = correctionCounts_;
    persistedCorrectionPatternCounts_ = correctionPatternCounts_;
    persistedKeyHabitCounts_ = keyHabitCounts_;
    persistedSegmentChainCounts_ = segmentChainCounts_;
}

std::string InputModel::preferenceKey(std::string_view preedit, std::string_view candidate) {
    std::string key;
    key.reserve(preedit.size() + candidate.size() + 1);
    key.append(preedit);
    key.push_back('\t');
    key.append(candidate);
    return key;
}

std::string InputModel::rawPreferenceKey(std::string_view preedit) { return preferenceKey(preedit, preedit); }

std::string InputModel::correctionKey(std::string_view typo, std::string_view correction) {
    std::string key("__correction__\t");
    key.append(typo);
    key.push_back('\t');
    key.append(correction);
    return key;
}

std::string InputModel::segmentChainKey(const SegmentChain &chain) { return segmentChainRecordKey(chain); }

bool InputModel::isPlausibleSegmentChain(const SegmentChain &chain) {
    if (!isSafeStoredText(chain.originalPreedit) || !isSafeStoredText(chain.consumedPreedit) ||
        !isSafeStoredText(chain.committedText) || !isSafeStoredText(chain.remainingPreedit) ||
        !isSafeStoredText(chain.correctedFullPreedit) || !isSafeStoredText(chain.combinedCandidate)) {
        return false;
    }
    if (!chain.combinedCandidate.starts_with(chain.committedText)) {
        return false;
    }
    const auto consumedAndRemaining = chain.consumedPreedit + chain.remainingPreedit;
    if (consumedAndRemaining != chain.originalPreedit && consumedAndRemaining != chain.correctedFullPreedit) {
        return false;
    }
    return chain.correctedFullPreedit == chain.originalPreedit ||
           isPlausibleCorrection(chain.originalPreedit, chain.correctedFullPreedit);
}

bool InputModel::isPlausibleCorrection(std::string_view typo, std::string_view correction) {
    if (typo.size() < 3 || correction.size() < 3 || typo == correction) {
        return false;
    }

    if (correction.size() == typo.size() + 1) {
        for (std::size_t skipped = 0; skipped < correction.size(); ++skipped) {
            std::size_t typoIndex = 0;
            bool matches = true;
            for (std::size_t correctionIndex = 0; correctionIndex < correction.size(); ++correctionIndex) {
                if (correctionIndex == skipped) {
                    continue;
                }
                if (typoIndex >= typo.size() || typo[typoIndex++] != correction[correctionIndex]) {
                    matches = false;
                    break;
                }
            }
            if (matches && typoIndex == typo.size()) {
                return true;
            }
        }
    }

    if (std::max(typo.size(), correction.size()) < 5) {
        return false;
    }

    std::vector<std::size_t> previous(correction.size() + 1);
    std::vector<std::size_t> current(correction.size() + 1);
    for (std::size_t index = 0; index <= correction.size(); ++index) {
        previous[index] = index;
    }
    for (std::size_t typoIndex = 1; typoIndex <= typo.size(); ++typoIndex) {
        current[0] = typoIndex;
        std::size_t rowBest = current[0];
        for (std::size_t correctionIndex = 1; correctionIndex <= correction.size(); ++correctionIndex) {
            const auto cost = typo[typoIndex - 1] == correction[correctionIndex - 1] ? 0 : 1;
            current[correctionIndex] = std::min({previous[correctionIndex] + 1, current[correctionIndex - 1] + 1,
                                                 previous[correctionIndex - 1] + cost});
            rowBest = std::min(rowBest, current[correctionIndex]);
        }
        if (rowBest > 2) {
            return false;
        }
        previous.swap(current);
    }
    return previous[correction.size()] <= 2;
}

} // namespace tipe
