#pragma once

#include "dictionary.h"
#include "input_model.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tipe {

enum class ActionType {
    None,
    Update,
    Commit,
    Clear,
};

struct Action {
    Action() = default;
    Action(ActionType actionType, std::string commit = {}, bool isAccepted = false, bool shouldKeepPreedit = false,
           std::string before = {}, std::string after = {}, std::string passthrough = {})
        : type(actionType), commitText(std::move(commit)), accepted(isAccepted), keepPreedit(shouldKeepPreedit),
          preeditBefore(std::move(before)), preeditAfter(std::move(after)),
          passthroughText(std::move(passthrough)) {}

    ActionType type = ActionType::None;
    std::string commitText;
    bool accepted = false;
    bool keepPreedit = false;
    std::string preeditBefore;
    std::string preeditAfter;
    std::string passthroughText;
};

struct DebugSnapshot {
    std::string preedit;
    std::size_t candidateCount = 0;
    std::size_t displayCandidateCount = 0;
    bool candidatesExpanded = false;
    InputEventCounts eventCounts;
};

class State {
public:
    using SessionContext = InputSessionContext;

    struct CompletedSupervisionRequest {
        std::string preedit;
        std::size_t candidateCount = 0;
        bool candidatesExpanded = false;
        std::string payload;
    };

    struct ExternalModelRequest {
        std::string preedit;
        std::vector<std::string> candidates;
        ModelRequestState state;
        std::string payload;
        std::string expectedPreedit;
        std::vector<std::string> expectedCandidates;
        std::size_t expectedPreeditCursor = 0;
        std::size_t expectedCandidateCursor = 0;
        bool expectedCandidatesExpanded = false;
    };

    struct RestorableSnapshot {
        struct PendingSegmentChain {
            std::string originalPreedit;
            std::string consumedPreedit;
            std::string committedText;
        };

        InputModel inputModel;
        std::string preedit;
        std::size_t preeditCursor = 0;
        std::optional<std::string> editedOriginalPreedit;
        std::optional<PendingSegmentChain> pendingSegmentChain;
        bool candidatesExpanded = false;
        std::size_t candidateCursorIndex = 0;
        bool rawPreeditMode = false;
    };

    explicit State(Dictionary dictionary = {}, std::filesystem::path preferencePath = {});

    const std::string &preedit() const;
    const std::vector<std::string> &candidates() const;
    std::vector<std::string> visibleCandidates() const;
    std::vector<std::string> displayCandidates() const;
    const std::vector<InputEvent> &recentEvents() const;
    const std::vector<std::string> &recentCommits() const;
    DebugSnapshot debugSnapshot() const;
    bool empty() const;
    bool candidatesExpanded() const;
    bool rawPreeditMode() const;
    std::size_t preeditCursorIndex() const;
    bool preeditCursorAtEnd() const;
    std::size_t candidateCursorIndex() const;
    std::size_t candidateCount() const;
    std::size_t candidateConsumedPrefixLength(std::size_t index) const;
    std::string candidateSource(std::size_t index) const;
    int candidateScore(std::size_t index) const;

    Action inputAscii(char ch);
    Action inputUppercaseAscii(char ch);
    Action inputAsciiDigit(char ch);
    Action inputRawTokenSymbol(char ch);
    Action backspace();
    Action deleteKey();
    Action observeKey(std::string_view keyName = {});
    Action space();
    Action punctuation(std::string_view keyName = {});
    Action commitCurrentPassthrough(std::string_view keyName = {});
    Action commitRawPreedit(std::string_view boundaryName = {});
    Action enter();
    Action escape();
    Action select(std::size_t index);
    Action selectVisibleDigit(std::size_t digitIndex, std::string_view keyName = {});
    Action cursorMove(std::string_view keyName = {});
    Action expandCandidates(std::string_view keyName = {});
    Action moveCandidateCursor(int delta, std::string_view keyName = {});
    Action moveCollapsedCandidateCursor(int delta, std::string_view keyName = {});
    Action moveCandidateCursorTo(std::size_t index, std::string_view keyName = {});
    Action beginExternalModelRerank(bool expandAfterRerank = true);
    Action rerankCandidates(std::string_view application = {}, std::string surroundingBefore = {},
                            std::string surroundingAfter = {}, bool expandAfterRerank = true,
                            bool allowExternalModel = true, bool continuousMode = false);
    std::optional<ExternalModelRequest>
    externalModelRequest(std::string_view application = {}, std::string surroundingBefore = {},
                         std::string surroundingAfter = {}, bool continuousMode = false) const;
    void armExternalModelRequest(ExternalModelRequest &request) const;
    Action applyExternalModelResponse(const ExternalModelRequest &request, std::string_view output,
                                      bool expandAfterRerank = true);
    std::string modelRequestSnapshot(std::string_view application = {}, std::string surroundingBefore = {},
                                     std::string surroundingAfter = {}, bool continuousMode = false) const;
    std::optional<CompletedSupervisionRequest>
    completedSupervisionRequest(std::string_view application = {}, std::string surroundingBefore = {},
                                std::string surroundingAfter = {}, bool continuousMode = false) const;
    void clearCompletedSupervisionRequest();
    RestorableSnapshot restorableSnapshot() const;
    SessionContext sessionContext() const;
    void restoreSessionContext(SessionContext context);
    Action restoreSnapshot(const RestorableSnapshot &snapshot);
    Action restorePreedit(std::string preedit, std::string_view keyName = {});
    void reset();

private:
    struct CandidateMetadata {
        std::size_t consumedPrefixLength = 0;
        std::string source;
        int score = 0;
    };

    Action updateAccepted();
    Action clearAccepted();
    Action commitCurrentCandidate(bool accepted, std::size_t selectionWeight = 1);
    Action commitCandidate(std::string candidate, bool accepted, std::size_t selectionWeight, bool rawCommit,
                           std::string preeditBefore, std::size_t consumedPrefixLength = 0);
    void commitPartialCandidate(std::string &candidate, std::size_t prefixLength, std::size_t selectionWeight);
    std::optional<std::string> candidateAt(std::size_t index) const;
    std::size_t candidateConsumedPrefixLengthAt(std::size_t index) const;
    void clearCandidateMetadata();
    void setCandidateMetadataDefaults(std::string source, int baseScore);
    void ensureCandidateMetadata();
    void insertCandidate(std::size_t index, std::string candidate, std::size_t consumedPrefixLength = 0,
                         std::string source = {}, int score = 0);
    void eraseCandidate(std::size_t index);
    void finalizeCandidateScores();
    void preserveCandidateMetadataFrom(const std::vector<std::string> &originalCandidates,
                                       const std::vector<CandidateMetadata> &originalMetadata,
                                       std::string_view sourcePrefix);
    void notePreeditEditBeforeMutation();
    void learnEditedPreeditCorrection(std::string_view correctedPreedit);
    void prepareInputEventRecord();
    void captureCompletedSupervision(std::optional<std::string_view> confirmedCandidate = std::nullopt,
                                     bool rawCommit = false);
    void resetComposition();
    std::string buildModelRequestSnapshot(std::string_view preedit, const std::vector<std::string> &candidates,
                                          std::size_t preeditCursor, std::size_t candidateCursor,
                                          bool candidatesExpanded,
                                          std::vector<ModelRequestState::CandidateMetadata> candidateMetadata,
                                          std::vector<ModelRequestState::PendingSegment> pendingSegments,
                                          std::string_view application, std::string surroundingBefore,
                                          std::string surroundingAfter, bool continuousMode) const;
    std::optional<std::size_t> partialCommitPrefixLength(const std::string &candidate) const;
    std::optional<std::size_t> approximatePartialCommitPrefixLength(const std::string &candidate,
                                                                    bool requireCandidateMatch = true) const;
    bool shouldPreserveUnresolvedPartialCandidate(const std::string &candidate) const;
    std::optional<std::string> correctedPreeditForCandidate(std::string_view typedPreedit,
                                                            const std::string &candidate) const;
    void learnPendingSegmentChain(std::string_view currentPreedit, const std::string &candidate);
    std::vector<std::string> explicitCorrectionCandidates() const;
    std::vector<ModelRequestState::CandidateMetadata> modelCandidateMetadata() const;
    void refreshCandidates();
    void insertPrefixCandidates(std::size_t protectedLeadingCandidates = 0);
    void annotateLearnedPreferenceCandidateMetadata();
    void annotatePartialCandidateMetadata(std::size_t protectedLeadingCandidates = 0);
    void annotatePartialCandidateMetadataAt(std::size_t index);
    void promotePrefixContinuationCandidates(std::size_t protectedLeadingCandidates = 0);
    void promoteSingleSyllableCandidates(std::size_t protectedLeadingCandidates = 0);
    void promotePendingSegmentChainCandidates(std::size_t protectedLeadingCandidates = 0);
    void demoteDivergentLongCandidates(std::size_t protectedLeadingCandidates = 0);
    void clampCandidateCursor();
    Action applyRerankedCandidates(std::vector<std::string> rerankedCandidates,
                                   std::size_t previousCorrectionVersion,
                                   const std::vector<std::string> &correctionCandidates,
                                   bool expandAfterRerank, std::string_view rankSource);

    Dictionary dictionary_;
    InputModel inputModel_;
    std::string preedit_;
    std::size_t preeditCursor_ = 0;
    std::vector<std::string> candidates_;
    std::vector<CandidateMetadata> candidateMetadata_;
    std::optional<std::string> editedOriginalPreedit_;
    struct PendingSegmentChain {
        std::string originalPreedit;
        std::string consumedPreedit;
        std::string committedText;
    };
    std::optional<PendingSegmentChain> pendingSegmentChain_;
    struct CompletedSupervision {
        std::string preedit;
        std::vector<std::string> candidates;
        std::size_t preeditCursor = 0;
        std::size_t candidateCursor = 0;
        bool candidatesExpanded = false;
        std::vector<ModelRequestState::CandidateMetadata> candidateMetadata;
        std::vector<ModelRequestState::PendingSegment> pendingSegments;
    };
    std::optional<CompletedSupervision> completedSupervision_;
    std::unordered_set<std::string> fullCorrectionCandidates_;
    bool candidatesExpanded_ = false;
    std::size_t candidateCursorIndex_ = 0;
    bool rawPreeditMode_ = false;
    bool clearEventsBeforeNextInput_ = false;
};

} // namespace tipe
