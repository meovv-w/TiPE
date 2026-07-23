#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tipe {

enum class InputEventType {
    Letter,
    Digit,
    Symbol,
    Backspace,
    Delete,
    Space,
    Enter,
    Escape,
    ObservedKey,
    CandidateSelected,
    RawCommitted,
    CursorMove,
    AiRerankRequested,
};

struct InputEvent {
    InputEventType type;
    std::string text;
};

struct InputEventCounts {
    std::size_t letters = 0;
    std::size_t digits = 0;
    std::size_t symbols = 0;
    std::size_t backspaces = 0;
    std::size_t deletes = 0;
    std::size_t spaces = 0;
    std::size_t enters = 0;
    std::size_t escapes = 0;
    std::size_t observedKeys = 0;
    std::size_t rawCommits = 0;
    std::size_t candidateSelections = 0;
    std::size_t cursorMoves = 0;
    std::size_t rerankRequests = 0;
};

struct ModelRequestState {
    struct CandidateMetadata {
        std::size_t consumedPrefixLength = 0;
        std::string source;
        int score = 0;
    };
    struct PendingSegment {
        std::string originalPreedit;
        std::string consumedPreedit;
        std::string committedText;
        std::string remainingPreedit;
    };

    std::size_t preeditCursor = 0;
    std::size_t candidateCursor = 0;
    bool candidatesExpanded = false;
    bool continuousMode = false;
    std::vector<CandidateMetadata> candidateMetadata;
    std::vector<PendingSegment> pendingSegments;
    std::vector<std::size_t> visibleCandidateIndices;
    std::vector<std::pair<std::string, std::size_t>> numberedCandidateIndices;
    std::string surroundingBefore;
    std::string surroundingAfter;
    bool forcePassThrough = false;
    std::string inputMode;
};

struct SegmentChain {
    std::string originalPreedit;
    std::string consumedPreedit;
    std::string committedText;
    std::string remainingPreedit;
    std::string correctedFullPreedit;
    std::string combinedCandidate;
};

struct InputSessionContext {
    std::vector<InputEvent> correctionEvents;
    std::vector<std::string> recentCommits;
    std::vector<SegmentChain> recentSegmentChains;

    bool empty() const {
        return correctionEvents.empty() && recentCommits.empty() && recentSegmentChains.empty();
    }
};

std::optional<std::string> configuredModelCommand();
std::optional<std::string> invokeModelCommand(std::string_view command, const std::string &payload);

class InputModel {
public:
    explicit InputModel(std::filesystem::path preferencePath = defaultPreferencePath());

    void record(InputEventType type, std::string_view text = {});
    void recordCandidateSelection(std::string_view preedit, std::string_view candidate, std::size_t weight = 1);
    void recordCandidatePreference(std::string_view preedit, std::string_view candidate, std::size_t weight = 1);
    void recordRawCommit(std::string_view preedit, std::size_t weight = 1);
    void recordCorrection(std::string_view typo, std::string_view correctedPreedit, std::size_t weight = 1);
    std::size_t recordSegmentChain(SegmentChain chain);
    void undoLatestRecentCommit(std::string_view text);
    const std::vector<InputEvent> &recentEvents() const;
    const std::vector<std::string> &recentCommits() const;
    const std::vector<SegmentChain> &recentSegmentChains() const;
    InputSessionContext sessionContext() const;
    void restoreSessionContext(InputSessionContext context);
    InputEventCounts eventCounts() const;
    void clear();
    void clearCorrectionEvents();
    void reloadPreferencesIfChanged(bool force = false);

    bool shouldPreferRaw(std::string_view preedit, const std::vector<std::string> &candidates) const;
    bool hasActiveCandidatePreference(std::string_view preedit, std::string_view candidate) const;
    bool hasNonRawCandidatePreferenceEvidence(std::string_view preedit) const;
    std::size_t correctionVersion() const;
    bool hasExactLearnedCorrection(std::string_view preedit) const;
    bool hasActiveKeyHabits() const;
    std::vector<std::string> learnedCandidatePreferences(std::string_view preedit) const;
    std::vector<std::string> learnedSegmentCandidates(std::string_view preedit) const;
    std::vector<std::string>
    learnedCorrections(std::string_view preedit,
                       const std::function<int(std::string_view)> &generatedCorrectionPriority = {}) const;
    std::vector<std::string> applyLearnedPreferences(std::string_view preedit,
                                                     const std::vector<std::string> &candidates,
                                                     ModelRequestState state = {}) const;
    std::string modelRequest(std::string_view preedit, const std::vector<std::string> &candidates,
                             ModelRequestState state = {}, std::string_view application = {}) const;
    std::vector<std::string> rerankCandidates(std::string_view preedit, const std::vector<std::string> &candidates,
                                              ModelRequestState state = {}, std::string_view application = {},
                                              bool allowExternalModel = true);
    std::vector<std::string> applyExternalModelOutput(std::string_view preedit,
                                                      const std::vector<std::string> &candidates,
                                                      ModelRequestState state,
                                                      std::string_view output);

private:
    static constexpr std::size_t maxEvents_ = 64;
    static constexpr std::size_t maxCorrectionEvents_ = 256;
    static constexpr std::size_t maxRecentCommits_ = 16;
    static constexpr std::size_t maxRecentSegmentChains_ = 16;

    static std::filesystem::path defaultPreferencePath();
    std::vector<std::string> externalRerankCandidates(std::string_view preedit,
                                                      const std::vector<std::string> &candidates,
                                                      ModelRequestState state,
                                                      std::string_view application);
    std::vector<std::string> localRerankCandidates(std::string_view preedit,
                                                   const std::vector<std::string> &candidates,
                                                   const ModelRequestState &state = {},
                                                   std::string_view application = {}) const;
    void learnCorrectionFromRecentEvents(std::string_view correctedPreedit);
    void recordCorrectionEvent(InputEventType type, std::string_view text);
    void recordRecentCommit(std::string_view text);
    std::optional<std::string> recentFullyErasedInputBefore(std::string_view correctedPreedit) const;
    void loadPreferences();
    void savePreferences(
        std::optional<std::pair<std::string, std::string>> explicitCandidatePromotion = std::nullopt);
    void promoteExplicitCandidatePreference(std::string_view preedit, std::string_view candidate);
    static std::string preferenceKey(std::string_view preedit, std::string_view candidate);
    static std::string rawPreferenceKey(std::string_view preedit);
    static std::string correctionKey(std::string_view typo, std::string_view correction);
    static std::string segmentChainKey(const SegmentChain &chain);
    static bool isPlausibleCorrection(std::string_view typo, std::string_view correction);
    static bool isPlausibleSegmentChain(const SegmentChain &chain);

    std::vector<InputEvent> events_;
    std::vector<InputEvent> correctionEvents_;
    std::vector<std::string> recentCommits_;
    std::vector<SegmentChain> recentSegmentChains_;
    std::unordered_map<std::string, std::size_t> selectedCounts_;
    std::unordered_map<std::string, std::size_t> rawTokenCounts_;
    std::unordered_map<std::string, std::size_t> correctionCounts_;
    std::unordered_map<std::string, std::size_t> correctionPatternCounts_;
    std::unordered_map<std::string, std::size_t> keyHabitCounts_;
    std::unordered_map<std::string, std::size_t> segmentChainCounts_;
    std::unordered_map<std::string, std::size_t> persistedSelectedCounts_;
    std::unordered_map<std::string, std::size_t> persistedRawTokenCounts_;
    std::unordered_map<std::string, std::size_t> persistedCorrectionCounts_;
    std::unordered_map<std::string, std::size_t> persistedCorrectionPatternCounts_;
    std::unordered_map<std::string, std::size_t> persistedKeyHabitCounts_;
    std::unordered_map<std::string, std::size_t> persistedSegmentChainCounts_;
    std::size_t correctionVersion_ = 0;
    std::filesystem::path preferencePath_;
    std::optional<std::filesystem::file_time_type> preferenceMtime_;
    std::chrono::steady_clock::time_point nextPreferenceReloadCheck_;
};

} // namespace tipe
