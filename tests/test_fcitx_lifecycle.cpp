#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/testing.h>
#include <testfrontend_public.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <fstream>
#include <thread>

using namespace fcitx;

#ifndef TIPE_TEST_BINARY_DIR
#define TIPE_TEST_BINARY_DIR "."
#endif

namespace {
class NativeTerminalContext final : public InputContext {
public:
    explicit NativeTerminalContext(InputContextManager &manager,
                                   std::string frontend = "wayland_v2")
        : InputContext(manager, "protocol-focus-regression"), frontend_(std::move(frontend)) { created(); }
    ~NativeTerminalContext() override { destroy(); }
    const char *frontend() const override { return frontend_.c_str(); }
    std::vector<std::string> commits;
protected:
    void commitStringImpl(const std::string &text) override { commits.push_back(text); }
    void forwardKeyImpl(const ForwardKeyEvent &) override {}
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void updatePreeditImpl() override {}
private:
    std::string frontend_;
};

bool sendKey(AddonInstance *frontend, const ICUUID &uuid,
             const std::string &value, bool expectedPress = true) {
    const Key key(value);
    if (!key.isValid()) return false;
    const bool press = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, key, false);
    const bool release = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, key, true);
    // TiPE consumes key-downs represented by a View and leaves releases to
    // the client. A modifier-only Shift tap is the one deliberate exception:
    // its release toggles mode and is consumed.
    return press == expectedPress && (!release || value == "Shift_L");
}
}

int main() {
    const std::string stateDir =
        std::string(TIPE_TEST_BINARY_DIR) + "/tipe-fcitx-lifecycle-state";
    std::filesystem::remove_all(stateDir);
    std::filesystem::create_directories(stateDir);
    setenv("XDG_STATE_HOME", stateDir.c_str(), 1);
    setupTestingEnvironmentPath(
        TIPE_TEST_BINARY_DIR, {"/usr/lib64/fcitx5", TIPE_TEST_BINARY_DIR},
        {"/usr/share/fcitx5", "/usr/share/fcitx5/testing",
         std::string(TIPE_TEST_BINARY_DIR) + "/test"});

    char a0[] = "tipe-fcitx-lifecycle-tests";
    char a1[] = "--disable=all";
    // testim is Fcitx's in-tree test input method. It gives this test a real
    // second input method, so switching away from TiPE exercises deactivate()
    // and the same lifecycle events a desktop IM switch uses.
    char a2[] = "--enable=testfrontend,tipe,testim";
    char *argv[] = {a0, a1, a2, nullptr};
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);

    int result = 1;
    auto checks = instance.eventLoop().addDeferEvent([&](EventSource *source) {
        source->setEnabled(false);
        auto *frontend = instance.addonManager().addon("testfrontend", true);
        auto *tipe = instance.addonManager().addon("tipe", true);
        auto *testim = instance.addonManager().addon("testim", true);
        if (!frontend || !tipe || !testim) {
            // Keep package-minimal builds green while making the omission
            // explicit. The normal testing package provides testim.
            std::cout << "SKIP lifecycle reason=testim-addon-unavailable\n";
            result = 0;
            instance.exit(result);
            return true;
        }

        std::string other;
        instance.inputMethodManager().foreachEntries(
            [&](const InputMethodEntry &entry) {
                if (entry.addon() == "testim") other = entry.uniqueName();
                return true;
            });
        if (other.empty()) {
            std::cout << "SKIP lifecycle reason=testim-entry-unavailable\n";
            result = 0;
            instance.exit(result);
            return true;
        }

        auto &methods = instance.inputMethodManager();
        methods.reset([](auto &manager) { manager.addEmptyGroup("Default"); });
        methods.setCurrentGroup("Default");
        InputMethodGroup group("Default");
        group.inputMethodList().emplace_back("tipe");
        group.inputMethodList().emplace_back(other);
        group.setDefaultInputMethod("tipe");
        methods.setGroup(std::move(group));

        auto fail = [&](const char *why) {
            std::cerr << "FAIL lifecycle reason=" << why << "\n";
            result = 1;
            instance.exit(result);
        };

        const auto uuidA = frontend->call<ITestFrontend::createInputContext>(
            "tipe-lifecycle-a");
        auto *icA = instance.inputContextManager().findByUUID(uuidA);
        if (!icA) {
            fail("context-a-not-created");
            return true;
        }
        icA->setCapabilityFlags(CapabilityFlags{
            CapabilityFlag::Preedit, CapabilityFlag::SurroundingText});
        instance.setCurrentInputMethod(icA, "tipe", true);

        // A mode switch with an unfinished spelling creates suspended state.
        // Switching to another IM must deactivate TiPE and clear that state.
        if (!sendKey(frontend, uuidA, "n") ||
            !sendKey(frontend, uuidA, "i")) {
            fail("composition-a-not-accepted");
            return true;
        }
        auto *entryA = instance.inputMethodEntry(icA);
        auto *engineA = instance.inputMethodEngine(icA);
        if (!entryA || !engineA || engineA->subMode(*entryA, *icA) != "中文") {
            fail("initial-submode-not-chinese");
            return true;
        }
        if (!sendKey(frontend, uuidA, "Shift_L", false)) {
            fail("shift-mode-switch-not-consumed");
            return true;
        }
        entryA = instance.inputMethodEntry(icA);
        engineA = instance.inputMethodEngine(icA);
        if (!entryA || !engineA || engineA->subMode(*entryA, *icA) != "英文") {
            fail("english-submode-not-visible");
            return true;
        }
        // Switching away from TiPE must not silently discard an unfinished
        // spelling. The deactivation path commits the suspended text
        // literally before destroying the per-IM state.
        frontend->call<ITestFrontend::pushCommitExpectation>("ni");
        instance.setCurrentInputMethod(icA, other, true);
        if (instance.inputMethod(icA) != other ||
            !icA->inputPanel().preedit().toString().empty()) {
            fail("switch-away-did-not-reset-panel");
            return true;
        }

        // Switching back creates a fresh ContextState. Space must pass
        // through (there is no suspended composition), and the submode must
        // be Chinese regardless of the mode used before deactivation.
        instance.setCurrentInputMethod(icA, "tipe", true);
        if (instance.inputMethod(icA) != "tipe") {
            fail("switch-back-to-tipe-failed");
            return true;
        }
        entryA = instance.inputMethodEntry(icA);
        engineA = instance.inputMethodEngine(icA);
        if (!entryA || !engineA || engineA->subMode(*entryA, *icA) != "中文" ||
            !icA->inputPanel().preedit().toString().empty() ||
            !sendKey(frontend, uuidA, "space", false)) {
            fail("stale-mode-or-composition-leaked-after-switch");
            return true;
        }

        // Build a committed chunk, then switch input methods. Backspace after
        // switching back must not resurrect TiPE's restoration transaction.
        for (const char c : std::string("nihao"))
            if (!sendKey(frontend, uuidA, std::string(1, c))) {
                fail("post-switch-composition-a-not-accepted");
                return true;
            }
        frontend->call<ITestFrontend::pushCommitExpectation>("你好");
        if (!sendKey(frontend, uuidA, "space")) {
            fail("post-switch-commit-a-failed");
            return true;
        }
        icA->surroundingText().setText("你好", 2, 2);
        instance.setCurrentInputMethod(icA, other, true);
        instance.setCurrentInputMethod(icA, "tipe", true);
        icA->surroundingText().setText("你好", 2, 2);
        if (!sendKey(frontend, uuidA, "BackSpace", false) ||
            !icA->inputPanel().preedit().toString().empty()) {
            fail("commit-restoration-history-survived-im-switch");
            return true;
        }

        // A second context has independent mode and composition state. Keep A
        // in English while B starts in Chinese, then destroy A and verify B's
        // state remains valid. This also catches stale pointer/address reuse.
        const auto uuidB = frontend->call<ITestFrontend::createInputContext>(
            "tipe-lifecycle-b");
        auto *icB = instance.inputContextManager().findByUUID(uuidB);
        if (!icB) {
            fail("context-b-not-created");
            return true;
        }
        icB->setCapabilityFlags(CapabilityFlags{
            CapabilityFlag::Preedit, CapabilityFlag::SurroundingText});
        instance.setCurrentInputMethod(icB, "tipe", true);
        auto *entryB = instance.inputMethodEntry(icB);
        auto *engineB = instance.inputMethodEngine(icB);
        if (!entryB || !engineB || engineB->subMode(*entryB, *icB) != "中文" ||
            !sendKey(frontend, uuidB, "n") ||
            icB->inputPanel().preedit().toString() != "n") {
            fail("context-b-inherited-a-state");
            return true;
        }
        frontend->call<ITestFrontend::destroyInputContext>(uuidA);
        if (!sendKey(frontend, uuidB, "i") ||
            icB->inputPanel().preedit().toString() != "ni") {
            fail("context-b-broke-after-a-destroy");
            return true;
        }
        frontend->call<ITestFrontend::destroyInputContext>(uuidB);

        // GTK commits its preedit locally before focusOut and advertises
        // ClientUnfocusCommit. Other frontends let Fcitx commit it in its
        // ReservedFirst handler. In both cases deactivate must clear state
        // without producing a second commit. The expectation queue rejects
        // any extra commit, including one containing the same spelling.
        for (const auto [clientCommits, inlinePreedit] :
             {std::pair{false, true}, std::pair{true, true},
              std::pair{false, false}, std::pair{true, false}}) {
            const auto uuid = frontend->call<ITestFrontend::createInputContext>(
                clientCommits ? "tipe-client-focus-commit" : "tipe-server-focus-commit");
            auto *ic = instance.inputContextManager().findByUUID(uuid);
            CapabilityFlags flags{CapabilityFlag::SurroundingText};
            if (inlinePreedit) flags |= CapabilityFlag::Preedit;
            if (clientCommits) flags |= CapabilityFlag::ClientUnfocusCommit;
            ic->setCapabilityFlags(flags);
            instance.setCurrentInputMethod(ic, "tipe", true);
            for (const char c : std::string("nihao")) {
                if (!sendKey(frontend, uuid, std::string(1, c))) {
                    fail("focus-composition-not-accepted");
                    return true;
                }
            }
            frontend->call<ITestFrontend::pushCommitExpectation>("nihao");
            if (clientCommits && inlinePreedit) {
                // Model the client-owned local commit, which is outside the
                // server on a real GTK connection.
                ic->commitString(ic->inputPanel().clientPreedit().toStringForCommit());
            }
            ic->focusOut();
            ic->focusIn();
            if (!ic->inputPanel().clientPreedit().toString().empty() ||
                !sendKey(frontend, uuid, "space", false)) {
                fail("focus-return-resurrected-composition");
                return true;
            }
            frontend->call<ITestFrontend::destroyInputContext>(uuid);
        }
        {
            NativeTerminalContext terminal(instance.inputContextManager());
            terminal.setCapabilityFlags({CapabilityFlag::Preedit,
                CapabilityFlag::Terminal, CapabilityFlag::SurroundingText,
                CapabilityFlag::ClientUnfocusCommit});
            const auto uuid = terminal.uuid();
            instance.setCurrentInputMethod(&terminal, "tipe", true);
            for (const char c : std::string("nihao"))
                if (!sendKey(frontend, uuid, std::string(1, c))) {
                    fail("native-terminal-composition-not-accepted");
                    return true;
                }
            terminal.focusOut();
            if (!terminal.commits.empty() ||
                !terminal.inputPanel().clientPreedit().toString().empty()) {
                fail("native-terminal-focus-out-committed-or-kept-ui");
                return true;
            }
            terminal.focusIn();
            if (terminal.inputPanel().clientPreedit().toString() != "nihao" ||
                !sendKey(frontend, uuid, "Return") ||
                terminal.commits != std::vector<std::string>{"nihao"}) {
                fail("native-terminal-focus-return-lost-composition");
                return true;
            }
            for (const char c : std::string("shijie"))
                sendKey(frontend, uuid, std::string(1, c));
            instance.setCurrentInputMethod(&terminal, other, true);
            instance.setCurrentInputMethod(&terminal, "tipe", true);
            if (terminal.commits != std::vector<std::string>{"nihao", "shijie"} ||
                !sendKey(frontend, uuid, "space", false)) {
                fail("native-terminal-explicit-im-switch-must-flush");
                return true;
            }
            for (const char c : std::string("tianqi"))
                sendKey(frontend, uuid, std::string(1, c));
            terminal.focusOut();
            terminal.setCapabilityFlags({CapabilityFlag::Password,
                                        CapabilityFlag::Preedit});
            terminal.focusIn();
            if (!terminal.inputPanel().clientPreedit().toString().empty() ||
                terminal.commits.size() != 2) {
                fail("native-terminal-suspension-leaked-into-password");
                return true;
            }
        }
        {
            const auto modelPath = stateDir + "/tipe/model.current";
            const auto readModel = [&] {
                std::ifstream file(modelPath);
                return std::string(std::istreambuf_iterator<char>(file), {});
            };
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto before = readModel();
            NativeTerminalContext xim(instance.inputContextManager(), "xim");
            xim.setCapabilityFlags({CapabilityFlag::Preedit});
            instance.setCurrentInputMethod(&xim, "tipe", true);
            const std::string synthetic = "tipeximprivacymarkertest";
            for (const char c : synthetic)
                sendKey(frontend, xim.uuid(), std::string(1, c));
            sendKey(frontend, xim.uuid(), "Return");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            if (xim.commits != std::vector<std::string>{synthetic} || readModel() != before) {
                fail("xim-unmarked-field-was-learned-or-input-was-blocked");
                return true;
            }
        }
        std::cout << "PASS lifecycle context-switch-mode-history-isolation focus-commit-once native-terminal-suspension xim-privacy\n";
        result = 0;
        instance.exit(result);
        return true;
    });
    instance.exec();
    return result;
}
