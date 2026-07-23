#pragma once

#include "input_model.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace tipe {

struct PassThroughSupervisionSnapshot {
    std::string token;
    std::size_t candidateCount = 0;
    std::string payload;
    bool terminal = false;
};

class PassThroughSupervisor {
public:
    explicit PassThroughSupervisor(std::filesystem::path preferencePath = {});

    void inputLetter(char ch);
    void inputDigit(char ch);
    void backspace();
    void deleteKey();
    void cursorMove(std::string_view keyName);
    void observeKey(std::string_view keyName, bool cursorContextUncertain = false);

    PassThroughSupervisionSnapshot snapshot(std::string_view application = {},
                                            bool continuousMode = false) const;
    PassThroughSupervisionSnapshot commitSpace(std::string_view application = {},
                                               bool continuousMode = false);
    PassThroughSupervisionSnapshot commitEnter(std::string_view application = {},
                                               bool continuousMode = false);
    PassThroughSupervisionSnapshot commitPunctuation(char ch, std::string_view application = {},
                                                     bool continuousMode = false);
    PassThroughSupervisionSnapshot commitObservedBoundary(std::string_view keyName,
                                                          std::string_view application = {},
                                                          bool continuousMode = false);
    PassThroughSupervisionSnapshot cancel(std::string_view application = {},
                                          bool continuousMode = false);
    void resetTracking();

    const std::string &token() const;
    std::size_t cursor() const;

private:
    static constexpr std::size_t maxTokenBytes_ = 64;

    void insert(char ch, InputEventType type);
    void abandonUncertainToken();
    PassThroughSupervisionSnapshot buildSnapshot(std::string_view token, std::size_t cursor,
                                                 std::string_view application, bool continuousMode,
                                                 bool terminal) const;
    PassThroughSupervisionSnapshot commitBoundary(InputEventType boundaryType, std::string_view boundaryText,
                                                  std::string_view application, bool continuousMode);

    InputModel inputModel_;
    std::string token_;
    std::size_t cursor_ = 0;
};

} // namespace tipe
