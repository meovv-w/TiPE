#include "tipe/core.hpp"
#include <fcitx/addoninstance.h>
#include <fcitx/addonfactory.h>
#include <fcitx/instance.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/addonmanager.h>
#include <fcitx/text.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/utf8.h>
#include <fcitx-utils/log.h>
#include <memory>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace {
// Keep this in sync with the core's numeric selection convention: 1..9 are
// the first nine candidates on a page and 0 selects the tenth one.  Fcitx's
// CommonCandidateList uses the labels as the *page-local* display labels, so
// setting them explicitly also avoids a locale/theme changing the numbering.
const std::vector<std::string> kCandidateLabels = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};

class Candidate final : public fcitx::CandidateWord {
 public:
  Candidate(std::string text, size_t index, class EngineAddon* owner) : fcitx::CandidateWord(fcitx::Text(text)), index_(index), owner_(owner) {}
  void select(fcitx::InputContext*) const override;
 private: size_t index_; class EngineAddon* owner_;
};
class EngineAddon final : public fcitx::InputMethodEngine {
 public:
  explicit EngineAddon(fcitx::AddonManager *manager) {
    tipe::Engine::prepareDecoder();
    // InputContext pointers are valid only until the corresponding destroy
    // event.  Erasing on that event releases each Engine (and its LibIME
    // context) before a frontend can reuse the address for a new context.
    if (manager && manager->instance()) {
      destroy_watch_ = manager->instance()->watchEvent(
          fcitx::EventType::InputContextDestroyed,
          fcitx::EventWatcherPhase::Default, [this](fcitx::Event &event) {
            auto *destroyed =
                dynamic_cast<fcitx::InputContextDestroyedEvent *>(&event);
            if (destroyed && destroyed->inputContext()) {
              engines_.erase(destroyed->inputContext());
            }
          });
      focus_watch_ = manager->instance()->watchEvent(
          fcitx::EventType::InputContextFocusOut,
          fcitx::EventWatcherPhase::PreInputMethod, [this](fcitx::Event &event) {
            auto *focused=dynamic_cast<fcitx::InputContextEvent *>(&event);
            if(!focused || !focused->inputContext()) return;
            auto it=engines_.find(focused->inputContext());
            if(it!=engines_.end()) confirmRetained(focused->inputContext(),it->second.engine,false);
          });
      activation_watch_ = manager->instance()->watchEvent(
          fcitx::EventType::InputContextInputMethodActivated,
          fcitx::EventWatcherPhase::Default, [this](fcitx::Event &event) {
            auto *activated = dynamic_cast<fcitx::InputMethodActivatedEvent *>(&event);
            if (activated && activated->name() != "tipe")
              engines_.erase(activated->inputContext());
          });
    }
  }

  void keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) override {
    auto* ic=event.inputContext(); if(!ic) return;
    auto& state=engines_[ic];
    auto& e=state.engine;
    // Native terminal clients do not expose retained surrounding text and
    // can discard preedit on text-input leave. Preserve their server-owned
    // composition across focus changes. This is a protocol/capability rule,
    // independent of the program name. Toolkit bridges own their blur
    // commits and must continue to clear composition here.
    state.native_terminal_without_surrounding =
        ic->frontendName().starts_with("wayland") &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Terminal) &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::ClientUnfocusCommit) &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit) &&
        !ic->surroundingText().isValid();
    e.setContext(ic->program());
    // XIM cannot carry a password/sensitive-field hint. Real Wine password
    // controls still send their keys through XIM, so absence of the hint is
    // not permission to learn. Keep input functional but exclude this entire
    // protocol from observation, including semantic commits and cloud work.
    const bool untrusted_privacy = ic->frontendName() == "xim";
    if (untrusted_privacy && !xim_privacy_logged_) {
      xim_privacy_logged_ = true;
      FCITX_INFO() << "TiPE learning fallback: XIM cannot report sensitive fields; "
                      "keyboard observation and personal learning disabled for XIM. "
                      "Input and candidates remain available.";
    }
    // Password and sensitive fields still receive normal IM behavior, but
    // their key stream and commits are excluded from personal learning.
    e.setSensitive(untrusted_privacy || ic->capabilityFlags().testAny(
        fcitx::CapabilityFlag::PasswordOrSensitive));
    e.setRetainedVerification(ic->capabilityFlags().test(
        fcitx::CapabilityFlag::SurroundingText));
    if(!event.isRelease() && !e.hasComposition()) {
      const auto symbol=event.key().sym();
      const bool editing = symbol==FcitxKey_BackSpace || symbol==FcitxKey_Delete ||
          event.key().hasModifier();
      confirmRetained(ic,e,editing);
    }
    // Use the keysym rather than the localized/string representation.  This
    // keeps navigation and editing keys stable across keyboard layouts and
    // frontends (X11, Wayland, GTK and Qt).
    const auto &key = event.key();
    const auto &orig_key = event.origKey();
    const auto sym = key.sym();
    const bool non_shift_modifier = key.states().testAny(
        fcitx::KeyStates{fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,
                         fcitx::KeyState::Super, fcitx::KeyState::Hyper,
                         fcitx::KeyState::Meta});
    const bool shift_symbol = sym == FcitxKey_Shift_L ||
                              sym == FcitxKey_Shift_R;

    // A Shift tap is a mode switch, but only after release.  Keeping the
    // press unhandled lets Shift+letter and Shift+punctuation reach the
    // application unchanged.  ISO_Left_Tab is handled below as the explicit
    // Shift+Tab input-method command.
    if (shift_symbol) {
      if (event.isRelease()) {
        const bool tap = state.shift_down && !state.shift_chord &&
                         !non_shift_modifier;
        state.shift_down = false;
        state.shift_chord = false;
        e.observe("Shift", true);
        if (tap) {
          auto v = e.modeSwitch();
          update(ic, v);
          event.filterAndAccept();
        }
      } else {
        state.shift_down = true;
        state.shift_chord = false;
        e.observe("Shift", false);
      }
      return;
    }

    // Stable names for pass-through keys.  They remain in the learning
    // stream even when a modifier chord belongs to the client.
    auto telemetry_key = [&]() {
      switch (sym) {
        case FcitxKey_BackSpace: return std::string("BackSpace");
        case FcitxKey_Delete: return std::string("Delete");
        case FcitxKey_Return:
        case FcitxKey_KP_Enter: return std::string("Enter");
        case FcitxKey_Tab: return std::string("Tab");
        case FcitxKey_ISO_Left_Tab: return std::string("ShiftTab");
        case FcitxKey_Escape: return std::string("Esc");
        case FcitxKey_Control_L:
        case FcitxKey_Control_R: return std::string("Ctrl");
        case FcitxKey_Alt_L:
        case FcitxKey_Alt_R: return std::string("Alt");
        case FcitxKey_Super_L:
        case FcitxKey_Super_R: return std::string("Super");
        case FcitxKey_Meta_L:
        case FcitxKey_Meta_R: return std::string("Meta");
        default: return fcitx::Key::keySymToUTF8(sym);
      }
    };

    // Modifier presses have no printable key-sym representation and would
    // otherwise fall through the translation switch.  Record their edges
    // explicitly so the learner can distinguish a complete chord from a
    // lone printable key while leaving the modifier available to the client.
    const bool modifier_symbol =
        sym == FcitxKey_Control_L || sym == FcitxKey_Control_R ||
        sym == FcitxKey_Alt_L || sym == FcitxKey_Alt_R ||
        sym == FcitxKey_Super_L || sym == FcitxKey_Super_R ||
        sym == FcitxKey_Meta_L || sym == FcitxKey_Meta_R;
    if (modifier_symbol) {
      e.observe(telemetry_key(), event.isRelease());
      return;
    }

    // A non-modifier key while Shift is held is a real application chord.
    // Some GTK/Qt frontends omit a separate Shift_L press and only annotate
    // the printable event with the Shift state, so consult both sources.
    const bool shift_held = state.shift_down ||
        key.states().testAny(fcitx::KeyState::Shift) ||
        orig_key.states().testAny(fcitx::KeyState::Shift);
    // Do not feed printable keys into the IM, which would otherwise consume
    // Shift+A and prevent the client from receiving the uppercase character.
    if (shift_held && !event.isRelease() && !non_shift_modifier &&
        sym != FcitxKey_Tab && sym != FcitxKey_ISO_Left_Tab) {
      state.shift_chord = true;
      e.observe(telemetry_key(), false);
      return;
    }
    if (shift_held && event.isRelease() && !non_shift_modifier &&
        sym != FcitxKey_Tab && sym != FcitxKey_ISO_Left_Tab) {
      // Release belongs to the client as well; do not generate a second IM
      // event for a Shift chord when no modifier press was observed.
      e.observe(telemetry_key(), true);
      return;
    }
    if (shift_held && !event.isRelease()) state.shift_chord = true;

    // Check modifiers before translating special keys.  In particular,
    // Ctrl+Backspace must not become an IM Backspace action.
    if (non_shift_modifier) {
      e.observe(telemetry_key(), event.isRelease());
      return;
    }

    std::string k;
    switch (sym) {
      case FcitxKey_space: k = "Space"; break;
      case FcitxKey_Return:
      case FcitxKey_KP_Enter: k = "Enter"; break;
      case FcitxKey_BackSpace: k = "BackSpace"; break;
      case FcitxKey_Delete: k = "Delete"; break;
      case FcitxKey_Escape: k = "Esc"; break;
      case FcitxKey_Tab:
        k = key.states().testAny(fcitx::KeyState::Shift) ? "ShiftTab" : "Tab";
        break;
      case FcitxKey_ISO_Left_Tab: k = "ShiftTab"; break;
      case FcitxKey_Shift_L:
      case FcitxKey_Shift_R:
        // A Shift modifier by itself toggles TiPE mode.  Shift combined with
        // another printable key must be allowed through as that character.
        // Modifier-only Shift is dealt with above.  This branch is retained
        // for unusual frontend key normalization and is deliberately inert.
        break;
      case FcitxKey_Down: k = "Down"; break;
      case FcitxKey_Page_Down: k = "PageDown"; break;
      case FcitxKey_Up: k = "Up"; break;
      case FcitxKey_Page_Up: k = "PageUp"; break;
      case FcitxKey_Left: k = "Left"; break;
      case FcitxKey_Right: k = "Right"; break;
      case FcitxKey_Home: k = "Home"; break;
      case FcitxKey_End: k = "End"; break;
      default: break;
    }
    if (k.empty() && key.isDigit() && !key.hasModifier()) {
      // Keep the ASCII form; Engine maps 1..9 and 0 (the tenth candidate).
      k = std::to_string(key.digit());
    }
    if (k.empty()) k = fcitx::Key::keySymToUTF8(sym);
    if (k.empty()) return;
    // Feed key-up events to the learner for complete event telemetry, but do
    // not update the input panel or consume the release in the application.
    if (event.isRelease()) { e.key(k, true); return; }

    // A committed Chinese chunk can be made editable only when the client
    // still exposes exactly that chunk immediately before its cursor.  Do
    // this transaction before calling Engine::key(BackSpace): calling key()
    // first would mutate the in-memory history even when the client cannot
    // honor the deletion, and would let the application delete once more.
    if (k == "BackSpace" && !e.hasComposition()) {
      // This branch bypasses Engine::key because the surrounding text
      // transaction must be checked first.  Keep the physical key-down in
      // telemetry before either restoring our chunk or passing the key on.
      e.observe(k, false);
      const auto restorable = e.restorableCommit();
      if (restorable &&
          ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        const auto &surrounding = ic->surroundingText();
        const auto &text = surrounding.text();
        const auto text_length = fcitx::utf8::lengthValidated(text);
        const auto word_length =
            fcitx::utf8::lengthValidated(restorable->text);
        if (text_length != fcitx::utf8::INVALID_LENGTH &&
            word_length != fcitx::utf8::INVALID_LENGTH &&
            surrounding.isValid() && surrounding.cursor() == surrounding.anchor() &&
            surrounding.cursor() <= text_length &&
            surrounding.cursor() >= word_length &&
            word_length <= static_cast<unsigned int>(INT_MAX)) {
          const auto prefix_end =
              fcitx::utf8::nextNChar(text.begin(), surrounding.cursor());
          const auto prefix_begin =
              fcitx::utf8::nextNChar(text.begin(),
                                     surrounding.cursor() - word_length);
          const std::string preceding(prefix_begin, prefix_end);
          if (preceding == restorable->text) {
            auto v = e.restoreCommit(restorable->text);
            if (v.handled && v.commit.empty()) {
              // The deletion is deliberately sent once, after the core has
              // atomically converted the chunk back to a preedit.
              ic->deleteSurroundingText(-static_cast<int>(word_length),
                                        static_cast<unsigned int>(word_length));
              update(ic, v);
              event.filterAndAccept();
              return;
            }
          }
        }
      }
      // The client did not prove that the text is ours. Drop stale history so
      // a later Backspace cannot repeat a delete against unrelated text, and
      // leave this key to the application exactly once.
      e.invalidateCommitHistory();
      return;
    }
    // A Chinese composition can be suspended while English mode is active;
    // the core will restore it on Space/Enter before committing.  Consult the
    // full composition state so the adapter consumes that commit exactly once.
    const bool had_composition = e.hasComposition();
    // Once the client moves its real insertion cursor, the last committed
    // chunk no longer has a trustworthy ownership relationship with the
    // surrounding snapshot.  Discard restoration history before forwarding
    // the navigation key; otherwise a later matching occurrence elsewhere in
    // the document could be mistaken for TiPE's own commit.
    if (!had_composition && (k == "Left" || k == "Right" || k == "Home" ||
                             k == "End" || k == "Up" || k == "Down"))
      e.invalidateCommitHistory();
    // Forward-delete is an edit of the client's surrounding text whenever
    // there is no active TiPE composition.  It therefore severs ownership of
    // the last committed chunk just like a forwarded Backspace or a real
    // insertion-cursor move.  Without this guard, a client could delete text
    // with Delete and then expose the same snapshot to Backspace; TiPE would
    // mistake the old chunk for its own still-present commit and issue a
    // second deleteSurroundingText transaction.
    if (!had_composition && k == "Delete") e.invalidateCommitHistory();
    auto v=e.key(k); update(ic,v);
    bool handled = v.handled || had_composition;
    const bool alpha = k.size() == 1 && std::isalpha(static_cast<unsigned char>(k[0]));
    const bool digit_or_punct = k.size() == 1 &&
                                (std::isdigit(static_cast<unsigned char>(k[0])) ||
                                 std::ispunct(static_cast<unsigned char>(k[0])));
    if (alpha || (digit_or_punct && (had_composition || !v.commit.empty()))) handled = true;
    if (k == "Tab" || k == "ShiftTab")
      handled = v.mode == tipe::Mode::Chinese && (had_composition || !v.candidates.empty());
    if ((k == "Space" || k == "Enter") && had_composition) handled = true;
    // With no preedit, Backspace/Delete belong to the client surrounding
    // text.  A composition that becomes empty is still consumed once, but a
    // native empty-field edit must pass through exactly once.
    if ((k == "BackSpace" || k == "Delete") && !had_composition &&
        v.commit.empty())
      handled = false;
    // Navigation and commit keys with no active composition are client
    // editing commands.  Tab/ShiftTab remain IM commands because they change
    // candidate presentation even when the preedit is empty.
    if (!had_composition && v.commit.empty() && v.preedit.empty() &&
        v.candidates.empty() && (k == "Space" || k == "Enter" ||
        k == "Esc" || k == "Down" || k == "Up" || k == "Left" ||
        k == "Right" || k == "Home" || k == "End" || k == "PageUp" ||
        k == "PageDown")) handled = false;
    if(handled) event.filterAndAccept();
  }
  void deactivate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
    auto *ic = event.inputContext();
    if (!ic) return;
    auto it = engines_.find(ic);
    if (it != engines_.end()) {
      confirmRetained(ic,it->second.engine,false);
      // Switching to another IM is an explicit user action. Preserve an
      // unfinished spelling by committing it literally before discarding the
      // per-IM state. Password fields and capability changes must never leak
      // text through this path; those transitions are reset below instead.
      const auto *switched =
          dynamic_cast<const fcitx::InputContextSwitchInputMethodEvent *>(&event);
      // Focus-out is already committed by either the client (when it sets
      // ClientUnfocusCommit) or Fcitx's ReservedFirst focus-out handler.
      // Committing it again here duplicates the spelling in the old client.
      // Require an actual switch event so focus/reset/destruction cannot
      // accidentally become a second commit transaction.
      const bool explicit_switch =
          switched &&
          switched->reason() != fcitx::InputMethodSwitchedReason::CapabilityChanged;
      // A frontend without inline preedit has nothing for either Fcitx or
      // the client to flush. Its composition lives only in the candidate
      // panel, so TiPE owns that focus-out commit. Test the actual published
      // preedit rather than guessing from a toolkit or application name.
      const bool panel_only_focus_out =
          event.type() == fcitx::EventType::InputContextFocusOut &&
          ic->inputPanel().clientPreedit().toStringForCommit().empty();
      const bool sensitive = ic->capabilityFlags().testAny(
          fcitx::CapabilityFlag::PasswordOrSensitive);
      if (event.type() == fcitx::EventType::InputContextFocusOut &&
          it->second.native_terminal_without_surrounding && !sensitive &&
          it->second.engine.hasComposition()) {
        it->second.focus_suspended = true;
        it->second.shift_down = it->second.shift_chord = false;
        it->second.engine.invalidateCommitHistory();
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
      }
      // Do not gate this on InputContext::hasFocus(): toolkit bridges may
      // clear focus before delivering the switch/deactivate event (common
      // for Ctrl+Space).  The context still owns the composition and must
      // flush it before its Engine state is destroyed.
      if ((explicit_switch || panel_only_focus_out) && !sensitive &&
          it->second.engine.hasComposition()) {
        const auto view = it->second.engine.commitLiteral(
            explicit_switch ? "SwitchIM" : "FocusOut");
        if (!view.commit.empty()) ic->commitString(view.commit);
      }
    }
    engines_.erase(ic);
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }
  void reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override { if(auto* ic=event.inputContext()){engines_.erase(ic);ic->inputPanel().reset();ic->updatePreedit();} }
  void activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
    if (auto *ic = event.inputContext()) {
      auto [it, inserted] = engines_.try_emplace(ic);
      if (it->second.focus_suspended) {
        if (ic->capabilityFlags().testAny(
                fcitx::CapabilityFlags{fcitx::CapabilityFlag::PasswordOrSensitive,
                                       fcitx::CapabilityFlag::Disable})) {
          engines_.erase(it);
          return;
        }
        it->second.focus_suspended = false;
        it->second.engine.setContext(ic->program());
        update(ic, it->second.engine.view());
      }
    }
  }
  std::string subMode(const fcitx::InputMethodEntry&, fcitx::InputContext& ic) override { auto it=engines_.find(&ic); return it!=engines_.end() && it->second.engine.mode()==tipe::Mode::English ? "英文" : "中文"; }
  std::string overrideIcon(const fcitx::InputMethodEntry&) override { return "tipe"; }
  std::vector<fcitx::InputMethodEntry> listInputMethods() override { std::vector<fcitx::InputMethodEntry> out; fcitx::InputMethodEntry e("tipe","TiPE","zh_CN","tipe"); e.setNativeName("TiPE 中文").setIcon("tipe").setLabel("T").setConfigurable(true); out.emplace_back(std::move(e)); return out; }
  void select(fcitx::InputContext* ic,size_t i){auto it=engines_.find(ic);if(it==engines_.end())return;auto v=it->second.engine.select(i);update(ic,v);}
 private:
  void confirmRetained(fcitx::InputContext *ic,tipe::Engine &engine,bool editing) {
    const auto own=engine.pendingRetainedText();
    if(own.empty()) return;
    bool retained=false;
    if(!editing && !ic->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
      const auto &surrounding=ic->surroundingText();
      const auto &text=surrounding.text();
      const auto length=fcitx::utf8::lengthValidated(text);
      const auto count=fcitx::utf8::lengthValidated(own);
      if(surrounding.isValid() && length!=fcitx::utf8::INVALID_LENGTH &&
          count!=fcitx::utf8::INVALID_LENGTH && surrounding.cursor()==surrounding.anchor() &&
          surrounding.cursor()<=length && surrounding.cursor()>=count) {
        const auto end=fcitx::utf8::nextNChar(text.begin(),surrounding.cursor());
        const auto begin=fcitx::utf8::nextNChar(text.begin(),surrounding.cursor()-count);
        retained=std::string_view(&*begin,static_cast<size_t>(std::distance(begin,end)))==own;
      }
    }
    engine.finalizeRetained(retained);
  }
  void update(fcitx::InputContext* ic,const tipe::View& v){
    if(!v.commit.empty()) ic->commitString(v.commit);
    fcitx::Text preedit(v.preedit);
    // Text::setCursor uses a UTF-8 byte offset, exactly what the core exposes
    // in View::preedit_cursor.  Clamp defensively for frontends that receive
    // a stale view during an InputContext transition.
    preedit.setCursor(static_cast<int>(std::min(v.preedit_cursor, v.preedit.size())));
    const bool client_preedit = ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
    // updatePreedit() sends clientPreedit(), not the popup's preedit().
    // Publishing only the latter hid suspended pinyin in English mode as
    // soon as the Chinese candidate popup disappeared. Keep it inline when
    // the frontend supports preedit, with a popup text fallback otherwise.
    const bool client_side_panel = ic->capabilityFlags().test(
        fcitx::CapabilityFlag::ClientSideInputPanel);
    ic->inputPanel().setClientPreedit(client_preedit ? preedit : fcitx::Text{});
    // GTK/Qt client-side panels render the preedit and candidates in the
    // application-owned popup. Sending the same preedit through InputPanel
    // makes classicui draw a second spelling line above that popup. Keep the
    // fallback text only for frontends that do not own a client-side panel.
    ic->inputPanel().setPreedit(
        client_side_panel || (v.mode == tipe::Mode::English && client_preedit)
            ? fcitx::Text{} : preedit);
    if(v.candidates.empty()) {
      // Clearing the list is essential in English mode and after commit;
      // an empty CommonCandidateList would otherwise leave a blank popup.
      ic->inputPanel().setCandidateList(nullptr);
    } else {
      auto list=std::make_unique<fcitx::CommonCandidateList>();
      // Preserve an explicit vertical fallback for unpatched classicui
      // installations; the user launcher defaults to a compact horizontal
      // strip and opts into the expanded-grid protocol below.
      const char *layout_env = std::getenv("TIPE_CLASSIC_LAYOUT");
      const bool force_vertical =
          layout_env && (std::string_view(layout_env) == "vertical" ||
                         std::string_view(layout_env) == "column");
      // The patched classicui uses Vertical as the explicit signal to enter
      // its wrapped TiPE grid.  Compact composition stays Horizontal so the
      // initial candidate strip remains a single line.  Without the opt-in
      // patch, preserve the upstream horizontal fallback to avoid turning an
      // arrow key into a tall ten-row list.
      const char *grid_env = std::getenv("TIPE_CLASSIC_GRID");
      const bool grid_enabled =
          grid_env && (std::string_view(grid_env) == "1" ||
                       std::string_view(grid_env) == "true" ||
                       std::string_view(grid_env) == "yes");
      const auto hint =
          (force_vertical || (grid_enabled && v.expanded))
              ? fcitx::CandidateLayoutHint::Vertical
              : fcitx::CandidateLayoutHint::Horizontal;
      list->setLayoutHint(hint);
      const int page_size = static_cast<int>(engines_.at(ic).engine.candidatePageSize());
      list->setPageSize(page_size);
      std::vector<std::string> labels(page_size);
      const size_t group = (v.cursor % page_size) / 10;
      for (size_t i = group * 10; i < group * 10 + 10 && i < labels.size(); ++i)
        labels[i] = kCandidateLabels[i % 10];
      list->setLabels(labels);
      list->setCursorKeepInSamePage(true);
      list->setCursorPositionAfterPaging(
          fcitx::CursorPositionAfterPaging::ResetToFirst);
      for(size_t i=0;i<v.candidates.size();++i) list->append<Candidate>(v.candidates[i].text,i,this);
      // Engine::View::cursor is a global index, while setCursorIndex() takes
      // an index relative to the currently selected page.  Set the page first
      // and then the local index so PageUp/PageDown and numeric labels remain
      // coherent after every redraw.
      const auto page = static_cast<int>(v.cursor / page_size);
      list->setPage(page);
      list->setCursorIndex(static_cast<int>(v.cursor % page_size));
      ic->inputPanel().setCandidateList(std::move(list));
    }
    ic->updatePreedit(); ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }
  struct ContextState {
    tipe::Engine engine;
    bool shift_down{};
    bool shift_chord{};
    bool native_terminal_without_surrounding{};
    bool focus_suspended{};
  };
  std::unordered_map<fcitx::InputContext*,ContextState> engines_;
  bool xim_privacy_logged_{};
  // Declared after engines_ so the watcher is destroyed first; no callback
  // can observe a map whose Engine values have already been torn down.
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> destroy_watch_;
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> focus_watch_;
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> activation_watch_;
};
void Candidate::select(fcitx::InputContext* ic) const { owner_->select(ic,index_); }
class EngineAddonFactory final : public fcitx::AddonFactory { public: fcitx::AddonInstance* create(fcitx::AddonManager* manager) override { return new EngineAddon(manager); } };
}
FCITX_ADDON_FACTORY(EngineAddonFactory)
