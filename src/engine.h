#pragma once

#include "candidate_snapshot.h"
#include "pass_through_supervisor.h"
#include "state.h"

#include <fcitx/addonfactory.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx-utils/rect.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tipe {

class Engine final : public fcitx::InputMethodEngine {
public:
    explicit Engine(fcitx::AddonManager *manager);
    ~Engine() override;

    std::vector<fcitx::InputMethodEntry> listInputMethods() override;
    void keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &event) override;
    void activate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;

private:
    enum class SupervisionSnapshotSource {
        Ready,
        ActiveState,
        PassThrough,
    };

    struct PartialCommitUndo {
        std::string committedText;
        std::string preeditBefore;
        std::string preeditAfter;
    };

    struct CursorRectSnapshot {
        fcitx::Rect rect;
        const char *source = "invalid";
    };

    struct LastCursorRect {
        fcitx::Rect rect;
        std::string frontend;
        std::string program;
        std::string display;
        uint64_t savedAtUsec = 0;
    };

    struct PreservedState {
        std::optional<State::RestorableSnapshot> state;
        fcitx::InputContext *inputContext = nullptr;
        std::string preedit;
        std::string frontend;
        std::string program;
        std::string display;
        std::string reason;
        std::string surroundingBefore;
        std::string surroundingAfter;
        bool hasSurrounding = false;
        uint64_t savedAtUsec = 0;
    };

    struct AsyncModelJob {
        AsyncModelJob(fcitx::InputContext *context, std::size_t requestSerial,
                      State::ExternalModelRequest modelRequest, std::string modelCommand)
            : inputContext(context->watch()), serial(requestSerial), request(std::move(modelRequest)),
              command(std::move(modelCommand)) {}

        fcitx::TrackableObjectReference<fcitx::InputContext> inputContext;
        std::size_t serial = 0;
        State::ExternalModelRequest request;
        std::string command;
        std::optional<std::string> output;
        std::atomic<bool> complete{false};
        std::thread worker;
    };

    struct PendingSupervisionSnapshot {
        SupervisionSnapshotSource source = SupervisionSnapshotSource::Ready;
        fcitx::InputContext *owner = nullptr;
        std::string program;
        std::string preedit;
        std::size_t candidateCount = 0;
        bool candidatesExpanded = false;
        std::string payload;
        bool terminal = false;
    };

    State &stateFor(fcitx::InputContext *ic);
    void clearState(fcitx::InputContext *ic);
    void dropState(fcitx::InputContext *ic);
    void preserveRestorableState(fcitx::InputContext *ic, const State &state, std::string_view reason);
    bool restorePreservedState(fcitx::InputContext *ic);
    void applyAction(fcitx::InputContext *ic, fcitx::KeyEvent *event, const Action &action);
    void selectCandidateFromUI(fcitx::InputContext *ic, std::size_t index,
                               std::string_view expectedCandidate);
    void showInputModeStatus(fcitx::InputContext *ic, const char *status);
    void scheduleFocusTransferSuppression();
    void scheduleActivationStatusClear(fcitx::InputContext *ic);
    void clearActivationStatusState(fcitx::InputContext *ic);
    void clearActivationStatusIfCurrent(fcitx::InputContext *ic, std::size_t serial);
    bool activationStatusVisible(fcitx::InputContext *ic) const;
    std::string activationStatusText(fcitx::InputContext *ic) const;
    void updateEnglishPendingPanel(fcitx::InputContext *ic, const State &state);
    void updatePanel(fcitx::InputContext *ic, const State &state);
    void clearPanel(fcitx::InputContext *ic);
    void writeSupervisionSnapshot(fcitx::InputContext *ic, const State &state);
    void writeCompletedSupervisionSnapshot(fcitx::InputContext *ic, const State &state);
    void writePassThroughSupervisionSnapshot(fcitx::InputContext *ic,
                                             const PassThroughSupervisionSnapshot &snapshot);
    void schedulePassThroughSupervisionSnapshot(fcitx::InputContext *ic);
    void persistSupervisionSnapshot(fcitx::InputContext *ic, std::string_view preedit,
                                    std::size_t candidateCount, bool candidatesExpanded,
                                    std::string_view snapshot, bool terminal);
    void scheduleSupervisionSnapshotFlush();
    void flushPendingSupervisionSnapshot();
    void persistSupervisionSnapshotNow(const PendingSupervisionSnapshot &pending);
    void appendSupervisionHistory(std::string_view program, std::string_view preedit,
                                  std::size_t candidateCount, bool candidatesExpanded,
                                  std::string_view snapshot, bool terminal);
    void clearSupervisionSnapshot(fcitx::InputContext *owner = nullptr);
    void updateCandidateWindow(fcitx::InputContext &ic, const State &state);
    void maybeContinuousRerank(fcitx::InputContext *ic, State &state);
    void scheduleDeferredCandidateWindowUpdate(fcitx::InputContext *ic);
    void clearCandidateWindow();
    bool ensureCandidateWindow();
    bool sendCandidateWindowMessage(std::string_view payload, bool retryAfterRestart);
    void closeCandidateWindow();
    void showStatusWindow(fcitx::InputContext &ic, const char *status);
    void closeStatusWindow();
    bool inputContextBlocksSupervision(fcitx::InputContext *ic) const;
    void discardBlockedInputContext(fcitx::InputContext *ic);
    void rememberCursorRect(fcitx::InputContext *ic);
    CursorRectSnapshot cursorRectSnapshotFor(fcitx::InputContext &ic);
    fcitx::Rect cursorRectFor(fcitx::InputContext &ic);
    void onCursorRectChanged(fcitx::Event &event);
    void onInputContextFocusOut(fcitx::Event &event);
    void onInputContextDestroyed(fcitx::Event &event);
    void onInputContextCapabilityChanged(fcitx::Event &event);
    void initializeInputModeControl();
    void refreshInputMode(bool announce);
    void setEnglishMode(bool enabled, bool announce);
    bool onInputModeControlEvent(int fd);
    void passThroughKeyEvent(fcitx::KeyEvent &event);
    void finishPassThroughTracking(fcitx::InputContext *ic, std::string_view reason);
    void initializeAsyncModelResults();
    void startAsyncModelRerank(fcitx::InputContext *ic, State::ExternalModelRequest request,
                               std::string command);
    bool onAsyncModelResultEvent(int fd);
    void collectAsyncModelResults();
    void stopAsyncModelJobs();

    std::unordered_map<fcitx::InputContext *, State> states_;
    std::unordered_set<fcitx::InputContext *> blockedInputContexts_;
    std::unordered_map<fcitx::InputContext *, State::SessionContext> sessionContexts_;
    std::unordered_map<fcitx::InputContext *, PassThroughSupervisor> passThroughSupervisors_;
    std::unordered_map<fcitx::InputContext *, fcitx::Rect> cursorRects_;
    std::unordered_map<fcitx::InputContext *, CandidateCursorFollowTracker> cursorFollowTrackers_;
    std::optional<LastCursorRect> lastCursorRect_;
    std::unordered_map<fcitx::InputContext *, PartialCommitUndo> partialCommitUndo_;
    std::unordered_map<fcitx::InputContext *, std::string> lastContinuousPreedit_;
    std::unordered_map<fcitx::InputContext *, std::unique_ptr<fcitx::EventSourceTime>> activationStatusClearTimers_;
    std::unordered_map<fcitx::InputContext *, std::size_t> activationStatusSerials_;
    std::unordered_map<fcitx::InputContext *, std::string> activationStatusTexts_;
    std::unordered_set<fcitx::InputContext *> preservePreeditUntilNextKey_;
    std::unordered_set<fcitx::InputContext *> modelRerankHeld_;
    std::unordered_map<fcitx::InputContext *, std::size_t> asyncModelSerials_;
    std::unordered_map<fcitx::InputContext *, uint64_t> lastModelRerankUsec_;
    std::vector<std::shared_ptr<AsyncModelJob>> asyncModelJobs_;
    std::vector<PreservedState> preservedStates_;
    fcitx::InputContext *supervisionSnapshotOwner_ = nullptr;
    std::optional<PendingSupervisionSnapshot> pendingSupervisionSnapshot_;
    std::string lastSupervisionHistorySnapshot_;
    uint64_t lastSupervisionHistoryUsec_ = 0;
    bool continuousMode_ = false;
    bool englishMode_ = false;
    std::filesystem::path inputModePath_;
    fcitx::Instance *instance_ = nullptr;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> cursorRectWatcher_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> inputContextFocusOutWatcher_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> inputContextDestroyedWatcher_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> inputContextCapabilityChangedWatcher_;
    std::unique_ptr<fcitx::EventSourceIO> inputModeWatcher_;
    std::unique_ptr<fcitx::EventSourceIO> asyncModelResultWatcher_;
    std::unique_ptr<fcitx::EventSourceTime> focusTransferTimer_;
    std::unique_ptr<fcitx::EventSourceTime> supervisionSnapshotFlushTimer_;
    std::unique_ptr<fcitx::EventSource> deferredCandidateWindowUpdate_;
    int inputModeWatchFd_ = -1;
    int asyncModelResultFd_ = -1;
    int candidateWindowFd_ = -1;
    pid_t candidateWindowPid_ = -1;
    pid_t statusWindowPid_ = -1;
};

class EngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};

} // namespace tipe
