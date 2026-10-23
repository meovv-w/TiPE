#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/rect.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/key.h>
#include <testfrontend_public.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <fcitx-utils/event.h>
#include <vector>

using namespace fcitx;

#ifndef TIPE_TEST_BINARY_DIR
#define TIPE_TEST_BINARY_DIR "."
#endif

namespace {
struct ProtocolCase {
    const char *name;
    const char *program;
    CapabilityFlags capabilities;
};

bool sendKey(AddonInstance *frontend, const ICUUID &uuid,
             const std::string &value) {
    const bool accepted = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, Key(value), false);
    // A release is deliberately sent as well: GTK and Qt normally report both
    // phases, while some terminal paths only report a press.
    const bool releaseAccepted = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, Key(value), true);
    return accepted && !releaseAccepted;
}

bool runCase(Instance &instance, AddonInstance *frontend,
             const ProtocolCase &protocol, int caseNumber) {
    const auto uuid = frontend->call<ITestFrontend::createInputContext>(
        protocol.program);
    auto *ic = instance.inputContextManager().findByUUID(uuid);
    if (!ic) {
        std::cerr << "FAIL protocol=" << protocol.name
                  << " reason=input-context-not-created\n";
        return false;
    }
    ic->setCapabilityFlags(protocol.capabilities);
    // Exercise the text-rectangle path used by GTK/Qt and the fallback path
    // used by terminal frontends. This must never require a compositor query.
    ic->setCursorRect(Rect(20 + caseNumber * 13, 40, 8, 18));

    frontend->call<ITestFrontend::pushCommitExpectation>("你好");
    for (const char c : std::string("nihao")) {
        if (!sendKey(frontend, uuid, std::string(1, c))) return false;
    }
    if (!sendKey(frontend, uuid, "space")) return false;

    frontend->call<ITestFrontend::pushCommitExpectation>("github");
    for (const char c : std::string("github")) {
        if (!sendKey(frontend, uuid, std::string(1, c))) return false;
    }
    if (!sendKey(frontend, uuid, "space")) return false;

    frontend->call<ITestFrontend::destroyInputContext>(uuid);
    std::cout << "PASS protocol=" << protocol.name << " program="
              << protocol.program << " capabilities="
                  << protocol.capabilities.toInteger() << "\n";
    return true;
}
} // namespace

int main() {
    const std::string stateDir =
        std::string(TIPE_TEST_BINARY_DIR) + "/tipe-protocol-state";
    std::filesystem::remove_all(stateDir);
    std::filesystem::create_directories(stateDir);
    setenv("XDG_STATE_HOME", stateDir.c_str(), 1);
    setupTestingEnvironmentPath(
        TIPE_TEST_BINARY_DIR, {"/usr/lib64/fcitx5", TIPE_TEST_BINARY_DIR},
        {"/usr/share/fcitx5", "/usr/share/fcitx5/testing",
         std::string(TIPE_TEST_BINARY_DIR) + "/test"});

    char a0[] = "tipe-protocol-tests";
    char a1[] = "--disable=all";
    char a2[] = "--enable=testfrontend,tipe";
    char *argv[] = {a0, a1, a2, nullptr};
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    auto run_checks = [&]() -> int {

    auto *frontend = instance.addonManager().addon("testfrontend", true);
    auto *tipe = instance.addonManager().addon("tipe", true);
    if (!frontend || !tipe) {
        std::cerr << "FAIL protocol=bootstrap reason=addon-not-loaded\n";
        return 2;
    }

    auto &methods = instance.inputMethodManager();
    methods.reset([](auto &manager) { manager.addEmptyGroup("Default"); });
    methods.setCurrentGroup("Default");
    InputMethodGroup group("Default");
    group.inputMethodList().emplace_back("tipe");
    group.setDefaultInputMethod("tipe");
    methods.setGroup(std::move(group));

    const std::vector<ProtocolCase> cases = {
        {"gtk", "gtk3-regression", {CapabilityFlag::Preedit,
                                      CapabilityFlag::FormattedPreedit,
                                      CapabilityFlag::SurroundingText,
                                      CapabilityFlag::KeyEventOrderFix}},
        {"qt", "qt6-regression", {CapabilityFlag::Preedit,
                                     CapabilityFlag::SurroundingText,
                                     CapabilityFlag::CommitStringWithCursor,
                                     CapabilityFlag::KeyEventOrderFix}},
        {"terminal", "foot-regression", {CapabilityFlag::Terminal,
                                          CapabilityFlag::Preedit,
                                          CapabilityFlag::KeyEventOrderFix}},
    };
    bool ok = true;
    for (size_t i = 0; i < cases.size(); ++i) {
        ok = runCase(instance, frontend, cases[i], static_cast<int>(i)) && ok;
    }
    return ok ? 0 : 1;
    };
    int result = 1;
    auto checks = instance.eventLoop().addDeferEvent([&](EventSource *source) {
        source->setEnabled(false);
        result = run_checks();
        instance.exit(result);
        return true;
    });
    instance.exec();
    return result;
}
