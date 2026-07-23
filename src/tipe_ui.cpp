#include "waylandim_public_compat.h"

#include "bounded_log.h"
#include "candidate_layout.h"
#include "candidate_render.h"
#include "candidate_snapshot.h"
#include "tipe_ui_public.h"

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/focusgroup.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/handlertable.h>
#include <wayland-client.h>
#include <wayland_public.h>
#include "wl_surface.h"
#include "zwp_input_method_v2.h"
#include "zwp_input_popup_surface_v2.h"

#include <cairo.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace tipe {

class TipeUI final : public fcitx::UserInterface {
public:
    explicit TipeUI(fcitx::AddonManager *manager) : instance_(manager ? manager->instance() : nullptr) {
        ::signal(SIGPIPE, SIG_IGN);
        FCITX_INFO() << "TiPE UI addon loaded";
        if (instance_) {
            inputContextDestroyedWatcher_ = instance_->watchEvent(
                fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::InputMethod,
                [this](fcitx::Event &event) { onInputContextDestroyed(event); });
        }
        registerWaylandCallbacks(manager);
        waylandIMAddon_ = manager ? manager->addon("waylandim", true) : nullptr;
    }

    ~TipeUI() override {
        inputContextDestroyedWatcher_.reset();
        deferredTextRectRerender_.reset();
        deferredWaylandPopupRetry_.reset();
        waylandCreatedCallback_.reset();
        waylandClosedCallback_.reset();
        popupByDisplay_.clear();
        for (auto &[_, display] : waylandDisplays_) {
            releaseWaylandDisplay(display);
        }
    }

    bool available() override { return true; }
    void suspend() override {
        suspended_ = true;
        hideAllPopups();
    }
    void resume() override { suspended_ = false; }

    void update(fcitx::UserInterfaceComponent component, fcitx::InputContext *inputContext) override {
        if (component != fcitx::UserInterfaceComponent::InputPanel) {
            return;
        }
        if (suspended_ || !inputContext) {
            hideAllPopups();
            return;
        }
        if (inputPanelEmpty(*inputContext)) {
            hidePopupForInputContext(*inputContext);
            clearGenericFallbackForInputContext(*inputContext);
            logInputPanel(*inputContext);
            return;
        }
        if (inputContext->frontendName() == "wayland_v2") {
            hideGenericFallback();
            ensureWaylandPopup(*inputContext);
        } else {
            hideWaylandPopups();
            renderGenericFallback(*inputContext);
        }
        logInputPanel(*inputContext);
    }

    void updateInputPanel(fcitx::InputContext *inputContext) {
        update(fcitx::UserInterfaceComponent::InputPanel, inputContext);
    }

    FCITX_ADDON_EXPORT_FUNCTION(TipeUI, updateInputPanel);

private:
    static std::filesystem::path tipeCacheDir() {
        if (const char *xdgCacheHome = std::getenv("XDG_CACHE_HOME"); xdgCacheHome && *xdgCacheHome) {
            return std::filesystem::path(xdgCacheHome) / "tipe";
        }
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            return {};
        }
        return std::filesystem::path(home) / ".cache" / "tipe";
    }

    struct WaylandDisplayState {
        TipeUI *owner = nullptr;
        std::string key;
        wl_display *display = nullptr;
        wl_registry *registry = nullptr;
        wl_compositor *compositor = nullptr;
        wl_shm *shm = nullptr;
        wl_seat *seat = nullptr;
        wl_pointer *pointer = nullptr;
        wl_surface *pointerSurface = nullptr;
        double pointerX = 0;
        double pointerY = 0;
        uint32_t compositorGlobal = 0;
        uint32_t shmGlobal = 0;
        uint32_t seatGlobal = 0;
    };

    struct PopupState {
        PopupState() = default;
        PopupState(const PopupState &) = delete;
        PopupState &operator=(const PopupState &) = delete;
        PopupState(PopupState &&other) noexcept
            : surface(std::move(other.surface)), popup(std::move(other.popup)), buffer(other.buffer),
              bufferData(other.bufferData), bufferSize(other.bufferSize), width(other.width), height(other.height),
              bufferWidth(other.bufferWidth), bufferHeight(other.bufferHeight), bufferScale(other.bufferScale),
              preferredScale(other.preferredScale), lastTextRect(other.lastTextRect),
              pendingStatusText(std::move(other.pendingStatusText)), pendingInputScale(other.pendingInputScale),
              lastStatusText(std::move(other.lastStatusText)),
              lastFallbackCandidateSnapshot(std::move(other.lastFallbackCandidateSnapshot)),
              lastInputContext(std::move(other.lastInputContext)), awaitingFreshTextRect(other.awaitingFreshTextRect),
              candidateTextRectStale(other.candidateTextRectStale), surfaceBufferAttached(other.surfaceBufferAttached),
              candidateRenderLogged(other.candidateRenderLogged),
              candidateHitRegions(std::move(other.candidateHitRegions)),
              hoveredCandidateIndex(other.hoveredCandidateIndex),
              fallbackCandidateFd(other.fallbackCandidateFd),
              fallbackEventFd(other.fallbackEventFd),
              fallbackEventWatcher(std::move(other.fallbackEventWatcher)),
              fallbackEventBuffer(std::move(other.fallbackEventBuffer)),
              fallbackSnapshotSerial(other.fallbackSnapshotSerial),
              fallbackCursorTracker(std::move(other.fallbackCursorTracker)),
              fallbackCandidatePid(other.fallbackCandidatePid),
              fallbackStatusPid(other.fallbackStatusPid),
              lastFallbackStatusText(std::move(other.lastFallbackStatusText)),
              lastFallbackStatusRect(other.lastFallbackStatusRect) {
            other.buffer = nullptr;
            other.bufferData = nullptr;
            other.bufferSize = 0;
            other.width = 0;
            other.height = 0;
            other.bufferWidth = 0;
            other.bufferHeight = 0;
            other.bufferScale = 1;
            other.preferredScale = 1;
            other.lastTextRect.reset();
            other.pendingStatusText.clear();
            other.pendingInputScale = 1.0;
            other.lastStatusText.clear();
            other.lastFallbackCandidateSnapshot.clear();
            other.lastInputContext.unwatch();
            other.awaitingFreshTextRect = false;
            other.candidateTextRectStale = false;
            other.surfaceBufferAttached = false;
            other.candidateRenderLogged = false;
            other.candidateHitRegions.clear();
            other.hoveredCandidateIndex.reset();
            other.fallbackCandidateFd = -1;
            other.fallbackEventFd = -1;
            other.fallbackEventBuffer.clear();
            other.fallbackSnapshotSerial = 0;
            other.fallbackCursorTracker.reset();
            other.fallbackCandidatePid = -1;
            other.fallbackStatusPid = -1;
            other.lastFallbackStatusText.clear();
            other.lastFallbackStatusRect.reset();
        }
        PopupState &operator=(PopupState &&other) noexcept {
            if (this != &other) {
                releaseBuffer();
                releaseFallbackCandidate();
                releaseFallbackStatus();
                surface = std::move(other.surface);
                popup = std::move(other.popup);
                buffer = other.buffer;
                bufferData = other.bufferData;
                bufferSize = other.bufferSize;
                width = other.width;
                height = other.height;
                bufferWidth = other.bufferWidth;
                bufferHeight = other.bufferHeight;
                bufferScale = other.bufferScale;
                preferredScale = other.preferredScale;
                lastTextRect = other.lastTextRect;
                pendingStatusText = std::move(other.pendingStatusText);
                pendingInputScale = other.pendingInputScale;
                lastStatusText = std::move(other.lastStatusText);
                lastFallbackCandidateSnapshot = std::move(other.lastFallbackCandidateSnapshot);
                lastInputContext = std::move(other.lastInputContext);
                awaitingFreshTextRect = other.awaitingFreshTextRect;
                candidateTextRectStale = other.candidateTextRectStale;
                surfaceBufferAttached = other.surfaceBufferAttached;
                candidateRenderLogged = other.candidateRenderLogged;
                candidateHitRegions = std::move(other.candidateHitRegions);
                hoveredCandidateIndex = other.hoveredCandidateIndex;
                fallbackCandidateFd = other.fallbackCandidateFd;
                fallbackEventFd = other.fallbackEventFd;
                fallbackEventWatcher = std::move(other.fallbackEventWatcher);
                fallbackEventBuffer = std::move(other.fallbackEventBuffer);
                fallbackSnapshotSerial = other.fallbackSnapshotSerial;
                fallbackCursorTracker = std::move(other.fallbackCursorTracker);
                fallbackCandidatePid = other.fallbackCandidatePid;
                fallbackStatusPid = other.fallbackStatusPid;
                lastFallbackStatusText = std::move(other.lastFallbackStatusText);
                lastFallbackStatusRect = other.lastFallbackStatusRect;
                other.buffer = nullptr;
                other.bufferData = nullptr;
                other.bufferSize = 0;
                other.width = 0;
                other.height = 0;
                other.bufferWidth = 0;
                other.bufferHeight = 0;
                other.bufferScale = 1;
                other.preferredScale = 1;
                other.lastTextRect.reset();
                other.pendingStatusText.clear();
                other.pendingInputScale = 1.0;
                other.lastStatusText.clear();
                other.lastFallbackCandidateSnapshot.clear();
                other.lastInputContext.unwatch();
                other.awaitingFreshTextRect = false;
                other.candidateTextRectStale = false;
                other.surfaceBufferAttached = false;
                other.candidateRenderLogged = false;
                other.candidateHitRegions.clear();
                other.hoveredCandidateIndex.reset();
                other.fallbackCandidateFd = -1;
                other.fallbackEventFd = -1;
                other.fallbackEventBuffer.clear();
                other.fallbackSnapshotSerial = 0;
                other.fallbackCursorTracker.reset();
                other.fallbackCandidatePid = -1;
                other.fallbackStatusPid = -1;
                other.lastFallbackStatusText.clear();
                other.lastFallbackStatusRect.reset();
            }
            return *this;
        }
        ~PopupState() {
            releaseBuffer();
            releaseFallbackCandidate();
            releaseFallbackStatus();
        }

        void releaseBuffer() {
            if (buffer) {
                wl_buffer_destroy(buffer);
                buffer = nullptr;
            }
            if (bufferData) {
                munmap(bufferData, bufferSize);
                bufferData = nullptr;
                bufferSize = 0;
            }
            width = 0;
            height = 0;
            bufferWidth = 0;
            bufferHeight = 0;
            bufferScale = 1;
        }

        void releaseFallbackCandidate() {
            fallbackEventWatcher.reset();
            if (fallbackEventFd >= 0) {
                close(fallbackEventFd);
                fallbackEventFd = -1;
            }
            fallbackEventBuffer.clear();
            fallbackCursorTracker.reset();
            if (fallbackCandidateFd >= 0) {
                close(fallbackCandidateFd);
                fallbackCandidateFd = -1;
            }
            if (fallbackCandidatePid > 0) {
                if (waitpid(fallbackCandidatePid, nullptr, WNOHANG) == 0) {
                    kill(fallbackCandidatePid, SIGTERM);
                    waitpid(fallbackCandidatePid, nullptr, WNOHANG);
                }
                fallbackCandidatePid = -1;
            }
        }

        void releaseFallbackStatus() {
            if (fallbackStatusPid > 0) {
                if (waitpid(fallbackStatusPid, nullptr, WNOHANG) == 0) {
                    kill(fallbackStatusPid, SIGTERM);
                    waitpid(fallbackStatusPid, nullptr, WNOHANG);
                }
                fallbackStatusPid = -1;
            }
            lastFallbackStatusText.clear();
            lastFallbackStatusRect.reset();
        }

        std::unique_ptr<fcitx::wayland::WlSurface> surface;
        std::unique_ptr<fcitx::wayland::ZwpInputPopupSurfaceV2> popup;
        wl_buffer *buffer = nullptr;
        void *bufferData = nullptr;
        std::size_t bufferSize = 0;
        int width = 0;
        int height = 0;
        int bufferWidth = 0;
        int bufferHeight = 0;
        int bufferScale = 1;
        int preferredScale = 1;
        std::optional<std::array<int32_t, 4>> lastTextRect;
        std::string pendingStatusText;
        double pendingInputScale = 1.0;
        std::string lastStatusText;
        std::string lastFallbackCandidateSnapshot;
        fcitx::TrackableObjectReference<fcitx::InputContext> lastInputContext;
        bool awaitingFreshTextRect = false;
        bool candidateTextRectStale = false;
        bool surfaceBufferAttached = false;
        bool candidateRenderLogged = false;
        std::vector<CandidateHitRegion> candidateHitRegions;
        std::optional<std::size_t> hoveredCandidateIndex;
        int fallbackCandidateFd = -1;
        int fallbackEventFd = -1;
        std::unique_ptr<fcitx::EventSourceIO> fallbackEventWatcher;
        std::string fallbackEventBuffer;
        int fallbackSnapshotSerial = 0;
        CandidateCursorFollowTracker fallbackCursorTracker;
        pid_t fallbackCandidatePid = -1;
        pid_t fallbackStatusPid = -1;
        std::string lastFallbackStatusText;
        std::optional<std::array<int32_t, 4>> lastFallbackStatusRect;
    };

    std::filesystem::path logPath() const {
        auto dir = tipeCacheDir();
        if (dir.empty()) {
            return {};
        }
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        return dir / "tipeui.log";
    }

    void logLine(const std::string &line) const {
        if (!verbosePanelLoggingEnabled()) {
            return;
        }
        const auto path = logPath();
        if (path.empty()) {
            return;
        }
        appendBoundedDiagnosticLog(path, line);
    }

    static bool verbosePanelLoggingEnabled() {
        const char *value = std::getenv("TIPE_DEBUG");
        return value && (std::string_view(value) == "1" || std::string_view(value) == "true" ||
                         std::string_view(value) == "on");
    }

    static std::optional<int> envInt(const char *name) {
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

    static bool waylandPopupEdgeFallbackEnabled() {
        const char *enabled = std::getenv("TIPE_WAYLAND_POPUP_EDGE_FALLBACK");
        return enabled && (std::string_view(enabled) == "1" || std::string_view(enabled) == "true" ||
                           std::string_view(enabled) == "on");
    }

    static bool statusEdgeFallbackEnabled() {
        const char *enabled = std::getenv("TIPE_STATUS_EDGE_FALLBACK");
        return enabled && (std::string_view(enabled) == "1" || std::string_view(enabled) == "true" ||
                           std::string_view(enabled) == "on");
    }

    static bool candidateEdgeFallbackNeededForRect(const std::array<int32_t, 4> &rect, int width, int height) {
        if (!waylandPopupEdgeFallbackEnabled()) {
            return false;
        }
        const int leftThreshold =
            envInt("TIPE_WAYLAND_POPUP_EDGE_LEFT").value_or(tipeUIPopupEdgeFallbackLeftThreshold);
        const int topThreshold =
            envInt("TIPE_WAYLAND_POPUP_EDGE_TOP").value_or(tipeUIPopupEdgeFallbackTopThreshold);
        return tipeUIPopupEdgeFallbackNeededForRect(rect[0], rect[1], rect[2], rect[3], leftThreshold, topThreshold,
                                                   width, height);
    }

    static bool statusEdgeFallbackNeededForRect(const std::array<int32_t, 4> &rect) {
        return statusEdgeFallbackEnabled() &&
               candidateEdgeFallbackNeededForRect(rect, tipeUIStatusPopupWidth, tipeUIStatusPopupHeight);
    }

    void reapStatusFallbackWindow(PopupState &popup) const {
        if (popup.fallbackStatusPid > 0 && waitpid(popup.fallbackStatusPid, nullptr, WNOHANG) != 0) {
            popup.fallbackStatusPid = -1;
            popup.lastFallbackStatusText.clear();
            popup.lastFallbackStatusRect.reset();
        }
    }

    std::string_view candidateFallbackLogName(const PopupState &popup) const {
        return &popup == &genericFallbackPopup_ ? "candidate-frontend-fallback" : "candidate-edge-fallback";
    }

    std::string_view statusFallbackLogName(const PopupState &popup) const {
        return &popup == &genericFallbackPopup_ ? "status-frontend-fallback" : "status-edge-fallback";
    }

    void closeStatusFallbackWindow(PopupState &popup, std::string_view reason) const {
        reapStatusFallbackWindow(popup);
        if (popup.fallbackStatusPid > 0) {
            if (waitpid(popup.fallbackStatusPid, nullptr, WNOHANG) == 0) {
                kill(popup.fallbackStatusPid, SIGTERM);
                waitpid(popup.fallbackStatusPid, nullptr, WNOHANG);
            }
            logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "-close\treason=" +
                    std::string(reason));
            popup.fallbackStatusPid = -1;
        }
        popup.lastFallbackStatusText.clear();
        popup.lastFallbackStatusRect.reset();
    }

    void showStatusFallbackWindow(PopupState &popup, const std::string &statusText,
                                  const std::array<int32_t, 4> &rect) const {
        reapStatusFallbackWindow(popup);
        if (popup.fallbackStatusPid > 0 && popup.lastFallbackStatusText == statusText &&
            popup.lastFallbackStatusRect == rect) {
            logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "-skip-duplicate");
            return;
        }
        closeStatusFallbackWindow(popup, "replace-status-edge-fallback");
        const char *home = std::getenv("HOME");
        if (!home) {
            logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "-no-home");
            return;
        }
        const auto logDir = tipeCacheDir();
        if (logDir.empty()) {
            logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "-no-cache-dir");
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(logDir, error);
        const auto binaryPath = std::filesystem::path(home) / ".local" / "bin" / "tipe-candidate-window";
        const auto logPath = logDir / "candidate-window.log";
        const auto cursor = std::to_string(rect[0]) + "," + std::to_string(rect[1]) + "," +
                            std::to_string(rect[2]) + "," + std::to_string(rect[3]);
        const pid_t child = fork();
        if (child < 0) {
            logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "-fork-failed");
            return;
        }
        if (child == 0) {
            const int logFd = openPrivateAppendFile(logPath);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
                close(logFd);
            }
            execl(binaryPath.c_str(), binaryPath.c_str(), "--status", statusText.c_str(), "--cursor",
                  cursor.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        popup.fallbackStatusPid = child;
        popup.lastFallbackStatusText = statusText;
        popup.lastFallbackStatusRect = rect;
        logLine("popup\t" + std::string(statusFallbackLogName(popup)) + "\tstatus=" + statusText + "\trect=" + cursor +
                "\tpid=" + std::to_string(child));
    }

    void closeCandidateFallbackWindow(PopupState &popup, std::string_view reason) {
        popup.fallbackEventWatcher.reset();
        if (popup.fallbackEventFd >= 0) {
            close(popup.fallbackEventFd);
            popup.fallbackEventFd = -1;
        }
        popup.fallbackEventBuffer.clear();
        if (popup.fallbackCandidateFd >= 0) {
            close(popup.fallbackCandidateFd);
            popup.fallbackCandidateFd = -1;
        }
        if (popup.fallbackCandidatePid > 0) {
            bool terminated = false;
            if (waitpid(popup.fallbackCandidatePid, nullptr, WNOHANG) == 0) {
                kill(popup.fallbackCandidatePid, SIGTERM);
                waitpid(popup.fallbackCandidatePid, nullptr, WNOHANG);
                terminated = true;
            }
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-close\treason=" +
                    std::string(reason));
            if (terminated) {
                logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-terminate\treason=" +
                        std::string(reason));
            }
            popup.fallbackCandidatePid = -1;
        }
        popup.fallbackSnapshotSerial = 0;
        popup.fallbackCursorTracker.reset();
        popup.lastFallbackCandidateSnapshot.clear();
    }

    void clearCandidateFallbackWindow(PopupState &popup, std::string_view reason) {
        popup.fallbackSnapshotSerial = 0;
        popup.fallbackCursorTracker.reset();
        const auto clearSnapshot = clearCandidateSnapshotLine();
        if (popup.fallbackCandidateFd < 0 && popup.fallbackEventFd < 0 && popup.fallbackCandidatePid <= 0) {
            popup.lastFallbackCandidateSnapshot.clear();
            return;
        }
        if (popup.fallbackCandidateFd < 0 || popup.fallbackEventFd < 0 || popup.fallbackCandidatePid <= 0) {
            closeCandidateFallbackWindow(popup, "clear-incomplete-channel");
            return;
        }
        if (popup.lastFallbackCandidateSnapshot != clearSnapshot &&
            !writeCandidateFallbackSnapshot(popup, clearSnapshot)) {
            return;
        }
        popup.lastFallbackCandidateSnapshot = clearSnapshot;
        logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-clear\treason=" +
                std::string(reason));
    }

    bool ensureCandidateFallbackWindow(PopupState &popup) {
        if (popup.fallbackCandidateFd >= 0 && popup.fallbackEventFd >= 0 && popup.fallbackCandidatePid > 0) {
            return true;
        }
        if (popup.fallbackCandidateFd >= 0 || popup.fallbackEventFd >= 0 || popup.fallbackCandidatePid > 0) {
            closeCandidateFallbackWindow(popup, "restart-incomplete-channel");
        }
        const char *home = std::getenv("HOME");
        if (!home) {
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-no-home");
            return false;
        }
        const auto homePath = std::filesystem::path(home);
        const auto logDir = tipeCacheDir();
        if (logDir.empty()) {
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-no-cache-dir");
            return false;
        }
        const auto logPath = logDir / "candidate-window.log";
        const auto binaryPath = homePath / ".local" / "bin" / "tipe-candidate-window";
        std::error_code error;
        std::filesystem::create_directories(logDir, error);

        int pipeFds[2]{-1, -1};
        int eventFds[2]{-1, -1};
        if (pipe(pipeFds) != 0 || pipe(eventFds) != 0) {
            if (pipeFds[0] >= 0) {
                close(pipeFds[0]);
                close(pipeFds[1]);
            }
            if (eventFds[0] >= 0) {
                close(eventFds[0]);
                close(eventFds[1]);
            }
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-pipe-failed");
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            close(pipeFds[0]);
            close(pipeFds[1]);
            close(eventFds[0]);
            close(eventFds[1]);
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-fork-failed");
            return false;
        }
        if (child == 0) {
            close(pipeFds[1]);
            close(eventFds[0]);
            dup2(pipeFds[0], STDIN_FILENO);
            close(pipeFds[0]);
            const int logFd = openPrivateAppendFile(logPath);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
                close(logFd);
            }
            const auto eventFd = std::to_string(eventFds[1]);
            execl(binaryPath.c_str(), binaryPath.c_str(), "--stdin", "--events-fd", eventFd.c_str(),
                  static_cast<char *>(nullptr));
            _exit(127);
        }
        close(pipeFds[0]);
        close(eventFds[1]);
        const auto flags = fcntl(eventFds[0], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(eventFds[0], F_SETFL, flags | O_NONBLOCK);
        }
        popup.fallbackCandidateFd = pipeFds[1];
        popup.fallbackEventFd = eventFds[0];
        popup.fallbackCandidatePid = child;
        if (instance_) {
            popup.fallbackEventWatcher = instance_->eventLoop().addIOEvent(
                popup.fallbackEventFd, fcitx::IOEventFlag::In,
                [this](fcitx::EventSourceIO *, int fd, fcitx::IOEventFlags) {
                    return readCandidateFallbackEvents(fd);
                });
        }
        if (!popup.fallbackEventWatcher) {
            closeCandidateFallbackWindow(popup, "event-watch-failed");
            return false;
        }
        logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-start\tpid=" +
                std::to_string(child) + "\tevents=ready");
        return true;
    }

    bool writeCandidateFallbackSnapshot(PopupState &popup, const std::string &snapshot) {
        const std::string line = !snapshot.empty() && snapshot.back() == '\n' ? snapshot : snapshot + '\n';
        const char *data = line.data();
        std::size_t remaining = line.size();
        while (remaining > 0) {
            const auto written = write(popup.fallbackCandidateFd, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                closeCandidateFallbackWindow(popup, "write-failed");
                return false;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
    }

    void showCandidateFallbackWindow(PopupState &popup, const std::string &snapshot) {
        if (popup.lastFallbackCandidateSnapshot == snapshot) {
            logLine("popup\t" + std::string(candidateFallbackLogName(popup)) + "-skip-duplicate");
            return;
        }
        if (!ensureCandidateFallbackWindow(popup)) {
            return;
        }
        if (!writeCandidateFallbackSnapshot(popup, snapshot)) {
            if (!ensureCandidateFallbackWindow(popup) || !writeCandidateFallbackSnapshot(popup, snapshot)) {
                return;
            }
        }
        popup.lastFallbackCandidateSnapshot = snapshot;
        logLine("popup\t" + std::string(candidateFallbackLogName(popup)));
    }

    PopupState *fallbackPopupForEventFd(int fd) {
        if (genericFallbackPopup_.fallbackEventFd == fd) {
            return &genericFallbackPopup_;
        }
        for (auto &[_, popup] : popupByDisplay_) {
            if (popup.fallbackEventFd == fd) {
                return &popup;
            }
        }
        return nullptr;
    }

    static std::optional<int> parseEventInteger(std::string_view value) {
        if (value.empty()) {
            return std::nullopt;
        }
        std::string owned(value);
        char *end = nullptr;
        const long parsed = std::strtol(owned.c_str(), &end, 10);
        if (end == owned.c_str() || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    }

    void scheduleCandidateFallbackSelection(PopupState &popup, int serial, int index) {
        auto *inputContext = popup.lastInputContext.get();
        auto candidates = inputContext ? inputContext->inputPanel().candidateList() : nullptr;
        if (!instance_ || !inputContext || !candidates || index < 0 || index >= candidates->size() ||
            serial != popup.fallbackSnapshotSerial) {
            logLine("popup\tcandidate-click-ignored-stale");
            return;
        }
        const auto expectedText = outputText(*inputContext, candidates->candidate(index).text()).toString();
        const auto watchedInputContext = inputContext->watch();
        fallbackSelectionEvent_ = instance_->eventLoop().addDeferEvent(
            [this, watchedInputContext, index, expectedText](fcitx::EventSource *) {
                fallbackSelectionEvent_.reset();
                auto *inputContext = watchedInputContext.get();
                auto candidates = inputContext ? inputContext->inputPanel().candidateList() : nullptr;
                if (!inputContext || !candidates || index < 0 || index >= candidates->size() ||
                    outputText(*inputContext, candidates->candidate(index).text()).toString() != expectedText) {
                    logLine("popup\tcandidate-click-ignored-changed");
                    return false;
                }
                logLine("popup\tcandidate-click\tindex=" + std::to_string(index));
                candidates->candidate(index).select(inputContext);
                return false;
            });
        if (fallbackSelectionEvent_) {
            fallbackSelectionEvent_->setOneShot();
        }
    }

    bool readCandidateFallbackEvents(int fd) {
        auto *popup = fallbackPopupForEventFd(fd);
        if (!popup) {
            return false;
        }
        std::array<char, 1024> buffer{};
        for (;;) {
            const auto size = read(fd, buffer.data(), buffer.size());
            if (size > 0) {
                popup->fallbackEventBuffer.append(buffer.data(), static_cast<std::size_t>(size));
                continue;
            }
            if (size < 0 && errno == EINTR) {
                continue;
            }
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            close(fd);
            popup->fallbackEventFd = -1;
            return false;
        }
        if (popup->fallbackEventBuffer.size() > 4096) {
            popup->fallbackEventBuffer.clear();
            logLine("popup\tcandidate-event-overflow");
            return true;
        }
        for (;;) {
            const auto newline = popup->fallbackEventBuffer.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            const auto line = popup->fallbackEventBuffer.substr(0, newline);
            popup->fallbackEventBuffer.erase(0, newline + 1);
            std::istringstream stream(line);
            std::string command;
            std::string serialText;
            std::string indexText;
            std::string extra;
            if (!std::getline(stream, command, '\t') || !std::getline(stream, serialText, '\t') ||
                !std::getline(stream, indexText, '\t') || std::getline(stream, extra, '\t') ||
                command != "select") {
                logLine("popup\tcandidate-event-invalid");
                continue;
            }
            const auto serial = parseEventInteger(serialText);
            const auto index = parseEventInteger(indexText);
            if (!serial || !index) {
                logLine("popup\tcandidate-event-invalid-number");
                continue;
            }
            scheduleCandidateFallbackSelection(*popup, *serial, *index);
        }
        return true;
    }

    static int tipeUIStateValue(std::string_view aux, std::string_view name, int fallback = 0) {
        std::istringstream stream{std::string(aux)};
        std::string token;
        while (std::getline(stream, token, '\t')) {
            if (token != name || !std::getline(stream, token, '\t')) {
                continue;
            }
            return parseEventInteger(token).value_or(fallback);
        }
        return fallback;
    }

    void renderGenericFallback(fcitx::InputContext &inputContext) {
        if (genericFallbackPopup_.lastInputContext.get() != &inputContext) {
            hideGenericFallback();
            genericFallbackPopup_.lastInputContext = inputContext.watch();
        }
        auto &popup = genericFallbackPopup_;
        const auto &panel = inputContext.inputPanel();
        const auto auxUp = outputText(inputContext, panel.auxUp()).toString();
        auto candidates = panel.candidateList();
        if (!candidates || candidates->size() == 0) {
            clearCandidateFallbackWindow(popup, "generic-status");
            if (!statusFromAux(auxUp)) {
                closeStatusFallbackWindow(popup, "generic-empty");
                return;
            }
            const auto rect = logicalCandidateSnapshotRect(
                {inputContext.cursorRect().left(), inputContext.cursorRect().top(), inputContext.cursorRect().width(),
                 inputContext.cursorRect().height()},
                inputContext.scaleFactor());
            showStatusFallbackWindow(popup, auxUp, {rect.x, rect.y, rect.width, rect.height});
            return;
        }

        closeStatusFallbackWindow(popup, "generic-candidate");
        const auto preeditText = outputText(inputContext, panel.preedit());
        const auto preedit = preeditText.toString();
        const auto preeditCursor = std::clamp(preeditText.cursor(), 0, static_cast<int>(preedit.size()));
        std::vector<std::string> candidateTexts;
        candidateTexts.reserve(candidates->size());
        for (int index = 0; index < candidates->size(); ++index) {
            candidateTexts.push_back(outputText(inputContext, candidates->candidate(index).text()).toString());
        }
        const auto selectedIndex = static_cast<std::size_t>(
            std::clamp(candidates->cursorIndex(), 0, std::max(0, candidates->size() - 1)));
        const auto rawRect = CandidateSnapshotRect{inputContext.cursorRect().left(), inputContext.cursorRect().top(),
                                                   inputContext.cursorRect().width(),
                                                   inputContext.cursorRect().height()};
        const auto anchor = candidateSnapshotAnchorFor(inputContext.frontendName(), rawRect,
                                                       inputContext.scaleFactor());
        const auto rect = anchor.rect;
        if (verbosePanelLoggingEnabled()) {
            const auto source = anchor.pointerFallback ? "pointer" :
                                (rawRect.height <= 0 && rect.height > 0 ? "xim-spot" : "cursor-rect");
            logLine("popup\tfrontend-fallback-anchor\tfrontend=" + std::string(inputContext.frontendName()) +
                    "\tsource=" + source + "\traw=" + std::to_string(rawRect.x) + "," +
                    std::to_string(rawRect.y) + "," + std::to_string(rawRect.width) + "," +
                    std::to_string(rawRect.height) + "\tlogical=" + std::to_string(rect.x) + "," +
                    std::to_string(rect.y) + "," + std::to_string(rect.width) + "," +
                    std::to_string(rect.height));
        }
        const bool staticCursorRect =
            anchor.pointerFallback ||
            popup.fallbackCursorTracker.observe(rect, preedit, static_cast<std::size_t>(preeditCursor));
        if (++nextFallbackSnapshotSerial_ <= 0) {
            nextFallbackSnapshotSerial_ = 1;
        }
        popup.fallbackSnapshotSerial = nextFallbackSnapshotSerial_;
        const auto metadata = std::string("supervision=") +
                              std::to_string(tipeUIStateValue(auxUp, "supervision")) + ",keys=" +
                              std::to_string(tipeUIStateValue(auxUp, "keys")) + ",selects=" +
                              std::to_string(tipeUIStateValue(auxUp, "selects")) + ",reranks=" +
                              std::to_string(tipeUIStateValue(auxUp, "reranks")) + ",continuous=" +
                              std::to_string(tipeUIStateValue(auxUp, "continuous")) + ",preedit_cursor=" +
                              std::to_string(preeditCursor) + ",cursor_static=" +
                              std::to_string(staticCursorRect ? 1 : 0) + ",snapshot=" +
                              std::to_string(popup.fallbackSnapshotSerial) + ",pointer_fallback=" +
                              std::to_string(anchor.pointerFallback ? 1 : 0);
        showCandidateFallbackWindow(
            popup, buildCandidateSnapshotLine(preedit, expandedFromAux(auxUp), selectedIndex, rect, candidateTexts,
                                              metadata));
    }

    void clearGenericFallbackForInputContext(fcitx::InputContext &inputContext) {
        if (genericFallbackPopup_.lastInputContext.get() == &inputContext) {
            clearCandidateFallbackWindow(genericFallbackPopup_, "input-panel-empty");
            closeStatusFallbackWindow(genericFallbackPopup_, "input-panel-empty");
        }
    }

    void hideGenericFallback() {
        hidePopup(genericFallbackPopup_);
        genericFallbackPopup_.lastInputContext.unwatch();
        genericFallbackPopup_.fallbackCursorTracker.reset();
    }

    void hideWaylandPopups() {
        for (auto &[_, popup] : popupByDisplay_) {
            hidePopup(popup);
        }
    }

    void scheduleTextRectRerender(const std::string &key) {
        if (!instance_) {
            return;
        }
        pendingTextRectRerenderDisplay_ = key;
        if (deferredTextRectRerenderPending_) {
            return;
        }
        deferredTextRectRerenderPending_ = true;
        deferredTextRectRerender_ = instance_->eventLoop().addDeferEvent([this](fcitx::EventSource *) {
            const auto key = pendingTextRectRerenderDisplay_;
            pendingTextRectRerenderDisplay_.clear();
            deferredTextRectRerenderPending_ = false;
            auto popupIter = popupByDisplay_.find(key);
            auto displayIter = waylandDisplays_.find(key);
            auto *inputContext = popupIter == popupByDisplay_.end() ? nullptr
                                                                    : popupIter->second.lastInputContext.get();
            if (popupIter == popupByDisplay_.end() || displayIter == waylandDisplays_.end() || !inputContext ||
                inputPanelEmpty(*inputContext)) {
                return false;
            }
            renderPopup(*inputContext, displayIter->second, popupIter->second);
            if (verbosePanelLoggingEnabled()) {
                logLine("popup\ttext-rect-rerender\tdisplay=" + key);
            }
            return false;
        });
        if (deferredTextRectRerender_) {
            deferredTextRectRerender_->setOneShot();
        } else {
            deferredTextRectRerenderPending_ = false;
            pendingTextRectRerenderDisplay_.clear();
        }
    }

    void scheduleWaylandPopupRetry(const std::string &key) {
        if (!instance_) {
            return;
        }
        pendingWaylandPopupDisplays_.insert(key);
        if (deferredWaylandPopupRetryPending_) {
            return;
        }
        deferredWaylandPopupRetryPending_ = true;
        deferredWaylandPopupRetry_ = instance_->eventLoop().addDeferEvent([this](fcitx::EventSource *) {
            auto pendingDisplays = std::move(pendingWaylandPopupDisplays_);
            pendingWaylandPopupDisplays_.clear();
            deferredWaylandPopupRetryPending_ = false;
            for (const auto &key : pendingDisplays) {
                auto displayIter = waylandDisplays_.find(key);
                if (displayIter == waylandDisplays_.end() || !displayIter->second.compositor ||
                    !displayIter->second.shm) {
                    continue;
                }
                auto *inputContext = instance_ ? instance_->mostRecentInputContext() : nullptr;
                if (!inputContext || inputContext->frontendName() != "wayland_v2" ||
                    displayKeyFor(*inputContext) != key || inputPanelEmpty(*inputContext)) {
                    continue;
                }
                ensureWaylandPopup(*inputContext);
            }
            return false;
        });
        if (deferredWaylandPopupRetry_) {
            deferredWaylandPopupRetry_->setOneShot();
        } else {
            deferredWaylandPopupRetryPending_ = false;
        }
    }

    void refreshPointerHover(WaylandDisplayState &display) {
        auto popupIter = popupByDisplay_.find(display.key);
        if (popupIter == popupByDisplay_.end()) {
            return;
        }
        auto &popup = popupIter->second;
        std::optional<std::size_t> hovered;
        if (popup.surface && display.pointerSurface == static_cast<wl_surface *>(*popup.surface)) {
            hovered = candidateIndexAtPoint(popup.candidateHitRegions, display.pointerX, display.pointerY);
        }
        if (popup.hoveredCandidateIndex == hovered) {
            return;
        }
        popup.hoveredCandidateIndex = hovered;
        auto *inputContext = popup.lastInputContext.get();
        if (!inputContext || inputPanelEmpty(*inputContext) || !popup.surfaceBufferAttached) {
            return;
        }
        renderPopup(*inputContext, display, popup);
    }

    void selectPointerCandidate(WaylandDisplayState &display) {
        auto popupIter = popupByDisplay_.find(display.key);
        if (popupIter == popupByDisplay_.end()) {
            return;
        }
        auto &popup = popupIter->second;
        auto *inputContext = popup.lastInputContext.get();
        if (!popup.surface || display.pointerSurface != static_cast<wl_surface *>(*popup.surface) || !inputContext) {
            return;
        }
        const auto index = candidateIndexAtPoint(popup.candidateHitRegions, display.pointerX, display.pointerY);
        if (!index) {
            return;
        }
        auto candidates = inputContext->inputPanel().candidateList();
        if (!candidates || *index >= static_cast<std::size_t>(candidates->size())) {
            return;
        }
        logLine("popup\tcandidate-click\tindex=" + std::to_string(*index));
        candidates->candidate(static_cast<int>(*index)).select(inputContext);
    }

    static void pointerEnter(void *data, wl_pointer *, uint32_t, wl_surface *surface, wl_fixed_t x,
                             wl_fixed_t y) {
        auto *display = static_cast<WaylandDisplayState *>(data);
        if (!display) {
            return;
        }
        display->pointerSurface = surface;
        display->pointerX = wl_fixed_to_double(x);
        display->pointerY = wl_fixed_to_double(y);
        if (display->owner) {
            display->owner->refreshPointerHover(*display);
        }
    }

    static void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *surface) {
        auto *display = static_cast<WaylandDisplayState *>(data);
        if (!display || display->pointerSurface != surface) {
            return;
        }
        display->pointerSurface = nullptr;
        if (display->owner) {
            display->owner->refreshPointerHover(*display);
        }
    }

    static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto *display = static_cast<WaylandDisplayState *>(data);
        if (!display) {
            return;
        }
        display->pointerX = wl_fixed_to_double(x);
        display->pointerY = wl_fixed_to_double(y);
        if (display->owner) {
            display->owner->refreshPointerHover(*display);
        }
    }

    static void pointerButton(void *data, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state) {
        auto *display = static_cast<WaylandDisplayState *>(data);
        if (display && display->owner && button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
            display->owner->selectPointerCandidate(*display);
        }
    }

    static void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
    static void pointerFrame(void *, wl_pointer *) {}
    static void pointerAxisSource(void *, wl_pointer *, uint32_t) {}
    static void pointerAxisStop(void *, wl_pointer *, uint32_t, uint32_t) {}
    static void pointerAxisDiscrete(void *, wl_pointer *, uint32_t, int32_t) {}
    static void pointerAxisValue120(void *, wl_pointer *, uint32_t, int32_t) {}
    static void pointerAxisRelativeDirection(void *, wl_pointer *, uint32_t, uint32_t) {}

    static const wl_pointer_listener &pointerListener() {
        static const wl_pointer_listener listener{
            .enter = pointerEnter,
            .leave = pointerLeave,
            .motion = pointerMotion,
            .button = pointerButton,
            .axis = pointerAxis,
            .frame = pointerFrame,
            .axis_source = pointerAxisSource,
            .axis_stop = pointerAxisStop,
            .axis_discrete = pointerAxisDiscrete,
            .axis_value120 = pointerAxisValue120,
            .axis_relative_direction = pointerAxisRelativeDirection,
        };
        return listener;
    }

    static void releasePointer(WaylandDisplayState &display) {
        display.pointerSurface = nullptr;
        if (!display.pointer) {
            return;
        }
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(display.pointer)) >=
            WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(display.pointer);
        } else {
            wl_pointer_destroy(display.pointer);
        }
        display.pointer = nullptr;
    }

    static void seatCapabilities(void *data, wl_seat *seat, uint32_t capabilities) {
        auto *display = static_cast<WaylandDisplayState *>(data);
        if (!display) {
            return;
        }
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
            if (!display->pointer) {
                display->pointer = wl_seat_get_pointer(seat);
                if (display->pointer) {
                    wl_pointer_add_listener(display->pointer, &pointerListener(), display);
                    if (display->owner) {
                        display->owner->logLine(
                            "wayland\tpointer-ready\tname=" + display->key + "\tdisplay=" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(display->display)) + "\tpointer=" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(display->pointer)));
                    }
                }
            }
            return;
        }
        if (display->owner && display->pointerSurface) {
            display->pointerSurface = nullptr;
            display->owner->refreshPointerHover(*display);
        }
        releasePointer(*display);
    }

    static void seatName(void *, wl_seat *, const char *) {}

    static const wl_seat_listener &seatListener() {
        static const wl_seat_listener listener{
            .capabilities = seatCapabilities,
            .name = seatName,
        };
        return listener;
    }

    static void releaseWaylandDisplay(WaylandDisplayState &display) {
        releasePointer(display);
        if (display.seat) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(display.seat)) >= WL_SEAT_RELEASE_SINCE_VERSION) {
                wl_seat_release(display.seat);
            } else {
                wl_seat_destroy(display.seat);
            }
            display.seat = nullptr;
        }
        if (display.shm) {
            wl_shm_destroy(display.shm);
            display.shm = nullptr;
        }
        if (display.compositor) {
            wl_compositor_destroy(display.compositor);
            display.compositor = nullptr;
        }
        if (display.registry) {
            wl_registry_destroy(display.registry);
            display.registry = nullptr;
        }
        display.display = nullptr;
    }

    void registerWaylandCallbacks(fcitx::AddonManager *manager) {
        auto *waylandAddon = manager ? manager->addon("wayland", true) : nullptr;
        if (!waylandAddon) {
            logLine("wayland\tunavailable");
            return;
        }
        waylandCreatedCallback_ = waylandAddon->call<fcitx::IWaylandModule::addConnectionCreatedCallback>(
            [this](const std::string &name, wl_display *display, fcitx::FocusGroup *group) {
                auto &state = waylandDisplays_[name];
                if (state.display) {
                    popupByDisplay_.erase(name);
                    releaseWaylandDisplay(state);
                }
                state.owner = this;
                state.key = name;
                state.display = display;
                discoverGlobals(display, state);
                logLine("wayland\tcreated\tname=" + name + "\tdisplay=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(display)) + "\tfocusDisplay=" +
                        (group ? group->display() : std::string{}) + "\tcompositor=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(state.compositor)) + "\tshm=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(state.shm)) + "\tseat=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(state.seat)) + "\tpointer=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(state.pointer)));
            });
        waylandClosedCallback_ = waylandAddon->call<fcitx::IWaylandModule::addConnectionClosedCallback>(
            [this](const std::string &name, wl_display *display) {
                popupByDisplay_.erase(name);
                if (auto iter = waylandDisplays_.find(name); iter != waylandDisplays_.end()) {
                    releaseWaylandDisplay(iter->second);
                }
                waylandDisplays_.erase(name);
                logLine("wayland\tclosed\tname=" + name + "\tdisplay=" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(display)));
            });
    }

    static void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface,
                               uint32_t version) {
        auto *state = static_cast<WaylandDisplayState *>(data);
        if (!state) {
            return;
        }
        const bool globalsWereReady = state->compositor && state->shm;
        if (!state->compositor && std::string_view(interface) == wl_compositor_interface.name) {
            state->compositorGlobal = name;
            state->compositor = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, std::min<uint32_t>(version, 6)));
        } else if (!state->shm && std::string_view(interface) == wl_shm_interface.name) {
            state->shmGlobal = name;
            state->shm = static_cast<wl_shm *>(
                wl_registry_bind(registry, name, &wl_shm_interface, std::min<uint32_t>(version, 1)));
        } else if (!state->seat && std::string_view(interface) == wl_seat_interface.name) {
            state->seatGlobal = name;
            state->seat = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min<uint32_t>(version, 9)));
            if (state->seat) {
                wl_seat_add_listener(state->seat, &seatListener(), state);
            }
        }
        if (!globalsWereReady && state->compositor && state->shm && state->owner) {
            state->owner->scheduleWaylandPopupRetry(state->key);
        }
    }

    static void registryGlobalRemove(void *data, wl_registry *, uint32_t name) {
        auto *state = static_cast<WaylandDisplayState *>(data);
        if (!state) {
            return;
        }
        if (name == state->seatGlobal) {
            releasePointer(*state);
            if (state->seat) {
                if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(state->seat)) >=
                    WL_SEAT_RELEASE_SINCE_VERSION) {
                    wl_seat_release(state->seat);
                } else {
                    wl_seat_destroy(state->seat);
                }
                state->seat = nullptr;
            }
            state->seatGlobal = 0;
        } else if (name == state->shmGlobal) {
            if (state->shm) {
                wl_shm_destroy(state->shm);
                state->shm = nullptr;
            }
            state->shmGlobal = 0;
        } else if (name == state->compositorGlobal) {
            if (state->compositor) {
                wl_compositor_destroy(state->compositor);
                state->compositor = nullptr;
            }
            state->compositorGlobal = 0;
        }
    }

    void discoverGlobals(wl_display *display, WaylandDisplayState &state) const {
        if (!display) {
            return;
        }
        state.registry = wl_display_get_registry(display);
        if (!state.registry) {
            return;
        }
        static const wl_registry_listener listener{registryGlobal, registryGlobalRemove};
        wl_registry_add_listener(state.registry, &listener, &state);
        // A roundtrip here can dispatch input-context destruction inside fcitx's UI flush.
        wl_display_flush(display);
    }

    std::string displayKeyFor(const fcitx::InputContext &inputContext) const {
        constexpr std::string_view prefix = "wayland:";
        const auto &display = inputContext.display();
        if (display.rfind(prefix, 0) == 0) {
            return display.substr(prefix.size());
        }
        return {};
    }

    void ensureWaylandPopup(fcitx::InputContext &inputContext) {
        if (!waylandIMAddon_ || inputContext.frontendName() != "wayland_v2") {
            return;
        }
        const auto key = displayKeyFor(inputContext);
        auto displayIter = waylandDisplays_.find(key);
        if (displayIter == waylandDisplays_.end() || !displayIter->second.compositor || !displayIter->second.shm) {
            return;
        }
        auto popupIter = popupByDisplay_.find(key);
        if (popupIter != popupByDisplay_.end()) {
            if (popupIter->second.lastInputContext.get() == &inputContext) {
                renderPopup(inputContext, displayIter->second, popupIter->second);
                return;
            }
            logLine("popup\tinput-context-changed-recreate\tdisplay=" + key);
            popupByDisplay_.erase(popupIter);
        }
        auto *inputMethod = waylandIMAddon_->call<fcitx::IWaylandIMModule::getInputMethodV2>(&inputContext);
        if (!inputMethod) {
            logLine("popup\tmissing-input-method\tdisplay=" + key);
            return;
        }

        auto *rawSurface = wl_compositor_create_surface(displayIter->second.compositor);
        if (!rawSurface) {
            logLine("popup\tmissing-surface\tdisplay=" + key);
            return;
        }

        PopupState popup;
        popup.surface = std::make_unique<fcitx::wayland::WlSurface>(rawSurface);
        popup.surface->preferredBufferScale().connect([this, key](int32_t scale) {
            auto popupIter = popupByDisplay_.find(key);
            if (popupIter == popupByDisplay_.end()) {
                return;
            }
            popupIter->second.preferredScale = std::clamp(static_cast<int>(scale), 1, 4);
            logLine("popup\tpreferred-scale\tdisplay=" + key + "\tscale=" +
                    std::to_string(popupIter->second.preferredScale));
        });
        popup.popup.reset(inputMethod->getInputPopupSurface(popup.surface.get()));
        if (!popup.popup) {
            logLine("popup\tmissing-popup-surface\tdisplay=" + key);
            return;
        }
        popup.popup->textInputRectangle().connect([this, key](int32_t x, int32_t y, int32_t width, int32_t height) {
            auto popupIter = popupByDisplay_.find(key);
            if (popupIter == popupByDisplay_.end()) {
                return;
            }
            const std::array<int32_t, 4> rect{x, y, width, height};
            const bool duplicateRect = popupIter->second.lastTextRect == rect;
            if (duplicateRect && !popupIter->second.awaitingFreshTextRect &&
                !popupIter->second.candidateTextRectStale &&
                popupIter->second.pendingStatusText.empty()) {
                return;
            }
            popupIter->second.lastTextRect = rect;
            popupIter->second.awaitingFreshTextRect = false;
            popupIter->second.candidateTextRectStale = false;
            if (verbosePanelLoggingEnabled()) {
                logLine(std::string("popup\t") + (duplicateRect ? "text-rect-refresh" : "text-rect") +
                        "\tdisplay=" + key + "\tx=" + std::to_string(x) + "\ty=" + std::to_string(y) +
                        "\tw=" + std::to_string(width) + "\th=" + std::to_string(height));
            }
            if (!popupIter->second.pendingStatusText.empty()) {
                auto displayIter = waylandDisplays_.find(key);
                if (displayIter != waylandDisplays_.end()) {
                    const auto statusText = popupIter->second.pendingStatusText;
                    const double inputScale = popupIter->second.pendingInputScale;
                    popupIter->second.pendingStatusText.clear();
                    const bool waylandRendered =
                        renderStatusPopupOrFallback(displayIter->second, popupIter->second, statusText, inputScale);
                    logLine(std::string("popup\t") +
                            (waylandRendered ? "deferred-status-rendered" : "deferred-status-edge-fallback") +
                            "\tdisplay=" + key);
                }
            }
            scheduleTextRectRerender(key);
        });
        auto inserted = popupByDisplay_.emplace(key, std::move(popup));
        auto insertedIter = inserted.first;
        insertedIter->second.lastInputContext = inputContext.watch();
        insertedIter->second.awaitingFreshTextRect = true;
        insertedIter->second.candidateTextRectStale = true;
        insertedIter->second.surface->commit();
        if (displayIter->second.display) {
            // Flush requests without synchronously dispatching nested Wayland events.
            wl_display_flush(displayIter->second.display);
        }
        logLine("popup\tcreated\tdisplay=" + key);
        if (!insertedIter->second.lastTextRect) {
            const auto &panel = inputContext.inputPanel();
            const auto auxUp = outputText(inputContext, panel.auxUp()).toString();
            auto candidates = panel.candidateList();
            if ((!candidates || candidates->size() == 0) && statusFromAux(auxUp)) {
                renderStatusPopup(displayIter->second, insertedIter->second, auxUp, inputContext.scaleFactor());
                logLine("popup\tstatus-first-render\ttext=" + auxUp + "\tdisplay=" + key);
            } else {
                logLine("popup\tdefer-first-render\tdisplay=" + key);
            }
            return;
        }
        renderPopup(inputContext, displayIter->second, insertedIter->second);
    }

    static int createAnonymousFile(std::size_t size) {
        int fd = memfd_create("tipe-popup-buffer", MFD_CLOEXEC);
        if (fd < 0) {
            return -1;
        }
        if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    static void destroyPopupBuffer(PopupState &popup) {
        popup.releaseBuffer();
    }

    bool ensurePopupBuffer(WaylandDisplayState &display, PopupState &popup, int width, int height, int scale) const {
        const int bufferWidth = width * scale;
        const int bufferHeight = height * scale;
        if (popup.buffer && popup.width == width && popup.height == height && popup.bufferScale == scale) {
            return true;
        }
        destroyPopupBuffer(popup);
        const int stride = bufferWidth * 4;
        const auto size = static_cast<std::size_t>(stride * bufferHeight);
        const int fd = createAnonymousFile(size);
        if (fd < 0) {
            return false;
        }
        void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return false;
        }
        auto *pool = wl_shm_create_pool(display.shm, fd, static_cast<int>(size));
        close(fd);
        if (!pool) {
            munmap(data, size);
            return false;
        }
        popup.buffer = wl_shm_pool_create_buffer(pool, 0, bufferWidth, bufferHeight, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (!popup.buffer) {
            munmap(data, size);
            return false;
        }
        popup.bufferData = data;
        popup.bufferSize = size;
        popup.width = width;
        popup.height = height;
        popup.bufferWidth = bufferWidth;
        popup.bufferHeight = bufferHeight;
        popup.bufferScale = scale;
        return true;
    }

    static bool expandedFromAux(std::string_view aux) {
        return aux.find("__tipe_ui_state\texpanded\t1") != std::string_view::npos;
    }

    static bool continuousFromAux(std::string_view aux) {
        return aux.find("\tcontinuous\t1") != std::string_view::npos;
    }

    static bool statusFromAux(std::string_view aux) {
        return aux == "TiPE" || aux == "Eng" || aux == "Auto" || aux == "Manual";
    }

    static bool auxHasTipeUIState(std::string_view aux) {
        return aux.find("__tipe_ui_state\t") != std::string_view::npos;
    }

    bool inputPanelEmpty(fcitx::InputContext &inputContext) const {
        const auto &panel = inputContext.inputPanel();
        if (!outputText(inputContext, panel.preedit()).toString().empty() ||
            !outputText(inputContext, panel.clientPreedit()).toString().empty()) {
            return false;
        }
        const auto auxUp = outputText(inputContext, panel.auxUp()).toString();
        if (statusFromAux(auxUp) || auxHasTipeUIState(auxUp)) {
            return false;
        }
        auto candidates = panel.candidateList();
        return !candidates || candidates->size() == 0;
    }

    void hidePopup(PopupState &popup) {
        const bool hadVisibleState = popup.surfaceBufferAttached || popup.buffer || !popup.pendingStatusText.empty() ||
                                     popup.fallbackCandidatePid > 0 || popup.fallbackStatusPid > 0;
        if (!hadVisibleState) {
            return;
        }
        popup.pendingStatusText.clear();
        popup.lastStatusText.clear();
        popup.candidateRenderLogged = false;
        popup.candidateHitRegions.clear();
        popup.hoveredCandidateIndex.reset();
        popup.candidateTextRectStale = true;
        closeCandidateFallbackWindow(popup, "hide-popup");
        closeStatusFallbackWindow(popup, "hide-popup");
        if (popup.surfaceBufferAttached) {
            wl_surface_attach(static_cast<wl_surface *>(*popup.surface), nullptr, 0, 0);
            popup.surface->commit();
            popup.surfaceBufferAttached = false;
        }
        destroyPopupBuffer(popup);
        logLine("popup\thidden\ttext-rect-stale=1");
    }

    void hidePopupForInputContext(fcitx::InputContext &inputContext) {
        const auto key = displayKeyFor(inputContext);
        auto popupIter = popupByDisplay_.find(key);
        if (popupIter == popupByDisplay_.end() || popupIter->second.lastInputContext.get() != &inputContext) {
            return;
        }
        hidePopup(popupIter->second);
    }

    void hideAllPopups() {
        hideWaylandPopups();
        for (auto &[_, popup] : popupByDisplay_) {
            popup.lastInputContext.unwatch();
            popup.lastTextRect.reset();
            popup.awaitingFreshTextRect = true;
            popup.lastFallbackCandidateSnapshot.clear();
        }
        hideGenericFallback();
    }

    void onInputContextDestroyed(fcitx::Event &event) {
        auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
        if (!inputContext) {
            return;
        }
        if (genericFallbackPopup_.lastInputContext.get() == inputContext) {
            hideGenericFallback();
        }
        for (auto iter = popupByDisplay_.begin(); iter != popupByDisplay_.end();) {
            if (iter->second.lastInputContext.get() != inputContext) {
                ++iter;
                continue;
            }
            logLine("popup\tinput-context-destroyed\tdisplay=" + iter->first);
            iter = popupByDisplay_.erase(iter);
        }
    }

    static int bufferScaleFor(const fcitx::InputContext &inputContext, const PopupState &popup) {
        return tipeUIBufferScaleFor(inputContext.scaleFactor(), popup.preferredScale);
    }

    void renderPopup(fcitx::InputContext &inputContext, WaylandDisplayState &display, PopupState &popup) {
        const auto &panel = inputContext.inputPanel();
        auto candidates = panel.candidateList();
        const auto auxUp = outputText(inputContext, panel.auxUp()).toString();
        if (!candidates || candidates->size() == 0) {
            popup.candidateRenderLogged = false;
            popup.candidateHitRegions.clear();
            popup.hoveredCandidateIndex.reset();
            if (!statusFromAux(auxUp)) {
                if (!auxUp.empty()) {
                    logLine("popup\tignore-foreign-aux\ttext=" + auxUp);
                }
                hidePopup(popup);
                return;
            }
            closeCandidateFallbackWindow(popup, "status-popup");
            if (popup.awaitingFreshTextRect || !popup.lastTextRect) {
                if (popup.surfaceBufferAttached && popup.lastStatusText == auxUp) {
                    logLine("popup\tstatus-keep-first-render\ttext=" + auxUp);
                    return;
                }
                destroyPopupBuffer(popup);
                wl_surface_attach(static_cast<wl_surface *>(*popup.surface), nullptr, 0, 0);
                popup.surface->commit();
                popup.surfaceBufferAttached = false;
                popup.pendingStatusText = auxUp;
                popup.pendingInputScale = inputContext.scaleFactor();
                popup.lastStatusText.clear();
                logLine(std::string("popup\tdefer-status-render\t") +
                        (popup.awaitingFreshTextRect ? "awaiting-fresh-text-rect" : "missing-text-rect") +
                        "\ttext=" + auxUp);
                return;
            }
            if (popup.candidateTextRectStale) {
                logLine("popup\tstatus-render-with-candidate-stale-rect\ttext=" + auxUp);
            }
            renderStatusPopupOrFallback(display, popup, auxUp, inputContext.scaleFactor());
            return;
        }

        closeStatusFallbackWindow(popup, "candidate-popup");
        const auto preeditText = outputText(inputContext, panel.preedit());
        const auto preedit = preeditText.toString();
        const int preeditCursor = std::clamp(preeditText.cursor(), 0, static_cast<int>(preedit.size()));
        const bool expanded = expandedFromAux(auxUp);
        const bool continuous = continuousFromAux(auxUp);
        std::vector<std::string> candidateTexts;
        candidateTexts.reserve(candidates->size());
        for (int index = 0; index < candidates->size(); ++index) {
            candidateTexts.push_back(outputText(inputContext, candidates->candidate(index).text()).toString());
        }
        const int cursor = std::clamp(candidates->cursorIndex(), 0, std::max(0, candidates->size() - 1));
        const auto cells = visibleVisualCellsFor(candidateTexts, static_cast<std::size_t>(cursor), expanded,
                                                 tipeUIPanelMaxExpandedRows);
        const auto metrics = tipeUIPanelMetricsFor(cells, candidateTexts, expanded, !preedit.empty());
        const int width = metrics.width;
        const int height = metrics.height;

        const int scale = bufferScaleFor(inputContext, popup);
        popup.lastStatusText.clear();
        const auto fallbackRect = (popup.awaitingFreshTextRect || popup.candidateTextRectStale)
                                      ? std::optional<std::array<int32_t, 4>>{}
                                      : popup.lastTextRect;
        if (popup.awaitingFreshTextRect || popup.candidateTextRectStale) {
            destroyPopupBuffer(popup);
            wl_surface_attach(static_cast<wl_surface *>(*popup.surface), nullptr, 0, 0);
            popup.surface->commit();
            popup.surfaceBufferAttached = false;
            closeCandidateFallbackWindow(popup, "awaiting-fresh-text-rect");
            logLine(std::string("popup\tdefer-candidate-render\t") +
                    (popup.awaitingFreshTextRect ? "awaiting-fresh-text-rect" : "stale-text-rect"));
            return;
        }
        if (fallbackRect && candidateEdgeFallbackNeededForRect(*fallbackRect, width, height)) {
            destroyPopupBuffer(popup);
            wl_surface_attach(static_cast<wl_surface *>(*popup.surface), nullptr, 0, 0);
            popup.surface->commit();
            popup.surfaceBufferAttached = false;
            if (++nextFallbackSnapshotSerial_ <= 0) {
                nextFallbackSnapshotSerial_ = 1;
            }
            popup.fallbackSnapshotSerial = nextFallbackSnapshotSerial_;
            const auto snapshot = buildCandidateSnapshotLine(
                preedit, expanded, static_cast<std::size_t>(cursor),
                {(*fallbackRect)[0], (*fallbackRect)[1], (*fallbackRect)[2], (*fallbackRect)[3]}, candidateTexts,
                std::string("continuous=") + (continuous ? "1" : "0") +
                    ",preedit_cursor=" + std::to_string(preeditCursor) +
                    ",snapshot=" + std::to_string(popup.fallbackSnapshotSerial));
            showCandidateFallbackWindow(popup, snapshot);
            return;
        }
        closeCandidateFallbackWindow(popup, "wayland-popup");
        if (!ensurePopupBuffer(display, popup, width, height, scale)) {
            logLine("popup\tbuffer-failed");
            return;
        }
        std::memset(popup.bufferData, 0, popup.bufferSize);
        auto *surface = cairo_image_surface_create_for_data(static_cast<unsigned char *>(popup.bufferData),
                                                            CAIRO_FORMAT_ARGB32, popup.bufferWidth,
                                                            popup.bufferHeight, popup.bufferWidth * 4);
        auto *cr = cairo_create(surface);
        cairo_scale(cr, scale, scale);
        popup.candidateHitRegions = candidatePanelHitRegions(cells, candidateTexts, width, expanded, !preedit.empty());
        if (popup.surface && display.pointerSurface == static_cast<wl_surface *>(*popup.surface)) {
            popup.hoveredCandidateIndex =
                candidateIndexAtPoint(popup.candidateHitRegions, display.pointerX, display.pointerY);
        } else {
            popup.hoveredCandidateIndex.reset();
        }
        const int visualColumnWidth = tipeUIVisualColumnWidthFor(width);
        auto renderResult = renderCandidatePanel(cr, width, height, preedit, preeditCursor, candidateTexts,
                                                 static_cast<std::size_t>(cursor), expanded, continuous,
                                                 popup.hoveredCandidateIndex);
        popup.candidateHitRegions = std::move(renderResult.hitRegions);

        cairo_destroy(cr);
        cairo_surface_flush(surface);
        cairo_surface_destroy(surface);
        popup.surface->setBufferScale(scale);
        wl_surface_attach(static_cast<wl_surface *>(*popup.surface), popup.buffer, 0, 0);
        popup.surface->damageBuffer(0, 0, popup.bufferWidth, popup.bufferHeight);
        popup.surface->commit();
        popup.surfaceBufferAttached = true;
        if (verbosePanelLoggingEnabled() || !renderResult.boundsOk || !popup.candidateRenderLogged) {
            logLine("popup\trendered\tw=" + std::to_string(width) + "\th=" + std::to_string(height) +
                    "\tscale=" + std::to_string(scale) + "\tpixelW=" + std::to_string(popup.bufferWidth) +
                    "\tpixelH=" + std::to_string(popup.bufferHeight) +
                    "\tcandidates=" + std::to_string(candidates->size()) + "\tcursor=" + std::to_string(cursor) +
                    "\tpreeditCursor=" + std::to_string(preeditCursor) +
                    "\texpanded=" + std::to_string(expanded ? 1 : 0) +
                    "\trows=" + std::to_string(metrics.visibleRows) +
                    "\tcolumnW=" + std::to_string(visualColumnWidth) +
                    "\tmaxRight=" + std::to_string(renderResult.maxDrawRight) +
                    "\tboundsOk=" + std::to_string(renderResult.boundsOk ? 1 : 0));
        }
        popup.candidateRenderLogged = true;
    }

    bool renderStatusPopup(fcitx::InputContext &inputContext, WaylandDisplayState &display, PopupState &popup,
                           const std::string &statusText) {
        return renderStatusPopupOrFallback(display, popup, statusText, inputContext.scaleFactor());
    }

    bool renderStatusPopupOrFallback(WaylandDisplayState &display, PopupState &popup, const std::string &statusText,
                                     double inputScale) {
        if (popup.lastTextRect && statusEdgeFallbackNeededForRect(*popup.lastTextRect)) {
            destroyPopupBuffer(popup);
            wl_surface_attach(static_cast<wl_surface *>(*popup.surface), nullptr, 0, 0);
            popup.surface->commit();
            popup.surfaceBufferAttached = false;
            popup.lastStatusText.clear();
            showStatusFallbackWindow(popup, statusText, *popup.lastTextRect);
            return false;
        }
        return renderStatusPopup(display, popup, statusText, inputScale);
    }

    bool renderStatusPopup(WaylandDisplayState &display, PopupState &popup, const std::string &statusText,
                           double inputScale) {
        const int width = tipeUIStatusPopupWidth;
        const int height = tipeUIStatusPopupHeight;
        const int scale = tipeUIBufferScaleFor(inputScale, popup.preferredScale);
        if (popup.buffer && popup.width == width && popup.height == height && popup.bufferScale == scale &&
            popup.lastStatusText == statusText) {
            if (verbosePanelLoggingEnabled()) {
                logLine("popup\tstatus-skip-duplicate\tw=" + std::to_string(width) +
                        "\th=" + std::to_string(height) + "\tscale=" + std::to_string(scale));
            }
            return true;
        }
        if (!ensurePopupBuffer(display, popup, width, height, scale)) {
            logLine("popup\tstatus-buffer-failed");
            return true;
        }
        closeStatusFallbackWindow(popup, "wayland-status-popup");

        std::memset(popup.bufferData, 0, popup.bufferSize);
        auto *surface = cairo_image_surface_create_for_data(static_cast<unsigned char *>(popup.bufferData),
                                                            CAIRO_FORMAT_ARGB32, popup.bufferWidth,
                                                            popup.bufferHeight, popup.bufferWidth * 4);
        auto *cr = cairo_create(surface);
        cairo_scale(cr, scale, scale);
        renderCandidateStatus(cr, width, height, statusText);

        cairo_destroy(cr);
        cairo_surface_flush(surface);
        cairo_surface_destroy(surface);
        popup.surface->setBufferScale(scale);
        wl_surface_attach(static_cast<wl_surface *>(*popup.surface), popup.buffer, 0, 0);
        popup.surface->damageBuffer(0, 0, popup.bufferWidth, popup.bufferHeight);
        popup.surface->commit();
        popup.surfaceBufferAttached = true;
        popup.lastStatusText = statusText;
        logLine("popup\tstatus-rendered\tw=" + std::to_string(width) + "\th=" + std::to_string(height) +
                "\tscale=" + std::to_string(scale) + "\tpixelW=" + std::to_string(popup.bufferWidth) +
                "\tpixelH=" + std::to_string(popup.bufferHeight));
        return true;
    }

    fcitx::Text outputText(fcitx::InputContext &inputContext, const fcitx::Text &text) const {
        return instance_ ? instance_->outputFilter(&inputContext, text) : text;
    }

    void logInputPanel(fcitx::InputContext &inputContext) const {
        if (!verbosePanelLoggingEnabled()) {
            return;
        }
        const auto path = logPath();
        if (path.empty()) {
            return;
        }

        std::ostringstream log;

        const auto &panel = inputContext.inputPanel();
        log << "update"
            << "\tfrontend=" << inputContext.frontendName() << "\tprogram=" << inputContext.program()
            << "\tdisplay=" << inputContext.display() << "\tpreedit="
            << outputText(inputContext, panel.preedit()).toString() << "\tclientPreedit="
            << outputText(inputContext, panel.clientPreedit()).toString() << "\tauxUp="
            << outputText(inputContext, panel.auxUp()).toString() << '\n';

        if (auto candidates = panel.candidateList()) {
            log << "candidates"
                << "\tsize=" << candidates->size() << "\tcursor=" << candidates->cursorIndex() << '\n';
            for (int index = 0; index < candidates->size(); ++index) {
                log << "candidate"
                    << '\t' << index << '\t'
                    << outputText(inputContext, candidates->label(index)).toString() << '\t'
                    << outputText(inputContext, candidates->candidate(index).text()).toString() << '\n';
            }
        } else {
            log << "candidates"
                << "\tsize=0"
                << "\tcursor=-1" << '\n';
        }
        appendBoundedDiagnosticLog(path, log.str());
    }

    fcitx::Instance *instance_ = nullptr;
    fcitx::AddonInstance *waylandIMAddon_ = nullptr;
    bool suspended_ = true;
    std::unordered_map<std::string, WaylandDisplayState> waylandDisplays_;
    std::unordered_map<std::string, PopupState> popupByDisplay_;
    PopupState genericFallbackPopup_;
    int nextFallbackSnapshotSerial_ = 0;
    std::unordered_set<std::string> pendingWaylandPopupDisplays_;
    std::string pendingTextRectRerenderDisplay_;
    bool deferredTextRectRerenderPending_ = false;
    std::unique_ptr<fcitx::EventSource> deferredTextRectRerender_;
    bool deferredWaylandPopupRetryPending_ = false;
    std::unique_ptr<fcitx::EventSource> deferredWaylandPopupRetry_;
    std::unique_ptr<fcitx::EventSource> fallbackSelectionEvent_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> inputContextDestroyedWatcher_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionCreated>> waylandCreatedCallback_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionClosed>> waylandClosedCallback_;
};

class TipeUIFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new TipeUI(manager);
    }
};

} // namespace tipe

FCITX_ADDON_FACTORY_V2(tipeui, tipe::TipeUIFactory)
