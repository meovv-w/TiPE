#pragma once
#include <memory>
#include "tipe/learning.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#ifdef TIPE_HAS_LIBIME
namespace libime { class PinyinIME; class PinyinContext; }
#endif

namespace tipe {
enum class Mode { Chinese, English };
// The system LibIME decoder is preferred when its runtime data is available.
// A small immutable table remains available when a package is absent or
// damaged so an input context never crashes merely because an optional data
// file is missing.
enum class DecoderBackend { BuiltinFallback, LibIME };
struct Candidate {
  std::string text, pinyin;
  int score{};
  // Number of bytes consumed from the original spelling. Decoder pinyin is
  // canonical and may expand abbreviations or contain extra separators.
  size_t consumed{};
  bool inferred{};
  bool operator==(const Candidate &) const = default;
};
struct View {
  std::string preedit;
  std::vector<Candidate> candidates;
  // Candidate cursor is a global candidate index.  `preedit_cursor` is a
  // byte offset into the UTF-8 preedit and is consumed by Fcitx Text.  Keep
  // the two positions separate: navigating candidates must not move the
  // insertion point in the spelling, and Left/Right editing must not change
  // the selected candidate.
  size_t cursor{};
  bool expanded{};
  Mode mode{Mode::Chinese};
  std::string commit;
  bool handled{};
  size_t preedit_cursor{};
  View() = default;
  View(std::string p, std::vector<Candidate> c, size_t candidate_cursor,
       bool is_expanded, Mode m, std::string committed = {},
       bool was_handled = false, size_t edit_cursor = std::string::npos)
      : preedit(std::move(p)), candidates(std::move(c)), cursor(candidate_cursor),
        expanded(is_expanded), mode(m), commit(std::move(committed)),
        handled(was_handled),
        preedit_cursor(edit_cursor == std::string::npos ? preedit.size() : edit_cursor) {}
};
struct RestorableCommit { std::string text, pinyin; };

class Engine {
 public:
  explicit Engine(std::string data_dir = {});
  ~Engine();
  // Load shared immutable decoder data before the first physical key.
  // Failure is harmless; ordinary construction still provides the fallback.
  static void prepareDecoder();
  View key(const std::string& key, bool release=false);
  // Repaint a suspended context without manufacturing a keyboard event.
  View view() const;
  // Record a physical key event without applying it to the composition.  The
  // Fcitx adapter uses this for modifier chords which belong to the client
  // (for example Shift+A and Ctrl+Backspace), so learning still sees the
  // complete keyboard stream while the application receives the chord.
  void observe(const std::string& key, bool release=false);
  // Apply a modifier-only mode switch after the adapter has recorded its
  // physical key-up event.  key("Shift") remains the public convenience
  // operation and records the corresponding key-down event itself.
  View modeSwitch();
  View select(size_t index); View commit(); View reset();
  // Finalize exactly the remaining spelling without selecting a Chinese
  // candidate. Enter and an explicit switch to another input method use
  // this path; already selected Chinese chunks have already reached the
  // client and must not be emitted again.
  View commitLiteral(const std::string& trigger = "literal");
  bool hasComposition() const { return !raw_.empty() || !suspended_.empty(); }
  std::optional<RestorableCommit> restorableCommit() const;
  View restoreCommit(const std::string& expected_text);
  void invalidateCommitHistory();
  // Frontends with surrounding-text support defer semantic learning until
  // the next client action proves the emitted text was retained. Only TiPE's
  // own bounded commit suffix crosses this API; never surrounding text.
  void setRetainedVerification(bool enabled);
  std::string pendingRetainedText() const;
  void finalizeRetained(bool retained);
  Mode mode() const { return mode_; } const std::string& raw() const { return raw_; }
  DecoderBackend decoderBackend() const { return decoder_backend_; }
  const std::string& decoderStatus() const { return decoder_status_; }
  // UTF-8 byte offset of the insertion point inside the active preedit.
  // This is separate from View::cursor, which denotes a candidate index.
  size_t candidatePageSize() const;
  size_t preeditCursor() const { return preedit_cursor_; }
  void setSensitive(bool sensitive) {
    if (sensitive != sensitive_) {
      clearPrefixCache();
      ++session_;
      // A privacy transition invalidates committed-chunk restoration.  The
      // next surrounding-text snapshot belongs to a different trust level;
      // it must never make a prior normal-field commit editable in a password
      // field (or vice versa).
      invalidateCommitHistory();
      pending_retained_.clear();
      // Timing telemetry must never bridge a privacy boundary.  Otherwise
      // the first key in a password field could inherit the interval from a
      // normal field and expose cross-field activity to the learner.
      last_event_ms_ = last_delete_ms_ = 0;
    }
    sensitive_ = sensitive;
  }
  void setContext(std::string context) {
    if (context != context_) {
      // Fcitx can reuse an InputContext while focus moves between programs.
      // Drop composition/history at that boundary so a later surrounding
      // snapshot from the new client cannot restore text typed in the old
      // client.  The new context starts a fresh event session.
      reset();
      ++session_;
      // A new application/context starts an independent event stream; do not
      // report a key interval measured in the previous client.
      last_event_ms_ = last_delete_ms_ = 0;
    }
    context_ = std::move(context);
  }
  Learner& learner() { return *learner_; }
 private:
  void clearPrefixCache() {
    prefix_cache_input_.clear();prefix_cache_base_.clear();prefix_cache_expanded_.clear();
  }
  void update(bool reset_page = true); void refreshCandidates(); void addPrefixCandidates(); bool english_like() const; std::vector<Candidate> lookup(const std::string&) const;
  View acceptCandidate(size_t index, const std::string& trigger);
  View commitWithKey(const std::string& trigger);
  void rememberCorrectionOrigin();
  // Keep committed-text restoration history bounded.  The frontend owns the
  // actual surrounding text; TiPE only needs a recent suffix for guarded
  // Backspace restoration.
  void recordCommitChunk(std::string text, std::string pinyin);
  void trimCommitHistory();
  uint64_t correction_started_{};
  bool verify_retained_{};
  std::vector<Event> pending_retained_;
  void learnCommit(Event event);
  struct CommitChunk { std::string text; std::string pinyin; };
  std::string raw_, suspended_, committed_, context_, correction_origin_; std::vector<CommitChunk> chunks_; bool english_touched_{}; bool literal_token_{}; bool sensitive_{}; Mode mode_{Mode::Chinese}; bool expanded_{}; size_t cursor_{}; size_t page_{}; size_t preedit_cursor_{}; size_t suspended_cursor_{}; uint64_t session_{1}; uint64_t last_event_ms_{}; uint64_t last_delete_ms_{}; std::vector<Candidate> candidates_; std::shared_ptr<Learner> learner_;
  DecoderBackend decoder_backend_{DecoderBackend::BuiltinFallback};
  std::string decoder_status_{"builtin fallback"};
  std::string prefix_cache_input_;
  std::vector<Candidate> prefix_cache_base_,prefix_cache_expanded_;
  // Collapsing the grid keeps candidate focus; Home/End or Left at its
  // first item explicitly return focus to the independent spelling cursor.
  bool editing_preedit_{};
#ifdef TIPE_HAS_LIBIME
  std::shared_ptr<libime::PinyinIME> libime_ime_; std::unique_ptr<libime::PinyinContext> libime_ctx_;
#endif
};
}
