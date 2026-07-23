#include "pass_through_supervisor.h"

#include <algorithm>
#include <vector>

namespace tipe {

PassThroughSupervisor::PassThroughSupervisor(std::filesystem::path preferencePath)
    : inputModel_(std::move(preferencePath)) {}

void PassThroughSupervisor::insert(char ch, InputEventType type) {
    const std::string_view text(&ch, 1);
    inputModel_.record(type, text);
    cursor_ = std::min(cursor_, token_.size());
    if (token_.size() >= maxTokenBytes_) {
        return;
    }
    token_.insert(token_.begin() + static_cast<std::ptrdiff_t>(cursor_), ch);
    ++cursor_;
}

void PassThroughSupervisor::inputLetter(char ch) { insert(ch, InputEventType::Letter); }

void PassThroughSupervisor::inputDigit(char ch) { insert(ch, InputEventType::Digit); }

void PassThroughSupervisor::backspace() {
    inputModel_.record(InputEventType::Backspace);
    cursor_ = std::min(cursor_, token_.size());
    if (cursor_ == 0) {
        abandonUncertainToken();
        return;
    }
    token_.erase(token_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
    --cursor_;
}

void PassThroughSupervisor::deleteKey() {
    inputModel_.record(InputEventType::Delete);
    cursor_ = std::min(cursor_, token_.size());
    if (cursor_ >= token_.size()) {
        abandonUncertainToken();
        return;
    }
    token_.erase(token_.begin() + static_cast<std::ptrdiff_t>(cursor_));
}

void PassThroughSupervisor::cursorMove(std::string_view keyName) {
    inputModel_.record(InputEventType::CursorMove, keyName);
    if (keyName == "Left" || keyName == "KP_Left") {
        if (cursor_ > 0) {
            --cursor_;
        } else {
            abandonUncertainToken();
        }
        return;
    }
    if (keyName == "Right" || keyName == "KP_Right") {
        if (cursor_ < token_.size()) {
            ++cursor_;
        } else {
            abandonUncertainToken();
        }
        return;
    }
    abandonUncertainToken();
}

void PassThroughSupervisor::observeKey(std::string_view keyName, bool cursorContextUncertain) {
    inputModel_.record(InputEventType::ObservedKey, keyName);
    if (cursorContextUncertain) {
        abandonUncertainToken();
    }
}

PassThroughSupervisionSnapshot PassThroughSupervisor::buildSnapshot(
    std::string_view token, std::size_t cursor, std::string_view application, bool continuousMode,
    bool terminal) const {
    std::vector<std::string> candidates;
    ModelRequestState requestState;
    requestState.preeditCursor = std::min(cursor, token.size());
    requestState.continuousMode = continuousMode;
    requestState.forcePassThrough = true;
    requestState.inputMode = "english";
    if (!token.empty()) {
        candidates.emplace_back(token);
        requestState.candidateMetadata.push_back({0, "raw-pass-through", 900000});
    }
    return {std::string(token), candidates.size(),
            inputModel_.modelRequest(token, candidates, std::move(requestState), application), terminal};
}

PassThroughSupervisionSnapshot PassThroughSupervisor::snapshot(std::string_view application,
                                                               bool continuousMode) const {
    return buildSnapshot(token_, cursor_, application, continuousMode, false);
}

PassThroughSupervisionSnapshot PassThroughSupervisor::commitBoundary(
    InputEventType boundaryType, std::string_view boundaryText, std::string_view application,
    bool continuousMode) {
    inputModel_.record(boundaryType, boundaryText);
    if (token_.empty()) {
        return snapshot(application, continuousMode);
    }

    const auto completedToken = token_;
    const auto completedCursor = cursor_;
    inputModel_.recordRawCommit(completedToken);
    auto completed = buildSnapshot(completedToken, completedCursor, application, continuousMode, true);
    token_.clear();
    cursor_ = 0;
    inputModel_.clear();
    return completed;
}

PassThroughSupervisionSnapshot PassThroughSupervisor::commitSpace(std::string_view application,
                                                                  bool continuousMode) {
    return commitBoundary(InputEventType::Space, {}, application, continuousMode);
}

PassThroughSupervisionSnapshot PassThroughSupervisor::commitEnter(std::string_view application,
                                                                  bool continuousMode) {
    return commitBoundary(InputEventType::Enter, {}, application, continuousMode);
}

PassThroughSupervisionSnapshot PassThroughSupervisor::commitPunctuation(
    char ch, std::string_view application, bool continuousMode) {
    return commitBoundary(InputEventType::Symbol, std::string_view(&ch, 1), application, continuousMode);
}

PassThroughSupervisionSnapshot PassThroughSupervisor::commitObservedBoundary(
    std::string_view keyName, std::string_view application, bool continuousMode) {
    return commitBoundary(InputEventType::ObservedKey, keyName, application, continuousMode);
}

PassThroughSupervisionSnapshot PassThroughSupervisor::cancel(std::string_view application,
                                                             bool continuousMode) {
    inputModel_.record(InputEventType::Escape);
    auto completed = buildSnapshot(token_, cursor_, application, continuousMode, !token_.empty());
    token_.clear();
    cursor_ = 0;
    inputModel_.clear();
    inputModel_.clearCorrectionEvents();
    return completed;
}

void PassThroughSupervisor::abandonUncertainToken() {
    token_.clear();
    cursor_ = 0;
    inputModel_.clearCorrectionEvents();
}

void PassThroughSupervisor::resetTracking() {
    token_.clear();
    cursor_ = 0;
    inputModel_.clear();
    inputModel_.clearCorrectionEvents();
}

const std::string &PassThroughSupervisor::token() const { return token_; }

std::size_t PassThroughSupervisor::cursor() const { return cursor_; }

} // namespace tipe
