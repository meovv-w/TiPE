#ifdef TIPE_TEST_PAINTER
#include "tipe/candidate_painter.hpp"
#endif
#include <unistd.h>
#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/key.h>
#include <testfrontend_public.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <fstream>
#include <fcitx-utils/event.h>
#include <fcitx-utils/capabilityflags.h>

using namespace fcitx;

#ifndef TIPE_TEST_BINARY_DIR
#define TIPE_TEST_BINARY_DIR "."
#endif

int main() {
  const std::string state_dir = std::string(TIPE_TEST_BINARY_DIR) + "/tipe-fcitx-state-" + std::to_string(getpid());
  std::filesystem::remove_all(state_dir);
  std::filesystem::create_directories(state_dir);
  setenv("XDG_STATE_HOME", state_dir.c_str(), 1);
  // Exercise the same compact/expanded protocol used by the user daemon.
  // The classicui patch consumes Vertical only in this explicit grid mode.
  setenv("TIPE_CLASSIC_GRID", "1", 1);
  setenv("TIPE_CLASSIC_LAYOUT", "horizontal", 1);
  setupTestingEnvironmentPath(
      TIPE_TEST_BINARY_DIR,
      {"/usr/lib64/fcitx5", TIPE_TEST_BINARY_DIR},
      {"/usr/share/fcitx5", "/usr/share/fcitx5/testing",
       std::string(TIPE_TEST_BINARY_DIR) + "/test"});

  char a0[] = "tipe-fcitx-test";
  char a1[] = "--disable=all";
  // Load the optional public-API UI addon as part of the integration harness.
  // It remains inactive unless TIPE_UI_SNAPSHOT_DIR, TIPE_UI_EVENT_LOG, or
  // the explicit TIPE_UI_X11 smoke mode is requested.
  char a2[] = "--enable=testfrontend,tipe,tipe-ui";
  char *argv[] = {a0, a1, a2, nullptr};
  Instance instance(3, argv);
  instance.addonManager().registerDefaultLoader(nullptr);

  auto run_checks = [&]() -> int {

  const char *ui_snapshot_dir = std::getenv("TIPE_UI_SNAPSHOT_DIR");
  const char *ui_event_log = std::getenv("TIPE_UI_EVENT_LOG");
  // Each invocation owns its diagnostic stream.  Truncating before the
  // fcitx instance starts keeps page-offset assertions from passing on a
  // stale frame left by an earlier test run.
  if (ui_event_log) {
    std::error_code log_ec;
    const auto log_path = std::filesystem::path(ui_event_log);
    if (log_path.has_parent_path())
      std::filesystem::create_directories(log_path.parent_path(), log_ec);
    std::ofstream fresh(ui_event_log, std::ios::trunc);
    if (!fresh) return 25;
  }

  auto *frontend = instance.addonManager().addon("testfrontend", true);
  auto *tipe = instance.addonManager().addon("tipe", true);
  if (!frontend || !tipe) {
    return 2;
  }

  auto &methods = instance.inputMethodManager();
  methods.reset([](auto &manager) { manager.addEmptyGroup("Default"); });
  methods.setCurrentGroup("Default");
  InputMethodGroup group("Default");
  group.inputMethodList().emplace_back("tipe");
  group.setDefaultInputMethod("tipe");
  methods.setGroup(std::move(group));

  auto uuid = frontend->call<ITestFrontend::createInputContext>("tipe-test");
  if (instance.inputMethod(instance.inputContextManager().findByUUID(uuid)) !=
      "tipe") {
    return 3;
  }

  auto key = [&](const std::string &value) {
    const Key parsed(value);
    assert(parsed.isValid());
    frontend->call<ITestFrontend::keyEvent>(uuid, parsed, false);
    frontend->call<ITestFrontend::keyEvent>(uuid, parsed, true);
  };
  auto key_accepted = [&](const std::string &value, bool expected_press,
                          bool expected_release) {
    const Key parsed(value);
    assert(parsed.isValid());
    const bool press = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, parsed, false);
    const bool release = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, parsed, true);
    assert(press == expected_press);
    assert(release == expected_release);
  };
  auto *ic = instance.inputContextManager().findByUUID(uuid);
  if (!ic) {
    return 4;
  }

  // The adapter must consume only keys represented by an active View. A
  // modifier chord belongs to the client, while a Shift tap changes mode on
  // release. The final Backspace with no preedit must also pass through so a
  // native client can delete its surrounding text exactly once.
  // A real Shift+letter chord is passed through in both phases and does not
  // toggle TiPE. A subsequent modifier-only tap toggles on release.
  auto raw_key = [&](const std::string &value, bool release, bool expected) {
    const Key parsed(value);
    assert(parsed.isValid());
    const bool accepted = frontend->call<ITestFrontend::sendKeyEvent>(
        uuid, parsed, release);
    assert(accepted == expected);
  };
  raw_key("Shift_L", false, false);
  raw_key("Shift+a", false, false);
  raw_key("Shift+a", true, false);
  raw_key("Shift_L", true, false);
  // Some frontends report only the chord, without a separate Shift press.
  key_accepted("Shift+a", false, false);
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  key_accepted("Control+BackSpace", false, false);
  // Return to Chinese mode before checking an empty preedit.
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  key_accepted("BackSpace", false, false);
  key("n");
  if (ic->inputPanel().preedit().cursor() != 1) return 17;
  key("i");
  if (ic->inputPanel().preedit().cursor() != 2) return 18;
  key("Home");
  if (ic->inputPanel().preedit().cursor() != 0) return 19;
  key("Delete");
  if (ic->inputPanel().preedit().toString() != "i" ||
      ic->inputPanel().preedit().cursor() != 0) return 20;
  key("Escape");
  // Re-establish an active composition before checking that Backspace is
  // consumed by TiPE once and then passes through after it becomes empty.
  key("n");
  key_accepted("BackSpace", true, false);
  key_accepted("BackSpace", false, false);

  // Switching TiPE's submode with a Shift tap must keep an unfinished
  // Chinese spelling visible while English mode is active.  Losing this
  // preedit makes the user's pinyin appear to vanish; switching back should
  // restore the same spelling and cursor.
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});
  key("n");
  key("i");
  key("Home");
  key("Right");
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  if (!ic->inputPanel().preedit().toString().empty() ||
      ic->inputPanel().clientPreedit().toString() != "ni" ||
      ic->inputPanel().clientPreedit().cursor() != 1 ||
      ic->inputPanel().candidateList()) return 26;
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  if (ic->inputPanel().clientPreedit().toString() != "ni" ||
      ic->inputPanel().clientPreedit().cursor() != 1) return 27;
  key("Escape");

  // A frontend without inline preedit still needs a visible text fallback
  // when Chinese candidates are hidden in English mode.
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::SurroundingText});
  key("n");
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  if (ic->inputPanel().preedit().toString() != "n" ||
      !ic->inputPanel().clientPreedit().toString().empty() ||
      ic->inputPanel().candidateList()) return 29;
  raw_key("Shift_L", false, false);
  raw_key("Shift_L", true, true);
  key("Escape");
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});

  // Enter commits the original spelling while Space selects the highlighted
  // Chinese candidate; both are consumed exactly once by TiPE.
  key("n");
  frontend->call<ITestFrontend::pushCommitExpectation>("n");
  key_accepted("Return", true, false);
  if (!ic->inputPanel().preedit().toString().empty()) return 28;

  // Inspect the actual fcitx InputPanel, rather than only the core View. This
  // catches the common integration bug where a global cursor is passed to
  // setCursorIndex() as if it were page-local.
  for (char c : std::string("nihao")) key(std::string(1, c));
  auto list = ic->inputPanel().candidateList();
  auto *common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common || common->pageSize() != 10 || common->label(0).toString() != "1" ||
      common->layoutHint() != CandidateLayoutHint::Horizontal) {
    return 5;
  }

  // Exercise the real CandidateWord callback rather than only numeric key
  // selection.  The prefix candidate must commit immediately while the
  // unconsumed suffix remains editable in the input panel.
  int prefix_page = -1;
  int prefix_index = -1;
  const int total_candidates = common->totalSize();
  const int total_pages =
      (total_candidates + common->pageSize() - 1) / common->pageSize();
  for (int page = 0; page < total_pages && prefix_page < 0; ++page) {
    common->setPage(page);
    for (int index = 0; index < common->size(); ++index) {
      if (common->candidate(index).text().toString() == "你") {
        prefix_page = page;
        prefix_index = index;
        break;
      }
    }
  }
  if (prefix_page < 0) return 8;
  common->setPage(prefix_page);
  frontend->call<ITestFrontend::pushCommitExpectation>("你");
  common->candidate(prefix_index).select(ic);
  if (ic->inputPanel().preedit().toString() != "hao") return 9;
  frontend->call<ITestFrontend::pushCommitExpectation>("好");
  key("space");
  key("Escape");

  // Rebuild the original composition after the direct prefix selection above.
  for (char c : std::string("nihao")) key(std::string(1, c));
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common) return 10;
#ifdef TIPE_TEST_PAINTER
  std::vector<std::string> compact_texts;
  for (int i = 0; i < common->size(); ++i)
    compact_texts.push_back(common->candidate(i).text().toString());
  const auto compact_frame = tipe::painter::measure(compact_texts, "", false, 768, 320);
  const int columns = static_cast<int>(compact_frame.layout.cells.size());
#else
  const int columns = 10;
#endif
  key("Down");
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  // The expanded Vertical signal is consumed by the patched classicui grid;
  // the selected item must remain in the compact strip's first column.
  if (!common || common->layoutHint() != CandidateLayoutHint::Vertical ||
      common->globalCursorIndex() != columns || common->pageSize() != 40 || common->size() <= columns) {
    return 7;
  }
  key("Up");
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common || common->globalCursorIndex() != 0 ||
      common->layoutHint() != CandidateLayoutHint::Vertical) return 31;
  key("Right");
  key("Down");
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common || common->globalCursorIndex() != columns + 1) return 32;
  key("Left");
  key("Up");
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common || common->globalCursorIndex() != 0) return 33;
  if (common->totalSize() > 40) {
    key("Page_Down");
    list = ic->inputPanel().candidateList();
    common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
    if (!common || common->currentPage() != 1 || common->globalCursorIndex() != 40 ||
        common->label(0).toString() != "1") {
      return 6;
    }
    const auto expected = common->candidate(9).text().toString();
    frontend->call<ITestFrontend::pushCommitExpectation>(expected);
    key("0");
    key("Escape");
  } else {
    key("Escape");
  }

  // Numeric labels follow the highlighted ten-item group after moving
  // into the next visual row; hidden/repeated labels must not select row 1.
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("Down");
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  assert(common && common->globalCursorIndex() >= 10);
  const int numbered_begin = (common->globalCursorIndex() / 10) * 10;
  assert(common->label(0).toString().empty());
  assert(common->label(numbered_begin).toString() == "1");
  frontend->call<ITestFrontend::pushCommitExpectation>(common->candidateFromAll(numbered_begin).text().toString());
  key("1");
  key("Escape");

  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");

  // With multiple committed chunks, surrounding-text restoration must touch
  // only the final chunk and retain the earlier chunk as history.
  frontend->call<ITestFrontend::pushCommitExpectation>("中国");
  for (char c : std::string("zhongguo")) key(std::string(1, c));
  key("space");
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});
  ic->surroundingText().setText("你好中国", 4, 4);
  key_accepted("BackSpace", true, false);
  if (ic->inputPanel().preedit().toString() != "zhongguo") return 15;
  ic->surroundingText().setText("", 0, 0);
  key_accepted("BackSpace", true, false);
  if (ic->inputPanel().preedit().toString() != "zhonggu") return 16;
  key("Escape");

  // A client that does not advertise surrounding text cannot prove ownership
  // of a committed chunk.  Backspace must pass through and invalidate the
  // stale history; adding the capability later with a matching snapshot must
  // not revive that old transaction.
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit});
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);

  // Re-establish one committed chunk for the following single-chunk guard.
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");

  // Verify the guarded committed-word restoration transaction.  The fake
  // frontend supplies an exact surrounding-text snapshot; a Backspace then
  // restores editable pinyin instead of forwarding a second delete.
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", true, false);
  if (ic->inputPanel().preedit().toString() != "nihao") return 11;
  // Once restored, the next Backspace edits the reconstructed pinyin
  // composition normally.  It must not re-check the old Chinese text.
  ic->surroundingText().setText("", 0, 0);
  key_accepted("BackSpace", true, false);
  if (ic->inputPanel().preedit().toString() != "niha") return 12;
  // Commit the shortened composition using the actual current candidate (the
  // dictionary backend may choose a different word across installations).
  list = ic->inputPanel().candidateList();
  common = list ? dynamic_cast<CommonCandidateList *>(list.get()) : nullptr;
  if (!common || common->globalCursorIndex() < 0) return 14;
  const std::string shortened_commit =
      common->candidateFromAll(common->globalCursorIndex()).text().toString();
  frontend->call<ITestFrontend::pushCommitExpectation>(shortened_commit);
  key_accepted("space", true, false);

  // Re-establish a Chinese commit, then present unrelated surrounding text.
  // The guarded transaction must fail closed: no IM key is consumed and the
  // stale commit history is invalidated.
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");
  ic->surroundingText().setText("世界", 2, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());
  // Even a later matching snapshot cannot revive invalidated history.
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());

  // A surrounding snapshot with a selection (cursor != anchor) is not a
  // proof that the text immediately before the insertion point belongs to
  // TiPE.  The adapter must leave Backspace to the client and invalidate the
  // stale history, so a later collapsed snapshot cannot trigger a duplicate
  // deletion transaction.
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");
  ic->surroundingText().setText("你好", 1, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());

  // Delete with no active composition belongs to the client as well.  This
  // explicit assertion covers the forward-delete path separately from the
  // Backspace restoration guard above.
  key_accepted("Delete", false, false);
  assert(ic->inputPanel().preedit().toString().empty());

  // Forward-delete is also a client-owned surrounding-text edit.  Once it
  // has been sent through, a later matching snapshot must not resurrect the
  // preceding TiPE commit and issue a duplicate delete transaction.
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::SurroundingText});
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("Delete", false, false);
  // The fake frontend leaves the snapshot unchanged; the key above still
  // represents a client edit, so this later Backspace must fail closed.
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());

  // Moving the client's real insertion cursor after a commit severs TiPE's
  // ownership of that committed chunk.  A later matching surrounding-text
  // snapshot must therefore leave Backspace to the client instead of
  // restoring the old pinyin and issuing deleteSurroundingText().
  frontend->call<ITestFrontend::pushCommitExpectation>("你好");
  for (char c : std::string("nihao")) key(std::string(1, c));
  key("space");
  key_accepted("Left", false, false);
  ic->surroundingText().setText("你好", 2, 2);
  key_accepted("BackSpace", false, false);
  assert(ic->inputPanel().preedit().toString().empty());

  frontend->call<ITestFrontend::pushCommitExpectation>("github");
  for (char c : std::string("github")) key(std::string(1, c));
  key("space");

  // An abbreviation does not force a hard-coded sentence to the top.
  // Select its actual word prefixes through the same callback as a click.
  auto choose_text = [&](const std::string &text) {
    auto current = ic->inputPanel().candidateList();
    auto *all = current ? dynamic_cast<CommonCandidateList *>(current.get()) : nullptr;
    assert(all);
    for (int i = 0; i < all->totalSize(); ++i) {
      if (all->candidateFromAll(i).text().toString() != text) continue;
      frontend->call<ITestFrontend::pushCommitExpectation>(text);
      all->candidateFromAll(i).select(ic);
      return true;
    }
    return false;
  };
  for (char c : std::string("woc")) key(std::string(1, c));
  assert(choose_text("我"));
  assert(ic->inputPanel().preedit().toString() == "c");
  assert(choose_text("操"));
  assert(ic->inputPanel().preedit().toString().empty());

  // A real Fcitx PasswordOrSensitive capability must suppress learning while
  // preserving normal input and commit behavior. Compare the published model
  // before and after a sensitive submission rather than relying only on the
  // core Engine unit test.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const std::string model_path = state_dir + "/tipe/model.current";
  std::ifstream model_before(model_path);
  const std::string model_snapshot((std::istreambuf_iterator<char>(model_before)), {});
  ic->setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit,
                                         CapabilityFlag::PasswordOrSensitive});
  for (char c : std::string("nihao"))
    key_accepted(std::string(1, c), false, false);
  key_accepted("space", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::ifstream model_after(model_path);
  const std::string model_sensitive((std::istreambuf_iterator<char>(model_after)), {});
  assert(model_sensitive == model_snapshot);

  frontend->call<ITestFrontend::destroyInputContext>(uuid);
  // A subsequent context may reuse the same address. It must start with a
  // fresh Engine state rather than stale Shift/chord or LibIME state.
  auto uuid2 = frontend->call<ITestFrontend::createInputContext>("tipe-test-2");
  const bool freshAccepted = frontend->call<ITestFrontend::sendKeyEvent>(
      uuid2, Key("n"), false);
  const bool freshRelease = frontend->call<ITestFrontend::sendKeyEvent>(
      uuid2, Key("n"), true);
  assert(freshAccepted && !freshRelease);
  frontend->call<ITestFrontend::destroyInputContext>(uuid2);
  if (ui_event_log) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ui_event_log, ec) ||
        std::filesystem::file_size(ui_event_log, ec) == 0)
      return 21;
    if (const char *x11 = std::getenv("TIPE_UI_X11"); x11 &&
        std::string(x11) == "1") {
      std::ifstream frames(ui_event_log);
      const std::string log((std::istreambuf_iterator<char>(frames)), {});
      if (log.find("\"x11_visible\":true") == std::string::npos)
        return 23;
    }
    // PageDown above moves the CommonCandidateList to page 2.  The public UI
    // addon must carry that page offset into its painter frame; otherwise a
    // BulkCursor's global index (40) is paired with cells starting at 0 and
    // the selected cell is neither highlighted nor mouse-selectable.  Keep
    // this assertion at the integration boundary where both APIs are real.
    {
    std::ifstream frames(ui_event_log);
      const std::string log((std::istreambuf_iterator<char>(frames)), {});
      // A PasswordOrSensitive update may still reach an explicitly enabled
      // diagnostic UI.  Its frame is allowed to report the capability, but
      // must not carry the sensitive preedit/commit text into JSONL.
      for (size_t begin = 0; begin < log.size();) {
        const size_t end = log.find('\n', begin);
        const std::string line = log.substr(begin, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - begin);
        if (line.find("\"sensitive\":true") != std::string::npos) {
          assert(line.find("nihao") == std::string::npos);
          assert(line.find("你好") == std::string::npos);
          assert(line.find("\"preedit\":\"<redacted>\"") !=
                 std::string::npos);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      if (log.find("\"begin\":40") == std::string::npos ||
          log.find("\"selected\":40") == std::string::npos)
        return 24;
      // The public Fcitx Wayland module exposes connection lifecycle only;
      // it has no popup-surface factory for a normal UI addon.  The addon
      // must publish that limitation explicitly and retain the standard
      // InputPanel fallback instead of silently claiming native Wayland UI.
      if (log.find("\"wayland_popup_supported\":false") ==
              std::string::npos ||
          log.find("\"wayland_popup_reason\":\"public-api-no-popup-factory\"") ==
              std::string::npos ||
          log.find("\"wayland_fallback\":\"standard-input-panel\"") ==
              std::string::npos)
        return 26;
      // The capability frame records whether the registry probe completed.
      // A testfrontend run may not load the optional Wayland addon, in which
      // case `not-run` is expected; a real connection reports `complete` or
      // `error` without ever claiming popup support from globals alone.
      if (log.find("\"wayland_global_probe\":\"not-run\"") ==
              std::string::npos &&
          log.find("\"wayland_global_probe\":\"complete\"") ==
              std::string::npos &&
          log.find("\"wayland_global_probe\":\"error\"") ==
              std::string::npos &&
          log.find("\"wayland_global_probe\":\"unavailable\"") ==
              std::string::npos)
        return 28;
      if (log.find("\"wayland_im_module_loaded\":") == std::string::npos ||
          log.find("\"wayland_popup_manager_v2\":") == std::string::npos ||
          log.find("\"wayland_popup_surface_v2\":") == std::string::npos ||
          log.find("\"wayland_input_method_v1_version\":") == std::string::npos ||
          log.find("\"wayland_input_panel_v1_version\":") == std::string::npos ||
          log.find("\"wayland_input_method_v2_version\":") == std::string::npos ||
          log.find("\"wayland_input_method_manager_v2_version\":") == std::string::npos ||
          log.find("\"wayland_popup_manager_v2_version\":") == std::string::npos ||
          log.find("\"wayland_popup_surface_v2_version\":") == std::string::npos ||
          log.find("\"wayland_public_popup_factory\":false") == std::string::npos ||
          log.find("\"wayland_anchor_source\":\"cursorRect-diagnostic-only\"") ==
              std::string::npos ||
          log.find("\"wayland_surface_lifecycle\":\"standard-input-panel\"") ==
              std::string::npos ||
          log.find("\"cursor_rect_x\":") == std::string::npos ||
          log.find("\"cursor_rect_y\":") == std::string::npos ||
          log.find("\"cursor_rect_width\":") == std::string::npos ||
          log.find("\"cursor_rect_height\":") == std::string::npos ||
          log.find("\"wayland_input_method_v2\":") == std::string::npos ||
          log.find("\"wayland_input_method_manager_v2\":") ==
              std::string::npos)
        return 29;
      if (std::getenv("WAYLAND_DISPLAY") &&
          log.find("\"wayland_display_env\":true") == std::string::npos)
        return 27;
    }
  }
  if (ui_snapshot_dir) {
    std::error_code ec;
    bool found = false;
    for (const auto &entry : std::filesystem::directory_iterator(ui_snapshot_dir, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".png" &&
          entry.file_size(ec) > 0) { found = true; break; }
    }
    if (!found) return 22;
  }
  return 0;
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
