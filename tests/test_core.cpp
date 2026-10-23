#include "tipe/core.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
static void retained_result_regression() {
 using namespace tipe;
 const std::string dir="/tmp/tipe-retained-core-test";
 std::filesystem::remove_all(dir);
 Engine e(dir); e.setContext("retained-editor");e.setRetainedVerification(true);
 auto type=[&]{for(char c:std::string("nihao"))e.key(std::string(1,c));};
 // Immediate deletion/restoration must not teach a rejected result.
 for(int n=0;n<3;++n) {
   type();auto v=e.key("Space");assert(v.commit=="你好");
   assert(e.pendingRetainedText()=="你好");
   assert(e.restoreCommit("你好").handled);
   assert(e.pendingRetainedText().empty());e.reset();
 }
 assert(e.learner().wait_until_idle(std::chrono::seconds(5)));
 assert(e.learner().exact_entries("nihao","retained-editor").empty());
 for(int n=0;n<3;++n) {type();e.key("Space");e.finalizeRetained(true);}
 assert(e.learner().wait_until_idle(std::chrono::seconds(5)));
 assert(!e.learner().exact_entries("nihao","retained-editor").empty());
 assert(e.learner().clear());
 type();e.key("Space");e.setSensitive(true);e.finalizeRetained(true);
 assert(e.pendingRetainedText().empty());
 assert(e.learner().wait_until_idle(std::chrono::seconds(5)));
 assert(e.learner().model()->entries.empty());
}
static void long_composition_regression() {
 using namespace tipe;
 const std::string dir="/tmp/tipe-long-composition-core-test";
 std::filesystem::remove_all(dir);
 Engine engine(dir);engine.setContext("long-composition");
 if(engine.decoderBackend()!=DecoderBackend::LibIME)return;
 for(int repetitions:{13,14,20,52}) {
   engine.reset();std::string raw,expected;
   for(int i=0;i<repetitions;++i){raw+="nihao";expected+="你好";}
   for(char c:raw) {
     const auto view=engine.key(std::string(1,c));
     assert(view.preedit==raw.substr(0,view.preedit.size()));
     assert(!view.candidates.empty());
     for(const auto &candidate:view.candidates)assert(candidate.consumed<=64);
   }
   auto view=engine.key("Down");assert(view.preedit==raw && view.expanded);
   engine.key("Shift");view=engine.key("Shift");assert(view.preedit==raw);
   assert(engine.key("Enter").commit==raw);
   for(char c:raw)engine.key(std::string(1,c));
   std::string committed;
   for(size_t step=0;!engine.raw().empty() && step<raw.size();++step) {
     const auto before=engine.raw();const auto candidate=engine.view().candidates.front();
     const auto selected=engine.key("Space");
     assert(selected.preedit==before.substr(candidate.consumed));
     assert(!selected.commit.empty());committed+=selected.commit;
   }
   assert(engine.raw().empty() && committed==expected);
 }
 // A 64-byte window ending in the middle of 'zhong' must stop at a real
 // decoder word endpoint, retain the whole syllable and restore its prefix.
 engine.reset();std::string raw;
 for(int i=0;i<12;++i)raw+="nihao";
 raw+="zhongguo";
 for(char c:raw)engine.key(std::string(1,c));
 auto selected=engine.key("Space");
 assert(selected.preedit=="zhongguo");
 assert(!engine.restorableCommit()); // the suffix is still actively editable
 assert(engine.key("Space").commit=="中国");
 auto restorable=engine.restorableCommit();
 assert(restorable && restorable->pinyin=="zhongguo");
 engine.reset();for(char c:raw)engine.key(std::string(1,c));
 selected=engine.key("Space");
 for(size_t n=0;n<8;++n)engine.key("BackSpace");
 restorable=engine.restorableCommit();
 assert(restorable && restorable->pinyin==raw.substr(0,60));
 assert(engine.restoreCommit(selected.commit).handled);
 assert(engine.key("Enter").commit==raw.substr(0,60));
 engine.reset();for(char c:raw)engine.key(std::string(1,c));
 engine.key("Home");engine.key("Delete");
 assert(engine.key("Enter").commit==raw.substr(1));

 // Editing reuses the unchanged decoder suffix. Its candidates must still
 // agree with a fresh incremental decode, including after shifting the
 // bounded window or editing beyond its end.
 Engine reference(dir+"-reference");reference.setContext("fresh-long-composition");
 auto compare=[&] {
   reference.reset();
   for(char ch:engine.raw())reference.key(std::string(1,ch));
   const auto actual=engine.view(),fresh=reference.view();
   assert(actual.preedit==fresh.preedit && actual.candidates.size()==fresh.candidates.size());
   for(size_t i=0;i<actual.candidates.size();++i) {
     assert(actual.candidates[i].text==fresh.candidates[i].text);
     assert(actual.candidates[i].consumed==fresh.candidates[i].consumed);
     assert(actual.candidates[i].pinyin==fresh.candidates[i].pinyin);
   }
 };
 for(const auto &spelling:{std::string("nihaozhongguo"),raw+raw}) {
   engine.reset();for(char ch:spelling)engine.key(std::string(1,ch));
   engine.key("Home");engine.key("Delete");compare();
   engine.key("x");compare();engine.key("BackSpace");compare();
   engine.key("n");compare();
   engine.key("End");engine.key("BackSpace");compare();
   engine.key("o");compare();
   engine.key("Home");engine.key("Right");engine.key("i");compare();
   engine.key("Delete");compare();engine.key("BackSpace");compare();
 }
}
int main(){using namespace tipe; retained_result_regression(); long_composition_regression(); std::filesystem::remove_all("/tmp/tipe-core-test"); Engine e("/tmp/tipe-core-test"); e.learner().clear();
 for(char c:std::string("nihao"))e.key(std::string(1,c)); auto v=e.key("Space"); assert(v.preedit.empty() && v.commit=="你好");
 // Public Engine callers may pass toolkit key names directly. These aliases
 // must preserve the canonical editing behavior rather than becoming text.
 e.reset(); for(char c:std::string("ni")) e.key(std::string(1,c));
 v=e.key("Backspace"); assert(v.preedit=="n" && e.raw()=="n");
 e.reset(); for(char c:std::string("ni")) e.key(std::string(1,c));
 v=e.key("Escape"); assert(v.preedit.empty() && e.raw().empty());
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Return"); assert(v.commit=="nihao" && v.preedit.empty());
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("KP_Enter"); assert(v.commit=="nihao" && v.preedit.empty());
 // Enter commits an English spelling literally as well.
 e.reset(); e.key("Shift"); for(char c:std::string("github")) e.key(std::string(1,c));
 v=e.key("Enter"); assert(v.commit=="github" && v.preedit.empty());
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Page_Down"); assert(v.expanded); v=e.key("Page_Up"); assert(v.expanded);
 e.reset(); for(char c:std::string("woc"))e.key(std::string(1,c)); v=e.key("1"); assert(v.preedit.empty() && e.raw().empty());
 e.reset(); for(char c:std::string("nihao"))e.key(std::string(1,c)); v=e.key("Down"); assert(v.expanded); assert(v.cursor > 1); v=e.key("Up"); assert(v.expanded && v.cursor == 0);
 // Once the expanded list contains prefix candidates, navigation followed by
 // Space must refresh the list without losing the selected text/consumed
 // boundary. This guards against committing a different candidate at the old
 // cursor index after a decoder refresh.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Tab"); size_t prefix_index=v.candidates.size();
 for(size_t i=0;i<v.candidates.size();++i) if(v.candidates[i].text=="你" && v.candidates[i].consumed==2) { prefix_index=i; break; }
 assert(prefix_index < v.candidates.size()); e.key("Home");
 for(size_t i=0;i<prefix_index;++i) e.key("Right");
 v=e.key("Space"); assert(v.commit=="你" && v.preedit=="hao" && e.raw()=="hao");
 v=e.key("ShiftTab"); assert(!v.expanded);
 // Expanding at a cursor inside a syllable must not invent a shorter
 // consumption boundary for a complete candidate. Every visible 你好 uses
 // all of nihao; every 你 leaves exactly hao. Selecting any such entry must
 // preserve that literal suffix, independent of its candidate index.
 if (e.decoderBackend() == DecoderBackend::LibIME) {
   e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
   const auto expanded = e.key("Tab");
   size_t complete_count = 0, prefix_count = 0;
   for (size_t i = 0; i < expanded.candidates.size(); ++i) {
     const auto &candidate = expanded.candidates[i];
     if (candidate.text != "你好" && candidate.text != "你") continue;
     const bool complete = candidate.text == "你好";
     assert(candidate.consumed == (complete ? 5 : 2));
     if (complete) ++complete_count; else ++prefix_count;
     e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
     e.key("Tab"); const auto chosen = e.select(i);
     assert(chosen.commit == candidate.text);
     assert(chosen.preedit == (complete ? "" : "hao"));
   }
   assert(complete_count == 1 && prefix_count == 1);
 }
 e.reset(); e.key("Shift"); assert(e.mode()==Mode::English); v=e.key("g"); assert(v.preedit=="g"&&v.candidates.empty()); e.key("Shift"); assert(e.mode()==Mode::Chinese);
 e.reset(); for(char c:std::string("ni"))e.key(std::string(1,c)); e.key("BackSpace"); assert(e.raw()=="n");
 e.reset(); for(char c:std::string("woc"))e.key(std::string(1,c)); v=e.key("1"); assert(v.commit.size()>0 && e.raw().empty());
 e.reset(); for(char c:std::string("nihao"))e.key(std::string(1,c)); v=e.key("Down"); size_t ni_index=0; for(size_t i=0;i<v.candidates.size();++i) if(v.candidates[i].text=="你"){ni_index=i;break;} v=e.select(ni_index); assert(v.preedit=="hao" && e.raw()=="hao" && v.commit=="你");
 e.key("BackSpace"); e.key("BackSpace"); e.key("BackSpace"); v=e.key("BackSpace"); assert(v.preedit=="ni" && e.raw()=="ni");
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); v=e.key("PageDown"); assert(v.expanded); if(v.candidates.size()>10) assert(v.cursor==10); v=e.key("PageUp"); assert(v.expanded); assert(v.cursor==0);
 // The tenth selection key is 0 and is page-local.  LibIME normally returns
 // at least 20 candidates; the conditional keeps the fallback dictionary
 // testable on systems without LibIME while still checking the full mapping
 // wherever paging is available.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); auto pageView=e.key("Page_Down"); if(pageView.candidates.size()>19){ auto expected=pageView.candidates[19].text; auto chosen=e.key("0"); assert(chosen.commit==expected); }
 // Page edges use the same visual grid navigation: Up from the first cell
 // of page two returns to the last candidate on page one.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("PageDown");
 if (v.candidates.size() > 10) {
   assert(v.cursor == 10);
   v=e.key("Up"); assert(v.expanded && v.cursor == 9);
   // The page edge uses the same row-major horizontal semantics as the
   // standalone grid: Left from page two's first candidate returns to page
   // one, while Right from that page's last candidate advances again.
   v=e.key("PageDown"); assert(v.expanded && v.cursor == 10);
   v=e.key("Left"); assert(v.expanded && v.cursor == 9);
   v=e.key("Right"); assert(v.expanded && v.cursor == 10);
   // Once back at the first candidate of the first page, one more Left closes
   // the candidate view and restores ordinary pinyin editing.
   v=e.key("PageUp"); assert(v.expanded && v.cursor == 0);
   v=e.key("Left"); assert(!v.expanded && v.preedit == "nihao");
 }
 // In an expanded view, Left/Right follow the same visual row geometry as
 // the candidate painter. Repeated movement eventually returns to pinyin
 // editing at the first page, while never leaving the expanded state early.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Tab");
 if (v.candidates.size() > 10) {
   const auto start = v.cursor;
   v=e.key("Right");
   assert(v.expanded && v.cursor != start);
   for (size_t i=0; i<64 && v.expanded; ++i) v=e.key("Left");
   assert(!v.expanded && v.preedit == "nihao");
 }
 e.reset(); for(char c:std::string("zhongguo"))e.key(std::string(1,c)); v=e.key("Space"); assert(v.commit=="中国");
 auto rest = e.restorableCommit(); assert(rest && rest->text=="中国" && rest->pinyin=="zhongguo"); v=e.restoreCommit("中国"); assert(v.handled && v.preedit=="zhongguo"); v=e.key("Space"); assert(v.commit=="中国");
 // Client-owned forward-delete and real-cursor navigation must sever the
 // guarded restoration suffix even when the core is used without the Fcitx
 // adapter. Backspace remains the explicit restoration transaction.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); v=e.key("Space"); assert(v.commit=="你好"); assert(e.restorableCommit());
 e.key("Delete"); assert(!e.restorableCommit());
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); v=e.key("Space"); assert(v.commit=="你好"); assert(e.restorableCommit());
 e.key("Left"); assert(!e.restorableCommit());
 e.reset(); for(char c:std::string("github"))v=e.key(std::string(1,c)); assert(v.candidates.empty() && v.preedit=="github"); v=e.key("Space"); assert(v.commit=="github");
 // Navigation after a completed token must not leave expanded/prefix state
 // that contaminates the next literal token with stale LibIME candidates.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Space"); assert(v.commit=="你好" && v.preedit.empty());
 v=e.key("Left"); assert(v.preedit.empty() && v.candidates.empty() && !v.expanded);
 for(char c:std::string("github")) v=e.key(std::string(1,c));
 assert(v.preedit=="github" && v.candidates.empty());
 v=e.key("Space"); assert(v.commit=="github" && v.candidates.empty());
 // Apostrophe is a pinyin separator while a Chinese composition is active;
 // it must not be lost through the literal-token branch.
 e.reset(); for(char c:std::string("ni'hao")) v=e.key(std::string(1,c));
 assert(v.preedit=="ni'hao");
 e.reset(); for(char c:std::string("TypeScript"))v=e.key(std::string(1,c)); assert(v.candidates.empty() && v.preedit=="TypeScript"); v=e.key("Space"); assert(v.commit=="TypeScript");
 // Stable English/code/URL/path smoke corpus: these tokens stay verbatim
 // while Chinese mode is active and never leave a blank Chinese candidate list.
 for (const auto *token : {"docker", "start", "foobar", "commit", "pull", "request", "browser", "window", "update", "version", "server", "client", "source", "build", "release", "debug", "string", "class", "function", "variable", "module", "package", "linux", "hello", "world", "example", "domain", "email", "message", "keyboard", "desktop", "folder", "system", "input", "output", "status", "control", "process", "thread", "memory", "network", "socket", "editor", "https://example.com", "https://example.com:8080/a2", "example.com", "www.example.org", "/usr/local/bin/tipe", "usr/local/bin", "makefile"}) {
   e.reset(); for (char c : std::string(token)) v = e.key(std::string(1, c));
   if (!(v.candidates.empty() && v.preedit == token)) { std::cerr << "token failure: " << token << " preedit=" << v.preedit << " candidates=" << v.candidates.size() << "\n"; return 2; }
   v = e.key("Space"); assert(v.commit == token);
 }
 e.reset(); e.key("Shift"); for(char c:std::string("TypeScript"))v=e.key(std::string(1,c)); assert(v.mode==Mode::English && v.candidates.empty() && v.preedit=="TypeScript"); v=e.key("Space"); assert(v.commit=="TypeScript");
 e.reset(); e.key("Shift"); e.key("n"); e.key("i"); v=e.key("1"); assert(v.mode==Mode::English && v.commit.empty() && v.preedit=="ni1"); v=e.key("Space"); assert(v.commit=="ni1");
 // English Backspace must not resurrect a previously committed Chinese word.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); v=e.key("Space"); assert(v.commit=="你好"); e.key("Shift"); v=e.key("BackSpace"); assert(v.mode==Mode::English && v.commit.empty() && v.preedit.empty() && e.raw().empty()); e.key("Shift"); assert(e.mode()==Mode::Chinese && e.raw().empty());
 // A partial commit keeps exactly the remaining syllables through a mode round trip.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); v=e.key("Down"); for(size_t i=0;i<v.candidates.size();++i) if(v.candidates[i].text=="你"){ni_index=i;break;} v=e.select(ni_index); assert(v.commit=="你" && v.preedit=="hao"); v=e.key("Shift"); assert(v.commit.empty()); v=e.key("Shift"); assert(v.commit.empty() && v.preedit=="hao"); v=e.key("Space"); assert(v.commit=="好" && e.raw().empty());
 // A temporary mode switch preserves the insertion point inside suspended
 // Chinese spelling, so resuming does not unexpectedly jump to the end.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c)); e.key("Home"); e.key("Right");
 v = e.key("Shift");
 assert(v.preedit == "nihao" && v.preedit_cursor == 1);
 // Rendering a release must preserve the suspended spelling too.  A
 // no-op key-up cannot temporarily clear a visible composition.
 v = e.key("Shift", true);
 assert(v.mode == Mode::English && v.preedit == "nihao" &&
        v.preedit_cursor == 1 && v.candidates.empty());
 v = e.key("Shift");
 assert(v.preedit == "nihao" && v.preedit_cursor == 1); v = e.key("x");
 assert(v.preedit == "nxihao" && v.preedit_cursor == 2);
 e.reset(); for(char c:std::string("shanghai"))v=e.key(std::string(1,c)); assert(!v.candidates.empty());
 // Ordinary pinyin must remain Chinese even as the English corpus grows.
 for (const auto *pinyin : {"beijing", "nihao", "women", "shenme", "zhongguo", "xianzai", "keyi", "shijie"}) {
   e.reset(); for (char c : std::string(pinyin)) v = e.key(std::string(1, c));
   assert(!v.candidates.empty() && v.preedit == pinyin);
 }
 // Punctuation commits the current Chinese candidate and remains undoable:
 // Backspace first removes the punctuation, then restores the preceding
 // Chinese chunk as editable pinyin (including UTF-8 committed text).
 e.reset(); for(char c:std::string("ni")) e.key(std::string(1,c));
 v=e.key("!"); assert(v.commit=="你!"); assert(v.preedit.empty());
 v=e.key("BackSpace"); assert(v.preedit.empty() && v.commit.empty());
 v=e.key("BackSpace"); assert(v.preedit=="ni" && e.raw()=="ni");
 // Home moves the preedit insertion point to the beginning; Delete then
 // removes the codepoint at that point.  At the end of the composition it is
 // a no-op because committed text is owned by the frontend's surrounding text.
 e.reset(); for(char c:std::string("ni")) e.key(std::string(1,c));
 v=e.key("Home"); assert(v.preedit=="ni" && v.preedit_cursor==0);
 v=e.key("Delete"); assert(v.preedit=="i" && e.raw()=="i" && v.preedit_cursor==0);
 v=e.key("Delete"); assert(v.preedit.empty() && e.raw().empty());
 v=e.key("Delete"); assert(v.preedit.empty() && v.commit.empty());
 // Editing in the middle keeps a byte cursor and inserts at that point.
 e.reset(); for(char c:std::string("nihao")) e.key(std::string(1,c));
 v=e.key("Home"); v=e.key("Right"); assert(v.preedit_cursor==1);
 v=e.key("x"); assert(v.preedit=="nxihao" && v.preedit_cursor==2);
 v=e.key("Home"); v=e.key("Delete"); assert(v.preedit=="xihao" && v.preedit_cursor==0);
 // A UTF-8 English commit is removed as one codepoint by Backspace, never a
 // partial byte sequence.
 e.reset(); e.key("Shift"); e.key("é");
 v=e.key("Space"); assert(v.commit=="é"); e.key("Shift");
 // Delete must remove a complete UTF-8 codepoint from an English preedit,
 // rather than erasing only its first byte and leaving malformed text.
 e.reset(); e.key("Shift"); e.key("é"); e.key("Home"); v=e.key("Delete");
 assert(v.preedit.empty() && e.raw().empty()); e.key("Shift");
 assert(!e.restorableCommit());
 v=e.key("BackSpace"); assert(v.preedit.empty() && v.commit.empty());
 // Delete in English mode must preserve a suspended Chinese composition in
 // the same way as Backspace.  The visible preedit belongs to the suspended
 // composition until the user switches back or starts English input.
 e.reset(); e.key("n"); e.key("i"); v=e.key("Shift");
 assert(v.mode==Mode::English && v.preedit=="ni" && e.raw().empty());
 for(const auto* key : {"Delete", "Tab", "ShiftTab", "Left", "Right", "Up", "Down", "Home", "End", "PageUp", "PageDown"}) {
   v=e.key(key); assert(v.mode==Mode::English && v.preedit=="ni" && e.raw().empty());
 }
 v=e.key("Shift"); assert(v.mode==Mode::Chinese && v.preedit=="ni" && e.raw()=="ni");
 // Mixed Chinese and English chunks keep their own history boundaries:
 // deleting one UTF-8 English codepoint must not resurrect the Chinese
 // chunk until the complete English chunk has been removed.
 e.reset(); for(char c:std::string("ni")) e.key(std::string(1,c));
 v=e.key("Space"); assert(v.commit=="你"); e.key("Shift"); e.key("é"); e.key("é");
 v=e.key("Space"); assert(v.commit=="éé"); e.key("Shift");
 e.key("BackSpace"); assert(!e.restorableCommit());
 e.key("BackSpace"); auto rr=e.restorableCommit(); assert(rr && rr->text=="你" && rr->pinyin=="ni");
 // Switching out of a live pinyin composition flushes its literal spelling;
 // that text is tracked as its own no-pinyin chunk for ordered deletion.
 e.reset(); e.key("n"); e.key("i"); e.key("Shift");
 v=e.key("g"); assert(v.commit=="ni" && v.preedit=="g"); e.key("Space"); e.key("Shift");
 e.key("BackSpace"); assert(e.raw().empty()); e.key("BackSpace"); assert(e.raw().empty() && !e.restorableCommit());
 // Malformed and very long compositions remain bounded and recoverable.
 e.reset(); for(int i=0;i<700;++i) e.key((i%3==0)?"q":"x"); assert(e.raw().size()==700); v=e.key("BackSpace"); assert(v.preedit.size()==699 && e.raw().size()==699); v=e.key("Enter"); assert(v.commit.size()==699 && v.preedit.empty()); e.reset();
 // Committed-chunk restoration history is bounded.  Keep adding Chinese
 // chunks beyond the 1024-entry cap, then walk the history through the same
 // Backspace path used by the Fcitx adapter.  Exactly the newest bounded
 // suffix remains restorable; older chunks have been discarded.
 {
   std::filesystem::remove_all("/tmp/tipe-history-bound-test");
   Engine bounded("/tmp/tipe-history-bound-test");
   bounded.setSensitive(true); // avoid learning I/O in this history stress loop
   bounded.reset();
   constexpr int kChunks = 1100;
   for (int i = 0; i < kChunks; ++i) {
     bounded.key("n"); bounded.key("i");
     auto committed = bounded.key("Space");
     assert(committed.commit == "你");
   }
   int restored = 0;
   while (auto chunk = bounded.restorableCommit()) {
     assert(chunk->text == "你" && chunk->pinyin == "ni");
     bounded.key("BackSpace");
     assert(bounded.raw() == "ni");
     bounded.key("BackSpace"); bounded.key("BackSpace");
     ++restored;
     assert(restored <= 1024);
   }
   assert(restored == 1024);
   assert(bounded.raw().empty());
 }
 // A single opaque token larger than the byte budget must not leave stale
 // restoration state behind.  A later normal Chinese commit remains
 // restorable, proving that the oversized chunk clears only prior history.
 {
   Engine oversized("/tmp/tipe-history-oversized-test");
   oversized.reset(); oversized.key("Shift");
   oversized.key(std::string(70 * 1024, 'x'));
   auto huge = oversized.key("Space");
   assert(huge.commit.size() == 70 * 1024);
   assert(!oversized.restorableCommit());
   oversized.key("Shift");
   oversized.key("n"); oversized.key("i");
   oversized.key("Space");
   auto tail = oversized.restorableCommit();
   assert(tail && tail->text == "你" && tail->pinyin == "ni");
 }
 e.reset(); for(char c:std::string("ni"))e.key(std::string(1,c)); v=e.key("Shift"); assert(v.mode==Mode::English && v.preedit=="ni" && v.candidates.empty()); v=e.key("g"); assert(v.commit=="ni" && v.preedit=="g"); v=e.key("Shift"); assert(v.mode==Mode::Chinese);
 { Event learned; learned.type=EventType::Commit; learned.key="Space"; learned.pinyin="ceshi"; learned.committed="测试词"; learned.context="terminal"; e.learner().push(learned); } assert(e.learner().wait_until_idle(std::chrono::seconds(2))); assert(e.learner().observed()>0 && e.learner().learned()>0);
 { Engine secret("/tmp/tipe-sensitive-test"); secret.learner().clear(); secret.setSensitive(true); for(char c:std::string("nihao")) secret.key(std::string(1,c)); secret.key("Space"); assert(secret.learner().observed()==0); }
 // A privacy or application-context boundary invalidates committed-text
 // restoration.  This prevents a later surrounding-text snapshot from a
 // password field or another client from deleting/reconstructing an older
 // chunk that TiPE no longer owns.
 { Engine boundary("/tmp/tipe-boundary-test");
   for (char c : std::string("nihao")) boundary.key(std::string(1, c));
   auto committed = boundary.key("Space"); assert(committed.commit == "你好");
   assert(boundary.restorableCommit());
   boundary.setSensitive(true); assert(!boundary.restorableCommit());
   boundary.setSensitive(false);
   for (char c : std::string("nihao")) boundary.key(std::string(1, c));
   boundary.key("Space"); assert(boundary.restorableCommit());
   boundary.setContext("another-client");
   assert(boundary.raw().empty() && !boundary.hasComposition());
   assert(!boundary.restorableCommit());
   for (char c : std::string("ni")) boundary.key(std::string(1, c));
   boundary.setSensitive(true);
   assert(boundary.raw() == "ni");
   assert(!boundary.restorableCommit());
 }
 { Engine chained("/tmp/tipe-chain-test"); chained.learner().clear(); for(char c:std::string("nihao")) chained.key(std::string(1,c)); auto cv=chained.key("Down"); size_t first=0; for(size_t i=0;i<cv.candidates.size();++i) if(cv.candidates[i].text=="你"){first=i;break;} chained.select(first); chained.key("Esc"); for(char c:std::string("zhongguo")) chained.key(std::string(1,c)); chained.key("Space"); assert(chained.learner().wait_until_idle(std::chrono::seconds(2))); for(const auto &entry: chained.learner().model()->entries) assert(entry.text!="你中国"); }
 { std::filesystem::remove_all("/tmp/tipe-personal-test"); Engine personal("/tmp/tipe-personal-test");
   for (int round = 0; round < 3; ++round) {
     personal.reset(); for (char c : std::string("woc")) personal.key(std::string(1,c));
     auto view = personal.key("Down"); size_t i = 0;
     for (; i < view.candidates.size(); ++i) if (view.candidates[i].text == "我") break;
     assert(i < view.candidates.size()); personal.select(i); assert(personal.raw() == "c");
     view = personal.key("Down"); i = 0;
     for (; i < view.candidates.size(); ++i) if (view.candidates[i].text == "操") break;
     assert(i < view.candidates.size()); personal.select(i); assert(personal.raw().empty());
     assert(personal.learner().wait_until_idle(std::chrono::seconds(2)));
   }
   personal.reset(); for (char c : std::string("woc")) personal.key(std::string(1,c));
   auto learned = personal.key("Down"); assert(!learned.candidates.empty());
   assert(learned.candidates.front().text == "我操");
 }
 // Physical key edges and navigation commands are observable independently
 // from composition semantics.  This guards the adapter-facing telemetry
 // API used for pass-through modifier chords and surrounding-text edits.
 {
   std::filesystem::remove_all("/tmp/tipe-event-coverage-test");
   Engine telemetry("/tmp/tipe-event-coverage-test");
   telemetry.learner().clear();
   telemetry.learner().wait_until_idle(std::chrono::seconds(1));
   const auto before = telemetry.learner().observed();
   auto wait_observed = [&](size_t n) {
     for (int i = 0; i < 100 && telemetry.learner().observed() < before + n; ++i)
       std::this_thread::sleep_for(std::chrono::milliseconds(5));
     return telemetry.learner().observed();
   };
   telemetry.observe("Shift");
   telemetry.observe("a");
   telemetry.observe("a", true);
   telemetry.observe("Shift", true);
   assert(wait_observed(4) == before + 4);
   assert(telemetry.raw().empty() && telemetry.mode() == Mode::Chinese);
   telemetry.key("n");
   assert(wait_observed(5) == before + 5);
   telemetry.key("Home");
   assert(wait_observed(7) == before + 7);
   telemetry.key("End");
   assert(wait_observed(9) == before + 9);
   telemetry.key("PageDown");
   assert(wait_observed(11) == before + 11);
   telemetry.key("PageUp");
   assert(wait_observed(13) == before + 13);
   telemetry.key("Esc");
   assert(telemetry.learner().wait_until_idle(std::chrono::seconds(2)));
   assert(telemetry.learner().observed() >= before + 14);
 }
 std::cout<<"all tests passed\n";   {
    const std::string dir = "/tmp/tipe-literal-preference";
    std::filesystem::remove_all(dir);
    auto type_literal = [](tipe::Engine &engine,const std::string &text) {
      tipe::View view;
      for (const char c : text) view=engine.key(std::string(1,c));
      return view;
    };
    {
      tipe::Engine engine(dir); engine.setContext("literal-editor");
      for (int i=0;i<3;++i) {
        const auto view=type_literal(engine,"nihao");
        assert(!view.candidates.empty());
        assert(engine.key("Enter").commit == "nihao");
        assert(engine.learner().wait_until_idle(std::chrono::seconds(3)));
      }
      auto preferred=type_literal(engine,"nihao");
      assert(preferred.candidates.size()>1 && preferred.candidates.front().text=="nihao");
      assert(engine.key("Space").commit == "nihao");
      assert(engine.learner().wait_until_idle(std::chrono::seconds(3)));
      engine.setContext("another-editor");
      assert(!type_literal(engine,"nihao").candidates.empty());
      engine.reset(); engine.setContext("literal-editor"); engine.setSensitive(true);
      assert(!type_literal(engine,"nihao").candidates.empty());
    }
    {
      tipe::Engine restarted(dir); restarted.setContext("literal-editor");
      auto preferred=type_literal(restarted,"nihao");
      assert(preferred.candidates.size()>1 && preferred.candidates.front().text=="nihao");
      size_t chinese=0;
      while(chinese<preferred.candidates.size() && preferred.candidates[chinese].text!="你好")++chinese;
      assert(chinese<preferred.candidates.size());
      assert(restarted.select(chinese).commit=="你好");
      assert(restarted.learner().wait_until_idle(std::chrono::seconds(3)));
      assert(type_literal(restarted,"nihao").candidates.front().text=="你好");
      restarted.reset();
      assert(restarted.learner().clear());
      assert(!type_literal(restarted,"nihao").candidates.empty());
    }
    std::filesystem::remove_all(dir);
  }
}
