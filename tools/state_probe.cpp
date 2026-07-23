#include "candidate_snapshot.h"
#include "candidate_layout.h"
#include "pinyin_utils.h"
#include "state.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

constexpr std::size_t visibleDigitCount = tipe::visualCandidateColumns;

void usage(const char *program) {
    std::cerr << "usage: " << program
              << " PINYIN [--user-data] [--preferences PATH] [--dictionary PATH] [--learned-corrections] [--observe KEY] [--move KEY] [--rerank] [--select CANDIDATE]"
                 " [--continuous-mode] [--continuous-rerank] [--digit 1-9] [--punct KEY] [--space] [--enter] [--backspace] [--delete] [--escape]"
                 " [--key KEY] [--application NAME] [--surrounding-before TEXT] [--surrounding-after TEXT]"
                 " [--script FILE] [--events] [--request] [--snapshot X,Y,W,H]\n"
                 "script command: print-first emits FIRST<TAB>PREEDIT<TAB>CANDIDATE without changing state\n";
}

std::string_view eventName(tipe::InputEventType type) {
    switch (type) {
    case tipe::InputEventType::Letter:
        return "letter";
    case tipe::InputEventType::Digit:
        return "digit";
    case tipe::InputEventType::Symbol:
        return "symbol";
    case tipe::InputEventType::Backspace:
        return "backspace";
    case tipe::InputEventType::Delete:
        return "delete";
    case tipe::InputEventType::Space:
        return "space";
    case tipe::InputEventType::Enter:
        return "enter";
    case tipe::InputEventType::Escape:
        return "escape";
    case tipe::InputEventType::ObservedKey:
        return "observed";
    case tipe::InputEventType::CandidateSelected:
        return "candidate-selected";
    case tipe::InputEventType::RawCommitted:
        return "raw-committed";
    case tipe::InputEventType::CursorMove:
        return "cursor-move";
    case tipe::InputEventType::AiRerankRequested:
        return "rerank-requested";
    }
    return "unknown";
}

std::string escapeField(std::string_view text) {
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

std::string probeModelRequest(const tipe::State &state, std::string_view application,
                              std::string_view surroundingBefore, std::string_view surroundingAfter,
                              bool continuousMode) {
    if (state.empty()) {
        if (const auto completed = state.completedSupervisionRequest(
                application, std::string(surroundingBefore), std::string(surroundingAfter), continuousMode)) {
            return completed->payload;
        }
    }
    return state.modelRequestSnapshot(application, std::string(surroundingBefore),
                                      std::string(surroundingAfter), continuousMode);
}

bool typePinyin(tipe::State &state, const std::string &pinyin) {
    for (const unsigned char ch : pinyin) {
        if (!std::isalpha(ch)) {
            std::cerr << "invalid pinyin byte: " << static_cast<char>(ch) << '\n';
            return false;
        }
        const auto action = state.inputAscii(static_cast<char>(ch));
        if (!action.accepted || action.type != tipe::ActionType::Update) {
            std::cerr << "failed to input pinyin byte: " << static_cast<char>(ch) << '\n';
            return false;
        }
    }
    return true;
}

std::size_t selectedVisibleRow(const std::vector<tipe::VisualCandidateCell> &cells, std::size_t selectedIndex) {
    for (const auto &cell : cells) {
        if (cell.index == selectedIndex) {
            return cell.row;
        }
    }
    return 0;
}

void printState(const tipe::State &state) {
    const auto snapshot = state.debugSnapshot();
    const auto &counts = snapshot.eventCounts;
    std::cout << "preedit\t" << state.preedit() << '\n';
    std::cout << "preedit_cursor\t" << state.preeditCursorIndex() << '\n';
    std::cout << "expanded\t" << (state.candidatesExpanded() ? 1 : 0) << '\n';
    std::cout << "selected\t" << state.candidateCursorIndex() << '\n';
    std::cout << "events\tletters=" << counts.letters << "\tdigits=" << counts.digits
              << "\tsymbols=" << counts.symbols
              << "\tbackspaces=" << counts.backspaces
              << "\tdeletes=" << counts.deletes << "\tspaces=" << counts.spaces << "\tenters=" << counts.enters
              << "\tescapes=" << counts.escapes << "\tobserved=" << counts.observedKeys
              << "\traw_commits=" << counts.rawCommits
              << "\tcandidate_selections=" << counts.candidateSelections
              << "\tcursor_moves=" << counts.cursorMoves << "\treranks=" << counts.rerankRequests << '\n';
    const auto &commits = state.recentCommits();
    for (std::size_t index = 0; index < commits.size(); ++index) {
        std::cout << "context\t" << index << '\t' << escapeField(commits[index]) << '\n';
    }
    const auto &candidates = state.candidates();
    const auto visibleCells =
        tipe::visibleVisualCellsFor(candidates, state.candidateCursorIndex(), state.candidatesExpanded());
    const auto visibleRow = state.candidatesExpanded() ? selectedVisibleRow(visibleCells, state.candidateCursorIndex()) : 0;
    const auto rowCells = state.candidatesExpanded() ? tipe::cellsInVisualRow(visibleCells, visibleRow) : visibleCells;
    for (std::size_t digitIndex = 0; digitIndex < rowCells.size(); ++digitIndex) {
        const auto candidateIndex = rowCells[digitIndex].index;
        std::cout << "visible\t" << (digitIndex + 1) << '\t' << candidateIndex << '\t'
                  << candidates[candidateIndex] << '\n';
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        std::cout << "candidate\t" << index << '\t' << candidates[index] << '\n';
        const auto consumed = state.candidateConsumedPrefixLength(index);
        const auto source = state.candidateSource(index);
        const auto score = state.candidateScore(index);
        if (consumed > 0 || !source.empty() || score != 0) {
            std::cout << "candidate-meta\t" << index << "\tconsumed-prefix\t" << consumed
                      << "\tsource\t" << source << "\tscore\t" << score << '\n';
        }
    }
}

void printRecentEvents(const tipe::State &state) {
    const auto &events = state.recentEvents();
    for (std::size_t index = 0; index < events.size(); ++index) {
        std::cout << "event\t" << index << '\t' << eventName(events[index].type) << '\t'
                  << escapeField(events[index].text) << '\n';
    }
}

void printAction(std::string_view name, const tipe::Action &action) {
    std::cout << "action\t" << name << '\t' << (action.accepted ? 1 : 0) << '\t';
    switch (action.type) {
    case tipe::ActionType::None:
        std::cout << "none";
        break;
    case tipe::ActionType::Update:
        std::cout << "update";
        break;
    case tipe::ActionType::Commit:
        std::cout << "commit";
        break;
    case tipe::ActionType::Clear:
        std::cout << "clear";
        break;
    }
    std::cout << '\n';
    if (!action.passthroughText.empty()) {
        std::cout << "passthrough\t" << escapeField(action.passthroughText) << '\n';
    }
    if (!action.commitText.empty()) {
        std::cout << "commit\t" << action.commitText << '\n';
    }
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::pair<std::string, std::string> splitCommandLine(std::string_view line) {
    const auto tab = line.find('\t');
    if (tab != std::string_view::npos) {
        return {trim(line.substr(0, tab)), trim(line.substr(tab + 1))};
    }
    const auto space = line.find_first_of(" \r\n\t");
    if (space == std::string_view::npos) {
        return {trim(line), {}};
    }
    return {trim(line.substr(0, space)), trim(line.substr(space + 1))};
}

std::pair<std::string, std::string> splitArgumentHead(std::string_view argument) {
    const auto tab = argument.find('\t');
    if (tab != std::string_view::npos) {
        return {trim(argument.substr(0, tab)), trim(argument.substr(tab + 1))};
    }
    const auto space = argument.find_first_of(" \r\n\t");
    if (space == std::string_view::npos) {
        return {trim(argument), {}};
    }
    return {trim(argument.substr(0, space)), trim(argument.substr(space + 1))};
}

std::optional<std::size_t> parseIndex(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (const unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

std::optional<int> parseInt(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    std::size_t offset = 0;
    if (text[0] == '-') {
        negative = true;
        offset = 1;
    }
    if (offset == text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (; offset < text.size(); ++offset) {
        const unsigned char ch = static_cast<unsigned char>(text[offset]);
        if (!std::isdigit(ch)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - '0');
    }
    return negative ? -value : value;
}

std::optional<tipe::CandidateSnapshotRect> parseSnapshotRect(std::string_view text) {
    tipe::CandidateSnapshotRect rect;
    int *fields[] = {&rect.x, &rect.y, &rect.width, &rect.height};
    std::size_t begin = 0;
    constexpr std::size_t fieldCount = sizeof(fields) / sizeof(fields[0]);
    for (std::size_t index = 0; index < fieldCount; ++index) {
        const auto separator = text.find(',', begin);
        if ((index < fieldCount - 1 && separator == std::string_view::npos) ||
            (index == fieldCount - 1 && separator != std::string_view::npos)) {
            return std::nullopt;
        }
        const auto end = separator == std::string_view::npos ? text.size() : separator;
        const auto value = parseInt(text.substr(begin, end - begin));
        if (!value) {
            return std::nullopt;
        }
        *fields[index] = *value;
        begin = end + 1;
    }
    return rect;
}

void printCandidateSnapshot(const tipe::State &state, tipe::CandidateSnapshotRect rect, bool continuousMode) {
    const auto counts = state.debugSnapshot().eventCounts;
    const auto supervisedKeys = counts.letters + counts.digits + counts.symbols + counts.backspaces + counts.deletes +
                                counts.spaces + counts.enters + counts.escapes + counts.observedKeys +
                                counts.cursorMoves;
    const auto metadata = std::string("supervision=1,keys=") + std::to_string(supervisedKeys) +
                          ",selects=" + std::to_string(counts.candidateSelections) +
                          ",reranks=" + std::to_string(counts.rerankRequests) +
                          ",continuous=" + (continuousMode ? "1" : "0") +
                          ",preedit_cursor=" + std::to_string(state.preeditCursorIndex());
    std::string line =
        tipe::buildCandidateSnapshotLine(state.preedit(), state.candidatesExpanded(), state.candidateCursorIndex(),
                                         rect, state.candidates(), metadata);
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
    }
    std::cout << "snapshot\t" << line << '\n';
}

bool selectCandidate(tipe::State &state, const std::string &selectedCandidate) {
    const auto &candidates = state.candidates();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index] != selectedCandidate) {
            continue;
        }
        const auto action = state.select(index);
        std::cout << "commit\t" << action.commitText << '\n';
        return action.accepted;
    }
    std::cerr << "candidate not found: " << selectedCandidate << '\n';
    return false;
}

std::string normalizedMoveKey(std::string_view key) {
    if (key == "KP_Down") {
        return "Down";
    }
    if (key == "KP_Up") {
        return "Up";
    }
    if (key == "Next" || key == "Page_Down" || key == "KP_Next" || key == "KP_Page_Down") {
        return "PageDown";
    }
    if (key == "Prior" || key == "Page_Up" || key == "KP_Prior" || key == "KP_Page_Up") {
        return "PageUp";
    }
    if (key == "KP_Home") {
        return "Home";
    }
    if (key == "KP_End") {
        return "End";
    }
    if (key == "KP_Left") {
        return "Left";
    }
    if (key == "KP_Right") {
        return "Right";
    }
    if (key == "ISO_Left_Tab" || key == "Shift+Tab" || key == "Shift_Tab") {
        return "ShiftTab";
    }
    return std::string(key);
}

bool isMoveKey(std::string_view key) {
    const auto normalized = normalizedMoveKey(key);
    return normalized == "Down" || normalized == "Up" || normalized == "PageDown" || normalized == "PageUp" ||
           normalized == "Home" || normalized == "End" || normalized == "Left" || normalized == "Right" ||
           normalized == "Tab" || normalized == "ShiftTab";
}

bool isPunctuationKey(std::string_view key) {
    static constexpr std::string_view punctuationKeys[] = {
        ",",          ".",          "/",           ";",        "'",       "[",        "]",
        "\\",         "-",          "=",           "`",        "!",       "@",        "#",
        "$",          "%",          "^",           "&",        "*",       "(",        ")",
        "_",          "+",          "{",           "}",        "|",       ":",        "\"",
        "<",          ">",          "?",           "~",        "comma",   "period",   "slash",
        "semicolon",  "apostrophe", "bracketleft", "bracketright", "backslash", "minus",    "equal",
        "grave",      "exclam",     "at",          "numbersign", "dollar", "percent",  "asciicircum",
        "ampersand",  "asterisk",   "parenleft",   "parenright", "underscore", "plus", "braceleft",
        "braceright", "bar",        "colon",       "quotedbl", "less",   "greater",  "question",
        "asciitilde", "KP_Decimal", "KP_Divide",   "KP_Multiply", "KP_Subtract", "KP_Add",
        "KP_Separator", "KP_Equal"};
    for (const auto punctuationKey : punctuationKeys) {
        if (punctuationKey == key) {
            return true;
        }
    }
    return false;
}

std::optional<char> rawTokenSymbolForProbeKey(std::string_view key) {
    if (key == "-" || key == "minus" || key == "KP_Subtract") {
        return '-';
    }
    if (key == "_" || key == "underscore") {
        return '_';
    }
    if (key == "." || key == "period" || key == "KP_Decimal") {
        return '.';
    }
    if (key == "/" || key == "slash" || key == "KP_Divide") {
        return '/';
    }
    return std::nullopt;
}

void moveProbeCursor(tipe::State &state, const std::string &key) {
    const auto normalizedKey = normalizedMoveKey(key);
    if (normalizedKey == "Down") {
        state.candidatesExpanded() ? state.moveCandidateCursor(static_cast<int>(visibleDigitCount), key)
                                   : state.expandCandidates(key);
    } else if (normalizedKey == "Up") {
        state.candidatesExpanded() ? state.moveCandidateCursor(-static_cast<int>(visibleDigitCount), key)
                                   : state.cursorMove(key);
    } else if (normalizedKey == "PageDown") {
        state.moveCandidateCursor(static_cast<int>(visibleDigitCount), key);
    } else if (normalizedKey == "PageUp") {
        state.moveCandidateCursor(-static_cast<int>(visibleDigitCount), key);
    } else if (normalizedKey == "Home") {
        state.moveCandidateCursorTo(0, key);
    } else if (normalizedKey == "End") {
        state.moveCandidateCursorTo(state.candidateCount() == 0 ? 0 : state.candidateCount() - 1, key);
    } else if (normalizedKey == "ShiftTab") {
        state.moveCandidateCursor(-1, key);
    } else if (normalizedKey == "Tab") {
        state.moveCandidateCursor(1, key);
    } else if (normalizedKey == "Right") {
        state.candidatesExpanded()
            ? state.moveCandidateCursor(1, key)
            : (!state.preeditCursorAtEnd() ? state.cursorMove(key) : state.moveCollapsedCandidateCursor(1, key));
    } else if (normalizedKey == "Left") {
        state.candidatesExpanded() ? state.moveCandidateCursor(-1, key)
                                   : (state.candidateCursorIndex() > 0 ? state.moveCollapsedCandidateCursor(-1, key)
                                                                       : state.cursorMove(key));
    } else {
        state.cursorMove(key);
    }
}

tipe::Action pressProbeKey(tipe::State &state, const std::string &key, std::string_view application,
                           std::string_view surroundingBefore, std::string_view surroundingAfter) {
    if (key.size() == 1 && std::isupper(static_cast<unsigned char>(key[0]))) {
        return state.inputUppercaseAscii(key[0]);
    }
    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0]))) {
        return state.inputAscii(static_cast<char>(std::tolower(static_cast<unsigned char>(key[0]))));
    }
    if (key == "BackSpace" || key == "Backspace") {
        return state.backspace();
    }
    if (key == "Delete") {
        return state.deleteKey();
    }
    if (key == "space" || key == "Space") {
        return state.space();
    }
    if (key == "Return" || key == "Enter" || key == "KP_Enter") {
        return state.enter();
    }
    if (key == "Escape" || key == "Esc") {
        return state.escape();
    }
    if (key == "F9" && !state.empty()) {
        return state.rerankCandidates(application, std::string(surroundingBefore), std::string(surroundingAfter));
    }
    if (!state.empty()) {
        if (const auto symbol = rawTokenSymbolForProbeKey(key); symbol) {
            auto action = state.inputRawTokenSymbol(*symbol);
            if (action.type != tipe::ActionType::None) {
                return action;
            }
        }
    }
    if (!state.empty() && key.size() == 1 && key[0] >= '1' && key[0] <= '9') {
        auto action = state.inputAsciiDigit(key[0]);
        if (action.type != tipe::ActionType::None) {
            return action;
        }
        return state.selectVisibleDigit(static_cast<std::size_t>(key[0] - '1'), key);
    }
    if (isPunctuationKey(key)) {
        return state.punctuation(key);
    }
    if (isMoveKey(key)) {
        if (state.empty()) {
            return state.cursorMove(key);
        }
        moveProbeCursor(state, key);
        tipe::Action action;
        action.type = tipe::ActionType::Update;
        action.accepted = true;
        return action;
    }
    if (state.empty()) {
        return state.observeKey(key);
    }
    return state.cursorMove(key);
}

bool runProbeCommand(tipe::State &state, const std::string &command, const std::string &argument,
                     std::string_view application, std::string_view surroundingBefore,
                     std::string_view surroundingAfter, bool &continuousMode) {
    if (command == "type") {
        return typePinyin(state, argument);
    }
    if (command == "observe") {
        state.observeKey(argument);
        return true;
    }
    if (command == "move") {
        moveProbeCursor(state, argument);
        return true;
    }
    if (command == "key") {
        printAction("key", pressProbeKey(state, argument, application, surroundingBefore, surroundingAfter));
        return true;
    }
    if (command == "rerank") {
        printAction("rerank", state.rerankCandidates(application, std::string(surroundingBefore),
                                                     std::string(surroundingAfter)));
        return true;
    }
    if (command == "continuous-rerank") {
        continuousMode = true;
        printAction("continuous-rerank", state.rerankCandidates(application, std::string(surroundingBefore),
                                                                std::string(surroundingAfter), false, false, true));
        return true;
    }
    if (command == "continuous-mode") {
        if (argument.empty() || argument == "1" || argument == "on" || argument == "true") {
            continuousMode = true;
        } else if (argument == "0" || argument == "off" || argument == "false") {
            continuousMode = false;
        } else {
            std::cerr << "continuous-mode requires on or off\n";
            return false;
        }
        printAction("continuous-mode", {tipe::ActionType::Update, {}, true});
        return true;
    }
    if (command == "digit") {
        if (argument.size() != 1 || argument[0] < '1' || argument[0] > '9') {
            std::cerr << "digit must be 1..9\n";
            return false;
        }
        printAction("digit", pressProbeKey(state, argument, application, surroundingBefore, surroundingAfter));
        return true;
    }
    if (command == "punct") {
        printAction("punct", state.punctuation(argument));
        return true;
    }
    if (command == "space") {
        printAction("space", state.space());
        return true;
    }
    if (command == "enter") {
        printAction("enter", state.enter());
        return true;
    }
    if (command == "backspace") {
        printAction("backspace", state.backspace());
        return true;
    }
    if (command == "delete") {
        printAction("delete", state.deleteKey());
        return true;
    }
    if (command == "escape") {
        printAction("escape", state.escape());
        return true;
    }
    if (command == "restore-preedit") {
        const auto [preedit, keyName] = splitArgumentHead(argument);
        if (preedit.empty()) {
            std::cerr << "restore-preedit requires: PREEDIT [KEY]\n";
            return false;
        }
        printAction("restore-preedit", state.restorePreedit(preedit, keyName.empty() ? "Delete" : keyName));
        return true;
    }
    if (command == "reset") {
        state.reset();
        printAction("reset", {tipe::ActionType::Clear, {}, true});
        return true;
    }
    if (command == "print-first") {
        std::cout << "first\t" << state.preedit() << '\t';
        if (!state.candidates().empty()) {
            std::cout << state.candidates().front();
        }
        std::cout << '\n';
        return true;
    }
    if (command == "select") {
        return selectCandidate(state, argument);
    }
    if (command == "expect-preedit") {
        if (state.preedit() != argument) {
            std::cerr << "expected preedit '" << argument << "', got '" << state.preedit() << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-preedit-cursor") {
        const auto expected = parseIndex(argument);
        if (!expected) {
            std::cerr << "expect-preedit-cursor requires a numeric index\n";
            return false;
        }
        if (state.preeditCursorIndex() != *expected) {
            std::cerr << "expected preedit cursor " << *expected << ", got " << state.preeditCursorIndex() << '\n';
            return false;
        }
        return true;
    }
    if (command == "expect-expanded") {
        const bool expected = argument == "1" || argument == "true";
        if (state.candidatesExpanded() != expected) {
            std::cerr << "expected expanded " << (expected ? 1 : 0) << ", got "
                      << (state.candidatesExpanded() ? 1 : 0) << '\n';
            return false;
        }
        return true;
    }
    if (command == "expect-selected") {
        const auto expected = parseIndex(argument);
        if (!expected) {
            std::cerr << "expect-selected requires a numeric index\n";
            return false;
        }
        if (state.candidateCursorIndex() != *expected) {
            std::cerr << "expected selected " << *expected << ", got " << state.candidateCursorIndex() << '\n';
            return false;
        }
        return true;
    }
    if (command == "expect-candidate") {
        const auto [indexText, expectedCandidate] = splitArgumentHead(argument);
        const auto index = parseIndex(indexText);
        if (!index || expectedCandidate.empty()) {
            std::cerr << "expect-candidate requires: INDEX CANDIDATE\n";
            return false;
        }
        const auto &candidates = state.candidates();
        if (*index >= candidates.size()) {
            std::cerr << "expected candidate index " << *index << " to exist, only " << candidates.size()
                      << " candidates available\n";
            return false;
        }
        if (candidates[*index] != expectedCandidate) {
            std::cerr << "expected candidate " << *index << " '" << expectedCandidate << "', got '"
                      << candidates[*index] << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-visible") {
        const auto [digitText, rest] = splitArgumentHead(argument);
        const auto [indexText, expectedCandidate] = splitArgumentHead(rest);
        const auto digit = parseIndex(digitText);
        const auto expectedIndex = parseIndex(indexText);
        if (!digit || *digit == 0 || !expectedIndex || expectedCandidate.empty()) {
            std::cerr << "expect-visible requires: DIGIT CANDIDATE_INDEX CANDIDATE\n";
            return false;
        }
        const auto &candidates = state.candidates();
        const auto visibleCells =
            tipe::visibleVisualCellsFor(candidates, state.candidateCursorIndex(), state.candidatesExpanded());
        const auto visibleRow =
            state.candidatesExpanded() ? selectedVisibleRow(visibleCells, state.candidateCursorIndex()) : 0;
        const auto rowCells = state.candidatesExpanded() ? tipe::cellsInVisualRow(visibleCells, visibleRow) : visibleCells;
        const auto digitOffset = *digit - 1;
        if (digitOffset >= rowCells.size()) {
            std::cerr << "expected visible digit " << *digit << " to exist, only " << rowCells.size()
                      << " visible candidates available\n";
            return false;
        }
        const auto candidateIndex = rowCells[digitOffset].index;
        if (candidateIndex != *expectedIndex) {
            std::cerr << "expected visible digit " << *digit << " to map to candidate " << *expectedIndex
                      << ", got " << candidateIndex << '\n';
            return false;
        }
        if (candidateIndex >= candidates.size() || candidates[candidateIndex] != expectedCandidate) {
            std::cerr << "expected visible digit " << *digit << " candidate '" << expectedCandidate << "', got '"
                      << (candidateIndex < candidates.size() ? candidates[candidateIndex] : std::string{}) << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-event") {
        const auto [indexText, rest] = splitArgumentHead(argument);
        const auto [expectedKind, expectedText] = splitArgumentHead(rest);
        const auto index = parseIndex(indexText);
        if (!index || expectedKind.empty()) {
            std::cerr << "expect-event requires: INDEX KIND [TEXT]\n";
            return false;
        }
        const auto &events = state.recentEvents();
        if (*index >= events.size()) {
            std::cerr << "expected event index " << *index << " to exist, only " << events.size()
                      << " events available\n";
            return false;
        }
        const auto actualKind = eventName(events[*index].type);
        if (actualKind != expectedKind) {
            std::cerr << "expected event " << *index << " kind '" << expectedKind << "', got '" << actualKind
                      << "'\n";
            return false;
        }
        if (!expectedText.empty() && events[*index].text != expectedText) {
            std::cerr << "expected event " << *index << " text '" << expectedText << "', got '"
                      << events[*index].text << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-has-candidate") {
        const auto &candidates = state.candidates();
        if (std::find(candidates.begin(), candidates.end(), argument) == candidates.end()) {
            std::cerr << "expected candidate list to contain '" << argument << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-context") {
        const auto [indexText, expectedContext] = splitArgumentHead(argument);
        const auto index = parseIndex(indexText);
        if (!index || expectedContext.empty()) {
            std::cerr << "expect-context requires: INDEX TEXT\n";
            return false;
        }
        const auto &commits = state.recentCommits();
        if (*index >= commits.size()) {
            std::cerr << "expected context index " << *index << " to exist, only " << commits.size()
                      << " context entries available\n";
            return false;
        }
        if (commits[*index] != expectedContext) {
            std::cerr << "expected context " << *index << " '" << expectedContext << "', got '" << commits[*index]
                      << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-no-context") {
        if (argument.empty()) {
            std::cerr << "expect-no-context requires: TEXT\n";
            return false;
        }
        const auto &commits = state.recentCommits();
        if (std::find(commits.begin(), commits.end(), argument) != commits.end()) {
            std::cerr << "expected context not to contain '" << argument << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-model-row") {
        if (argument.empty()) {
            std::cerr << "expect-model-row requires a row prefix\n";
            return false;
        }
        const auto request =
            probeModelRequest(state, application, surroundingBefore, surroundingAfter, continuousMode);
        if (request.find(argument) == std::string::npos) {
            std::cerr << "expected model request to contain row prefix '" << argument << "'\n";
            return false;
        }
        return true;
    }
    if (command == "expect-no-model-row") {
        if (argument.empty()) {
            std::cerr << "expect-no-model-row requires a row prefix\n";
            return false;
        }
        const auto request =
            probeModelRequest(state, application, surroundingBefore, surroundingAfter, continuousMode);
        if (request.find(argument) != std::string::npos) {
            std::cerr << "expected model request not to contain row prefix '" << argument << "'\n";
            return false;
        }
        return true;
    }

    std::cerr << "unknown script command: " << command << '\n';
    return false;
}

bool runProbeScriptStream(tipe::State &state, std::istream &input, std::string_view name,
                          std::string_view application, std::string_view surroundingBefore,
                          std::string_view surroundingAfter, bool &continuousMode) {
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }
        const auto [command, argument] = splitCommandLine(trimmed);
        if (!runProbeCommand(state, command, argument, application, surroundingBefore, surroundingAfter,
                             continuousMode)) {
            std::cerr << name << ':' << lineNumber << ": failed command: " << trimmed << '\n';
            return false;
        }
    }
    return true;
}

bool runProbeScript(tipe::State &state, std::string_view path, std::string_view application,
                    std::string_view surroundingBefore, std::string_view surroundingAfter, bool &continuousMode) {
    if (path == "-") {
        return runProbeScriptStream(state, std::cin, "<stdin>", application, surroundingBefore, surroundingAfter,
                                    continuousMode);
    }

    std::ifstream input{std::string(path)};
    if (!input) {
        std::cerr << "failed to open script: " << path << '\n';
        return false;
    }
    return runProbeScriptStream(state, input, path, application, surroundingBefore, surroundingAfter, continuousMode);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    bool useUserData = false;
    bool printEvents = false;
    bool printRequest = false;
    bool printLearnedCorrections = false;
    bool continuousMode = false;
    std::optional<tipe::CandidateSnapshotRect> snapshotRect;
    std::string application;
    std::string surroundingBefore;
    std::string surroundingAfter;
    std::string dictionaryPath;
    std::string preferencePath;
    for (int argIndex = 2; argIndex < argc; ++argIndex) {
        const std::string option = argv[argIndex];
        if (option == "--user-data") {
            useUserData = true;
        } else if (option == "--continuous-mode") {
            continuousMode = true;
        } else if (option == "--preferences") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            preferencePath = argv[argIndex];
        } else if (option == "--events") {
            printEvents = true;
        } else if (option == "--request") {
            printRequest = true;
        } else if (option == "--learned-corrections") {
            printLearnedCorrections = true;
        } else if (option == "--dictionary") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            dictionaryPath = argv[argIndex];
        } else if (option == "--application") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            application = argv[argIndex];
        } else if (option == "--surrounding-before") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            surroundingBefore = argv[argIndex];
        } else if (option == "--surrounding-after") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            surroundingAfter = argv[argIndex];
        } else if (option == "--script") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--snapshot") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                return 2;
            }
            snapshotRect = parseSnapshotRect(argv[argIndex]);
            if (!snapshotRect) {
                std::cerr << "--snapshot requires X,Y,W,H\n";
                return 2;
            }
        }
    }
    if (useUserData && !preferencePath.empty()) {
        std::cerr << "--user-data and --preferences cannot be used together\n";
        return 2;
    }
    if (!useUserData) {
        setenv("TIPE_DISABLE_LIBIME_LEARNING", "1", 1);
        setenv("TIPE_LIBIME_USER_HISTORY", "", 1);
    }

    const char *previousDictionary = std::getenv("TIPE_USER_DICTIONARY");
    const bool hadPreviousDictionary = previousDictionary != nullptr;
    const std::string previousDictionaryValue = hadPreviousDictionary ? previousDictionary : "";
    if (!dictionaryPath.empty()) {
        setenv("TIPE_USER_DICTIONARY", dictionaryPath.c_str(), 1);
    }

    const auto temporaryPreferencePath =
        std::filesystem::temp_directory_path() / ("tipe-state-probe-" + std::to_string(::getpid()) + ".tsv");
    if (!useUserData && preferencePath.empty()) {
        std::filesystem::remove(temporaryPreferencePath);
    }
    tipe::State state({}, useUserData ? std::filesystem::path{}
                                      : (preferencePath.empty() ? temporaryPreferencePath
                                                                : std::filesystem::path(preferencePath)));
    if (!typePinyin(state, argv[1])) {
        std::filesystem::remove(temporaryPreferencePath);
        if (!dictionaryPath.empty()) {
            hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                  : unsetenv("TIPE_USER_DICTIONARY");
        }
        return 2;
    }

    for (int argIndex = 2; argIndex < argc; ++argIndex) {
        const std::string option = argv[argIndex];
        if (option == "--user-data") {
            continue;
        }
        if (option == "--continuous-mode") {
            continuousMode = true;
            continue;
        }
        if (option == "--preferences") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            continue;
        }
        if (option == "--events") {
            continue;
        }
        if (option == "--request") {
            continue;
        }
        if (option == "--learned-corrections") {
            continue;
        }
        if (option == "--application") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            continue;
        }
        if (option == "--surrounding-before" || option == "--surrounding-after") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            continue;
        }
        if (option == "--snapshot") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            continue;
        }
        if (option == "--dictionary") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            continue;
        }
        if (option == "--script") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            if (!runProbeScript(state, argv[argIndex], application, surroundingBefore, surroundingAfter,
                                continuousMode)) {
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 1;
            }
            continue;
        }
        if (option == "--observe") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            state.observeKey(argv[argIndex]);
            continue;
        }
        if (option == "--move") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            moveProbeCursor(state, argv[argIndex]);
            continue;
        }
        if (option == "--key") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            const auto action = pressProbeKey(state, argv[argIndex], application, surroundingBefore, surroundingAfter);
            if (!printRequest) {
                printAction("key", action);
            }
            continue;
        }
        if (option == "--rerank") {
            const auto action = state.rerankCandidates(application, surroundingBefore, surroundingAfter);
            if (!printRequest) {
                printAction("rerank", action);
            }
            continue;
        }
        if (option == "--continuous-rerank") {
            continuousMode = true;
            const auto action = state.rerankCandidates(application, surroundingBefore, surroundingAfter, false, false,
                                                       true);
            if (!printRequest) {
                printAction("continuous-rerank", action);
            }
            continue;
        }
        if (option == "--digit") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            const std::string digitText = argv[argIndex];
            if (digitText.size() != 1 || digitText[0] < '1' || digitText[0] > '9') {
                std::cerr << "digit must be 1..9\n";
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            const auto action = pressProbeKey(state, digitText, application, surroundingBefore, surroundingAfter);
            if (!printRequest) {
                printAction("digit", action);
            }
            continue;
        }
        if (option == "--punct") {
            if (++argIndex >= argc) {
                usage(argv[0]);
                std::filesystem::remove(temporaryPreferencePath);
                if (!dictionaryPath.empty()) {
                    hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                          : unsetenv("TIPE_USER_DICTIONARY");
                }
                return 2;
            }
            const auto action = state.punctuation(argv[argIndex]);
            if (!printRequest) {
                printAction("punct", action);
            }
            continue;
        }
        if (option == "--space") {
            const auto action = state.space();
            if (!printRequest) {
                printAction("space", action);
            }
            continue;
        }
        if (option == "--enter") {
            const auto action = state.enter();
            if (!printRequest) {
                printAction("enter", action);
            }
            continue;
        }
        if (option == "--backspace") {
            const auto action = state.backspace();
            if (!printRequest) {
                printAction("backspace", action);
            }
            continue;
        }
        if (option == "--delete") {
            const auto action = state.deleteKey();
            if (!printRequest) {
                printAction("delete", action);
            }
            continue;
        }
        if (option == "--escape") {
            const auto action = state.escape();
            if (!printRequest) {
                printAction("escape", action);
            }
            continue;
        }
        if (option != "--select") {
            usage(argv[0]);
            std::filesystem::remove(temporaryPreferencePath);
            if (!dictionaryPath.empty()) {
                hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                      : unsetenv("TIPE_USER_DICTIONARY");
            }
            return 2;
        }
        if (++argIndex >= argc) {
            usage(argv[0]);
            std::filesystem::remove(temporaryPreferencePath);
            if (!dictionaryPath.empty()) {
                hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                      : unsetenv("TIPE_USER_DICTIONARY");
            }
            return 2;
        }
        const std::string selectedCandidate = argv[argIndex];
        if (!selectCandidate(state, selectedCandidate)) {
            std::filesystem::remove(temporaryPreferencePath);
            if (!dictionaryPath.empty()) {
                hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                      : unsetenv("TIPE_USER_DICTIONARY");
            }
            return 1;
        }
    }

    if (printRequest) {
        std::cout << probeModelRequest(state, application, surroundingBefore, surroundingAfter, continuousMode);
        std::filesystem::remove(temporaryPreferencePath);
        if (!dictionaryPath.empty()) {
            hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                                  : unsetenv("TIPE_USER_DICTIONARY");
        }
        return 0;
    }

    printState(state);
    if (printLearnedCorrections) {
        const auto activePreferencePath = useUserData ? std::filesystem::path{} :
            (preferencePath.empty() ? temporaryPreferencePath : std::filesystem::path(preferencePath));
        tipe::Dictionary dictionary;
        tipe::InputModel model(activePreferencePath);
        const auto originalScore = dictionary.languageModelNormalizedScore(state.preedit());
        const auto originalSyllables = tipe::shortestPinyinSyllableCount(state.preedit());
        for (const auto &correction : model.learnedCorrections(
                 state.preedit(), [&dictionary, originalSyllables](std::string_view candidate) {
                     if (originalSyllables &&
                         tipe::shortestPinyinSyllableCount(candidate) != originalSyllables) {
                         return 0;
                     }
                     return dictionary.learnedCorrectionPriority(candidate);
                 })) {
            std::cout << "learned-correction\t" << correction << "\tpriority\t"
                      << dictionary.learnedCorrectionPriority(correction) << "\texact-priority\t"
                      << dictionary.exactPinyinPriority(correction) << "\tconfident\t"
                      << (dictionary.hasConfidentLanguageModelSentence(correction) ? 1 : 0)
                      << "\tdecisive\t"
                      << (dictionary.hasDecisiveLanguageModelSentence(correction) ? 1 : 0)
                      << "\toriginal-score\t" << (originalScore ? std::to_string(*originalScore) : "none")
                      << "\tcorrected-score\t"
                      << ([&dictionary, &correction] {
                             const auto score = dictionary.languageModelNormalizedScore(correction);
                             return score ? std::to_string(*score) : std::string("none");
                         })()
                      << '\n';
        }
    }
    if (printEvents) {
        printRecentEvents(state);
    }
    if (snapshotRect) {
        printCandidateSnapshot(state, *snapshotRect, continuousMode);
    }
    std::filesystem::remove(temporaryPreferencePath);
    if (!dictionaryPath.empty()) {
        hadPreviousDictionary ? setenv("TIPE_USER_DICTIONARY", previousDictionaryValue.c_str(), 1)
                              : unsetenv("TIPE_USER_DICTIONARY");
    }
    return 0;
}
