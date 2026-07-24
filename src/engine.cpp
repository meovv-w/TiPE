#include "engine.h"

#include "bounded_log.h"
#include "candidate_layout.h"
#include "candidate_snapshot.h"
#include "input_privacy.h"
#include "nonblocking_pipe.h"
#include "pass_through_supervisor.h"
#include "supervision_snapshot.h"
#include "tipe_ui_public.h"

#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/instance.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <vector>

namespace tipe {

namespace {

constexpr int candidateGridColumns = static_cast<int>(visualCandidateColumns);
constexpr uint64_t activationStatusVisibleUsec = 2400000;
constexpr uint64_t activationStatusTimerAccuracyUsec = 50000;
constexpr uint64_t focusTransferDelayUsec = 300000;
constexpr uint64_t focusTransferAccuracyUsec = 25000;
constexpr uint64_t modelRerankDebounceUsec = 350000;
constexpr uint64_t preservedStateTtlUsec = 10 * 60 * 1000000ULL;
constexpr uint64_t preservedStateMetadataFallbackUsec = 30 * 1000000ULL;
constexpr uint64_t cursorRectFallbackTtlUsec = 30 * 1000000ULL;
constexpr std::size_t maxPreservedStates = 4;
constexpr std::uintmax_t supervisionHistoryMaxBytes = 256 * 1024;
constexpr std::uintmax_t supervisionTrainingHistoryMaxBytes = 1024 * 1024;
constexpr uint64_t supervisionSnapshotFlushIntervalUsec = 250000;
constexpr uint64_t supervisionSnapshotFlushAccuracyUsec = 25000;
constexpr uint64_t supervisionHistoryIntervalUsec = 2 * 1000000;
constexpr uint64_t slowKeyEventThresholdUsec = 50000;
static_assert(static_cast<std::uint64_t>(fcitx::CapabilityFlag::Password) == passwordInputCapability);
static_assert(static_cast<std::uint64_t>(fcitx::CapabilityFlag::Sensitive) == sensitiveInputCapability);
static_assert(static_cast<std::uint64_t>(fcitx::CapabilityFlag::Disable) == disabledInputMethodCapability);

bool isUsableRect(const fcitx::Rect &rect) {
    return rect.height() > 0;
}

CandidateSnapshotRect fallbackCursorRectFor(const fcitx::InputContext &ic, const fcitx::Rect &rect) {
    return logicalCandidateSnapshotRect({rect.left(), rect.top(), rect.width(), rect.height()}, ic.scaleFactor());
}

std::optional<int> envInt(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

bool waylandPopupEdgeFallbackEnabled() {
    const char *enabled = std::getenv("TIPE_WAYLAND_POPUP_EDGE_FALLBACK");
    return enabled && (std::string_view(enabled) == "1" || std::string_view(enabled) == "true" ||
                       std::string_view(enabled) == "on");
}

bool waylandPopupEdgeFallbackNeeded(const fcitx::Rect &rect, const State &state) {
    if (!waylandPopupEdgeFallbackEnabled()) {
        return false;
    }
    int popupWidth = 0;
    int popupHeight = 0;
    if (!state.candidates().empty()) {
        const auto cells = visibleVisualCellsFor(state.candidates(), state.candidateCursorIndex(),
                                                 state.candidatesExpanded(), tipeUIPanelMaxExpandedRows);
        const auto metrics = tipeUIPanelMetricsFor(cells, state.candidates(), state.candidatesExpanded(),
                                                   !state.preedit().empty());
        popupWidth = metrics.width;
        popupHeight = metrics.height;
    }
    const int leftThreshold =
        envInt("TIPE_WAYLAND_POPUP_EDGE_LEFT").value_or(tipeUIPopupEdgeFallbackLeftThreshold);
    const int topThreshold =
        envInt("TIPE_WAYLAND_POPUP_EDGE_TOP").value_or(tipeUIPopupEdgeFallbackTopThreshold);
    return tipeUIPopupEdgeFallbackNeededForRect(rect.left(), rect.top(), rect.width(), rect.height(), leftThreshold,
                                               topThreshold, popupWidth, popupHeight);
}

bool debugEnabled() {
    const char *value = std::getenv("TIPE_DEBUG");
    return value && std::string_view(value) == "1";
}

std::filesystem::path tipeCacheDir() {
    if (const char *xdgCacheHome = std::getenv("XDG_CACHE_HOME"); xdgCacheHome && *xdgCacheHome) {
        return std::filesystem::path(xdgCacheHome) / "tipe";
    }
    const char *home = std::getenv("HOME");
    if (!home) {
        return {};
    }
    return std::filesystem::path(home) / ".cache" / "tipe";
}

std::filesystem::path supervisionSnapshotPath() {
    const auto cacheDir = tipeCacheDir();
    if (cacheDir.empty()) {
        return {};
    }
    return cacheDir / "supervision-current.tsv";
}

std::filesystem::path supervisionLastSnapshotPath() {
    const auto cacheDir = tipeCacheDir();
    if (cacheDir.empty()) {
        return {};
    }
    return cacheDir / "supervision-last.tsv";
}

std::filesystem::path supervisionHistoryPath() {
    const auto cacheDir = tipeCacheDir();
    if (cacheDir.empty()) {
        return {};
    }
    return cacheDir / "supervision-history.tsv";
}

std::filesystem::path supervisionTrainingHistoryPath() {
    const auto cacheDir = tipeCacheDir();
    if (cacheDir.empty()) {
        return {};
    }
    return cacheDir / "supervision-training-history.tsv";
}

std::filesystem::path inputModeStatePath() {
    if (const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir) {
        return std::filesystem::path(runtimeDir) / "tipe" / "input-mode";
    }
    const auto cacheDir = tipeCacheDir();
    return cacheDir.empty() ? std::filesystem::path{} : cacheDir / "input-mode";
}

struct InputModeRequest {
    bool english = false;
    std::string mode = "chinese";
    std::string token;
};

InputModeRequest readInputModeRequest(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value)) {
        return {};
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    InputModeRequest request;
    request.english = value == "english" || value == "eng";
    request.mode = request.english ? "english" : "chinese";
    input >> request.token;
    if (request.token.size() > 128) {
        request.token.clear();
    }
    return request;
}

bool readEnglishMode(const std::filesystem::path &path) {
    return readInputModeRequest(path).english;
}

std::filesystem::path inputModeAppliedPath(const std::filesystem::path &requestPath) {
    return requestPath.empty() ? std::filesystem::path{} : requestPath.parent_path() / "input-mode-applied";
}

bool writeInputModeApplied(const std::filesystem::path &path, const InputModeRequest &request) {
    if (path.empty() || request.token.empty()) {
        return request.token.empty();
    }
    static std::atomic<std::uint64_t> serial{0};
    const auto temporary = path.string() + ".tmp." + std::to_string(getpid()) + "." +
                           std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const std::string contents = request.mode + "\t" + request.token + "\n";
    const auto removeTemporary = [&temporary] {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    };
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(fd);
            removeTemporary();
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (close(fd) != 0 || ::rename(temporary.c_str(), path.c_str()) != 0) {
        removeTemporary();
        return false;
    }
    return true;
}

std::string historyField(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\t') {
            escaped += "\\t";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else if (ch == '\\') {
            escaped += "\\\\";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

void traceEngine(std::string_view message) {
    if (!debugEnabled()) {
        return;
    }
    const auto logDir = tipeCacheDir();
    if (logDir.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(logDir, error);
    appendBoundedDiagnosticLog(logDir / "engine-trace.log", message);
}

void traceEngineDebug(std::string_view message) {
    if (debugEnabled()) {
        traceEngine(message);
    }
}

std::string traceSafe(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\t') {
            escaped += "\\t";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string lowercaseAscii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const unsigned char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

void traceCandidateUpdate(const State &state, std::string_view surface) {
    if (!debugEnabled()) {
        return;
    }
    if (state.preedit().empty() && state.candidates().empty()) {
        traceEngine(std::string("update-candidates surface=") + std::string(surface) + " preedit= candidates=0");
        return;
    }
    std::string line = std::string("update-candidates surface=") + std::string(surface) +
                       " preedit=" + traceSafe(state.preedit()) +
                       " selected=" + std::to_string(state.candidateCursorIndex()) +
                       " expanded=" + (state.candidatesExpanded() ? "1" : "0") +
                       " candidates=" + std::to_string(state.candidates().size());
    const auto limit = std::min<std::size_t>(state.candidates().size(), 8);
    for (std::size_t index = 0; index < limit; ++index) {
        line += " c" + std::to_string(index) + "=" + traceSafe(state.candidates()[index]);
    }
    traceEngine(line);
}

bool nativeFollowFallbackEnabled() {
    const char *value = std::getenv("TIPE_NATIVE_FOLLOW_FALLBACK");
    return value && std::string_view(value) == "1";
}

bool defaultContinuousModeEnabled() {
    const char *value = std::getenv("TIPE_CONTINUOUS_MODE");
    return value && (std::string_view(value) == "1" || std::string_view(value) == "true" ||
                     std::string_view(value) == "on");
}

bool tipeUIActive(fcitx::Instance *instance) {
    if (!instance) {
        return false;
    }
    const auto currentUI = instance->currentUI();
    return currentUI == "tipeui" || currentUI.find("tipeui") != std::string::npos;
}

bool tipeUIHandlesInputContext(fcitx::Instance *instance, const fcitx::InputContext &ic) {
    (void)ic;
    // The UI addon owns both its native Wayland popup and its GTK fallback.
    // Publishing one InputPanel contract for every frontend prevents XIM and
    // D-Bus clients from drifting onto a feature-reduced engine-side path.
    return tipeUIActive(instance);
}

void configureTipeInputPanelRoute(fcitx::Instance *instance, fcitx::InputContext *ic,
                                  bool useTipeUI) {
    if (!ic) {
        return;
    }
    auto &panel = ic->inputPanel();
    if (!instance || !useTipeUI ||
        !ic->capabilityFlags().test(fcitx::CapabilityFlag::ClientSideInputPanel)) {
        panel.setCustomInputPanelCallback({});
        return;
    }

    // The callback already runs in fcitx's deferred UI phase. Call tipeui
    // directly instead of queueing the same component a second time.
    panel.setCustomInputPanelCallback([instance](fcitx::InputContext *inputContext) {
        if (!inputContext || !tipeUIActive(instance)) {
            return;
        }
        auto *uiAddon = instance->addonManager().lookupAddon("tipeui");
        if (!uiAddon) {
            traceEngineDebug("ui-route direct missing-addon");
            return;
        }
        traceEngineDebug(std::string("ui-route direct frontend=") +
                         std::string(inputContext->frontendName()) +
                         " program=" + traceSafe(inputContext->program()));
        uiAddon->call<fcitx::ITipeUI::updateInputPanel>(inputContext);
    });
}

bool statusWindowFallbackEnabled() {
    const char *disabled = std::getenv("TIPE_STATUS_WINDOW_FALLBACK");
    if (disabled && (std::string_view(disabled) == "0" || std::string_view(disabled) == "false" ||
                     std::string_view(disabled) == "off")) {
        return false;
    }
    return true;
}

bool inputPanelHasCompositionContent(fcitx::InputContext *ic) {
    if (!ic) {
        return false;
    }
    const auto &panel = ic->inputPanel();
    if (!panel.preedit().toString().empty() || !panel.clientPreedit().toString().empty()) {
        return true;
    }
    if (auto candidates = panel.candidateList(); candidates && candidates->size() > 0) {
        return true;
    }
    return false;
}

bool hasSupervisionContent(const State &state) {
    return !state.empty() || !state.recentEvents().empty();
}

uint64_t monotonicUsec() {
    return fcitx::now(CLOCK_MONOTONIC);
}

class KeyEventLatencyGuard {
public:
    KeyEventLatencyGuard(const fcitx::InputContext &inputContext, const fcitx::Key &key)
        : frontend_(inputContext.frontendName()), program_(inputContext.program()), key_(key.toString()),
          startedUsec_(monotonicUsec()) {}

    ~KeyEventLatencyGuard() {
        const auto finishedUsec = monotonicUsec();
        if (finishedUsec < startedUsec_ || finishedUsec - startedUsec_ < slowKeyEventThresholdUsec) {
            return;
        }
        const auto computationUsec = computationCompleteUsec_ >= startedUsec_
                                         ? computationCompleteUsec_ - startedUsec_
                                         : uint64_t{0};
        const auto uiUsec = uiCompleteUsec_ >= computationCompleteUsec_ && computationCompleteUsec_ != 0
                                ? uiCompleteUsec_ - computationCompleteUsec_
                                : uint64_t{0};
        const auto tailStart = uiCompleteUsec_ != 0 ? uiCompleteUsec_
                                                    : (computationCompleteUsec_ != 0 ? computationCompleteUsec_
                                                                                   : startedUsec_);
        const auto tailUsec = finishedUsec >= tailStart ? finishedUsec - tailStart : uint64_t{0};
        std::ostringstream line;
        line << "slow-key-event\ttotal_us=" << (finishedUsec - startedUsec_)
             << "\tcompute_us=" << computationUsec << "\tui_us=" << uiUsec << "\ttail_us=" << tailUsec
             << "\tfrontend=" << historyField(frontend_) << "\tprogram=" << historyField(program_)
             << "\tkey=" << historyField(key_);
        FCITX_WARN() << line.str();
        const auto cacheDir = tipeCacheDir();
        if (!cacheDir.empty()) {
            std::error_code error;
            std::filesystem::create_directories(cacheDir, error);
            appendBoundedDiagnosticLog(cacheDir / "slow-key-events.log", line.str());
        }
    }

    void markComputationComplete() { computationCompleteUsec_ = monotonicUsec(); }
    void markUIComplete() { uiCompleteUsec_ = monotonicUsec(); }

private:
    std::string frontend_;
    std::string program_;
    std::string key_;
    uint64_t startedUsec_ = 0;
    uint64_t computationCompleteUsec_ = 0;
    uint64_t uiCompleteUsec_ = 0;
};

std::size_t utf8ByteOffsetForCodepoints(std::string_view text, std::size_t codepoints) {
    if (codepoints == 0) {
        return 0;
    }
    std::size_t seen = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const auto ch = static_cast<unsigned char>(text[offset]);
        if ((ch & 0xC0) == 0x80) {
            continue;
        }
        if (seen++ == codepoints) {
            return offset;
        }
    }
    return text.size();
}

std::string lastUtf8Codepoints(std::string_view text, std::size_t codepoints) {
    std::vector<std::size_t> starts;
    starts.reserve(std::min<std::size_t>(text.size(), codepoints + 1));
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const auto ch = static_cast<unsigned char>(text[offset]);
        if ((ch & 0xC0) != 0x80) {
            starts.push_back(offset);
        }
    }
    if (starts.size() <= codepoints) {
        return std::string(text);
    }
    return std::string(text.substr(starts[starts.size() - codepoints]));
}

std::string firstUtf8Codepoints(std::string_view text, std::size_t codepoints) {
    return std::string(text.substr(0, utf8ByteOffsetForCodepoints(text, codepoints)));
}

std::pair<std::string, std::string> surroundingContextFor(fcitx::InputContext *ic) {
    if (!ic || inputCapabilitiesBlockSupervision(ic->capabilityFlags().toInteger())) {
        return {};
    }
    const auto &surrounding = ic->surroundingText();
    if (!surrounding.isValid()) {
        return {};
    }
    constexpr std::size_t contextCodepointLimit = 80;
    const auto &text = surrounding.text();
    const auto cursorByteOffset = utf8ByteOffsetForCodepoints(text, surrounding.cursor());
    const auto before = std::string_view(text).substr(0, cursorByteOffset);
    const auto after = cursorByteOffset <= text.size() ? std::string_view(text).substr(cursorByteOffset)
                                                       : std::string_view{};
    return {lastUtf8Codepoints(before, contextCodepointLimit), firstUtf8Codepoints(after, contextCodepointLimit)};
}

std::size_t utf8CodepointCount(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xC0) != 0x80;
    }));
}

std::string tipeUIStateAux(const State &state, bool continuousMode) {
    const auto counts = state.debugSnapshot().eventCounts;
    const auto supervisedKeys = counts.letters + counts.digits + counts.symbols + counts.backspaces + counts.deletes +
                                counts.spaces + counts.enters + counts.escapes + counts.observedKeys +
                                counts.cursorMoves;
    return std::string("__tipe_ui_state\texpanded\t") + (state.candidatesExpanded() ? "1" : "0") +
           "\tsupervision\t1\tkeys\t" + std::to_string(supervisedKeys) + "\tselects\t" +
           std::to_string(counts.candidateSelections) + "\treranks\t" + std::to_string(counts.rerankRequests) +
           "\tcontinuous\t" + (continuousMode ? "1" : "0");
}

bool isCommitBeforePunctuationKey(fcitx::KeySym sym) {
    switch (sym) {
    case FcitxKey_comma:
    case FcitxKey_period:
    case FcitxKey_slash:
    case FcitxKey_semicolon:
    case FcitxKey_apostrophe:
    case FcitxKey_bracketleft:
    case FcitxKey_bracketright:
    case FcitxKey_backslash:
    case FcitxKey_minus:
    case FcitxKey_equal:
    case FcitxKey_grave:
    case FcitxKey_exclam:
    case FcitxKey_at:
    case FcitxKey_numbersign:
    case FcitxKey_dollar:
    case FcitxKey_percent:
    case FcitxKey_asciicircum:
    case FcitxKey_ampersand:
    case FcitxKey_asterisk:
    case FcitxKey_parenleft:
    case FcitxKey_parenright:
    case FcitxKey_underscore:
    case FcitxKey_plus:
    case FcitxKey_braceleft:
    case FcitxKey_braceright:
    case FcitxKey_bar:
    case FcitxKey_colon:
    case FcitxKey_quotedbl:
    case FcitxKey_less:
    case FcitxKey_greater:
    case FcitxKey_question:
    case FcitxKey_asciitilde:
    case FcitxKey_KP_Decimal:
    case FcitxKey_KP_Divide:
    case FcitxKey_KP_Multiply:
    case FcitxKey_KP_Subtract:
    case FcitxKey_KP_Add:
    case FcitxKey_KP_Separator:
    case FcitxKey_KP_Equal:
        return true;
    default:
        return false;
    }
}

bool startsEnglishText(const fcitx::Key &key) {
    const auto nonTextModifiers = fcitx::KeyStates{
        fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,    fcitx::KeyState::Super,
        fcitx::KeyState::Super2, fcitx::KeyState::Hyper, fcitx::KeyState::Hyper2,
        fcitx::KeyState::Meta, fcitx::KeyState::Mod5,
    };
    if (key.states().testAny(nonTextModifiers)) {
        return false;
    }
    if (key.digit() >= 0 || key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
        return true;
    }
    const auto sym = key.sym();
    return sym >= FcitxKey_space && sym <= FcitxKey_asciitilde;
}

std::optional<char> rawTokenSymbolForKey(fcitx::KeySym sym) {
    switch (sym) {
    case FcitxKey_minus:
        return '-';
    case FcitxKey_underscore:
        return '_';
    case FcitxKey_period:
    case FcitxKey_KP_Decimal:
        return '.';
    case FcitxKey_slash:
    case FcitxKey_KP_Divide:
        return '/';
    default:
        return std::nullopt;
    }
}

bool isDownKey(const fcitx::Key &key) { return key.check(FcitxKey_Down) || key.check(FcitxKey_KP_Down); }

bool isUpKey(const fcitx::Key &key) { return key.check(FcitxKey_Up) || key.check(FcitxKey_KP_Up); }

bool isPageDownKey(const fcitx::Key &key) {
    return key.check(FcitxKey_Next) || key.check(FcitxKey_Page_Down) || key.check(FcitxKey_KP_Next) ||
           key.check(FcitxKey_KP_Page_Down);
}

bool isPageUpKey(const fcitx::Key &key) {
    return key.check(FcitxKey_Prior) || key.check(FcitxKey_Page_Up) || key.check(FcitxKey_KP_Prior) ||
           key.check(FcitxKey_KP_Page_Up);
}

bool isHomeKey(const fcitx::Key &key) { return key.check(FcitxKey_Home) || key.check(FcitxKey_KP_Home); }

bool isEndKey(const fcitx::Key &key) { return key.check(FcitxKey_End) || key.check(FcitxKey_KP_End); }

bool isRightKey(const fcitx::Key &key) { return key.check(FcitxKey_Right) || key.check(FcitxKey_KP_Right); }

bool isLeftKey(const fcitx::Key &key) { return key.check(FcitxKey_Left) || key.check(FcitxKey_KP_Left); }

bool isShiftTabKey(const fcitx::Key &key) {
    return key.check(FcitxKey_ISO_Left_Tab) || key.check(FcitxKey_Tab, fcitx::KeyStates{fcitx::KeyState::Shift});
}

bool isTabKey(const fcitx::Key &key) { return key.check(FcitxKey_Tab); }

bool isModelRerankKey(const fcitx::Key &key) { return key.check(FcitxKey_F9); }

bool isContinuousModeToggleKey(const fcitx::Key &key) {
    return key.check(FcitxKey_F9, fcitx::KeyStates{fcitx::KeyState::Shift});
}

bool isActivationStatusText(std::string_view text) {
    return text == "TiPE" || text == "Eng" || text == "Auto" || text == "Manual";
}

bool reapProcess(pid_t pid) {
    if (pid <= 0) {
        return true;
    }
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

class TipeCandidateWord final : public fcitx::CandidateWord {
public:
    using SelectCallback = std::function<void(fcitx::InputContext *)>;

    TipeCandidateWord(std::string text, SelectCallback callback)
        : fcitx::CandidateWord(fcitx::Text(std::move(text))), callback_(std::move(callback)) {}

    void select(fcitx::InputContext *inputContext) const override {
        if (callback_) {
            callback_(inputContext);
        }
    }

private:
    SelectCallback callback_;
};

class TipeCandidateList final : public fcitx::CandidateList {
public:
    void append(std::string text, TipeCandidateWord::SelectCallback callback) {
        const auto ordinal = words_.size() + 1;
        labels_.emplace_back(ordinal <= 9 ? std::to_string(ordinal) : std::string{});
        words_.push_back(std::make_unique<TipeCandidateWord>(std::move(text), std::move(callback)));
    }

    void setCursorIndex(int index) { cursorIndex_ = index; }

    const fcitx::Text &label(int index) const override { return labels_.at(static_cast<std::size_t>(index)); }
    const fcitx::CandidateWord &candidate(int index) const override {
        return *words_.at(static_cast<std::size_t>(index));
    }
    int size() const override { return static_cast<int>(words_.size()); }
    int cursorIndex() const override { return cursorIndex_; }
    fcitx::CandidateLayoutHint layoutHint() const override { return fcitx::CandidateLayoutHint::Horizontal; }

private:
    std::vector<fcitx::Text> labels_;
    std::vector<std::unique_ptr<TipeCandidateWord>> words_;
    int cursorIndex_ = 0;
};

} // namespace

Engine::Engine(fcitx::AddonManager *manager) {
    instance_ = manager ? manager->instance() : nullptr;
    continuousMode_ = defaultContinuousModeEnabled();
    inputModePath_ = inputModeStatePath();
    englishMode_ = readEnglishMode(inputModePath_);
    sanitizePersistentSupervisionFile(supervisionLastSnapshotPath());
    sanitizePersistentSupervisionFile(supervisionHistoryPath());
    sanitizePersistentSupervisionFile(supervisionTrainingHistoryPath());
    std::signal(SIGPIPE, SIG_IGN);
    if (instance_) {
        cursorRectWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextCursorRectChanged, fcitx::EventWatcherPhase::InputMethod,
            [this](fcitx::Event &event) { onCursorRectChanged(event); });
        inputContextFocusOutWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::InputMethod,
            [this](fcitx::Event &event) { onInputContextFocusOut(event); });
        inputContextDestroyedWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::InputMethod,
            [this](fcitx::Event &event) { onInputContextDestroyed(event); });
        inputContextCapabilityChangedWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextCapabilityChanged, fcitx::EventWatcherPhase::InputMethod,
            [this](fcitx::Event &event) { onInputContextCapabilityChanged(event); });
        initializeInputModeControl();
        initializeAsyncModelResults();
    }
    FCITX_INFO() << "TiPE addon loaded in " << (englishMode_ ? "English" : "Chinese") << " mode";
}

Engine::~Engine() {
    stopAsyncModelJobs();
    focusTransferTimer_.reset();
    inputModeWatcher_.reset();
    if (inputModeWatchFd_ >= 0) {
        close(inputModeWatchFd_);
        inputModeWatchFd_ = -1;
    }
    clearSupervisionSnapshot();
    closeCandidateWindow();
    closeStatusWindow();
}

std::vector<fcitx::InputMethodEntry> Engine::listInputMethods() {
    std::vector<fcitx::InputMethodEntry> entries;
    entries.emplace_back("tipe", "TiPE", "zh_CN", "tipe");
    entries.back().setNativeName("TiPE").setIcon("tipe").setLabel("TiPE");
    return entries;
}

bool Engine::inputContextBlocksSupervision(fcitx::InputContext *ic) const {
    return ic && inputCapabilitiesBlockSupervision(ic->capabilityFlags().toInteger());
}

void Engine::discardBlockedInputContext(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    if (!blockedInputContexts_.insert(ic).second) {
        return;
    }
    std::erase_if(preservedStates_, [ic](const PreservedState &preserved) {
        return preserved.inputContext == ic;
    });
    dropState(ic);
    clearPanel(ic);
    traceEngine("private-input supervision-disabled");
}

void Engine::initializeInputModeControl() {
    if (!instance_ || inputModePath_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(inputModePath_.parent_path(), error);
    if (error) {
        FCITX_WARN() << "Failed to create TiPE input-mode directory: " << error.message();
        return;
    }

    inputModeWatchFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inputModeWatchFd_ < 0) {
        FCITX_WARN() << "Failed to initialize TiPE input-mode watcher";
        return;
    }
    const int watch = inotify_add_watch(inputModeWatchFd_, inputModePath_.parent_path().c_str(),
                                        IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO);
    if (watch < 0) {
        close(inputModeWatchFd_);
        inputModeWatchFd_ = -1;
        FCITX_WARN() << "Failed to watch TiPE input-mode directory";
        return;
    }
    inputModeWatcher_ = instance_->eventLoop().addIOEvent(
        inputModeWatchFd_, fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int fd, fcitx::IOEventFlags) { return onInputModeControlEvent(fd); });
    if (!inputModeWatcher_) {
        close(inputModeWatchFd_);
        inputModeWatchFd_ = -1;
        return;
    }
    // Close the read-before-watch race: a request written while the addon is
    // starting must still be applied and acknowledged.
    refreshInputMode(false);
}

bool Engine::onInputModeControlEvent(int fd) {
    std::array<char, 4096> buffer{};
    bool shouldRefresh = false;
    for (;;) {
        const ssize_t size = read(fd, buffer.data(), buffer.size());
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (size <= 0) {
            break;
        }
        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(size)) {
            const auto *event = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
            if ((event->mask & IN_Q_OVERFLOW) != 0 ||
                (event->len > 0 && std::string_view(event->name) == inputModePath_.filename().string())) {
                shouldRefresh = true;
            }
            offset += sizeof(inotify_event) + event->len;
        }
    }
    if (shouldRefresh) {
        refreshInputMode(true);
    }
    return true;
}

void Engine::initializeAsyncModelResults() {
    if (!instance_) {
        return;
    }
    asyncModelResultFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (asyncModelResultFd_ < 0) {
        FCITX_WARN() << "Failed to create TiPE async model result event";
        return;
    }
    asyncModelResultWatcher_ = instance_->eventLoop().addIOEvent(
        asyncModelResultFd_, fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int fd, fcitx::IOEventFlags) { return onAsyncModelResultEvent(fd); });
    if (!asyncModelResultWatcher_) {
        close(asyncModelResultFd_);
        asyncModelResultFd_ = -1;
    }
}

void Engine::startAsyncModelRerank(fcitx::InputContext *ic, State::ExternalModelRequest request,
                                   std::string command) {
    if (!ic || asyncModelResultFd_ < 0 || command.empty()) {
        return;
    }
    const auto serial = ++asyncModelSerials_[ic];
    auto job = std::make_shared<AsyncModelJob>(ic, serial, std::move(request), std::move(command));
    const int resultFd = asyncModelResultFd_;
    try {
        job->worker = std::thread([job, resultFd] {
            job->output = invokeModelCommand(job->command, job->request.payload);
            job->complete.store(true, std::memory_order_release);
            const uint64_t ready = 1;
            ssize_t result;
            do {
                result = write(resultFd, &ready, sizeof(ready));
            } while (result < 0 && errno == EINTR);
        });
    } catch (const std::system_error &error) {
        traceEngine("model-async-start-failed serial=" + std::to_string(serial) + " error=" + error.what());
        return;
    }
    traceEngine("model-async-start serial=" + std::to_string(serial) + " preedit=" +
                traceSafe(job->request.preedit));
    asyncModelJobs_.push_back(std::move(job));
}

bool Engine::onAsyncModelResultEvent(int fd) {
    uint64_t ready = 0;
    for (;;) {
        const ssize_t result = read(fd, &ready, sizeof(ready));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (result <= 0) {
            break;
        }
    }
    collectAsyncModelResults();
    return true;
}

void Engine::collectAsyncModelResults() {
    for (auto iter = asyncModelJobs_.begin(); iter != asyncModelJobs_.end();) {
        auto &job = *iter;
        if (!job->complete.load(std::memory_order_acquire)) {
            ++iter;
            continue;
        }
        if (job->worker.joinable()) {
            job->worker.join();
        }
        auto *inputContext = job->inputContext.get();
        const auto serial = asyncModelSerials_.find(inputContext);
        const bool currentRequest = serial != asyncModelSerials_.end() && serial->second == job->serial;
        auto state = states_.find(inputContext);
        if (!inputContext || !currentRequest || englishMode_ || inputContextBlocksSupervision(inputContext) ||
            state == states_.end() || state->second.empty()) {
            traceEngine("model-async-discard serial=" + std::to_string(job->serial) + " reason=stale-context");
        } else if (!job->output) {
            traceEngine("model-async-finish serial=" + std::to_string(job->serial) + " result=unavailable");
        } else {
            const auto action = state->second.applyExternalModelResponse(job->request, *job->output, true);
            if (action.type == ActionType::Update) {
                traceEngine("model-async-finish serial=" + std::to_string(job->serial) + " result=applied");
                applyAction(inputContext, nullptr, action);
            } else {
                traceEngine("model-async-discard serial=" + std::to_string(job->serial) + " reason=stale-state");
            }
        }
        iter = asyncModelJobs_.erase(iter);
    }
}

void Engine::stopAsyncModelJobs() {
    asyncModelResultWatcher_.reset();
    for (auto &job : asyncModelJobs_) {
        if (job->worker.joinable()) {
            job->worker.join();
        }
    }
    asyncModelJobs_.clear();
    if (asyncModelResultFd_ >= 0) {
        close(asyncModelResultFd_);
        asyncModelResultFd_ = -1;
    }
}

void Engine::refreshInputMode(bool announce) {
    if (inputModePath_.empty()) {
        return;
    }
    const auto request = readInputModeRequest(inputModePath_);
    setEnglishMode(request.english, announce);
    if (!request.token.empty()) {
        const auto appliedPath = inputModeAppliedPath(inputModePath_);
        if (writeInputModeApplied(appliedPath, request)) {
            traceEngine("input-mode applied=" + request.mode + " token=" + request.token);
        } else {
            FCITX_WARN() << "Failed to acknowledge TiPE input-mode request";
            traceEngine("input-mode acknowledge-failed mode=" + request.mode + " token=" + request.token);
        }
    }
}

void Engine::setEnglishMode(bool enabled, bool announce) {
    if (englishMode_ == enabled) {
        return;
    }
    focusTransferTimer_.reset();
    for (const auto &job : asyncModelJobs_) {
        if (auto *inputContext = job->inputContext.get()) {
            ++asyncModelSerials_[inputContext];
        }
    }
    if (englishMode_ && !enabled) {
        while (!passThroughSupervisors_.empty()) {
            finishPassThroughTracking(passThroughSupervisors_.begin()->first, "ModeSwitch");
        }
    } else {
        passThroughSupervisors_.clear();
    }
    englishMode_ = enabled;
    cursorFollowTrackers_.clear();
    traceEngine(std::string("input-mode changed=") + (englishMode_ ? "english" : "chinese"));

    auto *ic = instance_ ? instance_->mostRecentInputContext() : nullptr;
    if (!ic) {
        return;
    }
    if (!inputContextBlocksSupervision(ic)) {
        blockedInputContexts_.erase(ic);
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (englishMode_) {
        if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
            updateEnglishPendingPanel(ic, iter->second);
        } else {
            clearPanel(ic);
        }
        if (announce && (states_.find(ic) == states_.end() || states_.at(ic).empty())) {
            showInputModeStatus(ic, "Eng");
        }
        return;
    }
    clearPanel(ic);
    if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
        updatePanel(ic, iter->second);
    } else if (announce) {
        showInputModeStatus(ic, "TiPE");
    }
}

void Engine::passThroughKeyEvent(fcitx::KeyEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (activationStatusVisible(ic)) {
        clearPanel(ic);
    }
    auto &supervisor = passThroughSupervisors_.try_emplace(ic).first->second;
    const auto key = event.key();
    const auto sym = key.sym();
    std::optional<PassThroughSupervisionSnapshot> snapshot;

    if (!key.hasModifier() && sym >= FcitxKey_a && sym <= FcitxKey_z) {
        supervisor.inputLetter(static_cast<char>('a' + (sym - FcitxKey_a)));
    } else if (!key.hasModifier() && sym >= FcitxKey_A && sym <= FcitxKey_Z) {
        supervisor.inputLetter(static_cast<char>('A' + (sym - FcitxKey_A)));
    } else if (const int digit = key.digit(); digit >= 0) {
        supervisor.inputDigit(static_cast<char>('0' + digit));
    } else if (key.check(FcitxKey_BackSpace)) {
        supervisor.backspace();
    } else if (key.check(FcitxKey_Delete)) {
        supervisor.deleteKey();
    } else if (key.check(FcitxKey_space)) {
        snapshot = supervisor.commitSpace(ic->program(), continuousMode_);
    } else if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
        snapshot = supervisor.commitEnter(ic->program(), continuousMode_);
    } else if (key.check(FcitxKey_Escape)) {
        snapshot = supervisor.cancel(ic->program(), continuousMode_);
    } else if (isShiftTabKey(key) || isTabKey(key)) {
        snapshot = supervisor.commitObservedBoundary(key.toString(), ic->program(), continuousMode_);
    } else if (!key.hasModifier() && sym > FcitxKey_space && sym <= FcitxKey_asciitilde) {
        snapshot = supervisor.commitPunctuation(static_cast<char>(sym), ic->program(), continuousMode_);
    } else if (key.isCursorMove()) {
        supervisor.cursorMove(key.toString());
    } else {
        supervisor.observeKey(key.toString(), !key.isModifier());
    }
    if (snapshot) {
        writePassThroughSupervisionSnapshot(ic, *snapshot);
    } else {
        schedulePassThroughSupervisionSnapshot(ic);
    }
}

void Engine::finishPassThroughTracking(fcitx::InputContext *ic, std::string_view reason) {
    if (!ic) {
        return;
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    const auto iter = passThroughSupervisors_.find(ic);
    if (iter == passThroughSupervisors_.end()) {
        return;
    }
    if (!iter->second.token().empty()) {
        const auto tokenSize = iter->second.token().size();
        const auto snapshot = iter->second.commitObservedBoundary(reason, ic->program(), continuousMode_);
        writePassThroughSupervisionSnapshot(ic, snapshot);
        traceEngine("pass-through-finish reason=" + std::string(reason) + " bytes=" +
                    std::to_string(tokenSize));
    } else if (pendingSupervisionSnapshot_ && pendingSupervisionSnapshot_->owner == ic) {
        clearSupervisionSnapshot(ic);
    }
    passThroughSupervisors_.erase(iter);
}

void Engine::keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &event) {
    FCITX_UNUSED(entry);

    auto *ic = event.inputContext();
    const bool blockedContext = inputContextBlocksSupervision(ic);
    if (ic && !blockedContext) {
        blockedInputContexts_.erase(ic);
    }
    if (event.isRelease()) {
        if (blockedContext) {
            discardBlockedInputContext(ic);
            return;
        }
        if (ic && modelRerankHeld_.erase(ic) > 0) {
            event.filterAndAccept();
        }
        return;
    }

    if (!ic) {
        return;
    }
    if (!inputModeWatcher_) {
        refreshInputMode(false);
    }
    if (blockedContext) {
        discardBlockedInputContext(ic);
        return;
    }
    KeyEventLatencyGuard latencyGuard(*ic, event.key());
    if (englishMode_) {
        if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty() &&
            startsEnglishText(event.key())) {
            traceEngine("input-mode english commit-pending-raw preedit=" + traceSafe(iter->second.preedit()) +
                        " key=" + traceSafe(event.key().toString()));
            applyAction(ic, nullptr, iter->second.commitRawPreedit("InputModeEnglish"));
        }
        passThroughKeyEvent(event);
        latencyGuard.markComputationComplete();
        latencyGuard.markUIComplete();
        return;
    }

    if (activationStatusVisible(ic)) {
        clearActivationStatusState(ic);
        auto &panel = ic->inputPanel();
        panel.reset();
        configureTipeInputPanelRoute(instance_, ic, tipeUIHandlesInputContext(instance_, *ic));
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        closeStatusWindow();
    }

    if (auto iter = states_.find(ic); (iter == states_.end() || iter->second.empty()) && restorePreservedState(ic)) {
        traceEngine(std::string("restore-state before-key key=") + traceSafe(event.key().toString()));
    }

    auto &state = stateFor(ic);
    const bool hadPartialCommitResetProtection = preservePreeditUntilNextKey_.contains(ic);
    const auto key = event.key();
    const auto sym = key.sym();
    bool skipContinuousRerank = false;

    const bool modelRerankKey = isModelRerankKey(key) && !state.empty();
    if (modelRerankKey) {
        if (!modelRerankHeld_.insert(ic).second) {
            traceEngine("model-rerank-held-repeat key=" + traceSafe(key.toString()) +
                        " preedit=" + traceSafe(state.preedit()));
            event.filterAndAccept();
            return;
        }
        const auto now = monotonicUsec();
        if (const auto previous = lastModelRerankUsec_.find(ic);
            previous != lastModelRerankUsec_.end() && now >= previous->second &&
            now - previous->second < modelRerankDebounceUsec) {
            traceEngine("model-rerank-debounced key=" + traceSafe(key.toString()) +
                        " preedit=" + traceSafe(state.preedit()));
            event.filterAndAccept();
            return;
        }
        lastModelRerankUsec_[ic] = now;
    } else {
        modelRerankHeld_.erase(ic);
        lastModelRerankUsec_.erase(ic);
    }
    ++asyncModelSerials_[ic];

    Action action;
    if (isContinuousModeToggleKey(key)) {
        continuousMode_ = !continuousMode_;
        lastContinuousPreedit_.clear();
        traceEngine(std::string("continuous-mode toggled=") + (continuousMode_ ? "1" : "0") +
                    " active-preedit=" + (!state.empty() ? "1" : "0"));
        skipContinuousRerank = true;
        event.filterAndAccept();
        if (!state.empty()) {
            updatePanel(ic, state);
        } else {
            showInputModeStatus(ic, continuousMode_ ? "Auto" : "Manual");
        }
        return;
    }

    if (key.check(FcitxKey_Delete)) {
        if (const auto undo = partialCommitUndo_.find(ic);
            !state.empty() && undo != partialCommitUndo_.end() && state.preedit() == undo->second.preeditAfter) {
            ic->deleteSurroundingText(-static_cast<int>(utf8CodepointCount(undo->second.committedText)),
                                      static_cast<unsigned int>(utf8CodepointCount(undo->second.committedText)));
            action = state.restorePreedit(undo->second.preeditBefore, key.toString());
            partialCommitUndo_.erase(undo);
        } else {
            action = state.deleteKey();
        }
    } else {
        if (!key.check(FcitxKey_Delete)) {
            partialCommitUndo_.erase(ic);
        }
        if (modelRerankKey) {
            traceEngine("model-rerank-trigger key=" + traceSafe(key.toString()) +
                        " preedit=" + traceSafe(state.preedit()));
            auto [surroundingBefore, surroundingAfter] = surroundingContextFor(ic);
            const auto command = configuredModelCommand();
            auto externalRequest = command ? state.externalModelRequest(ic->program(), surroundingBefore,
                                                                        surroundingAfter, continuousMode_)
                                           : std::nullopt;
            if (command && externalRequest) {
                action = state.beginExternalModelRerank(true);
                state.armExternalModelRequest(*externalRequest);
                if (action.type == ActionType::Update) {
                    startAsyncModelRerank(ic, std::move(*externalRequest), *command);
                }
            } else {
                action = state.rerankCandidates(ic->program(), surroundingBefore, surroundingAfter, true, false,
                                                continuousMode_);
            }
            skipContinuousRerank = true;
        } else if (key.isUAZ()) {
            action = state.inputUppercaseAscii(static_cast<char>('A' + (sym - FcitxKey_A)));
        } else if (!key.hasModifier() && sym >= FcitxKey_a && sym <= FcitxKey_z) {
            action = state.inputAscii(static_cast<char>('a' + (sym - FcitxKey_a)));
        } else if (key.check(FcitxKey_BackSpace)) {
            action = state.backspace();
        } else if (key.check(FcitxKey_space)) {
            action = state.space();
        } else if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
            action = state.enter();
        } else if (key.check(FcitxKey_Escape)) {
            action = state.escape();
        } else if (!key.hasModifier() && isCommitBeforePunctuationKey(sym)) {
            if (const auto symbol = rawTokenSymbolForKey(sym); symbol) {
                action = state.inputRawTokenSymbol(*symbol);
            }
            if (action.type == ActionType::None) {
                action = state.punctuation(key.toString());
            }
        } else {
            const int index = key.digitSelection();
            if (state.empty()) {
                if (key.check(FcitxKey_BackSpace)) {
                    action = state.backspace();
                } else if (key.check(FcitxKey_space)) {
                    action = state.space();
                } else if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
                    action = state.enter();
                } else if (key.check(FcitxKey_Escape)) {
                    action = state.escape();
                } else if (isShiftTabKey(key)) {
                    action = state.cursorMove("ShiftTab");
                } else if (isTabKey(key)) {
                    action = state.cursorMove("Tab");
                } else if (key.isCursorMove()) {
                    action = state.cursorMove(key.toString());
                } else {
                    action = state.observeKey(key.toString());
                }
            } else if (index >= 0) {
                if (!key.hasModifier() && (sym >= FcitxKey_0 && sym <= FcitxKey_9)) {
                    action = state.inputAsciiDigit(static_cast<char>('0' + (sym - FcitxKey_0)));
                }
                if (action.type == ActionType::None) {
                    action = state.selectVisibleDigit(static_cast<std::size_t>(index), key.toString());
                }
            } else if (isDownKey(key)) {
                action = state.candidatesExpanded() ? state.moveCandidateCursor(candidateGridColumns, "Down")
                                                    : state.expandCandidates("Down");
            } else if (isUpKey(key)) {
                action = state.candidatesExpanded() ? state.moveCandidateCursor(-candidateGridColumns, "Up")
                                                    : state.cursorMove("Up");
            } else if (isPageDownKey(key)) {
                action = state.moveCandidateCursor(candidateGridColumns, "PageDown");
            } else if (isPageUpKey(key)) {
                action = state.moveCandidateCursor(-candidateGridColumns, "PageUp");
            } else if (isHomeKey(key)) {
                action = state.moveCandidateCursorTo(0, "Home");
            } else if (isEndKey(key)) {
                action = state.moveCandidateCursorTo(state.candidateCount() == 0 ? 0 : state.candidateCount() - 1, "End");
            } else if (isShiftTabKey(key)) {
                action = state.moveCandidateCursor(-1, "ShiftTab");
            } else if (isTabKey(key)) {
                action = state.moveCandidateCursor(1, "Tab");
            } else if (isRightKey(key)) {
                action = state.candidatesExpanded()
                             ? state.moveCandidateCursor(1, "Right")
                             : (!state.preeditCursorAtEnd() ? state.cursorMove("Right")
                                                            : state.moveCollapsedCandidateCursor(1, "Right"));
            } else if (isLeftKey(key)) {
                action = state.candidatesExpanded() ? state.moveCandidateCursor(-1, "Left")
                                                    : (state.candidateCursorIndex() > 0
                                                           ? state.moveCollapsedCandidateCursor(-1, "Left")
                                                           : state.cursorMove("Left"));
            } else if (key.isCursorMove()) {
                action = state.cursorMove(key.toString());
            } else {
                if (key.hasModifier() || (sym >= FcitxKey_F1 && sym <= FcitxKey_F35)) {
                    traceEngine("unhandled-composing-key key=" + traceSafe(key.toString()) +
                                " preedit=" + traceSafe(state.preedit()));
                }
                action = state.observeKey(key.toString());
            }
        }
    }

    if (!skipContinuousRerank && action.type == ActionType::Update) {
        maybeContinuousRerank(ic, state);
    }
    latencyGuard.markComputationComplete();
    applyAction(ic, &event, action);
    latencyGuard.markUIComplete();
    if (action.type == ActionType::None) {
        const auto current = states_.find(ic);
        if (current != states_.end() && hasSupervisionContent(current->second)) {
            writeSupervisionSnapshot(ic, current->second);
        }
    }
    if (hadPartialCommitResetProtection && !(action.type == ActionType::Commit && action.keepPreedit)) {
        preservePreeditUntilNextKey_.erase(ic);
    }
}

void Engine::activate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    const bool focusTransfer = focusTransferTimer_ != nullptr;
    focusTransferTimer_.reset();
    closeStatusWindow();
    refreshInputMode(false);
    if (!inputContextBlocksSupervision(ic)) {
        blockedInputContexts_.erase(ic);
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (englishMode_) {
        if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
            updateEnglishPendingPanel(ic, iter->second);
        } else if (!focusTransfer) {
            showInputModeStatus(ic, "Eng");
        }
        return;
    }
    traceEngineDebug(std::string("activate frontend=") + std::string(ic->frontendName()) +
                     " program=" + std::string(ic->program()) +
                     " preserved=" + std::to_string(preservedStates_.size()) +
                     " has-state=" + (states_.contains(ic) ? "1" : "0"));
    if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
        traceEngineDebug(std::string("activate existing-state preedit=") + traceSafe(iter->second.preedit()));
        updatePanel(ic, iter->second);
        return;
    }
    if (restorePreservedState(ic)) {
        return;
    }
    if (!focusTransfer) {
        showInputModeStatus(ic, "TiPE");
    }
}

void Engine::reset(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *ic = event.inputContext();
    if (ic) {
        ++asyncModelSerials_[ic];
    }
    if (ic && !inputContextBlocksSupervision(ic)) {
        blockedInputContexts_.erase(ic);
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (englishMode_) {
        finishPassThroughTracking(ic, "InputContextReset");
        if (ic) {
            if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
                updateEnglishPendingPanel(ic, iter->second);
            } else {
                clearPanel(ic);
            }
        }
        return;
    }
    if (ic && preservePreeditUntilNextKey_.contains(ic) && !stateFor(ic).empty()) {
        FCITX_INFO() << "TiPE preserved preedit across reset after partial commit";
        traceEngineDebug(std::string("reset preserved-partial preedit=") + traceSafe(stateFor(ic).preedit()));
        updatePanel(event.inputContext(), stateFor(event.inputContext()));
        return;
    }
    if (ic) {
        auto iter = states_.find(ic);
        if (iter != states_.end() && !iter->second.empty()) {
            preserveRestorableState(ic, iter->second, "soft-reset");
            FCITX_INFO() << "TiPE preserved active preedit across soft reset";
            traceEngineDebug(std::string("reset preserved-soft preedit=") + traceSafe(iter->second.preedit()));
            clearPanel(ic);
            return;
        }
    }
    if (activationStatusVisible(ic) && stateFor(ic).empty() && !inputPanelHasCompositionContent(ic)) {
        traceEngineDebug("reset ignored activation-status");
        return;
    }
    if (ic) {
        traceEngineDebug("reset clear-state");
    }
    clearState(ic);
}

void Engine::deactivate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    ++asyncModelSerials_[ic];
    if (!inputContextBlocksSupervision(ic)) {
        blockedInputContexts_.erase(ic);
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        scheduleFocusTransferSuppression();
        return;
    }
    traceEngineDebug(std::string("deactivate frontend=") + std::string(ic->frontendName()) +
                     " program=" + std::string(ic->program()) +
                     " has-state=" + (states_.contains(ic) ? "1" : "0"));
    cursorRects_.erase(ic);
    cursorFollowTrackers_.erase(ic);
    if (englishMode_) {
        finishPassThroughTracking(ic, "InputContextDeactivated");
    } else {
        passThroughSupervisors_.erase(ic);
    }
    if (auto iter = states_.find(ic); iter == states_.end() || iter->second.empty()) {
        partialCommitUndo_.erase(ic);
        lastContinuousPreedit_.erase(ic);
        preservePreeditUntilNextKey_.erase(ic);
        traceEngineDebug("deactivate empty-state");
    } else {
        preserveRestorableState(ic, iter->second, "deactivate");
        auto sessionContext = iter->second.sessionContext();
        if (!sessionContext.empty()) {
            sessionContexts_[ic] = std::move(sessionContext);
        }
        states_.erase(iter);
        partialCommitUndo_.erase(ic);
        lastContinuousPreedit_.erase(ic);
        preservePreeditUntilNextKey_.erase(ic);
        traceEngineDebug("deactivate dropped-active-state-after-preserve");
    }
    modelRerankHeld_.erase(ic);
    lastModelRerankUsec_.erase(ic);
    clearPanel(ic);
    scheduleFocusTransferSuppression();
}

State &Engine::stateFor(fcitx::InputContext *ic) {
    auto [iter, inserted] = states_.try_emplace(ic);
    if (inserted) {
        if (auto context = sessionContexts_.find(ic); context != sessionContexts_.end()) {
            iter->second.restoreSessionContext(std::move(context->second));
            sessionContexts_.erase(context);
        }
    }
    return iter->second;
}

void Engine::clearState(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    std::optional<State::SessionContext> sessionContext;
    if (auto iter = states_.find(ic); iter != states_.end()) {
        sessionContext = iter->second.sessionContext();
    }
    dropState(ic);
    if (sessionContext && !sessionContext->empty()) {
        sessionContexts_[ic] = std::move(*sessionContext);
    }
    clearPanel(ic);
}

void Engine::dropState(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    states_.erase(ic);
    sessionContexts_.erase(ic);
    passThroughSupervisors_.erase(ic);
    cursorRects_.erase(ic);
    cursorFollowTrackers_.erase(ic);
    partialCommitUndo_.erase(ic);
    lastContinuousPreedit_.erase(ic);
    activationStatusClearTimers_.erase(ic);
    activationStatusSerials_.erase(ic);
    activationStatusTexts_.erase(ic);
    preservePreeditUntilNextKey_.erase(ic);
    modelRerankHeld_.erase(ic);
    lastModelRerankUsec_.erase(ic);
    ++asyncModelSerials_[ic];
}

void Engine::preserveRestorableState(fcitx::InputContext *ic, const State &state, std::string_view reason) {
    if (!ic || state.empty() || inputContextBlocksSupervision(ic)) {
        return;
    }
    auto [surroundingBefore, surroundingAfter] = surroundingContextFor(ic);
    PreservedState preserved;
    preserved.state = state.restorableSnapshot();
    preserved.inputContext = reason == "destroyed" ? nullptr : ic;
    preserved.preedit = state.preedit();
    preserved.frontend = ic->frontendName();
    preserved.program = ic->program();
    preserved.display = ic->display();
    preserved.reason = reason;
    preserved.surroundingBefore = std::move(surroundingBefore);
    preserved.surroundingAfter = std::move(surroundingAfter);
    preserved.hasSurrounding = !preserved.surroundingBefore.empty() || !preserved.surroundingAfter.empty();
    preserved.savedAtUsec = monotonicUsec();
    std::erase_if(preservedStates_, [&](const PreservedState &existing) {
        const bool sameInputContext = existing.inputContext == preserved.inputContext;
        const bool sameExactContext = existing.frontend == preserved.frontend &&
                                      existing.program == preserved.program &&
                                      existing.display == preserved.display;
        const bool sameKnownProgram = !existing.program.empty() && !preserved.program.empty() &&
                                      existing.program == preserved.program;
        return sameInputContext || sameExactContext || sameKnownProgram;
    });
    preservedStates_.push_back(std::move(preserved));
    while (preservedStates_.size() > maxPreservedStates) {
        preservedStates_.erase(preservedStates_.begin());
    }
    const auto &saved = preservedStates_.back();
    traceEngine(std::string("preserve-state reason=") + std::string(reason) + " preedit=" +
                traceSafe(saved.preedit) + " cursor=" +
                std::to_string(saved.state ? saved.state->preeditCursor : saved.preedit.size()) +
                " expanded=" +
                (saved.state && saved.state->candidatesExpanded ? "1" : "0") +
                " inputContext=" + (saved.inputContext ? std::string("1") : std::string("0")) +
                " frontend=" + saved.frontend + " program=" + saved.program +
                " display=" + saved.display + " reason=" + saved.reason + " surrounding=" +
                (saved.hasSurrounding ? "1" : "0") + " preserved=" +
                std::to_string(preservedStates_.size()));
}

bool Engine::restorePreservedState(fcitx::InputContext *ic) {
    if (!ic || inputContextBlocksSupervision(ic) || preservedStates_.empty()) {
        return false;
    }
    const auto now = monotonicUsec();
    std::erase_if(preservedStates_, [&](const PreservedState &preserved) {
        return preserved.preedit.empty() || now < preserved.savedAtUsec ||
               now - preserved.savedAtUsec > preservedStateTtlUsec;
    });
    if (preservedStates_.empty()) {
        traceEngine("restore-state expired");
        return false;
    }

    const auto currentFrontend = std::string(ic->frontendName());
    const auto currentProgram = std::string(ic->program());
    const auto currentProgramLower = lowercaseAscii(currentProgram);
    const auto currentDisplay = std::string(ic->display());
    auto [surroundingBefore, surroundingAfter] = surroundingContextFor(ic);
    const bool currentHasSurrounding = !surroundingBefore.empty() || !surroundingAfter.empty();

    for (auto iter = preservedStates_.rbegin(); iter != preservedStates_.rend(); ++iter) {
        const bool sameInputContext = iter->inputContext == ic;
        const bool sameFrontend = iter->frontend.empty() || currentFrontend.empty() ||
                                  iter->frontend == currentFrontend;
        const bool sameProgram = iter->program.empty() || currentProgram.empty() ||
                                 lowercaseAscii(iter->program) == currentProgramLower;
        const bool sameDisplay = iter->display.empty() || currentDisplay.empty() ||
                                 iter->display == currentDisplay;
        const bool sameSurrounding = iter->hasSurrounding && currentHasSurrounding &&
                                     surroundingBefore == iter->surroundingBefore &&
                                     surroundingAfter == iter->surroundingAfter;
        const bool freshEnoughForMetadataFallback =
            now >= iter->savedAtUsec && now - iter->savedAtUsec <= preservedStateMetadataFallbackUsec;
        const bool metadataFallback = freshEnoughForMetadataFallback &&
                                      ((sameFrontend && sameDisplay) || sameSurrounding);
        if (!sameInputContext && !sameProgram && !metadataFallback) {
            continue;
        }
        if (sameInputContext && (!sameProgram || !sameFrontend || !sameDisplay)) {
            traceEngine(std::string("restore-state allowing-same-input-context saved=") +
                        traceSafe(iter->program) + " current=" + traceSafe(currentProgram) +
                        " frontend=" + (sameFrontend ? "same" : "changed") +
                        " display=" + (sameDisplay ? "same" : "changed"));
        }
        if (!sameProgram && metadataFallback) {
            traceEngine(std::string("restore-state allowing-program-change saved=") +
                        traceSafe(iter->program) + " current=" + traceSafe(currentProgram) +
                        " frontend=" + (sameFrontend ? "same" : "changed") +
                        " display=" + (sameDisplay ? "same" : "changed") +
                        " surrounding=" + (sameSurrounding ? "same" : "different-or-unavailable"));
        }
        if (!sameFrontend || !sameDisplay) {
            traceEngine(std::string("restore-state allowing metadata-change frontend=") +
                        (sameFrontend ? "same" : "changed") + " display=" +
                        (sameDisplay ? "same" : "changed"));
        }

        if (iter->hasSurrounding && currentHasSurrounding &&
            (surroundingBefore != iter->surroundingBefore || surroundingAfter != iter->surroundingAfter)) {
            traceEngine(std::string("restore-state allowing surrounding-change reason=") + iter->reason);
        }
        if (iter->hasSurrounding && !currentHasSurrounding) {
            traceEngine("restore-state surrounding-unavailable");
        }

        const auto preedit = iter->preedit;
        const auto preservedState = std::move(iter->state);
        preservedStates_.erase(std::next(iter).base());
        auto &state = stateFor(ic);
        if (preservedState) {
            state.restoreSnapshot(*preservedState);
        } else {
            state.restorePreedit(preedit, "restore-input-method-switch");
        }
        traceEngine(std::string("restore-state preedit=") + traceSafe(state.preedit()) +
                    " candidates=" + std::to_string(state.candidateCount()) +
                    " remaining=" + std::to_string(preservedStates_.size()));
        updatePanel(ic, state);
        return true;
    }

    traceEngine(std::string("restore-state rejected frontend=") + currentFrontend + " program=" +
                currentProgram + " display=" + currentDisplay);
    return false;
}

void Engine::applyAction(fcitx::InputContext *ic, fcitx::KeyEvent *event, const Action &action) {
    if (!ic) {
        return;
    }
    if (action.type == ActionType::None) {
        if (event && action.accepted) {
            event->filterAndAccept();
        }
        return;
    }

    if (action.accepted && event) {
        event->filterAndAccept();
    }

    const bool terminalAction = action.type == ActionType::Commit || action.type == ActionType::Clear;
    if (terminalAction) {
        writeCompletedSupervisionSnapshot(ic, stateFor(ic));
        cursorFollowTrackers_.erase(ic);
    }

    switch (action.type) {
    case ActionType::Update:
        updatePanel(ic, stateFor(ic));
        break;
    case ActionType::Commit:
        traceEngineDebug(std::string("commit text=") + action.commitText +
                         " keepPreedit=" + (action.keepPreedit ? std::string("1") : std::string("0")) +
                         " before=" + action.preeditBefore + " after=" + action.preeditAfter);
        if (debugEnabled() && !action.preeditBefore.empty()) {
            FCITX_INFO() << "TiPE commit selection text=" << action.commitText << " keepPreedit="
                         << action.keepPreedit << " preeditBefore=" << action.preeditBefore
                         << " preeditAfter=" << action.preeditAfter;
        }
        if (action.keepPreedit) {
            preservePreeditUntilNextKey_.insert(ic);
            partialCommitUndo_[ic] = {action.commitText, action.preeditBefore, action.preeditAfter};
            clearPanel(ic);
            ic->commitString(action.commitText);
            if (stateFor(ic).empty()) {
                preservePreeditUntilNextKey_.erase(ic);
                clearPanel(ic);
            } else {
                updatePanel(ic, stateFor(ic));
            }
        } else {
            partialCommitUndo_.erase(ic);
            preservePreeditUntilNextKey_.erase(ic);
            clearPanel(ic);
            ic->commitString(action.commitText);
            if (!stateFor(ic).empty()) {
                updatePanel(ic, stateFor(ic));
            }
        }
        break;
    case ActionType::Clear:
        clearPanel(ic);
        break;
    case ActionType::None:
        break;
    }
    if (!action.passthroughText.empty()) {
        ic->commitString(action.passthroughText);
    }
    if (terminalAction) {
        stateFor(ic).clearCompletedSupervisionRequest();
    }
}

void Engine::selectCandidateFromUI(fcitx::InputContext *ic, std::size_t index,
                                   std::string_view expectedCandidate) {
    if (!ic || englishMode_ || inputContextBlocksSupervision(ic)) {
        return;
    }
    const auto iter = states_.find(ic);
    if (iter == states_.end() || iter->second.empty() || index >= iter->second.candidateCount() ||
        iter->second.candidates()[index] != expectedCandidate) {
        traceEngineDebug("candidate-click ignored-stale index=" + std::to_string(index));
        return;
    }
    ++asyncModelSerials_[ic];
    traceEngineDebug("candidate-click index=" + std::to_string(index));
    applyAction(ic, nullptr, iter->second.select(index));
}

void Engine::showInputModeStatus(fcitx::InputContext *ic, const char *status) {
    if (!ic || !status || inputContextBlocksSupervision(ic)) {
        return;
    }
    activationStatusTexts_[ic] = status;
    scheduleActivationStatusClear(ic);
    auto &panel = ic->inputPanel();
    panel.reset();
    const bool useTipeUI = tipeUIHandlesInputContext(instance_, *ic);
    configureTipeInputPanelRoute(instance_, ic, useTipeUI);
    if (useTipeUI) {
        panel.setAuxUp(fcitx::Text(status));
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    clearCandidateWindow();
    if (!useTipeUI) {
        showStatusWindow(*ic, status);
    } else {
        closeStatusWindow();
    }
    traceEngineDebug(std::string("status-panel-show status=") + status);
}

void Engine::scheduleFocusTransferSuppression() {
    if (!instance_) {
        return;
    }
    focusTransferTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + focusTransferDelayUsec,
        focusTransferAccuracyUsec,
        [this](fcitx::EventSourceTime *, uint64_t) {
            focusTransferTimer_.reset();
            return false;
        });
    if (focusTransferTimer_) {
        focusTransferTimer_->setOneShot();
    }
}

void Engine::clearActivationStatusState(fcitx::InputContext *ic) {
    if (!ic) {
        return;
    }
    activationStatusClearTimers_.erase(ic);
    activationStatusSerials_.erase(ic);
    activationStatusTexts_.erase(ic);
}

void Engine::scheduleActivationStatusClear(fcitx::InputContext *ic) {
    if (!instance_ || !ic) {
        return;
    }
    const auto serial = ++activationStatusSerials_[ic];
    const auto inputContext = ic->watch();
    activationStatusClearTimers_[ic] = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + activationStatusVisibleUsec,
        activationStatusTimerAccuracyUsec,
        [this, inputContext, serial](fcitx::EventSourceTime *, uint64_t) {
            if (auto *ic = inputContext.get()) {
                clearActivationStatusIfCurrent(ic, serial);
            }
            return false;
        });
    if (auto iter = activationStatusClearTimers_.find(ic);
        iter != activationStatusClearTimers_.end() && iter->second) {
        iter->second->setOneShot();
    }
}

void Engine::clearActivationStatusIfCurrent(fcitx::InputContext *ic, std::size_t serial) {
    if (!ic) {
        return;
    }
    const auto serialIter = activationStatusSerials_.find(ic);
    if (serialIter == activationStatusSerials_.end() || serialIter->second != serial) {
        return;
    }
    auto &panel = ic->inputPanel();
    const auto statusText = panel.auxUp().toString();
    const auto trackedStatus = activationStatusText(ic);
    clearActivationStatusState(ic);
    if (isActivationStatusText(statusText) || isActivationStatusText(trackedStatus)) {
        if (auto candidates = panel.candidateList(); candidates && candidates->size() > 0) {
            return;
        }
        if (!englishMode_) {
            if (auto stateIter = states_.find(ic); stateIter != states_.end() && !stateIter->second.empty()) {
                return;
            }
        }
        panel.reset();
        configureTipeInputPanelRoute(instance_, ic, tipeUIHandlesInputContext(instance_, *ic));
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        closeStatusWindow();
    }
}

bool Engine::activationStatusVisible(fcitx::InputContext *ic) const {
    return ic && activationStatusSerials_.contains(ic) && activationStatusTexts_.contains(ic);
}

std::string Engine::activationStatusText(fcitx::InputContext *ic) const {
    if (!activationStatusVisible(ic)) {
        return {};
    }
    return activationStatusTexts_.at(ic);
}

void Engine::updateEnglishPendingPanel(fcitx::InputContext *ic, const State &state) {
    if (!ic || state.empty() || inputContextBlocksSupervision(ic)) {
        if (ic) {
            clearPanel(ic);
        }
        return;
    }
    clearActivationStatusState(ic);
    auto preedit = fcitx::Text(state.preedit());
    preedit.setCursor(static_cast<int>(state.preeditCursorIndex()));
    auto &panel = ic->inputPanel();
    panel.reset();
    panel.setClientPreedit(preedit);
    panel.setPreedit(preedit);
    const bool useTipeUI = tipeUIHandlesInputContext(instance_, *ic);
    configureTipeInputPanelRoute(instance_, ic, useTipeUI);
    if (useTipeUI) {
        panel.setAuxUp(fcitx::Text("Eng"));
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    clearCandidateWindow();
    writeSupervisionSnapshot(ic, state);
    if (useTipeUI) {
        closeStatusWindow();
    } else {
        showStatusWindow(*ic, "Eng");
    }
    traceEngineDebug("input-mode english pending-preedit=" + traceSafe(state.preedit()));
}

void Engine::updatePanel(fcitx::InputContext *ic, const State &state) {
    if (!ic || inputContextBlocksSupervision(ic)) {
        if (ic) {
            clearPanel(ic);
        }
        return;
    }
    auto &panel = ic->inputPanel();
    auto preedit = fcitx::Text(state.preedit());
    preedit.setCursor(static_cast<int>(state.preeditCursorIndex()));
    panel.reset();
    panel.setClientPreedit(preedit);
    const bool useTipeUI = tipeUIHandlesInputContext(instance_, *ic);
    configureTipeInputPanelRoute(instance_, ic, useTipeUI);
    const auto rect = cursorRectFor(*ic);
    const bool useEdgeFallback = !useTipeUI && waylandPopupEdgeFallbackNeeded(rect, state);
    const bool useNativePanel = !useTipeUI && nativeFollowFallbackEnabled() && !isUsableRect(rect);
    traceCandidateUpdate(state, useTipeUI ? "tipeui" : (useNativePanel ? "native" : "gtk-fallback"));
    if (useEdgeFallback && !state.candidates().empty()) {
        traceEngine(std::string("wayland-popup-edge-fallback rect=") + std::to_string(rect.left()) + "," +
                    std::to_string(rect.top()) + "," + std::to_string(rect.width()) + "," +
                    std::to_string(rect.height()) + " candidates=" + std::to_string(state.candidates().size()));
    }
    if (useTipeUI && !state.candidates().empty()) {
        panel.setPreedit(preedit);
        panel.setAuxUp(fcitx::Text(tipeUIStateAux(state, continuousMode_)));
        auto candidateList = std::make_unique<TipeCandidateList>();
        for (std::size_t index = 0; index < state.candidates().size(); ++index) {
            const auto candidate = state.candidates()[index];
            candidateList->append(candidate, [this, index, candidate](fcitx::InputContext *inputContext) {
                selectCandidateFromUI(inputContext, index, candidate);
            });
        }
        candidateList->setCursorIndex(static_cast<int>(state.candidateCursorIndex()));
        panel.setCandidateList(std::move(candidateList));
    } else if (useNativePanel && !state.candidates().empty()) {
        std::vector<std::string> nativeCandidates;
        std::vector<std::size_t> nativeCandidateIndices;
        std::size_t nativeCursorIndex = 0;
        if (state.candidatesExpanded()) {
            const auto cells = visualCandidateCells(state.candidates());
            const auto selectedCell = visualCellForIndex(cells, state.candidateCursorIndex());
            const auto rowCells = cellsInVisualRow(cells, selectedCell ? selectedCell->row : 0);
            nativeCandidates.reserve(rowCells.size());
            nativeCandidateIndices.reserve(rowCells.size());
            for (std::size_t index = 0; index < rowCells.size(); ++index) {
                nativeCandidates.push_back(state.candidates()[rowCells[index].index]);
                nativeCandidateIndices.push_back(rowCells[index].index);
                if (rowCells[index].index == state.candidateCursorIndex()) {
                    nativeCursorIndex = index;
                }
            }
        } else {
            nativeCandidates = state.visibleCandidates();
            const auto visibleCells = collapsedVisualCandidateCells(state.candidates());
            nativeCandidateIndices.reserve(visibleCells.size());
            for (const auto &cell : visibleCells) {
                nativeCandidateIndices.push_back(cell.index);
            }
            nativeCursorIndex = std::min(state.candidateCursorIndex(), nativeCandidates.empty() ? 0 : nativeCandidates.size() - 1);
        }

        auto candidateList = std::make_unique<TipeCandidateList>();
        for (std::size_t index = 0; index < nativeCandidates.size(); ++index) {
            const auto candidate = nativeCandidates[index];
            const auto stateIndex = nativeCandidateIndices[index];
            candidateList->append(candidate, [this, stateIndex, candidate](fcitx::InputContext *inputContext) {
                selectCandidateFromUI(inputContext, stateIndex, candidate);
            });
        }
        candidateList->setCursorIndex(static_cast<int>(nativeCursorIndex));
        panel.setCandidateList(std::move(candidateList));
    } else {
        panel.setCandidateList(nullptr);
    }

    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    writeSupervisionSnapshot(ic, state);
    if (useTipeUI || useNativePanel) {
        clearCandidateWindow();
    } else {
        updateCandidateWindow(*ic, state);
        scheduleDeferredCandidateWindowUpdate(ic);
    }
}

void Engine::maybeContinuousRerank(fcitx::InputContext *ic, State &state) {
    if (!ic || state.empty() || !continuousMode_ || inputContextBlocksSupervision(ic)) {
        return;
    }
    const auto currentPreedit = state.preedit();
    if (lastContinuousPreedit_[ic] == currentPreedit) {
        return;
    }
    lastContinuousPreedit_[ic] = currentPreedit;
    auto [surroundingBefore, surroundingAfter] = surroundingContextFor(ic);
    state.rerankCandidates(ic->program(), std::move(surroundingBefore), std::move(surroundingAfter), false, false,
                           true);
}

void Engine::clearPanel(fcitx::InputContext *ic) {
    auto &panel = ic->inputPanel();
    panel.reset();
    configureTipeInputPanelRoute(instance_, ic, tipeUIHandlesInputContext(instance_, *ic));
    activationStatusClearTimers_.erase(ic);
    activationStatusSerials_.erase(ic);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    clearSupervisionSnapshot(ic);
    clearCandidateWindow();
    closeStatusWindow();
}

void Engine::writeSupervisionSnapshot(fcitx::InputContext *ic, const State &state) {
    if (!ic || inputContextBlocksSupervision(ic) || !hasSupervisionContent(state)) {
        clearSupervisionSnapshot(ic);
        return;
    }
    PendingSupervisionSnapshot pending;
    pending.source = SupervisionSnapshotSource::ActiveState;
    pending.owner = ic;
    pending.program = ic->program();
    pendingSupervisionSnapshot_ = std::move(pending);
    supervisionSnapshotOwner_ = ic;
    scheduleSupervisionSnapshotFlush();
}

void Engine::writeCompletedSupervisionSnapshot(fcitx::InputContext *ic, const State &state) {
    if (!ic || inputContextBlocksSupervision(ic)) {
        return;
    }
    auto [surroundingBefore, surroundingAfter] = surroundingContextFor(ic);
    const auto completed = state.completedSupervisionRequest(ic->program(), std::move(surroundingBefore),
                                                             std::move(surroundingAfter), continuousMode_);
    if (!completed) {
        return;
    }
    persistSupervisionSnapshot(ic, completed->preedit, completed->candidateCount,
                               completed->candidatesExpanded, completed->payload, true);
}

void Engine::writePassThroughSupervisionSnapshot(
    fcitx::InputContext *ic, const PassThroughSupervisionSnapshot &snapshot) {
    if (!ic || inputContextBlocksSupervision(ic) || snapshot.payload.empty()) {
        return;
    }
    persistSupervisionSnapshot(ic, snapshot.token, snapshot.candidateCount, false, snapshot.payload,
                               snapshot.terminal);
}

void Engine::schedulePassThroughSupervisionSnapshot(fcitx::InputContext *ic) {
    if (!ic || inputContextBlocksSupervision(ic)) {
        return;
    }
    PendingSupervisionSnapshot pending;
    pending.source = SupervisionSnapshotSource::PassThrough;
    pending.owner = ic;
    pending.program = ic->program();
    pendingSupervisionSnapshot_ = std::move(pending);
    supervisionSnapshotOwner_ = ic;
    scheduleSupervisionSnapshotFlush();
}

void Engine::persistSupervisionSnapshot(fcitx::InputContext *ic, std::string_view preedit,
                                        std::size_t candidateCount, bool candidatesExpanded,
                                        std::string_view snapshot, bool terminal) {
    if (!ic || inputContextBlocksSupervision(ic) || snapshot.empty()) {
        return;
    }

    PendingSupervisionSnapshot pending;
    pending.owner = ic;
    pending.program = ic->program();
    pending.preedit = preedit;
    pending.candidateCount = candidateCount;
    pending.candidatesExpanded = candidatesExpanded;
    pending.payload = snapshot;
    pending.terminal = terminal;
    supervisionSnapshotOwner_ = ic;
    if (!terminal) {
        pendingSupervisionSnapshot_ = std::move(pending);
        scheduleSupervisionSnapshotFlush();
        return;
    }

    supervisionSnapshotFlushTimer_.reset();
    pendingSupervisionSnapshot_.reset();
    persistSupervisionSnapshotNow(pending);
}

void Engine::scheduleSupervisionSnapshotFlush() {
    if (supervisionSnapshotFlushTimer_ || !pendingSupervisionSnapshot_) {
        return;
    }
    if (!instance_) {
        flushPendingSupervisionSnapshot();
        return;
    }
    supervisionSnapshotFlushTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + supervisionSnapshotFlushIntervalUsec,
        supervisionSnapshotFlushAccuracyUsec,
        [this](fcitx::EventSourceTime *, uint64_t) {
            supervisionSnapshotFlushTimer_.reset();
            flushPendingSupervisionSnapshot();
            return false;
        });
    if (supervisionSnapshotFlushTimer_) {
        supervisionSnapshotFlushTimer_->setOneShot();
    } else {
        flushPendingSupervisionSnapshot();
    }
}

void Engine::flushPendingSupervisionSnapshot() {
    if (!pendingSupervisionSnapshot_) {
        return;
    }
    auto pending = std::move(*pendingSupervisionSnapshot_);
    pendingSupervisionSnapshot_.reset();
    if (inputContextBlocksSupervision(pending.owner)) {
        if (supervisionSnapshotOwner_ == pending.owner) {
            clearSupervisionSnapshot(pending.owner);
        }
        return;
    }
    if (pending.source == SupervisionSnapshotSource::ActiveState) {
        const auto iter = states_.find(pending.owner);
        if (!pending.owner || iter == states_.end() || !hasSupervisionContent(iter->second)) {
            return;
        }
        auto [surroundingBefore, surroundingAfter] = surroundingContextFor(pending.owner);
        pending.preedit = iter->second.preedit();
        pending.candidateCount = iter->second.candidateCount();
        pending.candidatesExpanded = iter->second.candidatesExpanded();
        pending.payload = iter->second.modelRequestSnapshot(pending.program, std::move(surroundingBefore),
                                                            std::move(surroundingAfter), continuousMode_);
    } else if (pending.source == SupervisionSnapshotSource::PassThrough) {
        const auto iter = passThroughSupervisors_.find(pending.owner);
        if (!pending.owner || iter == passThroughSupervisors_.end()) {
            return;
        }
        const auto snapshot = iter->second.snapshot(pending.program, continuousMode_);
        pending.preedit = snapshot.token;
        pending.candidateCount = snapshot.candidateCount;
        pending.payload = snapshot.payload;
    }
    persistSupervisionSnapshotNow(pending);
}

void Engine::persistSupervisionSnapshotNow(const PendingSupervisionSnapshot &pending) {
    if (inputContextBlocksSupervision(pending.owner)) {
        return;
    }
    const auto path = supervisionSnapshotPath();
    if (path.empty() || pending.payload.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto persistentSnapshot = persistentSupervisionSnapshot(pending.payload);
    if (pending.terminal) {
        std::filesystem::remove(path, error);
        supervisionSnapshotOwner_ = nullptr;
        const auto lastPath = supervisionLastSnapshotPath();
        if (!lastPath.empty()) {
            writeSupervisionSnapshotAtomically(lastPath, persistentSnapshot);
        }
        appendSupervisionHistory(pending.program, pending.preedit, pending.candidateCount,
                                 pending.candidatesExpanded, persistentSnapshot, true);
        return;
    }
    if (!writeSupervisionSnapshotAtomically(path, pending.payload)) {
        return;
    }
    supervisionSnapshotOwner_ = pending.owner;
    appendSupervisionHistory(pending.program, pending.preedit, pending.candidateCount,
                             pending.candidatesExpanded, persistentSnapshot, false);
}

void Engine::appendSupervisionHistory(std::string_view program, std::string_view preedit,
                                      std::size_t candidateCount, bool candidatesExpanded,
                                      std::string_view snapshot, bool terminal) {
    const auto path = supervisionHistoryPath();
    if (path.empty() || snapshot.empty()) {
        return;
    }

    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    std::ostringstream header;
    header << "---\tunix_ms\t" << (static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000)
           << "\tprogram\t" << historyField(program)
           << "\tpreedit\t" << historyField(preedit)
           << "\tcandidates\t" << candidateCount
           << "\texpanded\t" << (candidatesExpanded ? 1 : 0)
           << "\tterminal\t" << (terminal ? 1 : 0);

    const auto monotonicNow = monotonicUsec();
    const bool intervalElapsed = lastSupervisionHistoryUsec_ == 0 || monotonicNow < lastSupervisionHistoryUsec_ ||
                                 monotonicNow - lastSupervisionHistoryUsec_ >= supervisionHistoryIntervalUsec;
    if ((terminal || intervalElapsed) && lastSupervisionHistorySnapshot_ != snapshot &&
        appendBoundedSupervisionHistory(path, header.str(), snapshot, supervisionHistoryMaxBytes)) {
        lastSupervisionHistorySnapshot_ = snapshot;
        lastSupervisionHistoryUsec_ = monotonicNow;
    }
    if (terminal) {
        appendBoundedSupervisionHistory(supervisionTrainingHistoryPath(), header.str(), snapshot,
                                        supervisionTrainingHistoryMaxBytes);
    }
}

void Engine::clearSupervisionSnapshot(fcitx::InputContext *owner) {
    if (owner && supervisionSnapshotOwner_ && owner != supervisionSnapshotOwner_) {
        traceEngine("supervision-clear ignored-non-owner");
        return;
    }
    const auto path = supervisionSnapshotPath();
    supervisionSnapshotFlushTimer_.reset();
    pendingSupervisionSnapshot_.reset();
    lastSupervisionHistorySnapshot_.clear();
    supervisionSnapshotOwner_ = nullptr;
    if (path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}

void Engine::rememberCursorRect(fcitx::InputContext *ic) {
    if (!ic || inputContextBlocksSupervision(ic)) {
        return;
    }
    const auto &rect = ic->cursorRect();
    if (isUsableRect(rect)) {
        cursorRects_[ic] = rect;
        lastCursorRect_ = LastCursorRect{rect, std::string(ic->frontendName()), std::string(ic->program()),
                                         std::string(ic->display()), monotonicUsec()};
        if (debugEnabled()) {
            FCITX_INFO() << "TiPE remembered cursor rect x=" << rect.left() << " y=" << rect.top()
                         << " w=" << rect.width() << " h=" << rect.height();
        }
    }
}

Engine::CursorRectSnapshot Engine::cursorRectSnapshotFor(fcitx::InputContext &ic) {
    const auto &rect = ic.cursorRect();
    if (isUsableRect(rect)) {
        cursorRects_[&ic] = rect;
        lastCursorRect_ = LastCursorRect{rect, std::string(ic.frontendName()), std::string(ic.program()),
                                         std::string(ic.display()), monotonicUsec()};
        return {rect, "live"};
    }
    if (auto iter = cursorRects_.find(&ic); iter != cursorRects_.end()) {
        return {iter->second, "cache"};
    }
    if (lastCursorRect_) {
        const auto now = monotonicUsec();
        const auto frontend = std::string(ic.frontendName());
        const auto program = std::string(ic.program());
        const auto display = std::string(ic.display());
        const bool fresh = now >= lastCursorRect_->savedAtUsec &&
                           now - lastCursorRect_->savedAtUsec <= cursorRectFallbackTtlUsec;
        const bool sameDisplay = lastCursorRect_->display.empty() || display.empty() ||
                                 lastCursorRect_->display == display;
        const bool sameFrontend = lastCursorRect_->frontend.empty() || frontend.empty() ||
                                  lastCursorRect_->frontend == frontend;
        const bool sameProgram = lastCursorRect_->program.empty() || program.empty() ||
                                 lastCursorRect_->program == program;
        if (fresh && sameDisplay && sameFrontend && sameProgram && isUsableRect(lastCursorRect_->rect)) {
            return {lastCursorRect_->rect, "recent"};
        }
    }
    return {rect, "invalid"};
}

fcitx::Rect Engine::cursorRectFor(fcitx::InputContext &ic) {
    return cursorRectSnapshotFor(ic).rect;
}

void Engine::onCursorRectChanged(fcitx::Event &event) {
    auto *ic = static_cast<fcitx::InputContextEvent &>(event).inputContext();
    if (!ic) {
        return;
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (debugEnabled()) {
        const auto &rect = ic->cursorRect();
        FCITX_INFO() << "TiPE cursor rect changed frontend=" << ic->frontendName() << " program=" << ic->program()
                     << " display=" << ic->display() << " scale=" << ic->scaleFactor() << " rect=" << rect.left()
                     << ',' << rect.top() << ',' << rect.width() << ',' << rect.height();
    }
    rememberCursorRect(ic);
    if (activationStatusVisible(ic) && !tipeUIHandlesInputContext(instance_, *ic)) {
        const auto status = activationStatusText(ic);
        if (!status.empty()) {
            showStatusWindow(*ic, status.c_str());
        }
    }
    if (englishMode_) {
        return;
    }
    const auto iter = states_.find(ic);
    if (iter == states_.end() || iter->second.empty()) {
        return;
    }
    const auto rectSnapshot = cursorRectSnapshotFor(*ic);
    const auto rect = rectSnapshot.rect;
    if (tipeUIHandlesInputContext(instance_, *ic) || (nativeFollowFallbackEnabled() && !isUsableRect(rect))) {
        traceEngineDebug(std::string("cursor-rect-update source=") + rectSnapshot.source + " rect=" +
                         std::to_string(rect.left()) + "," + std::to_string(rect.top()) + "," +
                         std::to_string(rect.width()) + "," + std::to_string(rect.height()) +
                         " panel=fcitx");
        updatePanel(ic, iter->second);
        return;
    }
    traceEngineDebug(std::string("cursor-rect-update source=") + rectSnapshot.source + " rect=" +
                     std::to_string(rect.left()) + "," + std::to_string(rect.top()) + "," +
                     std::to_string(rect.width()) + "," + std::to_string(rect.height()) +
                     " panel=fallback-window");
    updateCandidateWindow(*ic, iter->second);
}

void Engine::onInputContextFocusOut(fcitx::Event &event) {
    auto *ic = static_cast<fcitx::InputContextEvent &>(event).inputContext();
    if (ic) {
        ++asyncModelSerials_[ic];
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        return;
    }
    if (englishMode_) {
        finishPassThroughTracking(ic, "InputContextFocusOut");
    }
}

void Engine::onInputContextDestroyed(fcitx::Event &event) {
    auto *ic = static_cast<fcitx::InputContextEvent &>(event).inputContext();
    if (!ic) {
        return;
    }
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
        asyncModelSerials_.erase(ic);
        blockedInputContexts_.erase(ic);
        deferredCandidateWindowUpdate_.reset();
        return;
    }
    if (englishMode_) {
        finishPassThroughTracking(ic, "InputContextDestroyed");
    }
    if (auto iter = states_.find(ic); iter != states_.end() && !iter->second.empty()) {
        preserveRestorableState(ic, iter->second, "destroyed");
    }
    for (auto &preserved : preservedStates_) {
        if (preserved.inputContext == ic) {
            preserved.inputContext = nullptr;
        }
    }
    dropState(ic);
    asyncModelSerials_.erase(ic);
    blockedInputContexts_.erase(ic);
    deferredCandidateWindowUpdate_.reset();
    clearSupervisionSnapshot(ic);
    clearCandidateWindow();
    closeStatusWindow();
    if (debugEnabled()) {
        FCITX_INFO() << "TiPE dropped state for destroyed input context";
    }
}

void Engine::onInputContextCapabilityChanged(fcitx::Event &event) {
    auto *ic = static_cast<fcitx::InputContextEvent &>(event).inputContext();
    if (inputContextBlocksSupervision(ic)) {
        discardBlockedInputContext(ic);
    } else if (ic) {
        blockedInputContexts_.erase(ic);
    }
}

void Engine::scheduleDeferredCandidateWindowUpdate(fcitx::InputContext *ic) {
    if (!instance_ || !ic) {
        return;
    }
    const auto inputContext = ic->watch();
    deferredCandidateWindowUpdate_ = instance_->eventLoop().addDeferEvent([this, inputContext](fcitx::EventSource *) {
        auto *ic = inputContext.get();
        if (!ic) {
            return false;
        }
        if (englishMode_ || inputContextBlocksSupervision(ic)) {
            return false;
        }
        const auto iter = states_.find(ic);
        if (iter != states_.end() && !iter->second.empty()) {
            if (debugEnabled()) {
                const auto &rect = ic->cursorRect();
                FCITX_INFO() << "TiPE deferred candidate snapshot frontend=" << ic->frontendName()
                             << " program=" << ic->program() << " display=" << ic->display()
                             << " scale=" << ic->scaleFactor() << " rect=" << rect.left() << ',' << rect.top()
                             << ',' << rect.width() << ',' << rect.height();
            }
            updateCandidateWindow(*ic, iter->second);
        }
        return false;
    });
    if (deferredCandidateWindowUpdate_) {
        deferredCandidateWindowUpdate_->setOneShot();
    }
}

bool Engine::ensureCandidateWindow() {
    if (candidateWindowFd_ >= 0) {
        return true;
    }

    const char *home = std::getenv("HOME");
    if (!home) {
        return false;
    }
    const auto homePath = std::filesystem::path(home);
    const auto logDir = tipeCacheDir();
    if (logDir.empty()) {
        return false;
    }
    const auto logPath = logDir / "candidate-window.log";
    const auto binaryPath = homePath / ".local" / "bin" / "tipe-candidate-window";
    std::error_code error;
    std::filesystem::create_directories(logDir, error);
    trimDiagnosticLogFile(logPath);

    int pipeFds[2]{};
    if (pipe(pipeFds) != 0) {
        FCITX_WARN() << "Failed to create candidate-window pipe";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        FCITX_WARN() << "Failed to fork tipe-candidate-window";
        return false;
    }
    if (pid == 0) {
        close(pipeFds[1]);
        dup2(pipeFds[0], STDIN_FILENO);
        close(pipeFds[0]);
        const int logFd = openPrivateAppendFile(logPath);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }
        execl(binaryPath.c_str(), binaryPath.c_str(), "--stdin", static_cast<char *>(nullptr));
        _exit(127);
    }

    close(pipeFds[0]);
    candidateWindowPid_ = pid;
    if (!setFileDescriptorNonblocking(pipeFds[1])) {
        close(pipeFds[1]);
        closeCandidateWindow();
        FCITX_WARN() << "Failed to start tipe-candidate-window";
        return false;
    }
    candidateWindowFd_ = pipeFds[1];
    return true;
}

void Engine::closeCandidateWindow() {
    if (candidateWindowFd_ >= 0) {
        close(candidateWindowFd_);
        candidateWindowFd_ = -1;
    }
    if (candidateWindowPid_ > 0) {
        for (int attempt = 0; attempt < 10 && !reapProcess(candidateWindowPid_); ++attempt) {
            usleep(10000);
        }
        if (!reapProcess(candidateWindowPid_)) {
            kill(candidateWindowPid_, SIGTERM);
            for (int attempt = 0; attempt < 10 && !reapProcess(candidateWindowPid_); ++attempt) {
                usleep(10000);
            }
        }
        if (!reapProcess(candidateWindowPid_)) {
            kill(candidateWindowPid_, SIGKILL);
            while (!reapProcess(candidateWindowPid_)) {
                usleep(10000);
            }
        }
        candidateWindowPid_ = -1;
    }
}

void Engine::showStatusWindow(fcitx::InputContext &ic, const char *status) {
    if (!status || inputContextBlocksSupervision(&ic) || !statusWindowFallbackEnabled()) {
        closeStatusWindow();
        return;
    }
    const char *home = std::getenv("HOME");
    if (!home) {
        traceEngine(std::string("status-window-no-home status=") + status);
        return;
    }
    const auto logDir = tipeCacheDir();
    if (logDir.empty()) {
        traceEngine(std::string("status-window-no-cache-dir status=") + status);
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(logDir, error);
    const auto rectSnapshot = cursorRectSnapshotFor(ic);
    if (!isUsableRect(rectSnapshot.rect)) {
        traceEngine(std::string("status-window-skip-invalid-snapshot source=") + rectSnapshot.source +
                    " status=" + status);
        closeStatusWindow();
        return;
    }
    closeStatusWindow();
    const auto binaryPath = std::filesystem::path(home) / ".local" / "bin" / "tipe-candidate-window";
    const auto logPath = logDir / "candidate-window.log";
    trimDiagnosticLogFile(logPath);
    const auto fallbackRect = fallbackCursorRectFor(ic, rectSnapshot.rect);
    const auto cursor = std::to_string(fallbackRect.x) + "," + std::to_string(fallbackRect.y) + "," +
                        std::to_string(fallbackRect.width) + "," + std::to_string(fallbackRect.height);
    const pid_t pid = fork();
    if (pid < 0) {
        traceEngine(std::string("status-window-fork-failed status=") + status);
        return;
    }
    if (pid == 0) {
        const int logFd = openPrivateAppendFile(logPath);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }
        execl(binaryPath.c_str(), binaryPath.c_str(), "--status", status, "--cursor", cursor.c_str(),
              static_cast<char *>(nullptr));
        _exit(127);
    }
    statusWindowPid_ = pid;
    traceEngine(std::string("status-window-fallback source=") + rectSnapshot.source + " rect=" + cursor +
                " status=" + status + " frontend=" + std::string(ic.frontendName()) +
                " program=" + std::string(ic.program()) + " pid=" + std::to_string(pid));
}

void Engine::closeStatusWindow() {
    if (statusWindowPid_ <= 0) {
        return;
    }
    if (!reapProcess(statusWindowPid_)) {
        kill(statusWindowPid_, SIGTERM);
        for (int attempt = 0; attempt < 10 && !reapProcess(statusWindowPid_); ++attempt) {
            usleep(10000);
        }
    }
    if (!reapProcess(statusWindowPid_)) {
        kill(statusWindowPid_, SIGKILL);
        while (!reapProcess(statusWindowPid_)) {
            usleep(10000);
        }
    }
    statusWindowPid_ = -1;
}

void Engine::updateCandidateWindow(fcitx::InputContext &ic, const State &state) {
    if (inputContextBlocksSupervision(&ic)) {
        clearCandidateWindow();
        return;
    }
    rememberCursorRect(&ic);
    const auto rectSnapshot = cursorRectSnapshotFor(ic);
    const auto rect = rectSnapshot.rect;
    traceEngineDebug(std::string("candidate-snapshot rectSource=") + rectSnapshot.source +
                     " frontend=" + std::string(ic.frontendName()) + " program=" + std::string(ic.program()) +
                     " display=" + std::string(ic.display()) + " scale=" + std::to_string(ic.scaleFactor()) +
                     " rect=" + std::to_string(rect.left()) + "," + std::to_string(rect.top()) + "," +
                     std::to_string(rect.width()) + "," + std::to_string(rect.height()) +
                     " candidates=" + std::to_string(state.candidates().size()) +
                     " expanded=" + (state.candidatesExpanded() ? "1" : "0"));
    if (debugEnabled()) {
        FCITX_INFO() << "TiPE candidate snapshot frontend=" << ic.frontendName() << " program=" << ic.program()
                     << " display=" << ic.display() << " scale=" << ic.scaleFactor() << " rectSource="
                     << rectSnapshot.source << " rect=" << rect.left() << ',' << rect.top() << ',' << rect.width()
                     << ',' << rect.height();
    }
    const auto counts = state.debugSnapshot().eventCounts;
    const auto supervisedKeys = counts.letters + counts.digits + counts.symbols + counts.backspaces + counts.deletes +
                                counts.spaces + counts.enters + counts.escapes + counts.observedKeys +
                                counts.cursorMoves;
    const CandidateSnapshotRect rawFallbackRect{rect.left(), rect.top(), rect.width(), rect.height()};
    const bool staticCursorRect = cursorFollowTrackers_[&ic].observe(
        rawFallbackRect, state.preedit(), state.preeditCursorIndex());
    const auto metadata = std::string("supervision=1,keys=") + std::to_string(supervisedKeys) +
                          ",selects=" + std::to_string(counts.candidateSelections) +
                          ",reranks=" + std::to_string(counts.rerankRequests) +
                          ",continuous=" + (continuousMode_ ? "1" : "0") +
                          ",preedit_cursor=" + std::to_string(state.preeditCursorIndex()) +
                          ",cursor_static=" + (staticCursorRect ? "1" : "0");
    const auto fallbackRect = fallbackCursorRectFor(ic, rect);
    traceEngineDebug(std::string("candidate-fallback-rect raw=") + std::to_string(rect.left()) + "," +
                     std::to_string(rect.top()) + "," + std::to_string(rect.width()) + "," +
                     std::to_string(rect.height()) + " logical=" + std::to_string(fallbackRect.x) + "," +
                     std::to_string(fallbackRect.y) + "," + std::to_string(fallbackRect.width) + "," +
                     std::to_string(fallbackRect.height));
    const auto payload =
        buildCandidateSnapshotLine(state.preedit(), state.candidatesExpanded(), state.candidateCursorIndex(),
                                   fallbackRect, state.candidates(),
                                   metadata);
    if (!sendCandidateWindowMessage(payload, true)) {
        FCITX_WARN() << "Failed to send candidate snapshot";
    }
}

void Engine::clearCandidateWindow() {
    if (candidateWindowFd_ < 0) {
        return;
    }
    const auto clearMessage = clearCandidateSnapshotLine();
    if (!sendCandidateWindowMessage(clearMessage, false)) {
        FCITX_WARN() << "Failed to clear candidate snapshot";
    }
}

bool Engine::sendCandidateWindowMessage(std::string_view payload, bool retryAfterRestart) {
    const int attempts = retryAfterRestart ? 2 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!ensureCandidateWindow()) {
            return false;
        }
        const auto result = writeNonblockingMessage(candidateWindowFd_, payload);
        if (result == NonblockingWriteResult::Complete) {
            return true;
        }
        if (result == NonblockingWriteResult::WouldBlock) {
            traceEngineDebug("candidate-window-backpressure dropped-latest-snapshot");
            return true;
        }
        traceEngineDebug("candidate-window-pipe-failed attempt=" + std::to_string(attempt + 1));
        closeCandidateWindow();
    }
    return false;
}

fcitx::AddonInstance *EngineFactory::create(fcitx::AddonManager *manager) { return new Engine(manager); }

} // namespace tipe

FCITX_ADDON_FACTORY(tipe::EngineFactory)
