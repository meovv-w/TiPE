#include "tipe/core.hpp"
#include "tipe/candidate_grid.hpp"
#include <algorithm>
#ifdef TIPE_HAS_PANGO_METRICS
#include <pango/pangocairo.h>
#endif
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <exception>
#include <thread>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <stdexcept>
#include <sys/stat.h>
#include <string_view>
#include <unordered_set>
#ifdef TIPE_HAS_LIBIME
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/core/userlanguagemodel.h>
#include "base_language_model.hpp"
#endif
using namespace std::chrono;
namespace tipe {
static bool grid_enabled() {
  const char *value = std::getenv("TIPE_CLASSIC_GRID");
  return value && (std::string_view(value) == "1" || std::string_view(value) == "true" || std::string_view(value) == "yes");
}
size_t Engine::candidatePageSize() const {
  return expanded_ && grid_enabled() ? 40 : 10;
}
// Approximate the bundled Pango Sans font for headless navigation. Counting
// UTF-8 bytes (the old implementation) made CJK cells three times wider than
// they are painted, so Up/Down landed on a different visual row.
static int navigation_text_width(std::string_view text, int font_pixels = 16) {
#ifdef TIPE_HAS_PANGO_METRICS
  struct Metrics {
    PangoFontMap *map = pango_cairo_font_map_new();
    PangoContext *context = pango_font_map_create_context(map);
    PangoLayout *layout = pango_layout_new(context);
    Metrics() {
      auto *font = pango_font_description_new();
      pango_font_description_set_family(font, "Sans");
      pango_font_description_set_absolute_size(font, 16 * PANGO_SCALE);
      pango_layout_set_font_description(layout, font);
      pango_layout_set_single_paragraph_mode(layout, true);
      pango_font_description_free(font);
    }
    ~Metrics() { g_object_unref(layout); g_object_unref(context); g_object_unref(map); }
  };
  thread_local Metrics metrics;
  pango_layout_set_text(metrics.layout, text.data(), static_cast<int>(text.size()));
  int measured_width = 0;
  pango_layout_get_pixel_size(metrics.layout, &measured_width, nullptr);
  return std::max(1, measured_width);
#else
  const int em = std::clamp(font_pixels, 8, 96);
  int width = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0; size_t count = 1;
    if (lead < 0x80) cp = lead;
    else if ((lead & 0xe0) == 0xc0 && i + 1 < text.size()) { cp = lead & 0x1f; count = 2; }
    else if ((lead & 0xf0) == 0xe0 && i + 2 < text.size()) { cp = lead & 0x0f; count = 3; }
    else if ((lead & 0xf8) == 0xf0 && i + 3 < text.size()) { cp = lead & 0x07; count = 4; }
    else { ++i; width += em / 2; continue; }
    for (size_t j = 1; j < count; ++j) cp = (cp << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
    const bool wide = cp >= 0x1100 && (cp <= 0x115f || cp >= 0x2329) &&
                      (cp <= 0x2ffb || cp >= 0x3000);
    width += wide ? em : std::max(1, em * 9 / 16);
    i += count;
  }
  return std::max(1, width);
#endif
}
// Committed text is owned by the client; this is only a bounded suffix used
// to prove and reconstruct the most recent TiPE commit after surrounding-text
// Backspace.  Keep both chunk count and payload bounded so a long-lived input
// context cannot grow without limit.
static constexpr size_t kCommitHistoryMaxChunks = 1024;
static constexpr size_t kCommitHistoryMaxBytes = 64 * 1024;
static uint64_t now_ms(){return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
static void erase_last_utf8(std::string &s) {
  if (s.empty()) return;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) --i;
  s.erase(i);
}
static void erase_first_utf8(std::string &s) {
  if (s.empty()) return;
  size_t n = 1;
  const auto lead = static_cast<unsigned char>(s[0]);
  if ((lead & 0x80) == 0) n = 1;
  else if ((lead & 0xe0) == 0xc0) n = 2;
  else if ((lead & 0xf0) == 0xe0) n = 3;
  else if ((lead & 0xf8) == 0xf0) n = 4;
  // Be defensive about malformed input: never erase beyond the string.
  s.erase(0, std::min(n, s.size()));
}
static size_t prev_utf8_boundary(const std::string &s, size_t pos) {
  pos = std::min(pos, s.size());
  if (pos == 0) return 0;
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xc0) == 0x80) --pos;
  return pos;
}
static size_t next_utf8_boundary(const std::string &s, size_t pos) {
  pos = std::min(pos, s.size());
  if (pos == s.size()) return pos;
  ++pos;
  while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xc0) == 0x80) ++pos;
  return pos;
}
static std::shared_ptr<Learner> process_learner(std::string dir){
  if(!dir.empty()) return std::make_shared<Learner>(std::move(dir));
  static std::shared_ptr<Learner> shared=std::make_shared<Learner>();
  return shared;
}

#ifdef TIPE_HAS_LIBIME
// LibIME's C++ API deliberately takes filenames and reports malformed data
// through exceptions.  Keep discovery here (rather than in the hot path) and
// only hand verified regular files to the library.  TIPE_LIBIME_DATA_DIR is
// an explicit override: when set, it is the complete search scope, which is
// useful for packaged installs and makes a missing-data deployment fail
// closed to the bounded built-in decoder instead of silently picking a stale
// system copy.
static std::vector<std::filesystem::path> split_path_list(const char *value) {
  std::vector<std::filesystem::path> result;
  if (!value || !*value) return result;
  std::string text(value);
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t end = text.find(':', begin);
    const auto item = text.substr(begin, end == std::string::npos ? end : end - begin);
    if (!item.empty()) result.emplace_back(item);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

static void add_unique_path(std::vector<std::filesystem::path> &paths,
                            std::filesystem::path path) {
  if (path.empty()) return;
  std::error_code ec;
  path = std::filesystem::weakly_canonical(path, ec);
  if (ec) path = path.lexically_normal();
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(std::move(path));
}

static std::vector<std::filesystem::path> libime_data_dirs() {
  if (const char *override_dir = std::getenv("TIPE_LIBIME_DATA_DIR")) {
    // An explicit empty value intentionally disables runtime discovery. This
    // is useful to package tests and avoids accidentally crossing a sandbox.
    if (*override_dir == '\0') return {};
    return split_path_list(override_dir);
  }
  std::vector<std::filesystem::path> dirs;
#ifdef TIPE_LIBIME_DATA_DIR
  add_unique_path(dirs, TIPE_LIBIME_DATA_DIR);
#endif
  if (const char *xdg_home = std::getenv("XDG_DATA_HOME"))
    add_unique_path(dirs, std::filesystem::path(xdg_home) / "libime");
  if (const char *xdg_dirs = std::getenv("XDG_DATA_DIRS")) {
    for (auto dir : split_path_list(xdg_dirs)) add_unique_path(dirs, dir / "libime");
  }
  // Keep distro layouts as a final fallback.  These are checked with
  // filesystem error codes and therefore remain harmless in a sandbox.
  add_unique_path(dirs, "/usr/share/libime");
  add_unique_path(dirs, "/usr/local/share/libime");
  return dirs;
}

static std::vector<std::filesystem::path> libime_model_dirs() {
  if (const char *override_file = std::getenv("TIPE_LIBIME_LM")) {
    if (*override_file == '\0') return {};
    return {std::filesystem::path(override_file)};
  }
  std::vector<std::filesystem::path> dirs;
  if (const char *override_dir = std::getenv("TIPE_LIBIME_MODEL_DIR")) {
    if (*override_dir == '\0') return {};
    dirs = split_path_list(override_dir);
  } else {
    dirs = libime_data_dirs();
    // TIPE_LIBIME_DATA_DIR is a hermetic package/test override. Do not
    // silently pair a caller-supplied dictionary with an unrelated system
    // language model; callers that intentionally split the two locations can
    // set TIPE_LIBIME_MODEL_DIR explicitly.
    if (std::getenv("TIPE_LIBIME_DATA_DIR")) return dirs;
    // The Fedora/Debian packages conventionally keep the large language model
    // under libime in the architecture-specific lib directory.
    add_unique_path(dirs, "/usr/lib64/libime");
    add_unique_path(dirs, "/usr/lib/libime");
    add_unique_path(dirs, "/usr/lib/x86_64-linux-gnu/libime");
  }
  return dirs;
}

static std::optional<std::filesystem::path>
find_libime_file(const std::vector<std::filesystem::path> &dirs,
                 std::string_view filename) {
  for (const auto &entry : dirs) {
    std::error_code ec;
    std::filesystem::path candidate = entry;
    // TIPE_LIBIME_LM may name the file itself rather than a directory.
    if (candidate.filename() == filename && std::filesystem::is_regular_file(candidate, ec))
      return candidate;
    candidate /= filename;
    if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
  }
  return std::nullopt;
}

struct LibimeRuntimeData {
  std::filesystem::path dictionary;
  std::filesystem::path language_model;
};

static std::optional<LibimeRuntimeData> discover_libime_data() {
  std::filesystem::path dictionary;
  if (const char *override_file = std::getenv("TIPE_LIBIME_DICT")) {
    if (*override_file == '\0') return std::nullopt;
    dictionary = override_file;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dictionary, ec)) return std::nullopt;
  } else {
    const auto found = find_libime_file(libime_data_dirs(), "sc.dict");
    if (!found) return std::nullopt;
    dictionary = *found;
  }
  const auto model = find_libime_file(libime_model_dirs(), "zh_CN.lm");
  if (!model) return std::nullopt;
  return LibimeRuntimeData{std::move(dictionary), *model};
}


static std::shared_ptr<libime::PinyinIME> shared_libime_data(const LibimeRuntimeData &data) {
  // PinyinIME explicitly owns the data shared by PinyinContexts. TiPE never
  // writes its dictionary/model or calls LibIME learn(); personal evidence
  // lives in Learner. Keep one warm data set per thread, avoiding both
  // concurrent mutable LibIME access and an unbounded path-keyed cache.
  // Include file identity/timestamps so package replacement, overrides and
  // corrupt-data recovery cannot silently reuse an obsolete successful load.
  auto fingerprint=[](const std::filesystem::path &path) {
    struct stat info{};
    if (::stat(path.c_str(),&info)!=0) throw std::runtime_error("LibIME data unavailable");
    return path.string()+":"+std::to_string(info.st_dev)+":"+std::to_string(info.st_ino)+":"+
      std::to_string(info.st_size)+":"+std::to_string(info.st_mtim.tv_sec)+":"+
      std::to_string(info.st_mtim.tv_nsec)+":"+std::to_string(info.st_ctim.tv_sec)+":"+
      std::to_string(info.st_ctim.tv_nsec);
  };
  const auto identity=fingerprint(data.dictionary)+"\n"+fingerprint(data.language_model);
  struct Cache {std::string identity;std::shared_ptr<libime::PinyinIME> ime;};
  thread_local Cache cache;
  if(cache.ime && cache.identity==identity)return cache.ime;
  auto dictionary=std::make_unique<libime::PinyinDictionary>();
  dictionary->load(0,data.dictionary.c_str(),libime::PinyinDictFormat::Binary);
  auto model=std::make_unique<BaseLanguageModel>(data.language_model.c_str());
  auto ime=std::make_shared<libime::PinyinIME>(std::move(dictionary),std::move(model));
  ime->setNBest(30);
  ime->setWordCandidateLimit(15);
  cache={identity,ime};
  return ime;
}
#endif

static const std::unordered_map<std::string,std::vector<std::pair<std::string,int>>> dict={
 {"ni",{{"你",100},{"尼",20},{"拟",10}}},{"hao",{{"好",100},{"号",30},{"浩",20}}},{"nihao",{{"你好",200}}},
 {"wo",{{"我",100},{"握",20},{"窝",10}}},{"c",{{"操",80},{"才",40},{"从",30}}},{"shi",{{"是",120},{"时",80},{"事",60},{"市",40}}},{"jie",{{"界",80},{"解",70}}},
 {"zhongguo",{{"中国",220}}},{"zaijian",{{"再见",180}}},{"de",{{"的",160},{"得",80},{"地",70}}}
};
static std::atomic<uint64_t> next_engine_id{1};
void Engine::prepareDecoder() {
#ifdef TIPE_HAS_LIBIME
  try {if(const auto data=discover_libime_data())shared_libime_data(*data);} catch (...) {}
#endif
}
Engine::Engine(std::string d):session_(next_engine_id.fetch_add(1) << 32),learner_(process_learner(std::move(d))){
#ifdef TIPE_HAS_LIBIME
  // Runtime data is optional even when the shared library was found at
  // configure time. A partially installed/corrupt dictionary must degrade to
  // the bounded local table rather than aborting the Fcitx process.
  if (const auto data = discover_libime_data()) {
    try {
      auto ime = shared_libime_data(*data);
      auto context = std::make_unique<libime::PinyinContext>(ime.get());
      libime_ime_ = std::move(ime);
      libime_ctx_ = std::move(context);
      decoder_backend_ = DecoderBackend::LibIME;
      decoder_status_ = "libime: " + data->dictionary.string() + ", " +
                        data->language_model.string();
    } catch (const std::exception &error) {
      // Do not log untrusted paths or exception text from a plugin process;
      // the status is available to the diagnostic tool and tests.
      decoder_status_ = std::string("builtin fallback (LibIME data rejected: ") + error.what() + ")";
      libime_ctx_.reset();
      libime_ime_.reset();
    } catch (...) {
      decoder_status_ = "builtin fallback (LibIME data rejected)";
      libime_ctx_.reset();
      libime_ime_.reset();
    }
  } else {
    decoder_status_ = "builtin fallback (LibIME runtime data unavailable)";
  }
#endif
  update();}
Engine::~Engine() = default;
void Engine::rememberCorrectionOrigin() {
  if (!correction_origin_.empty() || sensitive_ || mode_ != Mode::Chinese ||
      raw_.empty() || raw_.size() > 63 ||
      !std::all_of(raw_.begin(), raw_.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return;
  correction_origin_ = raw_;
  correction_started_ = now_ms();
}
bool Engine::english_like()const{
 if(raw_.empty())return false;
 // Keep URL/path tokens literal from the first reliable prefix.  In
 // particular, `http`/`https` must be recognized before the following `:`
 // arrives; otherwise the punctuation path below would ask the pinyin
 // decoder to commit the protocol as Chinese text.  A leading slash is the
 // corresponding unambiguous signal for absolute filesystem paths.
 std::string lower = raw_;
 std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
 if (lower.starts_with("http") || lower.starts_with("www.") ||
     raw_.front() == '/' || raw_.find('.') != std::string::npos ||
     raw_.find('/') != std::string::npos || raw_.find('_') != std::string::npos)
   return true;
 // Mixed alphanumeric identifiers are common in source trees and URLs. Keep
 // the numeric candidate keys available for ordinary pinyin (ni1/shi2), but
 // recognize unambiguous identifier prefixes as literal text.
 if ((lower == "v" || lower == "x" ||
      ((lower.starts_with("v") || lower.starts_with("x")) &&
       std::any_of(raw_.begin(), raw_.end(), [](unsigned char c) { return std::isdigit(c); })) ||
      lower.starts_with("x86") || lower.starts_with("x64") ||
      lower.starts_with("arm") || lower.starts_with("lib") ||
      (std::any_of(raw_.begin(), raw_.end(), [](unsigned char c) { return std::isdigit(c); }) &&
       (lower.starts_with("sha") || lower.starts_with("utf") || lower.starts_with("gpu"))))
     && raw_.size() >= 1) return true;
 if(raw_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)return false;
 if(std::any_of(raw_.begin(),raw_.end(),[](unsigned char ch){return std::isupper(ch);})) return true;
 // A few language independent spelling signals catch code/English tokens
  // without treating ordinary unknown pinyin (e.g. shanghai) as English.
 static const std::unordered_set<std::string> software={
   "github","docker","start","foobar","typescript","javascript","python",
   "discord","terminal","chromium","firefox","telegram","flatpak","kubernetes",
   "localhost","makefile","readme","config","frontend","backend","opensource",
   "commit","pull","request","browser","window","update","version","server",
   "client","source","build","release","debug","string","class","function",
   "variable","module","package","linux","hello","world","example","domain",
   "email","message","keyboard","desktop","folder","system","input","output",
   "status","control","process","thread","memory","network","socket","editor",
   // High-frequency English words used by the offline recognizer evaluation.
   "about","access","account","action","active","address","after","again","agent","alert",
   "allow","always","answer","anyone","apple","archive","around","arrive","article","aspect",
   "assign","audio","author","backup","balance","battery","because","before","believe","binary",
   "button","camera","cancel","change","channel","choice","circle","clean","close","cloud",
   "command","common","compile","complete","compute","connect","content","context","cookie",
   "copy","correct","create","current","custom","database","default","delete","design","device",
   "dialog","direct","display","download","driver","editor","enable","error","event",
   "export","feature","field","filter","finish","folder","format","forward","frame",
   "general","global","google","handle","header","helper","history","image","import","include",
   "index","install","internet","issue","language","layout","learn","library","limit","local",
   "location","machine","manager","memory","method","minute","model","notice","object","office",
   "option","output","package","parent","parser","password","pathway","pattern","people","permission",
   "plugin","pointer","profile","project","prompt","query","reason","record","remove","report",
   "reset","result","return","review","runner","safe","today","tomorrow","morning","night",
   "user","text","there","what","where","when","which","could","would","should","without",
   "another","common","future","general","important","please","thankyou","quickly","application",
   "repository","testing","compile","feature","versioning","readme","cloud","program","rust","golang",
   "json","yaml","xml","api","http","secure","service","database","storage","release","snapshot",
   "main","core","foo","bar","license","cmake","pyproject","toml","cpp","hpp","so","data","state",
   "current","previous","snapshot","candidates","grid","item","text","offset","match","loop","thread",
   "value","sum","build","modules","ui","gitlab","codeberg","docs","cdn","mirror",
   "pypi","crates","npmjs","dev","qt","gtk","wayland","fcitx","kernel","gnu","developer",
   "wikipedia","openai","chatgpt","news","ycombinator","bugs","mail","login","media","files","go",
   "src","cache","logs","session","jsonl","com","org","net","io","app","example"
 };
 if(software.contains(raw_)) return true;
 // Inflected forms and common derivations are frequent in normal English
 // prose. Match them against the bounded root vocabulary while retaining the
 // conservative pinyin behavior for short/unknown spellings.
 if (raw_.size() >= 5) {
   for (const auto *suffix : {"ing", "ed", "es", "er", "ly", "s", "ment", "ness", "tion", "al"}) {
     const std::string_view tail = suffix;
     if (raw_.size() <= tail.size()) continue;
     const std::string root = raw_.substr(0, raw_.size() - tail.size());
     if (software.contains(root) && root.size() >= 4) return true;
   }
 }
 for(const auto *seq : {"ck","th","ph","st","str","oo","ee"}) if(raw_.find(seq)!=std::string::npos) return true;
 // Pinyin syllables do not contain runs of three consonants.  This catches
 // generic code/URL/path fragments such as `www`, `usr`, and `example`
 // (`mpl`) without maintaining a dictionary of English words.
 size_t consonants = 0;
 std::string run;
 for (unsigned char ch : raw_) {
   if (std::isalpha(ch) && std::string_view("aeiouyAEIOUY").find(ch) == std::string_view::npos)
     { ++consonants; run.push_back(static_cast<char>(ch)); }
   else { consonants = 0; run.clear(); }
 if (consonants >= 3 && run.rfind("ng", 0) != 0 &&
       (run.find_first_not_of("ngNG") != std::string::npos ||
                           run.front() == 'w' || run.front() == 'W') &&
        raw_.size() >= 6 && raw_.find("sh") == std::string::npos &&
        raw_.find("zh") == std::string::npos && raw_.find("ch") == std::string::npos) return true;
 }
 return false;
}
std::vector<Candidate> Engine::lookup(const std::string& s) const {
  std::vector<Candidate> out;
  for (size_t length = s.size(); length > 0; --length) {
    auto it = dict.find(s.substr(0, length));
    if (it == dict.end()) continue;
    for (const auto &[text, score] : it->second)
      out.push_back({text, s.substr(0, length), score, length});
  }
  return out;
}
void Engine::update(bool reset_page) {
  candidates_.clear();
  preedit_cursor_ = std::min(preedit_cursor_, raw_.size());
  if (reset_page) cursor_ = 0;
  if (reset_page) page_ = 0;
  // Bound decoder work without making a long composition lose its Chinese
  // candidates. Decode a leading window, with one maximum pinyin syllable
  // of lookahead, and expose only complete graph endpoints inside the first
  // 64 bytes. Selection consumes that prefix and leaves every suffix byte
  // editable. Looking ahead avoids turning a syllable cut at byte 64 into a
  // different, incomplete syllable or manufacturing a false commit boundary.
  if (mode_ == Mode::English || raw_.empty() || english_like()) return;
  const bool windowed = raw_.size() > 64;
  const std::string decoded = raw_.substr(0, 70);
#ifdef TIPE_HAS_LIBIME
  if (libime_ctx_) {
    // Apply the actual changed range. In particular, an insertion/deletion
    // near the beginning must keep the unchanged suffix in LibIME's graph;
    // erasing that entire suffix and typing it again forces a cold decode.
    const auto previous=libime_ctx_->userInput();
    size_t common=0;
    while(common<previous.size() && common<decoded.size() && previous[common]==decoded[common]) ++common;
    size_t suffix=0;
    while(suffix<previous.size()-common && suffix<decoded.size()-common &&
          previous[previous.size()-suffix-1]==decoded[decoded.size()-suffix-1]) ++suffix;
    if(common<previous.size()-suffix) libime_ctx_->erase(common,previous.size()-suffix);
    libime_ctx_->setCursor(common);
    if(common<decoded.size()-suffix) libime_ctx_->type(decoded.substr(common,decoded.size()-common-suffix));
    // LibIME's stream already contains N-best sentences, word prefixes and
    // individual characters in language-model order. Enumerating arbitrary
    // character cursors reordered partial words and wasted decoder work.
    std::unordered_set<std::string> seen;
    for (const auto &result : libime_ctx_->candidates()) {
      if (candidates_.size() >= 256) break;
      // Preserve the mature sentence ordering, clipping only at the actual
      // end of a decoded word. candidatesToCursor() can fall back to just a
      // short first word when the requested position cuts through a syllable.
      libime::SentenceResult prefix;
      if (windowed) {
        libime::SentenceResult::Sentence words;
        for (const auto *word : result.sentence()) {
          if (word->to()->index() > 64) break;
          words.push_back(word);
        }
        prefix = libime::SentenceResult(std::move(words),result.score());
      }
      const auto &r = windowed ? prefix : result;
      if (r.sentence().empty()) continue;
      const size_t consumed = r.sentence().back()->to()->index();
      if (!consumed || consumed > std::min<size_t>(raw_.size(),64)) continue;
      auto text = r.toString();
      const auto identity = text + "\t" + std::to_string(consumed);
      if (!seen.insert(identity).second) continue;
      candidates_.push_back({std::move(text), libime_ctx_->candidateFullPinyin(r),
                             static_cast<int>(1000 * r.score()), consumed});
    }
    libime_ctx_->setCursor(decoded.size());
  } else {
    // Compile-time LibIME support does not imply that its runtime package is
    // present. Keep exactly the same bounded fallback used by no-LibIME
    // builds so behavior remains deterministic in a minimal installation.
    candidates_ = lookup(raw_.substr(0,64));
  }
#else
  candidates_ = lookup(raw_.substr(0,64));
#endif
  // Optional local-small backend: a bounded, immutable on-disk candidate
  // table is queried only when the user explicitly selects it.  TiP remains
  // the default and its decoder ordering is untouched unless this backend is
  // selected.  No process launch, network request or disk I/O occurs here;
  // ModelSettings keeps the parsed table in memory.
  if (learner_->models().selected() == ModelBackend::LocalSmall &&
      !sensitive_ && !raw_.empty()) {
    // Publication trains this model in the background. Protect the mature
    // decoder's first result and prefix boundaries while ranking lower full
    // candidates from learned character transitions.
    const auto snapshot = learner_->model();
    if (snapshot && snapshot->language_model.transitions()) {
      std::vector<size_t> positions;
      struct ScoredCandidate { Candidate candidate; double score; };
      std::vector<ScoredCandidate> lower;
      for(size_t i=1;i<candidates_.size() && i<32;++i) {
        if(candidates_[i].consumed!=raw_.size()) continue;
        positions.push_back(i);
        lower.push_back({candidates_[i],snapshot->language_model.score(candidates_[i].text,context_)});
      }
      // Decode UTF-8 and evaluate transitions once per candidate. A sort
      // comparator otherwise repeats that allocation and logarithm work.
      std::stable_sort(lower.begin(),lower.end(),[](const ScoredCandidate &a,const ScoredCandidate &b) {
        return a.score > b.score;
      });
      for(size_t i=0;i<positions.size();++i) candidates_[positions[i]]=std::move(lower[i].candidate);
    }
    const auto local = learner_->models().local_small_candidates(raw_, context_);
    for (auto it = local.rbegin(); it != local.rend(); ++it) {
      if (it->text.empty()) continue;
      candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(),
        [&](const Candidate &c) { return c.text == it->text && c.consumed == raw_.size(); }),
        candidates_.end());
      candidates_.insert(candidates_.begin(),
                         {it->text, it->pinyin, it->score, raw_.size(), false});
    }
  }
  // LibIME can return literal input instead of an empty list for undecodable
  // spellings. That is the only low-confidence tier in this implementation:
  // every usable base Chinese result takes precedence over generalized rules.
#ifdef TIPE_HAS_LIBIME
  const bool literal_only = std::all_of(candidates_.begin(), candidates_.end(),
      [&](const Candidate &c) { return c.text == raw_ && c.pinyin.empty(); });
  if (libime_ctx_ && literal_only && !sensitive_ && !windowed) {
    const auto corrections = learner_->correction_candidates(raw_, context_);
    std::vector<Candidate> inferred;
    for (const auto &corrected : corrections) {
      if (corrected.empty() || corrected.size() > 64) continue;
      if (libime_ctx_->size()) libime_ctx_->erase(0, libime_ctx_->size());
      libime_ctx_->type(corrected);
      for (const auto &r : libime_ctx_->candidates()) {
        if (r.sentence().empty()) continue;
        if (r.sentence().back()->to()->index() != corrected.size()) continue;
        auto pinyin = libime_ctx_->candidateFullPinyin(r);
        auto text = r.toString();
        if (pinyin.empty() || text.empty() || text == corrected) continue;
        if (std::none_of(inferred.begin(), inferred.end(),
                        [&](const Candidate &c) { return c.text == text; }))
          inferred.push_back({std::move(text), std::move(pinyin),
                              static_cast<int>(700 * r.score()), raw_.size(), true});
        break;
      }
    }
    candidates_.insert(candidates_.begin(), inferred.begin(), inferred.end());
    if (!corrections.empty()) {
      if (libime_ctx_->size()) libime_ctx_->erase(0, libime_ctx_->size());
      libime_ctx_->type(raw_);
      libime_ctx_->setCursor(raw_.size());
    }
  }
#endif
  // Explicit repeated choices have their own exact-input tier. Preserve the
  // decoder ordering of every other candidate, including the entire base
  // list for inputs with no matching personal evidence.
  const auto exact = sensitive_ ? std::vector<PersonalEntry>{} : learner_->exact_entries(raw_, context_);
  for (auto it = exact.rbegin(); it != exact.rend(); ++it) {
    if (it->text.empty()) continue;
    const auto found = std::find_if(candidates_.begin(), candidates_.end(),
        [&](const Candidate &c) { return c.text == it->text && c.consumed == raw_.size(); });
    Candidate preferred{it->text, it->pinyin, 0, raw_.size()};
    if (found != candidates_.end()) {
      preferred = *found; // keep the decoder's expanded syllables
      candidates_.erase(found);
    }
    candidates_.insert(candidates_.begin(), std::move(preferred));
  }
  // Optional remote suggestions are an additional, temporary tier. They
  // never displace a local first candidate or mutate LibIME/TiP. A later
  // explicit user choice may become ordinary exact personal evidence.
  if (!sensitive_ && learner_->models().selected() == ModelBackend::Cloud) {
    for (const auto &remote : learner_->cloud_candidates(raw_, context_)) {
      if (std::any_of(candidates_.begin(), candidates_.end(), [&](const Candidate &candidate) {
            return candidate.text == remote.text && candidate.consumed == raw_.size();
          })) continue;
      candidates_.push_back({remote.text, remote.pinyin, remote.score, raw_.size(), true});
      if (candidates_.size() >= 288) break;
    }
  }
  // Learned literal habits prefer the original spelling without hiding the
  // mature Chinese alternatives. A later explicit Chinese selection can
  // revoke the habit instead of permanently trapping an input as English.
  if (!sensitive_ && learner_->literal_preferred(raw_,context_)) {
    candidates_.erase(std::remove_if(candidates_.begin(),candidates_.end(),
        [&](const Candidate &candidate){return candidate.text==raw_;}),candidates_.end());
    candidates_.insert(candidates_.begin(),{raw_,{},0,raw_.size(),false});
  }
}
void Engine::refreshCandidates() {
  std::string selected; size_t selected_consumed = 0;
  const bool preserve = expanded_;
  if (preserve && cursor_ < candidates_.size()) { selected = candidates_[cursor_].text; selected_consumed = candidates_[cursor_].consumed; }
  update(false);
  // Expanded navigation exposes prefix candidates appended by the LibIME
  // cursor enumeration. Rebuild that same set before restoring the selected
  // item; otherwise Space/Enter after navigating onto a prefix would silently
  // commit whichever base candidate happened to occupy the old index.
  if (preserve) addPrefixCandidates();
  if (!preserve) { cursor_ = 0; page_ = 0; }
  bool restored = false;
  if (!selected.empty()) for (size_t i=0;i<candidates_.size();++i)
    if (candidates_[i].text == selected && candidates_[i].consumed == selected_consumed) { cursor_=i; page_=i/candidatePageSize(); restored = true; break; }
  if (preserve && !selected.empty() && !restored) cursor_ = page_ = 0;
  // Decoder/personal-preference updates can remove the previously selected
  // item. Keep navigation and numeric selection inside the new list rather
  // than carrying a stale global cursor into the next commit.
  if (candidates_.empty()) {
    cursor_ = page_ = 0;
  } else {
    cursor_ = std::min(cursor_, candidates_.size() - 1);
    page_ = cursor_ / candidatePageSize();
  }
}
void Engine::addPrefixCandidates() {
#ifdef TIPE_HAS_LIBIME
  // Prefix enumeration is meaningful only for an active Chinese spelling.
  // A previous navigation key can leave expanded_ set after the composition
  // has been cleared; never feed a later literal/English token through the
  // old LibIME cursor state in that case.
  if (!libime_ctx_ || raw_.empty() || mode_ != Mode::Chinese ||
      literal_token_ || english_like()) return;
  // Arrow keys refresh personal/cloud ranking, but a stable decoder spelling
  // and identical base list need not enumerate every prefix again. Key both
  // lists by value, not a model version: an asynchronous publication may
  // actually change the base ranking, so it must invalidate this result.
  // This cache is per-context, bounded to one spelling (at most 70 bytes)
  // and two lists of at most 512 candidates. No raw key stream is retained.
  if(candidates_.size()>=512)return;
  const auto input=libime_ctx_->userInput();
  if(input==prefix_cache_input_) {
    if(candidates_==prefix_cache_expanded_)return;
    if(candidates_==prefix_cache_base_) {candidates_=prefix_cache_expanded_;return;}
  }
  prefix_cache_input_=input;
  prefix_cache_base_=candidates_;
  std::unordered_set<std::string> seen;
  seen.reserve(1024);
  for(const auto &candidate:candidates_)
    seen.insert(candidate.text+"\t"+std::to_string(candidate.consumed));
  for (size_t pos = 1; pos < std::min<size_t>(raw_.size(), 24) && candidates_.size() < 512; ++pos) {
    libime_ctx_->setCursor(pos);
    for (const auto &r : libime_ctx_->candidatesToCursor()) {
      if (candidates_.size() >= 512 || r.sentence().empty()) break;
      // candidatesToCursor() may include a complete syllable/sentence which
      // extends beyond the requested cursor. The graph endpoint, not that
      // request position, tells us how much of the original input is used.
      // Tagging e.g. 你好 with pos=3 left "ao" after selecting the full word,
      // and also manufactured duplicate candidates with false boundaries.
      const size_t consumed = r.sentence().back()->to()->index();
      if (!consumed || consumed > std::min<size_t>(raw_.size(),64)) continue;
      auto text = r.toString();
      if (!seen.insert(text+"\t"+std::to_string(consumed)).second) continue;
      candidates_.push_back({std::move(text), libime_ctx_->candidateFullPinyin(r),
                             static_cast<int>(900 * r.score()), consumed});
    }
  }
  libime_ctx_->setCursor(libime_ctx_->size());
  prefix_cache_expanded_=candidates_;
#endif
}
void Engine::observe(const std::string& k, bool release) {
  const auto event_time = now_ms();
  Event ev;
  ev.type = release ? EventType::KeyUp : EventType::KeyDown;
  ev.key = k;
  ev.preedit = raw_;
  ev.context = context_;
  ev.ts_ms = event_time;
  ev.session = session_;
  ev.sensitive = sensitive_;
  if (last_event_ms_)
    ev.interval_ms = static_cast<uint32_t>(std::min<uint64_t>(
        event_time - last_event_ms_, UINT32_MAX));
  if ((k == "BackSpace" || k == "Delete") && last_delete_ms_)
    ev.delete_interval_ms = static_cast<uint32_t>(std::min<uint64_t>(
        event_time - last_delete_ms_, UINT32_MAX));
  if (k == "BackSpace" || k == "Delete") last_delete_ms_ = event_time;
  last_event_ms_ = event_time;
  learner_->push(std::move(ev));
}

void Engine::trimCommitHistory() {
  // Remove complete oldest chunks only.  A chunk is the atomic unit needed by
  // surrounding-text restoration, so never split its pinyin/text payload.
  size_t bytes = 0;
  for (const auto &chunk : chunks_) bytes += chunk.text.size() + chunk.pinyin.size();
  while (!chunks_.empty() &&
         (chunks_.size() > kCommitHistoryMaxChunks ||
          (bytes > kCommitHistoryMaxBytes && chunks_.size() > 1))) {
    const auto &oldest = chunks_.front();
    const size_t text_size = oldest.text.size();
    if (committed_.size() >= text_size &&
        committed_.compare(0, text_size, oldest.text) == 0)
      committed_.erase(0, text_size);
    else {
      // Keep the two representations synchronized even if a future edit path
      // changes their ordering.  This branch is defensive and uncommon.
      committed_.clear();
      for (size_t i = 1; i < chunks_.size(); ++i) committed_ += chunks_[i].text;
    }
    bytes -= std::min(bytes, oldest.text.size() + oldest.pinyin.size());
    chunks_.erase(chunks_.begin());
  }
}

void Engine::recordCommitChunk(std::string text, std::string pinyin) {
  // An individual commit larger than the complete bounded history cannot be
  // safely represented for restoration.  It has already been sent to the
  // client, so discard stale history and leave this oversized token opaque.
  if (text.size() + pinyin.size() > kCommitHistoryMaxBytes) {
    committed_.clear();
    chunks_.clear();
    return;
  }
  committed_ += text;
  chunks_.push_back({std::move(text), std::move(pinyin)});
  trimCommitHistory();
}

View Engine::modeSwitch() {
  // This operation is used by protocol adapters after they have already
  // recorded the physical Shift release.  Keep the mode transition and its
  // semantic event in one place without manufacturing a second key-down.
  std::string flushed;
  auto old_session = session_++;
  if (mode_ == Mode::Chinese) {
    mode_ = Mode::English;
    suspended_ = raw_;
    suspended_cursor_ = preedit_cursor_;
    raw_.clear();
    preedit_cursor_ = 0;
    english_touched_ = false;
    candidates_.clear();
  } else {
    if (!english_touched_) {
      raw_ = suspended_;
      preedit_cursor_ = std::min(suspended_cursor_, raw_.size());
    } else {
      flushed = raw_;
      if (!raw_.empty()) recordCommitChunk(raw_, "");
      raw_.clear();
      preedit_cursor_ = 0;
    }
    suspended_.clear();
    suspended_cursor_ = 0;
    english_touched_ = false;
    mode_ = Mode::Chinese;
    update();
  }
  Event m;
  m.type = EventType::ModeSwitch;
  m.key = "Shift";
  m.preedit = raw_;
  m.context = context_;
  m.ts_ms = now_ms();
  m.session = old_session;
  m.sensitive = sensitive_;
  learner_->push(std::move(m));
  return {raw_.empty() ? suspended_ : raw_,
          mode_ == Mode::English ? std::vector<Candidate>{} : candidates_,
          cursor_, expanded_, mode_, flushed, false,
          raw_.empty() ? std::min(suspended_cursor_, suspended_.size())
                       : preedit_cursor_};
}

View Engine::key(const std::string& k,bool release){
  // Adapters normally translate platform keysyms to these canonical names,
  // but Engine is also a public protocol boundary used by lightweight
  // frontends and test harnesses.  Accept the spellings emitted by common
  // X11/Wayland/Qt/GTK adapters here as well, so editing and commit behavior
  // does not depend on which adapter happened to be in front of the core.
  // Re-enter once with the canonical value so telemetry contains one event
  // carrying the same stable key name as the Fcitx adapter.
  if (k == "Backspace") return key("BackSpace", release);
  if (k == "Escape") return key("Esc", release);
  if (k == "Return" || k == "KP_Enter" || k == "KPEnter")
    return key("Enter", release);
  if (k == "Page_Up") return key("PageUp", release);
  if (k == "Page_Down") return key("PageDown", release);
  if (k == "ISO_Left_Tab" || k == "Shift+Tab") return key("ShiftTab", release);
  const auto event_time=now_ms();
  if (!correction_origin_.empty() && event_time - correction_started_ > 30000)
    correction_origin_.clear();
  if (!release && k == "Shift") correction_origin_.clear();
  Event ev; ev.type=release?EventType::KeyUp:EventType::KeyDown; ev.key=k; ev.preedit=raw_; ev.context=context_; ev.ts_ms=event_time; ev.session=session_; ev.sensitive=sensitive_;
  if(last_event_ms_) ev.interval_ms=static_cast<uint32_t>(std::min<uint64_t>(event_time-last_event_ms_,UINT32_MAX));
  if((k=="BackSpace"||k=="Delete") && last_delete_ms_) ev.delete_interval_ms=static_cast<uint32_t>(std::min<uint64_t>(event_time-last_delete_ms_,UINT32_MAX));
  if(k=="BackSpace"||k=="Delete") last_delete_ms_=event_time;
  last_event_ms_=event_time;
  learner_->push(ev);
  if (release) {
    // Key-up is observational: its View must not erase a Chinese spelling
    // suspended by a mode switch.  Adapters that render both edges should
    // see the same preedit and insertion point until the next editing key.
    const bool suspended = mode_ == Mode::English && raw_.empty() &&
                           !suspended_.empty();
    return {suspended ? suspended_ : raw_,
            mode_ == Mode::English ? std::vector<Candidate>{} : candidates_,
            cursor_, expanded_, mode_, {}, false,
            suspended ? std::min(suspended_cursor_, suspended_.size())
                      : preedit_cursor_};
  }
  // Once a composition is empty, these keys belong to the client's real
  // insertion cursor.  Drop the guarded committed-chunk suffix at the core
  // boundary as well as in the Fcitx adapter, so other protocol adapters
  // cannot later resurrect an old commit from a matching surrounding snapshot
  // after the client has edited or moved elsewhere.  Backspace is deliberately
  // excluded: it is the explicit transaction that may restore that suffix.
  if (raw_.empty() && suspended_.empty() &&
      (k == "Delete" || k == "Left" || k == "Right" || k == "Home" ||
       k == "End" || k == "Up" || k == "Down"))
    invalidateCommitHistory();
  // While a Chinese spelling is suspended by a Shift tap, it remains the
  // visible preedit even though the active mode is English.  Keep this view
  // stable for no-op presentation/navigation keys; otherwise a Tab or arrow
  // key would transiently clear the preedit until the next mode switch.
 const auto suspended_view = [&]() -> View {
    const auto &text = suspended_.empty() ? raw_ : suspended_;
    const auto point = suspended_.empty() ? preedit_cursor_ :
                       std::min(suspended_cursor_, text.size());
    return {text, {}, 0, false, mode_, {}, false, point};
  };
 const auto edit_view = [&]() -> View {
    return {raw_, mode_ == Mode::English ? std::vector<Candidate>{} : candidates_,
            cursor_, expanded_, mode_, {}, false, preedit_cursor_};
  };
  const auto insert_text = [&](std::string_view text) {
    preedit_cursor_ = std::min(preedit_cursor_, raw_.size());
    raw_.insert(preedit_cursor_, text);
    preedit_cursor_ += text.size();
  };
 if(k=="Shift") return modeSwitch();
 // Space chooses the highlighted Chinese candidate. Enter is the explicit
 // way to keep the original spelling, even after candidate navigation or a
 // partial selection, and must never silently convert it to Chinese.
 if(k=="Enter") return commitLiteral(k);
 if(k=="Space") return commitWithKey(k);
 if(k=="BackSpace"){
   ++session_;
   // In English mode the suspended Chinese composition is not an editable
   // committed chunk.  Backspace must never resurrect it or remove already
   // committed Chinese text; with an empty English preedit the client owns
   // the surrounding-text edit.
   if(mode_==Mode::English){
     if(!raw_.empty() && preedit_cursor_ > 0){
       const auto begin = prev_utf8_boundary(raw_, preedit_cursor_);
       raw_.erase(begin, preedit_cursor_ - begin); preedit_cursor_ = begin; update();
     }
     // A Chinese preedit suspended by a mode tap remains visible and intact
     // until the user either resumes Chinese mode or commits it explicitly.
    if(raw_.empty() && !suspended_.empty())
       return suspended_view();
     return edit_view();
   }
   if(!raw_.empty() && preedit_cursor_ > 0){
     rememberCorrectionOrigin();
     const auto begin = prev_utf8_boundary(raw_, preedit_cursor_);
     raw_.erase(begin, preedit_cursor_ - begin); preedit_cursor_ = begin;
     if(raw_.empty()) literal_token_=false; update();
   }
   else if(raw_.empty() && !chunks_.empty() && !chunks_.back().pinyin.empty()) {
     // Restore the last committed Chinese chunk as editable pinyin.
     auto ch=chunks_.back(); chunks_.pop_back();
     if(committed_.size()>=ch.text.size()) committed_.erase(committed_.size()-ch.text.size());
     raw_=ch.pinyin; preedit_cursor_=raw_.size(); update();
   } else if(!committed_.empty()) {
     // English commits have no pinyin; remove one UTF-8 codepoint.
     erase_last_utf8(committed_);
     if(!chunks_.empty() && chunks_.back().pinyin.empty()) {
       erase_last_utf8(chunks_.back().text);
       if(chunks_.back().text.empty()) chunks_.pop_back();
     }
   }
   return edit_view();
 }
 if(k=="Delete"){
   ++session_;
   if(!raw_.empty() && preedit_cursor_ < raw_.size()){
     rememberCorrectionOrigin();
     const auto end = next_utf8_boundary(raw_, preedit_cursor_);
     raw_.erase(preedit_cursor_, end - preedit_cursor_);
     if(raw_.empty()) literal_token_=false;
     update();
     return edit_view();
   }
   // A Chinese composition suspended by a Shift tap is still the visible
   // preedit while English mode is active.  Delete has no editable English
   // codepoint in this state, so preserve that suspended spelling instead of
   // returning an empty view (which used to make the preedit disappear until
   // the next mode switch).
   if(mode_==Mode::English && !suspended_.empty()) return suspended_view();
   update();
   return edit_view();
 }
 if(k=="Esc"){
   if (expanded_) {
     Event close{EventType::CandidateClose, k, raw_, {}, context_, now_ms()};
     close.session = session_; close.sensitive = sensitive_;
     learner_->push(std::move(close));
   }
   return reset();
 }
 if(k=="Tab"){if(mode_==Mode::English)return suspended_view(); addPrefixCandidates(); expanded_=!raw_.empty() && !candidates_.empty(); page_=cursor_/candidatePageSize(); Event open{EventType::CandidateOpen,k,raw_,{},context_,now_ms()}; open.session=session_; open.sensitive=sensitive_; learner_->push(std::move(open)); return edit_view();}
 if(k=="ShiftTab"){if(mode_==Mode::English)return suspended_view(); expanded_=false; page_=cursor_/candidatePageSize(); Event close{EventType::CandidateClose,k,raw_,{},context_,now_ms()}; close.session=session_; close.sensitive=sensitive_; learner_->push(std::move(close)); return edit_view();}
 if((k=="Home" || k=="End") && mode_==Mode::Chinese &&
    (!expanded_ || candidates_.empty())) {
   preedit_cursor_ = k == "Home" ? 0 : raw_.size();
   Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()};
   move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move));
   return edit_view();
 }
 if(k=="Home" && mode_==Mode::Chinese){expanded_=true; cursor_=0; page_=0; Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()}; move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move)); return edit_view();}
 if(k=="End" && mode_==Mode::Chinese){expanded_=true; if(!candidates_.empty()){cursor_=candidates_.size()-1; page_=cursor_/candidatePageSize();} Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()}; move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move)); return edit_view();}
 if(k=="PageDown"||k=="Page_Down") { if(mode_ != Mode::Chinese) return suspended_view(); if(!candidates_.empty()){ expanded_=true; page_=cursor_/candidatePageSize(); if((page_+1)*candidatePageSize()<candidates_.size()){ ++page_; cursor_=page_*candidatePageSize(); } } Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()}; move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move)); return edit_view(); }
 if(k=="PageUp"||k=="Page_Up") { if(mode_ != Mode::Chinese) return suspended_view(); if(!candidates_.empty()){ expanded_=true; page_=cursor_/candidatePageSize(); if(page_>0){ --page_; cursor_=page_*candidatePageSize(); } } Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()}; move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move)); return edit_view(); }
 if(k=="Down"||k=="Up"||k=="Left"||k=="Right"||k=="Home"||k=="End"){
   if(mode_ == Mode::English && !raw_.empty() &&
      (k == "Left" || k == "Right" || k == "Home" || k == "End")) {
     if (k == "Left") preedit_cursor_ = prev_utf8_boundary(raw_, preedit_cursor_);
     else if (k == "Right") preedit_cursor_ = next_utf8_boundary(raw_, preedit_cursor_);
     else preedit_cursor_ = k == "Home" ? 0 : raw_.size();
     Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()};
     move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move));
     return edit_view();
   }
   if(mode_ != Mode::Chinese || english_like())return suspended_view();
   if ((k == "Left" || k == "Right") && (!expanded_ || candidates_.empty())) {
     preedit_cursor_ = k == "Left" ? prev_utf8_boundary(raw_, preedit_cursor_)
                                    : next_utf8_boundary(raw_, preedit_cursor_);
     Event move{EventType::CandidateMove,k,raw_,{},context_,now_ms()};
     move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move));
     return edit_view();
   }
   refreshCandidates();
   // Do not leave a phantom expanded state when navigation is pressed with
   // no composition.  That stale flag would cause the next unrelated token
   // to invoke addPrefixCandidates() during Space/Enter refresh.
   expanded_=!raw_.empty() && !candidates_.empty();
   page_=cursor_/candidatePageSize();
   if(!candidates_.empty()) {
     if(k=="Home") cursor_=page_*candidatePageSize();
     else if(k=="End") cursor_=std::min(candidates_.size()-1, page_*candidatePageSize()+candidatePageSize()-1);
    else {
       const size_t begin = page_ * candidatePageSize();
       std::vector<grid::Candidate> cells;
       cells.reserve(candidates_.size());
       for (size_t i = 0; i < candidates_.size(); ++i) {
         const bool visible = i >= begin && i < begin + candidatePageSize();
         cells.push_back({candidates_[i].text, visible ? navigation_text_width(candidates_[i].text) : 0});
       }
       grid::Layout grid_layout;
       if (grid_enabled()) {
         grid_layout = grid::strip_layout(cells, true, 768, 16384, begin,
                                         static_cast<int>(cursor_));
       } else {
         grid::Options options;
         options.expanded = true;
         const char *layout = std::getenv("TIPE_CLASSIC_LAYOUT");
         options.available_width = layout && (std::string_view(layout) == "vertical" || std::string_view(layout) == "column") ? 1 : 640;
         grid_layout = grid::layout(cells, options, begin);
       }
       const auto direction = k=="Up" ? grid::Direction::Up :
                              k=="Down" ? grid::Direction::Down :
                              k=="Left" ? grid::Direction::Left : grid::Direction::Right;
       const auto movement = grid::navigate(grid_layout, cursor_, direction);
       if (movement.candidate_index) cursor_ = *movement.candidate_index;
       else if (movement.action == grid::Action::PreviousPage && begin > 0) {
         --page_;
         cursor_ = begin - 1;
       } else if (movement.action == grid::Action::NextPage &&
                  (page_ + 1) * candidatePageSize() < candidates_.size()) {
         ++page_;
         cursor_ = page_ * candidatePageSize();
       } else if (movement.action == grid::Action::EditPinyin) {
         expanded_ = false; page_ = 0;
       }
     }
     page_=cursor_/candidatePageSize();
   }
   Event move{EventType::CandidateMove,k,raw_,{} ,context_,now_ms()}; move.session=session_; move.sensitive=sensitive_; learner_->push(std::move(move));
   return edit_view();
 }
 // Once a literal URL/path token has started, digits are part of the token
 // (ports, query parameters and versioned paths), not candidate-selection
 // keys.  Keep the normal 1..9/0 selection behavior for Chinese pinyin.
 if(k.size()==1 && std::isdigit((unsigned char)k[0]) && !english_like() && !literal_token_ &&
    raw_.find_first_of("0123456789") == std::string::npos &&
    raw_ != "x" && raw_ != "sha" && raw_ != "utf" && raw_ != "gpu" && raw_ != "arm") {
   refreshCandidates(); size_t d=k[0]-'0'; size_t n=(d==0?9:d-1); size_t idx=(cursor_/10)*10+n; if(idx<candidates_.size()) return acceptCandidate(idx,k);
 }
 if(mode_==Mode::English){std::string flushed; if(!suspended_.empty()){flushed=suspended_;suspended_.clear(); /* The adapter commits this flushed text immediately. Keep
    an internal chunk so subsequent Backspace can remove it in order. */ recordCommitChunk(flushed, ""); } insert_text(k);english_touched_=true;update();if(!flushed.empty()){Event ce;ce.type=EventType::Commit;ce.key="mode-flush";ce.committed=flushed;ce.before=flushed;ce.context=context_;ce.ts_ms=now_ms();ce.sensitive=sensitive_;learner_->push(ce);}return {raw_,{},0,false,mode_,flushed,false,preedit_cursor_};}
 // Digits are literal characters once URL/path evidence is present (for
 // example `:8080` or `?page=2`).  They must bypass numeric candidate
 // selection, which is only meaningful while decoding Chinese pinyin.
 if(k.size()==1 && std::isdigit(static_cast<unsigned char>(k[0])) &&
    (english_like() || literal_token_ || raw_.find_first_of("0123456789") != std::string::npos ||
     raw_ == "x" || raw_ == "sha" ||
     raw_ == "utf" || raw_ == "gpu" || raw_ == "arm")) {
   insert_text(k); update(); return edit_view();
 }
 const bool alpha_key = k.size() == 1 &&
                        std::isalpha(static_cast<unsigned char>(k[0]));
 const bool pinyin_separator = k == "'" && !raw_.empty() && !english_like();
 if(k.size()==1 && (literal_token_ || alpha_key || pinyin_separator)) {
   // Preserve capitalization so names such as TypeScript remain intact when
   // typed while TiPE is in Chinese mode. Lowercase pinyin is unchanged and
   // continues through the LibIME decoder.
   insert_text(k); update();
 }
 else if(k.size()==1 && std::ispunct(static_cast<unsigned char>(k[0]))) {
   // Once a URL/path/code token has a reliable literal signal, punctuation
   // belongs to that token.  Appending it keeps the whole spelling in one
   // preedit so Space/Enter emits the exact original text.  A leading '/'
   // starts the same path mode while the composition is still empty.
   if ((raw_.empty() && k == "/") || (!raw_.empty() && english_like()) ||
       (k == "_" && !raw_.empty() &&
        std::all_of(raw_.begin(), raw_.end(), [](unsigned char c) {
          return std::isalpha(c) || std::isdigit(c) || c == '-' || c == '_';
        })) ||
       (k == "-" && !raw_.empty() && raw_.size() >= 3 &&
        std::all_of(raw_.begin(), raw_.end(), [](unsigned char c) {
          return std::isalpha(c) || std::isdigit(c) || c == '_' || c == '-';
        })) ||
       (k == "." && !raw_.empty() &&
        std::any_of(raw_.begin(), raw_.end(), [](unsigned char c) { return std::isdigit(c); })) ||
       (k == "/" && !raw_.empty() &&
        std::none_of(candidates_.begin(), candidates_.end(),
          [&](const Candidate &c) { return c.consumed == raw_.size() && !c.pinyin.empty(); }) &&
        std::all_of(raw_.begin(), raw_.end(), [](unsigned char c) {
          return std::isalpha(c) || std::isdigit(c) || c == '-' || c == '_';
        })) ||
       // A period following an undecodable ASCII token is the first reliable
       // domain/filename signal (for example `example.`).  Keep that token
       // intact instead of committing a low-confidence decoder result before
       // the suffix arrives.
       (k == "." && raw_ == "www") ||
       (k == "." &&
        std::none_of(candidates_.begin(), candidates_.end(),
          [&](const Candidate &c) { return c.consumed == raw_.size() && !c.pinyin.empty(); }) &&
        std::all_of(raw_.begin(), raw_.end(), [](unsigned char c) {
          return std::isalpha(c) || std::isdigit(c) || c == '-' || c == '_';
        }))) {
     insert_text(k);
     literal_token_ = true;
     update();
     return {raw_, {}, 0, false, mode_, {}, false, preedit_cursor_};
   }
   std::string committed;
   while(!raw_.empty()) {
     // Punctuation finalizes the composition. A selected prefix may leave
     // several syllables; consume those through the same path so none are
     // discarded and the learner sees one continuous selection chain.
     auto cv=commitWithKey(k);
     committed+=cv.commit;
     if(cv.commit.empty())
       return {raw_,candidates_,cursor_,expanded_,mode_,committed,false,preedit_cursor_};
   }
   committed += k; recordCommitChunk(k, "");
   literal_token_ = false;
   return {raw_,{},0,false,mode_,committed,false,preedit_cursor_};
 }
 return {raw_,english_like()?std::vector<Candidate>{}:candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};}
View Engine::select(size_t index) { return acceptCandidate(index,"candidate"); }

View Engine::acceptCandidate(size_t index,const std::string& trigger) {
  if(index>=candidates_.size()) return {raw_,candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};
  const Candidate candidate=candidates_[index];
  if(candidate.text==raw_ && candidate.pinyin.empty()) return commitLiteral("Space");
  // A candidate without a valid decoder boundary must never emit text: it
  // would otherwise remain selectable and duplicate commits indefinitely.
  if(candidate.text.empty() || candidate.consumed==0 || candidate.consumed>raw_.size())
    return {raw_,candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};
  const std::string before=raw_;
  recordCommitChunk(candidate.text, raw_.substr(0,candidate.consumed));
  raw_.erase(0,candidate.consumed);
  preedit_cursor_ = preedit_cursor_ >= candidate.consumed
                        ? preedit_cursor_ - candidate.consumed : 0;
  if (raw_.empty()) literal_token_ = false;
  update(); expanded_=false; cursor_=0; page_=0;
  Event event;
  event.type=EventType::CandidateSelect;
  event.key=trigger;
  event.before=before;
  event.preedit=before;
  event.committed=candidate.text;
  event.pinyin=candidate.pinyin;
  event.remaining=raw_;
  event.context=context_;
  event.corrected_from=correction_origin_;
  event.session=session_;
  event.ts_ms=now_ms();
  event.sensitive=sensitive_;
  event.inferred=candidate.inferred;
  learnCommit(std::move(event));
  if(raw_.empty()) { correction_origin_.clear(); ++session_; }
  preedit_cursor_ = std::min(preedit_cursor_, raw_.size());
  return {raw_,candidates_,cursor_,expanded_,mode_,candidate.text,false,preedit_cursor_};
}

View Engine::commit() { return commitWithKey("commit"); }

View Engine::commitWithKey(const std::string& trigger) {
  if(!hasComposition()) return {raw_,candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};
  if(mode_==Mode::Chinese) {
    refreshCandidates();
    if (!candidates_.empty())
      return acceptCandidate(std::min(cursor_,candidates_.size()-1),trigger);
  }
  return commitLiteral(trigger);
}

View Engine::view() const {
  const bool suspended = mode_ == Mode::English && raw_.empty() &&
                         !suspended_.empty();
  return {suspended ? suspended_ : raw_,
          mode_ == Mode::English ? std::vector<Candidate>{} : candidates_,
          cursor_, expanded_, mode_, {}, false,
          suspended ? std::min(suspended_cursor_, suspended_.size())
                    : preedit_cursor_};
}

View Engine::commitLiteral(const std::string& trigger) {
  if (!hasComposition())
    return {raw_,candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};
  const std::string out=suspended_+raw_;
  const std::string correction_origin = correction_origin_;
  recordCommitChunk(out, {});
  raw_.clear(); suspended_.clear(); preedit_cursor_=0; suspended_cursor_=0;
  correction_origin_.clear(); literal_token_=false; update(); expanded_=false; cursor_=0; page_=0;
  Event event;
  event.type=EventType::Commit;
  event.key=trigger;
  event.before=out;
  event.preedit=out;
  event.committed=out;
  event.context=context_;
  event.corrected_from=correction_origin;
  event.session=session_++;
  event.ts_ms=now_ms();
  event.sensitive=sensitive_;
  learnCommit(std::move(event));
  return {raw_,candidates_,cursor_,expanded_,mode_,out,false,preedit_cursor_};
}
View Engine::reset(){clearPrefixCache();pending_retained_.clear();++session_; raw_.clear();preedit_cursor_=0;suspended_cursor_=0;correction_origin_.clear();suspended_.clear();committed_.clear();chunks_.clear(); english_touched_=false; literal_token_=false; mode_=Mode::Chinese;
#ifdef TIPE_HAS_LIBIME
  if(libime_ctx_) libime_ctx_->cancel();
#endif
 last_event_ms_=last_delete_ms_=0; update();expanded_=false;cursor_=0;page_=0;return {raw_,candidates_,cursor_,expanded_,mode_,{},false,preedit_cursor_};}
std::optional<RestorableCommit> Engine::restorableCommit() const {
  if (mode_ != Mode::Chinese || hasComposition() || chunks_.empty()) return std::nullopt;
  const auto &c = chunks_.back();
  if (c.pinyin.empty() || c.text.empty()) return std::nullopt;
  return RestorableCommit{c.text, c.pinyin};
}
View Engine::restoreCommit(const std::string& expected_text) {
  auto r = restorableCommit();
  if (!r || r->text != expected_text) return {raw_, candidates_, cursor_, expanded_, mode_};
  finalizeRetained(false);
  chunks_.pop_back();
  if (committed_.size() >= r->text.size()) committed_.erase(committed_.size() - r->text.size());
  // The chunk stores the exact consumed raw spelling, including user-typed
  // separators and abbreviations, rather than expanded decoder pinyin.
  raw_ = r->pinyin; preedit_cursor_ = raw_.size();
  update(); expanded_ = false; cursor_ = 0; page_ = 0;
  return {raw_, candidates_, cursor_, expanded_, mode_, {}, true, preedit_cursor_};
}
void Engine::invalidateCommitHistory() { chunks_.clear(); committed_.clear(); correction_origin_.clear(); }
void Engine::setRetainedVerification(bool enabled) {
  if (verify_retained_ != enabled) pending_retained_.clear();
  verify_retained_=enabled;
}
std::string Engine::pendingRetainedText() const {
  std::string text;
  for(const auto &event:pending_retained_) text+=event.committed;
  return text;
}
void Engine::learnCommit(Event event) {
  if(!verify_retained_) { learner_->push(std::move(event)); return; }
  if(event.sensitive) { pending_retained_.clear(); return; }
  if(pending_retained_.size()>=16 || pendingRetainedText().size()+event.committed.size()>512)
    pending_retained_.clear();
  pending_retained_.push_back(std::move(event));
}
void Engine::finalizeRetained(bool retained) {
  if(retained) for(auto &event:pending_retained_) {
    event.retained=event.committed;
    learner_->push(std::move(event));
  }
  pending_retained_.clear();
}
}
