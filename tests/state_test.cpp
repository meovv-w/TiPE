#include "state.h"
#include "input_model.h"
#include "input_privacy.h"
#include "pass_through_supervisor.h"
#include "pinyin_utils.h"
#include "bounded_log.h"
#include "candidate_layout.h"
#include "candidate_snapshot.h"
#include "supervision_snapshot.h"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void type(tipe::State &state, const std::string &text) {
    for (char ch : text) {
        const auto action = state.inputAscii(ch);
        require(action.accepted, "letter should be accepted");
        require(action.type == tipe::ActionType::Update, "letter should update state");
    }
}

void eraseAll(tipe::State &state) {
    while (!state.empty()) {
        const auto action = state.backspace();
        require(action.accepted, "backspace should erase composing text");
    }
}

void typeFresh(tipe::State &state, const std::string &text) {
    state.reset();
    type(state, text);
}

bool hasEvent(const tipe::State &state, tipe::InputEventType type) {
    for (const auto &event : state.recentEvents()) {
        if (event.type == type) {
            return true;
        }
    }
    return false;
}

bool hasEventText(const tipe::State &state, tipe::InputEventType type, const std::string &text) {
    for (const auto &event : state.recentEvents()) {
        if (event.type == type && event.text == text) {
            return true;
        }
    }
    return false;
}

std::size_t candidateIndex(const tipe::State &state, const std::string &candidate) {
    const auto &candidates = state.candidates();
    const auto iter = std::find(candidates.begin(), candidates.end(), candidate);
    require(iter != candidates.end(), "candidate should exist: " + candidate + " in preedit " + state.preedit());
    return static_cast<std::size_t>(iter - candidates.begin());
}

std::size_t optionalCandidateIndex(const tipe::State &state, const std::string &candidate) {
    const auto &candidates = state.candidates();
    const auto iter = std::find(candidates.begin(), candidates.end(), candidate);
    return iter == candidates.end() ? candidates.size() : static_cast<std::size_t>(iter - candidates.begin());
}

bool hasDuplicateCandidates(const tipe::State &state) {
    const auto &candidates = state.candidates();
    for (auto iter = candidates.begin(); iter != candidates.end(); ++iter) {
        if (std::find(std::next(iter), candidates.end(), *iter) != candidates.end()) {
            return true;
        }
    }
    return false;
}

std::size_t utf8CodepointCount(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char ch) {
        return (ch & 0xC0) != 0x80;
    }));
}

std::size_t lineCount(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    return count;
}

bool fileContains(const std::filesystem::path &path, const std::string &needle) {
    std::ifstream input(path);
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return content.find(needle) != std::string::npos;
}

bool fileIsPrivate(const std::filesystem::path &path) {
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    const auto exposed = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    return !error && (permissions & exposed) == std::filesystem::perms::none;
}

bool hasTemporarySibling(const std::filesystem::path &path) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const auto prefix = path.filename().string() + ".tmp.";
    std::error_code error;
    for (std::filesystem::directory_iterator iter(parent, error), end; !error && iter != end; iter.increment(error)) {
        if (iter->path().filename().string().starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    require(!tipe::inputCapabilitiesBlockSupervision(0),
            "ordinary input capabilities allow TiPE supervision");
    require(!tipe::inputCapabilitiesBlockSupervision(1ULL << 1),
            "preedit capability alone does not disable supervision");
    require(tipe::inputCapabilitiesBlockSupervision(tipe::passwordInputCapability),
            "password input disables supervision");
    require(tipe::inputCapabilitiesBlockSupervision(tipe::sensitiveInputCapability),
            "sensitive input disables supervision");
    require(tipe::inputCapabilitiesBlockSupervision(tipe::disabledInputMethodCapability),
            "client-disabled input methods do not supervise pass-through keys");
    require(tipe::inputCapabilitiesBlockSupervision((1ULL << 1) | tipe::sensitiveInputCapability),
            "sensitive input remains blocked when ordinary capabilities are also present");

    const auto preferenceBase =
        std::filesystem::temp_directory_path() / ("tipe-state-test-" + std::to_string(::getpid()));
    const auto mainPreferencePath = preferenceBase.string() + "-main.tsv";
    const auto cleanPreferencePath = preferenceBase.string() + "-clean.tsv";
    const auto enterCorrectionPreferencePath = preferenceBase.string() + "-enter-correction.tsv";
    const auto noisyCorrectionPreferencePath = preferenceBase.string() + "-noisy-correction.tsv";
    const auto genericCorrectionPreferencePath = preferenceBase.string() + "-generic-correction.tsv";
    const auto generalizedPatternPreferencePath = preferenceBase.string() + "-generalized-pattern.tsv";
    const auto correctionConflictPreferencePath = preferenceBase.string() + "-correction-conflict.tsv";
    const auto partialRewriteCorrectionPreferencePath = preferenceBase.string() + "-partial-rewrite-correction.tsv";
    const auto genericSegmentContinuationPreferencePath = preferenceBase.string() + "-generic-segment-continuation.tsv";
    const auto trimPreferencePath = preferenceBase.string() + "-trim.tsv";
    const auto modelPreferencePath = preferenceBase.string() + "-model.tsv";
    const auto modelCandidateLearningPreferencePath = preferenceBase.string() + "-model-candidate-learning.tsv";
    const auto modelTimeoutPreferencePath = preferenceBase.string() + "-model-timeout.tsv";
    const auto invalidModelPreferencePath = preferenceBase.string() + "-invalid-model.tsv";
    const auto persistentPreferencePath = preferenceBase.string() + "-persistent.tsv";
    const auto passiveSelectionPreferencePath = preferenceBase.string() + "-passive-selection.tsv";
    const auto malformedPreferencePath = preferenceBase.string() + "-malformed.tsv";
    const auto crlfPreferencePath = preferenceBase.string() + "-crlf.tsv";
    const auto hotReloadPreferencePath = preferenceBase.string() + "-hot-reload.tsv";
    const auto userDictionaryPreferencePath = preferenceBase.string() + "-user-dictionary-preferences.tsv";
    const auto rerankScriptPath = preferenceBase.string() + "-rerank.sh";
    const auto modelDescendantMarkerPath = preferenceBase.string() + "-model-descendant-survived";
    const auto userDictionaryPath = preferenceBase.string() + "-user-dictionary.tsv";
    const auto automaticDictionaryPath = preferenceBase.string() + "-automatic-dictionary.tsv";
    const auto rareWordDictionaryPath = preferenceBase.string() + "-rare-word-dictionary.tsv";
    const auto rareWordPreferencePath = preferenceBase.string() + "-rare-word-preferences.tsv";
    const auto rareWordReloadPreferencePath = preferenceBase.string() + "-rare-word-reload-preferences.tsv";
    const auto knownPinyinHabitPreferencePath = preferenceBase.string() + "-known-pinyin-habit.tsv";
    const auto isolatedDefaultUserDictionaryPath = preferenceBase.string() + "-no-user-dictionary.tsv";
    const auto isolatedLibIMEHistoryPath = preferenceBase.string() + "-libime-history";
    const auto learnedLibIMEHistoryPath = preferenceBase.string() + "-learned-libime-history";
    const auto reloadedLibIMEHistoryPath = preferenceBase.string() + "-reloaded-libime-history";
    const auto boundedLogPath = preferenceBase.string() + "-bounded.log";
    const auto supervisionSnapshotTestPath = preferenceBase.string() + "-supervision.tsv";
    const auto boundedSupervisionHistoryTestPath = preferenceBase.string() + "-supervision-history.tsv";
    const auto passThroughPreferencePath = preferenceBase.string() + "-pass-through.tsv";
    std::filesystem::remove(mainPreferencePath);
    std::filesystem::remove(cleanPreferencePath);
    std::filesystem::remove(enterCorrectionPreferencePath);
    std::filesystem::remove(noisyCorrectionPreferencePath);
    std::filesystem::remove(genericCorrectionPreferencePath);
    std::filesystem::remove(generalizedPatternPreferencePath);
    std::filesystem::remove(correctionConflictPreferencePath);
    std::filesystem::remove(partialRewriteCorrectionPreferencePath);
    std::filesystem::remove(hotReloadPreferencePath);
    std::filesystem::remove(trimPreferencePath);
    std::filesystem::remove(modelPreferencePath);
    std::filesystem::remove(modelCandidateLearningPreferencePath);
    std::filesystem::remove(modelTimeoutPreferencePath);
    std::filesystem::remove(invalidModelPreferencePath);
    std::filesystem::remove(persistentPreferencePath);
    std::filesystem::remove(passiveSelectionPreferencePath);
    std::filesystem::remove(malformedPreferencePath);
    std::filesystem::remove(crlfPreferencePath);
    std::filesystem::remove(userDictionaryPreferencePath);
    std::filesystem::remove(rerankScriptPath);
    std::filesystem::remove(modelDescendantMarkerPath);
    std::filesystem::remove(userDictionaryPath);
    std::filesystem::remove(automaticDictionaryPath);
    std::filesystem::remove(automaticDictionaryPath + ".lock");
    std::filesystem::remove(rareWordDictionaryPath);
    std::filesystem::remove(rareWordDictionaryPath + ".lock");
    std::filesystem::remove(rareWordPreferencePath);
    std::filesystem::remove(rareWordPreferencePath + ".lock");
    std::filesystem::remove(rareWordReloadPreferencePath);
    std::filesystem::remove(knownPinyinHabitPreferencePath);
    std::filesystem::remove(isolatedDefaultUserDictionaryPath);
    std::filesystem::remove(isolatedLibIMEHistoryPath);
    std::filesystem::remove(isolatedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(learnedLibIMEHistoryPath);
    std::filesystem::remove(learnedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(reloadedLibIMEHistoryPath);
    std::filesystem::remove(reloadedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(boundedLogPath);
    std::filesystem::remove(supervisionSnapshotTestPath);
    std::filesystem::remove(boundedSupervisionHistoryTestPath);
    std::filesystem::remove(passThroughPreferencePath);

    const auto previousUmask = ::umask(0022);
    {
        tipe::PassThroughSupervisor supervisor(passThroughPreferencePath);
        tipe::PassThroughSupervisionSnapshot completed;
        for (int repetition = 0; repetition < 3; ++repetition) {
            for (const char ch : std::string("start")) {
                supervisor.inputLetter(ch);
            }
            const auto live = supervisor.snapshot("Editor", true);
            require(live.token == "start" && live.candidateCount == 1 && !live.terminal,
                    "English pass-through supervision tracks the current bounded token");
            require(live.payload.find("supervision_state\tmode\tpass-through-only\tactive_preedit\t0") !=
                        std::string::npos &&
                        live.payload.find("runtime_state\tcontinuous\t1\tinput_mode\tenglish") !=
                            std::string::npos &&
                        live.payload.find("candidate_metadata\t0\tconsumed_prefix\t0\tsource\traw-pass-through") !=
                            std::string::npos,
                    "English pass-through requests identify their non-composing mode and raw candidate source");
            completed = supervisor.commitSpace("Editor", true);
            require(completed.terminal && completed.token == "start" &&
                        completed.payload.find("space:\traw-committed:start") != std::string::npos,
                    "English token boundary creates a terminal raw-commit training sample");
        }
        require(fileContains(passThroughPreferencePath, "start\tstart\t3"),
                "repeated pass-through English tokens become generic raw-English preference evidence");
        require(fileIsPrivate(passThroughPreferencePath) &&
                    fileIsPrivate(passThroughPreferencePath + ".lock"),
                "preference and lock files remain private under a permissive process umask");

        for (const char ch : std::string("unfinished")) {
            supervisor.inputLetter(ch);
        }
        completed = supervisor.commitObservedBoundary("ModeSwitch", "Editor", true);
        require(completed.terminal && completed.token == "unfinished" &&
                    completed.payload.find("observed:ModeSwitch\traw-committed:unfinished") != std::string::npos,
                "leaving pass-through mode seals the current token as a terminal sample");
        require(fileContains(passThroughPreferencePath, "unfinished\tunfinished\t1"),
                "mode-switch boundaries retain the final raw-English preference evidence");

        for (const char ch : std::string("focusword")) {
            supervisor.inputLetter(ch);
        }
        completed = supervisor.commitObservedBoundary("InputContextFocusOut", "Editor", true);
        require(completed.terminal && completed.token == "focusword" &&
                    completed.payload.find("observed:InputContextFocusOut\traw-committed:focusword") !=
                        std::string::npos,
                "focus changes seal a known pass-through token exactly once");

        for (int repetition = 0; repetition < 2; ++repetition) {
            for (const char ch : std::string("ihao")) {
                supervisor.inputLetter(ch);
            }
            for (int index = 0; index < 4; ++index) {
                supervisor.backspace();
            }
            for (const char ch : std::string("nihao")) {
                supervisor.inputLetter(ch);
            }
            supervisor.commitEnter("Editor");
        }
        require(fileContains(passThroughPreferencePath, "__correction__\tihao\tnihao\t2"),
                "delete-and-retype behavior in pass-through mode feeds the generic correction learner");

        for (int index = 0; index < 80; ++index) {
            supervisor.inputLetter('x');
        }
        require(supervisor.token().size() == 64,
                "pass-through supervision bounds the current token instead of retaining arbitrary text");
        supervisor.cursorMove("Down");
        require(supervisor.token().empty(),
                "navigation outside the known token drops uncertain text tracking");
    }

    {
        std::ofstream output(boundedLogPath, std::ios::binary | std::ios::trunc);
        for (int index = 0; index < 200; ++index) {
            output << "old-record-" << index << "-xxxxxxxxxxxxxxxx\n";
        }
        output.close();
        require(tipe::trimDiagnosticLogFile(boundedLogPath, 512, 256),
                "oversized diagnostic log should be trimmed");
        require(std::filesystem::file_size(boundedLogPath) <= 256,
                "trimmed diagnostic log should honor the retained byte limit");
        require(fileContains(boundedLogPath, "old-record-199"),
                "diagnostic log trim should preserve the newest complete records");
        require(!fileContains(boundedLogPath, "old-record-0-"),
                "diagnostic log trim should discard old records");
        tipe::appendBoundedDiagnosticLog(boundedLogPath, "tail-marker");
        require(fileContains(boundedLogPath, "tail-marker"),
                "bounded diagnostic log append should preserve new records");
        require(fileIsPrivate(boundedLogPath),
                "diagnostic logs remain private under a permissive process umask");
        std::filesystem::permissions(
            boundedLogPath,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);
        const int privateAppendDescriptor = tipe::openPrivateAppendFile(boundedLogPath);
        require(privateAppendDescriptor >= 0,
                "private append helper should open an existing diagnostic log");
        if (privateAppendDescriptor >= 0) {
            ::close(privateAppendDescriptor);
        }
        require(fileIsPrivate(boundedLogPath),
                "private append helper should repair exposed existing log permissions");
        std::filesystem::remove(boundedLogPath);
    }

    {
        tipe::State passiveSelection({}, passiveSelectionPreferencePath);
        type(passiveSelection, "nihao");
        const auto passiveCandidate = passiveSelection.candidates().front();
        const auto passiveAction = passiveSelection.space();
        require(passiveAction.accepted && passiveAction.commitText == passiveCandidate,
                "space still commits the default candidate without explicit navigation");
        require(!fileContains(passiveSelectionPreferencePath,
                              "nihao\t" + passiveCandidate + "\t"),
                "passive default-candidate commits do not persist ranking preferences");

        {
            std::ofstream weakPreference(passiveSelectionPreferencePath, std::ios::trunc);
            weakPreference << "nihao\t你号\t1\n";
        }
        tipe::State weakLegacyPreference({}, passiveSelectionPreferencePath);
        type(weakLegacyPreference, "nihao");
        require(!weakLegacyPreference.candidates().empty() &&
                    weakLegacyPreference.candidates().front() != "你号",
                "one passive legacy observation cannot override the base candidate order");
    }
    {
        const std::string liveSnapshot =
            "protocol\t1\npreedit\tnihao\nsurrounding_before\tprivate-left\n"
            "surrounding_after\tprivate-right\n"
            "surrounding_features\tbefore:v1:11111111111111111111111111111111\n"
            "context\tolder-commit\ncontext_features\tv1:22222222222222222222222222222222\n"
            "context\nevents\tletter:n\n";
        const auto persistentSnapshot = tipe::persistentSupervisionSnapshot(liveSnapshot);
        require(persistentSnapshot.find("private-left") == std::string::npos &&
                    persistentSnapshot.find("private-right") == std::string::npos &&
                    persistentSnapshot.find("older-commit") == std::string::npos &&
                    persistentSnapshot.find("\ncontext\n") == std::string::npos,
                "persistent supervision snapshot should remove ephemeral surrounding and commit context");
        require(persistentSnapshot.find("preedit\tnihao\n") != std::string::npos &&
                    persistentSnapshot.find("surrounding_features\tbefore:v1:11111111111111111111111111111111\n") !=
                        std::string::npos &&
                    persistentSnapshot.find("context_features\tv1:22222222222222222222222222222222\n") !=
                        std::string::npos &&
                    persistentSnapshot.find("events\tletter:n\n") != std::string::npos,
                "persistent supervision snapshot should retain active input, key evidence, and opaque context features");
        require(tipe::writeSupervisionSnapshotAtomically(supervisionSnapshotTestPath, persistentSnapshot),
                "supervision snapshot should be written atomically");
        require(fileContains(supervisionSnapshotTestPath, "events\tletter:n"),
                "atomic supervision snapshot should contain the complete payload");
        require(!std::filesystem::exists(supervisionSnapshotTestPath + ".tmp." + std::to_string(::getpid())),
                "atomic supervision snapshot should not leave a temporary file");
        require(fileIsPrivate(supervisionSnapshotTestPath),
                "atomic supervision snapshots remain private under a permissive process umask");
        require(tipe::writeSupervisionSnapshotAtomically(supervisionSnapshotTestPath, liveSnapshot) &&
                    tipe::sanitizePersistentSupervisionFile(supervisionSnapshotTestPath),
                "existing supervision file should be sanitized atomically");
        require(!fileContains(supervisionSnapshotTestPath, "private-left") &&
                    fileContains(supervisionSnapshotTestPath, "events\tletter:n"),
                "existing supervision migration should remove context and retain key evidence");
        std::filesystem::remove(supervisionSnapshotTestPath);
    }
    {
        for (int index = 0; index < 12; ++index) {
            const auto snapshot = "protocol\t1\npreedit\tvalue" + std::to_string(index) +
                                  "\nevents\tletter:v\tcandidate-selected:值" + std::to_string(index) + "\n";
            require(tipe::appendBoundedSupervisionHistory(
                        boundedSupervisionHistoryTestPath,
                        "---\tunix_ms\t" + std::to_string(index) + "\tterminal\t1", snapshot, 512),
                    "bounded supervision history append should succeed");
        }
        require(std::filesystem::file_size(boundedSupervisionHistoryTestPath) <= 512,
                "bounded supervision history honors its byte limit");
        require(fileIsPrivate(boundedSupervisionHistoryTestPath),
                "supervision history remains private under a permissive process umask");
        std::ifstream historyInput(boundedSupervisionHistoryTestPath, std::ios::binary);
        const std::string history((std::istreambuf_iterator<char>(historyInput)),
                                  std::istreambuf_iterator<char>());
        require(history.starts_with("---\t") && history.find("preedit\tvalue11") != std::string::npos &&
                    history.find("preedit\tvalue0\n") == std::string::npos,
                "bounded supervision history keeps newest complete records");
        const auto beforeOversizedAppend = history;
        require(!tipe::appendBoundedSupervisionHistory(
                    boundedSupervisionHistoryTestPath, "---\tunix_ms\t99\tterminal\t1",
                    "protocol\t1\npreedit\t" + std::string(600, 'x') + "\n", 512),
                "one oversized supervision record is rejected without partial output");
        std::ifstream unchangedInput(boundedSupervisionHistoryTestPath, std::ios::binary);
        const std::string unchanged((std::istreambuf_iterator<char>(unchangedInput)),
                                    std::istreambuf_iterator<char>());
        require(unchanged == beforeOversizedAppend,
                "rejected supervision record leaves the bounded history unchanged");
        std::filesystem::remove(boundedSupervisionHistoryTestPath);
    }
    ::umask(previousUmask);
    setenv("TIPE_USER_DICTIONARY", isolatedDefaultUserDictionaryPath.c_str(), 1);
    setenv("TIPE_LIBIME_USER_HISTORY", isolatedLibIMEHistoryPath.c_str(), 1);
    setenv("TIPE_DISABLE_LIBIME_LEARNING", "1", 1);

    {
        unsetenv("TIPE_TEST_FALLBACK_DICTIONARY");
        tipe::State productionDictionaryState({}, cleanPreferencePath);
        type(productionDictionaryState, "houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen");
        require(optionalCandidateIndex(productionDictionaryState,
                                       "候选窗显示的不是github这个英文而是中文") ==
                    productionDictionaryState.candidates().size(),
                "production dictionary does not expose historical feedback sentences as built-in candidates");
        productionDictionaryState.reset();
        type(productionDictionaryState, "nihao");
        require(optionalCandidateIndex(productionDictionaryState, "你好") <
                    productionDictionaryState.candidates().size(),
                "production dictionary keeps a minimal stable fallback for basic composition");
#ifdef TIPE_TEST_HAVE_LIBIME
        productionDictionaryState.reset();
        type(productionDictionaryState, "n");
        require(!productionDictionaryState.candidates().empty() &&
                    productionDictionaryState.candidates().front() == "你",
                "incomplete initial uses the language model instead of a rare exact Rime code");
        for (const auto *initial : {"s", "j", "zh"}) {
            productionDictionaryState.reset();
            type(productionDictionaryState, initial);
            require(!productionDictionaryState.candidates().empty() &&
                        utf8CodepointCount(productionDictionaryState.candidates().front()) == 1,
                    std::string("single-syllable initial prefers a character over a backend phrase: ") + initial);
        }
        productionDictionaryState.reset();
        type(productionDictionaryState, "guangb");
        require(!productionDictionaryState.candidates().empty() &&
                    (productionDictionaryState.candidates().front() == "光标" ||
                     productionDictionaryState.candidates().front() == "广播"),
                "incomplete final keeps a coherent high-frequency language-model completion first");
        productionDictionaryState.reset();
        type(productionDictionaryState, "jichen");
        require(!productionDictionaryState.candidates().empty() &&
                    productionDictionaryState.candidates().front() == "击沉" &&
                    optionalCandidateIndex(productionDictionaryState, "击沉") <
                        optionalCandidateIndex(productionDictionaryState, "擊沈"),
                "default system Rime exact entries do not override the simplified language-model candidate");
        productionDictionaryState.reset();
        type(productionDictionaryState, "zhexiedoushiganmade");
        require(!productionDictionaryState.candidates().empty() &&
                    productionDictionaryState.candidates().front() == "这些都是干嘛的",
                "LibIME language model provides a natural long-sentence base candidate");
        productionDictionaryState.reset();
        type(productionDictionaryState, "changjuxianxuanqianbanduanhoumiandepinyinbubaoliu");
        require(!productionDictionaryState.candidates().empty() &&
                    productionDictionaryState.candidates().front() == "长句先选前半段后面的拼音不保留",
                "simplified decoder prefixes keep a coherent long sentence ahead of short or traditional prefixes");
        require(optionalCandidateIndex(productionDictionaryState, "长居先选前半段后面的拼音不保留") >
                    optionalCandidateIndex(productionDictionaryState, "长句先选前半段后面的拼音不保留"),
                "a divergent long sentence stays behind the candidate matching the active decoder prefix");
        const std::array<std::pair<std::string_view, std::string_view>, 8> exactSentenceRegressions{{
            {"fenxiangyixia", "分享一下"},
            {"jiejuele", "解决了"},
            {"cikuhennanyong", "词库很难用"},
            {"yinggaishi", "应该是"},
            {"meibiyao", "没必要"},
            {"wobuxiaoxin", "我不小心"},
            {"nizhaoyige", "你找一个"},
            {"youmeiyougenghaodefangfa", "有没有更好的方法"},
        }};
        for (const auto &[pinyin, expected] : exactSentenceRegressions) {
            productionDictionaryState.reset();
            type(productionDictionaryState, std::string(pinyin));
            require(!productionDictionaryState.candidates().empty() &&
                        productionDictionaryState.candidates().front() == expected,
                    std::string("complete pinyin prefers an exact natural sentence over fuzzy completion: ") +
                        std::string(pinyin));
        }
        productionDictionaryState.reset();
        type(productionDictionaryState, "zuo");
        require(productionDictionaryState.candidates().size() <= 40,
                "normal LibIME lookup does not append an unbounded rare Rime tail");

        tipe::Dictionary languageModelDictionary;
        for (const auto *pinyin : {"nizaiganshenme", "woxiangzhidao", "wobuzhidao", "mingtianzaishuo",
                                   "zhengzaichuli", "zheyangkeyima", "youmeiyougenghaodefangfa"}) {
            require(languageModelDictionary.hasConfidentLanguageModelSentence(pinyin),
                    std::string("natural sentence has a confident base language-model result: ") + pinyin);
        }
        for (const auto *pinyin : {"ihao", "woc", "woxiangyo", "jibengongnegn"}) {
            require(!languageModelDictionary.hasConfidentLanguageModelSentence(pinyin),
                    std::string("typo or fuzzy completion remains eligible for learned correction: ") + pinyin);
        }
        require(languageModelDictionary.hasDecisiveLanguageModelSentence("youmeiyougenghaodefangfa"),
                "a clearly leading exact sentence is protected from heuristic prefix reranking");
        require(!languageModelDictionary.hasDecisiveLanguageModelSentence(
                    "changjuxianxuanqianbanduanhoumiandepinyinbubaoliu"),
                "near-tied homophones remain eligible for decoder-prefix disambiguation");

        {
            std::ofstream habits(knownPinyinHabitPreferencePath);
            habits << "__correction_pattern__\tmissing\t\tn\t0\t0\t2\n";
            habits << "__key_habit__\tmissing\t\tn\t6\n";
            habits << "__key_habit__\tmissing\t\tg\t5\n";
            habits << "__key_habit__\tmissing\t\ti\t5\n";
            habits << "__key_habit__\tmissing\t\tu\t4\n";
            habits << "__key_habit__\treplace\tj\tx\t4\n";
            habits << "__correction_pattern__\treplace\tj\tx\t0\t0\t2\n";
            habits << "houxuanchuang\t候选窗\t7\n";
            habits << "obsidian\tobsidian\t4\n";
        }
        tipe::State guardedLanguageModelState({}, knownPinyinHabitPreferencePath);
        const std::array<std::pair<std::string_view, std::string_view>, 6> naturalSentences{{
            {"nizaiganshenme", "你在干什么"},
            {"woxiangzhidao", "我想知道"},
            {"wobuzhidao", "我不知道"},
            {"mingtianzaishuo", "明天再说"},
            {"zhengzaichuli", "正在处理"},
            {"zheyangkeyima", "这样可以吗"},
        }};
        for (const auto &[pinyin, expected] : naturalSentences) {
            type(guardedLanguageModelState, std::string(pinyin));
            require(!guardedLanguageModelState.candidates().empty() &&
                        guardedLanguageModelState.candidates().front() == expected,
                    std::string("global key habits preserve a confident natural sentence: ") +
                        std::string(pinyin));
            guardedLanguageModelState.reset();
        }
        type(guardedLanguageModelState, "ihao");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "你好",
                "confidence guard still lets a strong missing-key habit repair an unclear typo");
        guardedLanguageModelState.reset();
        type(guardedLanguageModelState, "woxiangyo");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "我想用",
                "confidence guard still lets two strong omissions repair an unclear sentence");
        guardedLanguageModelState.reset();
        type(guardedLanguageModelState, "jiucaidan");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "就菜单",
                "a weak global missing-u habit does not rewrite complete pinyin into an unrelated phrase");
        guardedLanguageModelState.reset();
        type(guardedLanguageModelState, "louda");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "楼大",
                "global omission habits do not create an extra pinyin syllable");
        guardedLanguageModelState.reset();
        type(guardedLanguageModelState, "houxuanchuang");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "候选窗",
                "a complete aligned learned phrase remains available after the base dictionary changes");
        guardedLanguageModelState.reset();
        type(guardedLanguageModelState, "obsidian");
        require(!guardedLanguageModelState.candidates().empty() &&
                    guardedLanguageModelState.candidates().front() == "obsidian",
                "repeated non-pinyin English input suppresses speculative Chinese typo correction");

        {
            std::ofstream exactCorrection(knownPinyinHabitPreferencePath, std::ios::app);
            exactCorrection << "__correction__\tnizaiganshenme\tnizaiganshenmene\t2\n";
        }
        tipe::State exactCorrectionState({}, knownPinyinHabitPreferencePath);
        type(exactCorrectionState, "nizaiganshenme");
        require(!exactCorrectionState.candidates().empty() &&
                    exactCorrectionState.candidates().front() == "你在干什么呢",
                "an explicit repeated correction can override the base confidence guard");
#endif
        setenv("TIPE_TEST_FALLBACK_DICTIONARY", TIPE_TEST_FALLBACK_DICTIONARY_PATH, 1);
    }
    tipe::State state({}, mainPreferencePath);

    {
        tipe::State snapshotState({}, cleanPreferencePath);
        type(snapshotState, "pinyin");
        require(!snapshotState.empty() && snapshotState.preedit() == "pinyin",
                "restorable snapshot source has active preedit");
        const auto expanded = snapshotState.expandCandidates("Down");
        require(expanded.accepted && snapshotState.candidatesExpanded(),
                "restorable snapshot source can expand candidates");
        const auto moved = snapshotState.moveCandidateCursor(1, "Right");
        require(moved.accepted && snapshotState.candidateCursorIndex() == 1,
                "restorable snapshot source keeps highlighted candidate");
        const auto snapshot = snapshotState.restorableSnapshot();

        tipe::State restored({}, cleanPreferencePath);
        const auto restoredAction = restored.restoreSnapshot(snapshot);
        require(restoredAction.accepted && restoredAction.type == tipe::ActionType::Update,
                "restoring a restorable snapshot updates composition");
        require(restored.preedit() == "pinyin", "restored snapshot keeps active preedit");
        require(restored.candidatesExpanded(), "restored snapshot keeps expanded candidate state");
        require(restored.candidateCursorIndex() == 1, "restored snapshot keeps highlighted candidate");
        require(hasEvent(restored, tipe::InputEventType::Letter),
                "restored snapshot keeps in-memory supervision events");
        const auto continued = restored.inputAscii('j');
        require(continued.accepted && restored.preedit() == "pinyinj" && !restored.candidates().empty(),
                "restored snapshot can continue composing after an input-method switch");
    }

    {
        const std::vector<std::string> snapshotCandidates{"你好", "A|B", "slash\\value", "tab\tvalue"};
        const auto snapshot =
            tipe::buildCandidateSnapshotLine("nihao", true, 2, {10, 20, 3, 18}, snapshotCandidates);
        require(snapshot == "nihao\t1\t2\t10\t20\t3\t18\t你好|A\\|B|slash\\\\value|tab\\tvalue\n",
                "candidate snapshot encoder escapes pipe, slash, and tab fields");
        const auto metadataSnapshot =
            tipe::buildCandidateSnapshotLine("nihao", false, 0, {10, 20, 3, 18}, {"你好"},
                                             "supervision=1,keys=5,selects=0,reranks=1,continuous=1");
        require(metadataSnapshot ==
                    "nihao\t0\t0\t10\t20\t3\t18\t你好\tsupervision=1,keys=5,selects=0,reranks=1,continuous=1\n",
                "candidate snapshot encoder appends optional supervision metadata");
        require(tipe::clearCandidateSnapshotLine() == "\t0\t0\t0\t0\t0\t0\t\n",
                "candidate snapshot clear line matches candidate-window parser");
        const auto browserRect = tipe::logicalCandidateSnapshotRect({1736, 1836, 2, 46}, 2.0);
        require(browserRect.x == 868 && browserRect.y == 918 && browserRect.width == 1 && browserRect.height == 23,
                "HiDPI D-Bus cursor rectangles convert from physical to GTK logical coordinates");
        const auto unscaledRect = tipe::logicalCandidateSnapshotRect({582, 1762, 4, 44}, 1.0);
        require(unscaledRect.x == 582 && unscaledRect.y == 1762 && unscaledRect.width == 4 &&
                    unscaledRect.height == 44,
                "unit-scale fallback cursor rectangles remain unchanged");
        const auto ximSpot = tipe::candidateSnapshotAnchorFor("xim", {320, 640, 0, 0}, 1.0);
        require(ximSpot.rect.x == 320 && ximSpot.rect.y == 640 && ximSpot.rect.width == 1 &&
                    ximSpot.rect.height == 22 && !ximSpot.pointerFallback,
                "XIM spot locations remain usable when the protocol omits their dimensions");
        const auto ximMissingSpot = tipe::candidateSnapshotAnchorFor("xim", {0, 0, 0, 0}, 1.0);
        require(ximMissingSpot.rect.height == 0 && ximMissingSpot.pointerFallback,
                "XIM contexts without any spot location request the pointer fallback");
        const auto nonXimSentinel = tipe::candidateSnapshotAnchorFor("dbus", {38, 38, 0, 0}, 1.0);
        require(nonXimSentinel.rect.height == 0 && !nonXimSentinel.pointerFallback,
                "non-XIM position-only startup sentinels remain unusable");

        require(tipe::shouldRunFullPinyinDecoder("nihap"),
                "short incomplete input keeps mature decoder typo completion");
        require(!tipe::shouldRunFullPinyinDecoder("heshagnxiayinyue"),
                "long unsegmentable input avoids an explosive mature decoder graph");
        require(tipe::shouldRunFullPinyinDecoder("woxianzainongzhege"),
                "complete long pinyin keeps mature sentence decoding");
        require(!tipe::shouldRunFullPinyinDecoder("woxianzainongq"),
                "a long unrecognized tail uses the bounded fallback until it becomes valid");

        tipe::CandidateCursorFollowTracker staticTracker;
        require(!staticTracker.observe({505, 899, 1, 23}, "n", 1),
                "the first cursor snapshot does not assume a broken client");
        require(staticTracker.observe({505, 899, 1, 23}, "ni", 2),
                "an unchanged client rectangle with a moving preedit cursor enables compensation");
        require(staticTracker.observe({505, 899, 1, 23}, "nih", 3),
                "a static client remains compensated during the composition");

        tipe::CandidateCursorFollowTracker liveTracker;
        require(!liveTracker.observe({300, 500, 2, 22}, "n", 1),
                "the first live cursor snapshot starts undecided");
        require(!liveTracker.observe({310, 500, 2, 22}, "ni", 2),
                "a client-provided cursor move is trusted without a synthetic offset");

        tipe::CandidateCursorFollowTracker deferredTracker;
        require(!deferredTracker.observe({300, 500, 2, 22}, "n", 1),
                "a deferred cursor sequence starts undecided");
        require(deferredTracker.observe({300, 500, 2, 22}, "ni", 2),
                "a stale immediate cursor snapshot can be compensated temporarily");
        require(!deferredTracker.observe({310, 500, 2, 22}, "ni", 2),
                "a later client cursor update permanently disables compensation for that composition");
        require(!deferredTracker.observe({310, 500, 2, 22}, "nih", 3),
                "a proven live client is not misclassified by a later stale snapshot");
        deferredTracker.reset();
        require(!deferredTracker.observe({400, 600, 2, 22}, "x", 1),
                "reset starts a new cursor-follow classification");
    }

    {
        const std::vector<std::string> layoutCandidates{"很长很长很长很长的候选", "短", "也很长很长很长的候选",
                                                        "中", "正常", "尾巴"};
        const auto cells = tipe::visualCandidateCells(layoutCandidates);
        require(cells.size() == layoutCandidates.size(), "visual layout keeps one cell per candidate");
        require(cells[0].row == 0 && cells[0].column == 0 && cells[0].span == 3,
                "very long collapsed candidate occupies enough visual columns");
        require(cells[1].row == 0 && cells[1].column == 3 && cells[1].span == 1,
                "short candidate follows the multi-column long candidate");
        require(cells[2].row == 1 && cells[2].column == 0 && cells[2].span == 3,
                "second long candidate moves to the next row when it would be clipped");
        require(cells[3].row == 0 && cells[3].column == 4 && cells[3].span == 1,
                "expanded visual layout can backfill the earlier row gap");
        require(tipe::visualRowCount(cells) == 2, "visual row count follows long-candidate spans");
        const auto firstRow = tipe::cellsInVisualRow(cells, 0);
        require(firstRow.size() == 4, "expanded first visual row contains candidates that backfill available columns");
        const auto collapsedFirstRow = tipe::collapsedVisualCandidateCells(layoutCandidates);
        require(collapsedFirstRow.size() == firstRow.size() && collapsedFirstRow[0].index == 0 &&
                    collapsedFirstRow[1].index == 1 && collapsedFirstRow[2].index == 3 &&
                    collapsedFirstRow[3].index == 4,
                "collapsed visual layout uses the same packed first row as expanded layout");
        require(tipe::visualCellForIndex(cells, 2) && tipe::visualCellForIndex(cells, 2)->span == 3,
                "visual cell lookup returns the long candidate span");

        const auto collapsedMetrics = tipe::tipeUIPanelMetricsFor(collapsedFirstRow, layoutCandidates, false, true);
        require(collapsedMetrics.width < 596 && collapsedMetrics.width >= 150 && collapsedMetrics.height == 68 &&
                    collapsedMetrics.visibleRows == 1,
                "tipeui collapsed panel metrics shrink to visible candidate text without extra bottom slack");
        require(tipe::tipeUIPreeditTextWidthFor(collapsedMetrics.width) == collapsedMetrics.width - 24,
                "tipeui preedit text is clipped inside the collapsed panel padding");
        const auto collapsedDrawCells =
            tipe::tipeUIDrawCellsFor(collapsedFirstRow, layoutCandidates, collapsedMetrics.width, false);
        require(collapsedDrawCells.size() == collapsedFirstRow.size(), "collapsed tipeui draw cells match visible cells");
        for (const auto &drawCell : collapsedDrawCells) {
            require(drawCell.x >= tipe::tipeUIPanelHorizontalPadding &&
                        drawCell.x + drawCell.width <= collapsedMetrics.width - tipe::tipeUIPanelHorizontalPadding,
                    "collapsed tipeui draw cell stays inside the measured panel width");
        }
        const std::vector<tipe::CandidateHitRegion> hitRegions{
            {collapsedDrawCells[0].cell.index, collapsedDrawCells[0].x, 35,
             collapsedDrawCells[0].x + collapsedDrawCells[0].width, 63},
            {collapsedDrawCells[1].cell.index, collapsedDrawCells[1].x, 35,
             collapsedDrawCells[1].x + collapsedDrawCells[1].width, 63},
        };
        require(tipe::candidateIndexAtPoint(hitRegions, hitRegions[0].left, hitRegions[0].top) ==
                    hitRegions[0].index &&
                    tipe::candidateIndexAtPoint(hitRegions, hitRegions[1].right - 1, hitRegions[1].bottom - 1) ==
                        hitRegions[1].index,
                "candidate pointer hit testing includes each cell interior");
        require(!tipe::candidateIndexAtPoint(hitRegions, hitRegions[0].right, hitRegions[0].bottom),
                "candidate pointer hit testing excludes cell edges and panel gaps");
        const auto expandedMetrics = tipe::tipeUIPanelMetricsFor(cells, layoutCandidates, true, true);
        require(expandedMetrics.width == 596 && expandedMetrics.height == 96 && expandedMetrics.visibleRows == 2,
                "tipeui expanded panel metrics use fixed width and measured visual rows");
        require(tipe::tipeUIPreeditTextWidthFor(expandedMetrics.width) == 572,
                "tipeui preedit text is clipped inside the expanded panel padding");
        const auto expandedColumnWidth = tipe::tipeUIVisualColumnWidthFor(expandedMetrics.width);
        const int expandedRightEdge =
            tipe::tipeUIVisualCellX(tipe::visualCandidateColumns - 1, expandedColumnWidth) +
            tipe::tipeUIVisualCellWidth(1, expandedColumnWidth);
        const int expandedRightPadding = expandedMetrics.width - expandedRightEdge;
        require(expandedColumnWidth == tipe::tipeUIPanelExpandedCellWidth &&
                    expandedRightPadding >= tipe::tipeUIPanelHorizontalPadding &&
                    expandedRightPadding - tipe::tipeUIPanelHorizontalPadding <=
                        tipe::tipeUIPanelCompactCellGap,
                "expanded tipeui columns distribute fixed panel width with balanced side padding");
        for (const auto &cell : cells) {
            const int x = tipe::tipeUIVisualCellX(cell.column, expandedColumnWidth);
            const int width = tipe::tipeUIVisualCellWidth(cell.span, expandedColumnWidth);
            require(x >= tipe::tipeUIPanelHorizontalPadding &&
                        x + width <= expandedMetrics.width - tipe::tipeUIPanelHorizontalPadding,
                    "expanded tipeui draw cell stays inside the measured panel width");
        }
        require(tipe::tipeUIBufferScaleFor(1.0, 1) == 1 && tipe::tipeUIBufferScaleFor(1.25, 1) == 2 &&
                    tipe::tipeUIBufferScaleFor(1.5, 1) == 2 && tipe::tipeUIBufferScaleFor(2.25, 1) == 3 &&
                    tipe::tipeUIBufferScaleFor(5.0, 1) == 4,
                "tipeui buffer scale rounds fractional monitor scale up to avoid blurry upscaling");
        require(!tipe::tipeUIPopupEdgeFallbackNeededForRect(0, 0, 0, 0),
                "tipeui edge fallback ignores unusable cursor rectangles");
        require(!tipe::tipeUIPopupEdgeFallbackNeededForRect(32, 420, 21, 24),
                "tipeui edge fallback keeps normal cursor positions on the Wayland popup path");
        require(tipe::tipeUIPopupEdgeFallbackNeededForRect(tipe::tipeUIPopupEdgeFallbackLeftThreshold, 420, 21, 24),
                "tipeui edge fallback switches near the right edge");
        require(tipe::tipeUIPopupEdgeFallbackNeededForRect(32, tipe::tipeUIPopupEdgeFallbackTopThreshold, 21, 24),
                "tipeui edge fallback switches near the bottom edge");
        require(tipe::tipeUIPopupEdgeFallbackNeededForRect(640, 500, 21, 24, 600, 900),
                "tipeui edge fallback accepts custom threshold values");

        const std::vector<std::string> packedCandidates{"很长很长很长很长的候选",
                                                        "特别特别特别特别特别特别特别长的候选",
                                                        "短", "中", "尾"};
        const auto packedCells = tipe::visualCandidateCells(packedCandidates);
        require(packedCells.size() == packedCandidates.size(), "packed visual layout keeps every candidate");
        require(packedCells[0].row == 0 && packedCells[0].column == 0 && packedCells[0].span == 3,
                "packed visual layout places the first long candidate at the row start");
        require(packedCells[1].row == 1 && packedCells[1].column == 0 && packedCells[1].span == 4,
                "packed visual layout moves an oversized next candidate to a new row");
        require(packedCells[2].row == 0 && packedCells[2].column == 3 && packedCells[2].span == 1,
                "expanded packed visual layout backfills earlier row gaps with later short candidates");
        require(packedCells[3].row == 0 && packedCells[3].column == 4 && packedCells[3].span == 1,
                "expanded packed visual layout continues filling the earlier row gap");
        require(packedCells[4].row == 0 && packedCells[4].column == 5 && packedCells[4].span == 1,
                "expanded packed visual layout fills the final earlier row gap before using lower rows");
        const auto collapsedPackedCells = tipe::collapsedVisualCandidateCells(packedCandidates);
        require(collapsedPackedCells.size() == 4 && collapsedPackedCells[0].index == 0 &&
                    collapsedPackedCells[1].index == 2 && collapsedPackedCells[2].index == 3 &&
                    collapsedPackedCells[3].index == 4,
                "collapsed packed visual layout backfills first-row gaps with later short candidates");
    }

    {
        std::vector<std::string> manyCandidates;
        for (int index = 0; index < 42; ++index) {
            manyCandidates.push_back("候选" + std::to_string(index));
        }
        const auto collapsedCells = tipe::visibleVisualCellsFor(manyCandidates, 0, false);
        require(collapsedCells.size() == tipe::visualCandidateColumns,
                "collapsed visible cells show exactly the first visual row");
        require(collapsedCells.front().index == 0 && collapsedCells.back().index == 5,
                "collapsed visible cells keep the first six short candidates");

        const auto expandedCells = tipe::visibleVisualCellsFor(manyCandidates, 24, true);
        require(!expandedCells.empty(), "expanded visible cells are not empty");
        require(tipe::visualRowCount(expandedCells) == 5, "expanded visible cells are capped to five rows");
        require(expandedCells.front().index == 12 && expandedCells.front().row == 0,
                "expanded visible cells clamp to a full five-row window near the end");
        require(tipe::selectedVisualRow(expandedCells, 24) == 2,
                "selected row is rebased inside the visible expanded window");
        const auto selectedCell = tipe::visualCellForIndex(expandedCells, 24);
        require(selectedCell && tipe::shortcutForVisualCell(expandedCells, *selectedCell, 2, true) == "1",
                "expanded shortcut labels restart at one on the selected row");
        const auto otherRowCell = tipe::visualCellForIndex(expandedCells, 18);
        require(otherRowCell && tipe::shortcutForVisualCell(expandedCells, *otherRowCell, 2, true).empty(),
                "expanded shortcut labels are hidden on non-selected rows");
        const auto expandedMetrics = tipe::tipeUIPanelMetricsFor(expandedCells, manyCandidates, true, false);
        require(expandedMetrics.width == 596 && expandedMetrics.height == 152 && expandedMetrics.visibleRows == 5,
                "tipeui expanded panel metrics cap the visible candidate window to five rows");
    }

    auto action = state.inputAscii('1');
    require(!action.accepted && action.type == tipe::ActionType::None, "non-letter should be ignored");
    require(state.empty() && state.candidates().empty(), "ignored input keeps state empty");
    require(state.recentEvents().empty(), "ignored input is not recorded");
    type(state, "qwen");
    action = state.inputAsciiDigit('2');
    require(action.accepted && action.type == tipe::ActionType::Update && state.preedit() == "qwen2",
            "known alphanumeric English token accepts its digit suffix");
    require(hasEventText(state, tipe::InputEventType::Digit, "2"),
            "accepted alphanumeric suffix digit is recorded as a digit event");
    require(!state.candidates().empty() && state.candidates()[0] == "qwen2",
            "known alphanumeric English token is shown as raw text first");
    state.reset();
    type(state, "nihao");
    const auto digitsBeforeRejectedCandidateDigit = state.debugSnapshot().eventCounts.digits;
    action = state.inputAsciiDigit('2');
    require(!action.accepted && action.type == tipe::ActionType::None && state.preedit() == "nihao",
            "digits that do not form a known English token remain available for candidate selection");
    require(state.debugSnapshot().eventCounts.digits == digitsBeforeRejectedCandidateDigit,
            "candidate-selection digits are not recorded as typed alphanumeric input");
    state.reset();
    action = state.space();
    require(!action.accepted && action.type == tipe::ActionType::None, "space on empty state should pass through");
    type(state, "nihao");
    action = state.space();
    require(action.accepted && action.type == tipe::ActionType::Commit && !action.commitText.empty(),
            "space commits the active candidate");
    require(state.empty() && state.preedit().empty() && state.candidates().empty(),
            "space commit clears preedit and candidates");
    type(state, "sdjfkasfkjahfja");
    action = state.space();
    require(action.accepted && action.type == tipe::ActionType::Commit && !action.commitText.empty(),
            "space commits raw fallback text when no useful Chinese candidate exists");
    require(state.empty() && state.preedit().empty() && state.candidates().empty(),
            "raw fallback space commit also clears preedit and candidates");

    tipe::State emptyObservedState({}, cleanPreferencePath);
    action = emptyObservedState.space();
    require(!action.accepted && action.type == tipe::ActionType::None, "space on empty state should pass through");
    action = emptyObservedState.backspace();
    require(!action.accepted && action.type == tipe::ActionType::None, "backspace on empty state should pass through");
    action = emptyObservedState.deleteKey();
    require(!action.accepted && action.type == tipe::ActionType::None, "delete on empty state should pass through");
    action = emptyObservedState.enter();
    require(!action.accepted && action.type == tipe::ActionType::None, "enter on empty state should pass through");
    action = emptyObservedState.escape();
    require(!action.accepted && action.type == tipe::ActionType::None, "escape on empty state should pass through");
    action = emptyObservedState.cursorMove("Down");
    require(!action.accepted && action.type == tipe::ActionType::None, "cursor move on empty state should pass through");
    action = emptyObservedState.punctuation(",");
    require(!action.accepted && action.type == tipe::ActionType::None, "punctuation on empty state should pass through");
    require(hasEvent(emptyObservedState, tipe::InputEventType::Space), "space on empty state is observed");
    require(hasEvent(emptyObservedState, tipe::InputEventType::Backspace), "backspace on empty state is observed");
    require(hasEvent(emptyObservedState, tipe::InputEventType::Delete), "delete on empty state is observed");
    require(hasEvent(emptyObservedState, tipe::InputEventType::Enter), "enter on empty state is observed");
    require(hasEvent(emptyObservedState, tipe::InputEventType::Escape), "escape on empty state is observed");
    require(hasEvent(emptyObservedState, tipe::InputEventType::CursorMove), "cursor move on empty state is observed");
    require(hasEventText(emptyObservedState, tipe::InputEventType::ObservedKey, ","), "punctuation on empty state is observed");
    require(hasEventText(emptyObservedState, tipe::InputEventType::CursorMove, "Down"),
            "named cursor move on empty state is observed");
    auto counts = emptyObservedState.debugSnapshot().eventCounts;
    require(counts.spaces >= 1, "debug counts include observed spaces");
    require(counts.enters >= 1, "debug counts include observed enters");
    require(counts.escapes >= 1, "debug counts include observed escapes");
    {
        const auto emptySupervisionPreferencePath = preferenceBase.string() + "-empty-supervision.tsv";
        {
            std::ofstream preferences(emptySupervisionPreferencePath);
            preferences << "nihao\t你好\t9\n";
            preferences << "__correction__\tihao\tnihao\t4\n";
            preferences << "__segment_chain__\twoc\two\t我\tc\twocao\t我操\t3\n";
        }
        tipe::State emptySupervisionState({}, emptySupervisionPreferencePath);
        emptySupervisionState.space();
        emptySupervisionState.backspace();
        emptySupervisionState.deleteKey();
        emptySupervisionState.enter();
        emptySupervisionState.escape();
        emptySupervisionState.cursorMove("Down");
        emptySupervisionState.punctuation(",");
        const auto emptySupervisionRequest = emptySupervisionState.modelRequestSnapshot();
        require(emptySupervisionRequest.find("preedit\t\n") != std::string::npos,
                "empty-state supervision request keeps an explicit empty preedit row");
        require(emptySupervisionRequest.find("candidates\n") != std::string::npos,
                "empty-state supervision request keeps an explicit empty candidate row");
        require(emptySupervisionRequest.find("supervision_state\tmode\tpass-through-only\tactive_preedit\t0") !=
                    std::string::npos,
                "empty-state supervision request declares pass-through-only mode");
        require(emptySupervisionRequest.find(
                    "events\tspace:\tbackspace:\tdelete:\tenter:\tescape:\tcursor-move:Down\tobserved:,") !=
                    std::string::npos,
                "empty-state supervision request keeps pass-through key order");
        require(emptySupervisionRequest.find(
                    "event_counts\tspace:1\tbackspace:1\tdelete:1\tenter:1\tescape:1\tcursor-move:1\tobserved:1") !=
                    std::string::npos,
                "empty-state supervision request summarizes pass-through key counts");
        require(emptySupervisionRequest.find("\npreference\t") == std::string::npos &&
                    emptySupervisionRequest.find("\ncorrection\t") == std::string::npos &&
                    emptySupervisionRequest.find("\nsegment_chain\t") == std::string::npos &&
                    emptySupervisionRequest.find("\npending_segment\t") == std::string::npos,
                "empty-state pass-through supervision does not expose unrelated learned history");
        std::filesystem::remove(emptySupervisionPreferencePath);
    }
    {
        tipe::State emptyNavigationState({}, cleanPreferencePath);
        emptyNavigationState.expandCandidates("Down");
        emptyNavigationState.moveCandidateCursor(static_cast<int>(tipe::visualCandidateColumns), "PageDown");
        emptyNavigationState.moveCandidateCursor(-static_cast<int>(tipe::visualCandidateColumns), "PageUp");
        emptyNavigationState.moveCandidateCursorTo(0, "Home");
        emptyNavigationState.moveCandidateCursorTo(0, "End");
        emptyNavigationState.moveCandidateCursor(1, "Tab");
        emptyNavigationState.moveCandidateCursor(-1, "ShiftTab");
        const auto emptyNavigationCounts = emptyNavigationState.debugSnapshot().eventCounts;
        require(emptyNavigationCounts.cursorMoves == 7,
                "empty-state candidate navigation keys are still supervised as cursor moves");
        require(hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "PageDown") &&
                    hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "PageUp") &&
                    hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "Home") &&
                    hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "End") &&
                    hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "Tab") &&
                    hasEventText(emptyNavigationState, tipe::InputEventType::CursorMove, "ShiftTab"),
                "empty-state navigation supervision preserves key names");
    }
    {
        tipe::State passThroughContextState({}, cleanPreferencePath);
        passThroughContextState.space();
        passThroughContextState.deleteKey();
        passThroughContextState.cursorMove("Down");
        passThroughContextState.observeKey("Tab");
        type(passThroughContextState, "nihao");
        const auto passThroughContextRequest = passThroughContextState.modelRequestSnapshot();
        require(passThroughContextRequest.find(
                    "events\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o") !=
                    std::string::npos,
                "pass-through key supervision is preserved when later composing pinyin");
        require(passThroughContextRequest.find("event_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\tletter:5") !=
                    std::string::npos,
                "pass-through key supervision remains summarized with later pinyin input");
        require(passThroughContextRequest.find(
                    "correction_events\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o") !=
                    std::string::npos,
                "pass-through key supervision is also available to correction-aware model prompts");
    }

    type(state, "NiHao");
    require(state.preedit() == "nihao", "letters normalize to lowercase pinyin");
    state.reset();

    type(state, "ni");
    require(!state.candidates().empty() && state.candidates()[0] == "你", "exact short pinyin shows single-character candidate");
    state.reset();

    type(state, "nih");
    require(!state.candidates().empty(), "incomplete pinyin can show candidates");
    state.reset();

    type(state, "suoyi");
    require(!state.candidates().empty() && state.candidates()[0] == "所以", "common pinyin suoyi has candidates");
    action = state.space();
    require(action.accepted && action.commitText == "所以", "space commits common candidate");

    type(state, "jixuzuo");
    require(!state.candidates().empty() && state.candidates()[0] == "继续做", "long common phrase has candidates");
    state.reset();

    {
        tipe::State rankingState({}, cleanPreferencePath);
        type(rankingState, "guangb");
        require(!rankingState.candidates().empty() &&
                    (rankingState.candidates()[0] == "光标" || rankingState.candidates()[0] == "广播"),
                "partial guangb should prefer a common language-model completion");
        require(candidateIndex(rankingState, "广播") < 6,
                "partial guangb should keep the common word 广播 near the front");
        require(candidateIndex(rankingState, "光标") < 6,
                "partial guangb should keep the alternative common word 光标 near the front");
        rankingState.reset();
        type(rankingState, "tuij");
        require(candidateIndex(rankingState, "推荐") < 6,
                "partial tuij should keep 推荐 near the front instead of noisy cross-segment guesses");
        rankingState.reset();
        type(rankingState, "ganj");
        require(!rankingState.candidates().empty() && rankingState.candidates()[0] == "感觉",
                "partial ganj should prefer 感觉");
        rankingState.reset();
        type(rankingState, "weiz");
        require(!rankingState.candidates().empty() && rankingState.candidates()[0] == "位置",
                "partial weiz should prefer 位置");
        rankingState.reset();
        type(rankingState, "chux");
        require(!rankingState.candidates().empty() &&
                    (rankingState.candidates()[0] == "出现" || rankingState.candidates()[0] == "出席"),
                "partial chux should prefer a coherent high-frequency completion");
        rankingState.reset();
        type(rankingState, "geciw");
        require(!rankingState.candidates().empty() && rankingState.candidates()[0].starts_with("歌词") &&
                    rankingState.candidates()[0] != "歌词",
                "short continuation after a strong multi-syllable prefix should outrank cross-segment guesses");
        require(candidateIndex(rankingState, "歌词") < 6,
                "the strong prefix candidate remains visible behind its short continuation");
        const auto expectedContinuation = rankingState.candidates()[0];
        action = rankingState.space();
        require(action.accepted && action.commitText == expectedContinuation && rankingState.empty(),
                "committing a short continuation candidate consumes the whole current preedit");
    }

    type(state, "zhong");
    require(!state.candidates().empty() && state.candidates()[0] == "中",
            "single-syllable pinyin should prefer single-character candidates over words");
    require(candidateIndex(state, "中国") > candidateIndex(state, "中"),
            "single-syllable word candidates stay behind the single-character default");
    state.reset();

    type(state, "zhongguo");
    require(!state.candidates().empty() && state.candidates()[0] == "中国",
            "multi-syllable complete words still prefer the full phrase");
    state.reset();

    type(state, "woc");
    require(!state.candidates().empty() && state.candidates()[0] == "我操",
            "short prefix completion should promote the common exact phrase over noisy composed guesses");
    require(state.candidateConsumedPrefixLength(candidateIndex(state, "我")) == 2,
            "short prefix candidate metadata records the consumed pinyin even when the same text came from lookup");
    action = state.select(candidateIndex(state, "我"));
    require(action.accepted && action.commitText == "我", "short prefix completion still allows selecting the first character");
    require(state.preedit() == "c", "selecting the first character from woc keeps the remaining typed tail");
    state.reset();

    type(state, "woxiangyo");
    require(candidateIndex(state, "我想") < state.candidates().size(),
            "corrected long pinyin still exposes a shorter prefix candidate");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用",
            "corrected long pinyin prefers the useful Chinese completion");
    require(state.candidateConsumedPrefixLength(candidateIndex(state, "我想")) == 7,
            "multi-syllable prefix metadata records the consumed pinyin before the unresolved tail");
    action = state.select(candidateIndex(state, "我想"));
    require(action.accepted && action.commitText == "我想", "multi-syllable prefix from corrected long pinyin commits");
    require(state.preedit() == "yo", "corrected long pinyin keeps the unresolved typed tail after prefix commit");
    state.reset();

    type(state, "meiwenti");
    require(!state.candidates().empty() && state.candidates()[0] == "没问题",
            "complete multi-syllable candidates should outrank prefix-continuation composed guesses");
    require(optionalCandidateIndex(state, "美文提") > candidateIndex(state, "没问题"),
            "prefix-continuation composed guesses stay behind a complete candidate");
    state.reset();

    type(state, "dierge");
    require(!state.candidates().empty() && state.candidates().front() == "第二个" &&
                candidateIndex(state, "第二个") < candidateIndex(state, "第二格"),
            "natural phrase candidates should outrank lower-probability exact-pinyin sentences");
    state.reset();

    {
        const auto pollutedCorrectionPreferencePath = preferenceBase.string() + "-polluted-correction.tsv";
        std::ofstream pollutedPreferences(pollutedCorrectionPreferencePath);
        pollutedPreferences << "__correction__\tanjing\tganjing\t2\n";
        pollutedPreferences << "dierge\t第二个\t3\n";
        pollutedPreferences.close();
        tipe::State pollutedCorrectionState({}, pollutedCorrectionPreferencePath);
        type(pollutedCorrectionState, "dierge");
        require(!pollutedCorrectionState.candidates().empty() &&
                    pollutedCorrectionState.candidates()[0] == "第二个",
                "broad missing-letter corrections must not promote candidates that wrap an existing good phrase");
        require(optionalCandidateIndex(pollutedCorrectionState, "个第二个") >
                    candidateIndex(pollutedCorrectionState, "第二个"),
                "learned correction pollution stays behind the natural full candidate");
        std::filesystem::remove(pollutedCorrectionPreferencePath);
    }

    {
        const auto realWorldPollutionPreferencePath = preferenceBase.string() + "-real-world-pollution.tsv";
        std::ofstream pollutedPreferences(realWorldPollutionPreferencePath);
        pollutedPreferences << "woxiangyo\twoxiangyo\t5\n";
        pollutedPreferences << "__correction__\tanjing\tganjing\t2\n";
        pollutedPreferences << "__correction__\tihao\tnihao\t2\n";
        pollutedPreferences.close();

        tipe::State pollutedUserState({}, realWorldPollutionPreferencePath);
        type(pollutedUserState, "woxiangyo");
        require(!pollutedUserState.candidates().empty() && pollutedUserState.candidates()[0] == "我想用",
                "learned raw pollution cannot outrank a useful Chinese long-pinyin completion");
        require(optionalCandidateIndex(pollutedUserState, "woxiangyo") >
                    candidateIndex(pollutedUserState, "我想用"),
                "polluted raw text is absent or remains behind the useful Chinese candidate");
        pollutedUserState.reset();
        type(pollutedUserState, "haishm");
        require(!pollutedUserState.candidates().empty() && pollutedUserState.candidates()[0] == "还是没",
                "broad learned correction pollution cannot outrank haishm's useful Chinese completion");
        require(optionalCandidateIndex(pollutedUserState, "还三个画面") >
                    candidateIndex(pollutedUserState, "还是没"),
                "polluted haishm correction candidates stay behind the useful completion");
        pollutedUserState.reset();
        type(pollutedUserState, "haishmei");
        require(!pollutedUserState.candidates().empty() && pollutedUserState.candidates()[0] == "还是没",
                "broad learned correction pollution cannot outrank haishmei's useful Chinese completion");
        require(optionalCandidateIndex(pollutedUserState, "还什么你") >
                    candidateIndex(pollutedUserState, "还是没"),
                "polluted haishmei correction candidates stay behind the useful completion");
        pollutedUserState.reset();
        type(pollutedUserState, "engli");
        require(!pollutedUserState.candidates().empty() && pollutedUserState.candidates()[0] == "能力",
                "generalized typo learning still rescues phonetic fallback candidates");
        std::filesystem::remove(realWorldPollutionPreferencePath);
    }

    type(state, "gongneg");
    require(!state.candidates().empty() && state.candidates()[0] == "功能",
            "local typo repair promotes an exact known word when the current pinyin is a noisy segmentation");
    action = state.select(0);
    require(action.accepted && action.commitText == "功能" && state.empty(),
            "local full-word typo repair commits as a complete corrected candidate");
    state.reset();

    type(state, "jibengongneg");
    require(!state.candidates().empty() && state.candidates()[0] == "基本功能",
            "local typo repair works for a longer phrase without hard-coding the typo");
    state.reset();

    {
        tipe::State prefixState({}, cleanPreferencePath);
        type(prefixState, "jixuzuo");
        require(candidateIndex(prefixState, "继续") < 6, "multi-syllable prefix candidate is inserted near the front");
        require(prefixState.candidateConsumedPrefixLength(candidateIndex(prefixState, "继续")) == 4,
                "prefix candidate records the consumed pinyin length");
        require(prefixState.candidateConsumedPrefixLength(candidateIndex(prefixState, "继续做")) == 0,
                "full candidate does not pretend to consume only a prefix");
        require(prefixState.candidateSource(candidateIndex(prefixState, "继续")) == "prefix",
                "prefix candidate records its ranking source");
        require(!prefixState.candidateSource(candidateIndex(prefixState, "继续做")).empty(),
                "full candidate records a ranking source");
        require(prefixState.candidateScore(candidateIndex(prefixState, "继续")) > 0,
                "prefix candidate records a ranking score");
        action = prefixState.select(candidateIndex(prefixState, "继续"));
        require(action.accepted && action.commitText == "继续", "long pinyin can commit a multi-character prefix");
        require(prefixState.preedit() == "zuo", "multi-character prefix commit keeps remaining pinyin");
        require(candidateIndex(prefixState, "做") < prefixState.candidates().size(),
                "remaining pinyin after prefix commit refreshes candidates");
    }

    {
        const auto learnedPrefixPreferencePath = preferenceBase.string() + "-learned-prefix.tsv";
        std::filesystem::remove(learnedPrefixPreferencePath);
        {
            tipe::State trainer({}, learnedPrefixPreferencePath);
            type(trainer, "jixuzuo");
            action = trainer.select(candidateIndex(trainer, "继续"));
            require(action.accepted && action.commitText == "继续", "prefix candidate can be trained by selection");
            require(trainer.preedit() == "zuo", "trained prefix selection keeps remaining pinyin");
        }
        tipe::State learnedPrefixState({}, learnedPrefixPreferencePath);
        type(learnedPrefixState, "jixuzuo");
        require(!learnedPrefixState.candidates().empty() && learnedPrefixState.candidates()[0] == "继续做",
                "partial prefix selection should not poison the full long-preedit default candidate");
        require(candidateIndex(learnedPrefixState, "继续") < learnedPrefixState.candidates().size(),
                "learned prefix candidate remains available behind the full long-preedit candidate");
        learnedPrefixState.reset();
        type(learnedPrefixState, "jixu");
        require(!learnedPrefixState.candidates().empty() && learnedPrefixState.candidates()[0] == "继续",
                "partial prefix selection trains the actual prefix pinyin instead of the whole long preedit");
        learnedPrefixState.reset();
        type(learnedPrefixState, "jixuzuo");
        action = learnedPrefixState.moveCandidateCursorTo(candidateIndex(learnedPrefixState, "继续"), "End");
        require(action.accepted, "learned prefix remains selectable from the full long-preedit candidate list");
        action = learnedPrefixState.space();
        require(action.accepted && action.commitText == "继续",
                "space can still commit a highlighted learned prefix candidate");
        require(learnedPrefixState.preedit() == "zuo",
                "space keeps remaining pinyin when committing a highlighted prefix candidate");
        std::filesystem::remove(learnedPrefixPreferencePath);
    }
    {
        const auto shortCorrectionPreferencePath = preferenceBase.string() + "-short-correction.tsv";
        std::filesystem::remove(shortCorrectionPreferencePath);
        std::ofstream shortCorrectionPreferences(shortCorrectionPreferencePath);
        shortCorrectionPreferences << "__correction__\txue\txuexi\t2\n";
        shortCorrectionPreferences << "__correction__\tihao\tnihao\t2\n";
        shortCorrectionPreferences << "__correction__\tanjing\tganjing\t2\n";
        shortCorrectionPreferences << "__correction__\tgongnegn\tgongneng\t2\n";
        shortCorrectionPreferences << "__correction__\tyinggshi\tyinggaishi\t2\n";
        shortCorrectionPreferences.close();

        tipe::State protectedShortState({}, shortCorrectionPreferencePath);
        type(protectedShortState, "jixu");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "继续",
                "generalized learned correction patterns do not poison short common pinyin");
        require(std::find(protectedShortState.candidates().begin(), protectedShortState.candidates().end(), "个继续") ==
                    protectedShortState.candidates().end(),
                "short common pinyin is not prefixed by unrelated generalized corrections");
        protectedShortState.reset();
        type(protectedShortState, "ihao");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "你好",
                "exact learned short correction remains active");
        protectedShortState.reset();
        type(protectedShortState, "ganj");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "感觉",
                "learned correction patterns do not poison useful short prefix completions");
        protectedShortState.reset();
        type(protectedShortState, "guangb");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "光标",
                "learned correction patterns do not poison useful longer prefix completions");
        protectedShortState.reset();
        type(protectedShortState, "gongneg");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "功能",
                "local exact typo repair outranks noisy learned correction candidates");
        require(std::find(protectedShortState.candidates().begin(), protectedShortState.candidates().end(), "年贡呢个") ==
                    protectedShortState.candidates().end(),
                "local exact typo repair suppresses noisy generalized correction candidates");
        require(std::find(protectedShortState.candidates().begin(), protectedShortState.candidates().end(), "贡呢个") ==
                    protectedShortState.candidates().end(),
                "local exact typo repair suppresses noisy original segmentation candidates");
        require(!hasDuplicateCandidates(protectedShortState),
                "local exact typo repair deduplicates candidates already produced by learned corrections");
        protectedShortState.reset();
        type(protectedShortState, "yinggshi");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "应该是",
                "an exact learned correction outranks a conflicting one-edit local guess");
        protectedShortState.reset();
        type(protectedShortState, "wenti");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "问题",
                "known pinyin words are not prefixed by unrelated learned missing-letter corrections");
        require(std::find(protectedShortState.candidates().begin(), protectedShortState.candidates().end(), "个问题") ==
                    protectedShortState.candidates().end(),
                "known pinyin words do not expose unrelated corrected-prefix candidates");
        protectedShortState.reset();
        type(protectedShortState, "jixuxiu");
        require(!protectedShortState.candidates().empty() && protectedShortState.candidates()[0] == "继续修",
                "known segmented pinyin phrases are protected from unrelated learned missing-letter corrections");
        require(std::find(protectedShortState.candidates().begin(), protectedShortState.candidates().end(), "国际续修") ==
                    protectedShortState.candidates().end(),
                "known segmented pinyin phrases do not expose unrelated corrected-prefix phrase candidates");
        std::filesystem::remove(shortCorrectionPreferencePath);
    }
    {
        std::filesystem::remove(genericSegmentContinuationPreferencePath);
        tipe::InputModel model(genericSegmentContinuationPreferencePath);
        model.recordCandidateSelection("qian", "前");
        model.recordSegmentChain({"qianhou", "qian", "前", "hou", "qianhou", "前后"});
        auto ranked = model.applyLearnedPreferences("hou", {"弱", "后"});
        require(ranked.size() == 2 && ranked[0] == "后",
                "learned segment chains boost the suffix candidate after the matching recent prefix commit");

        tipe::InputModel reloadedWithoutContext(genericSegmentContinuationPreferencePath);
        ranked = reloadedWithoutContext.applyLearnedPreferences("hou", {"弱", "后"});
        require(ranked.size() == 2 && ranked[0] == "弱",
                "learned segment-chain continuation does not apply without matching recent commit context");

        model.recordCandidatePreference("dayiban", "打", 3);
        model.recordSegmentChain({"dayiban", "dayi", "打一", "ban", "dayiban", "打一半"});
        const auto supportedStagedSegment = model.learnedSegmentCandidates("dayiban");
        require(supportedStagedSegment.size() == 1 && supportedStagedSegment.front() == "打一半",
                "one complete segment chain can stage a phrase when an active prefix choice supports it");

        model.recordSegmentChain({"renyitai", "renyi", "任意", "tai", "renyitai", "任意态"});
        model.recordSegmentChain({"renyitai", "renyi", "任意", "tai", "renyitai", "任意态"});
        const auto learnedFullSegments = model.learnedSegmentCandidates("renyitai");
        require(learnedFullSegments.size() == 1 && learnedFullSegments.front() == "任意态",
                "a repeatedly confirmed segment chain becomes full-preedit ranking evidence");
        tipe::State learnedFullSegmentState({}, genericSegmentContinuationPreferencePath);
        type(learnedFullSegmentState, "renyitai");
        require(!learnedFullSegmentState.candidates().empty() &&
                    learnedFullSegmentState.candidates().front() == "任意态",
                "ordinary input promotes a repeatedly confirmed full segment chain");

        model.recordSegmentChain({"nihao", "ni", "你", "hao", "nihao", "你号"});
        model.recordSegmentChain({"nihao", "ni", "你", "hao", "nihao", "你号"});
        tipe::State learnedSuffixSegmentState({}, genericSegmentContinuationPreferencePath);
        type(learnedSuffixSegmentState, "nihao");
        action = learnedSuffixSegmentState.select(candidateIndex(learnedSuffixSegmentState, "你"));
        require(action.accepted && action.commitText == "你" && learnedSuffixSegmentState.preedit() == "hao",
                "a learned full chain still allows selecting its prefix");
        require(!learnedSuffixSegmentState.candidates().empty() &&
                    learnedSuffixSegmentState.candidates().front() == "号",
                "a learned full chain also controls the pending suffix order");
        std::filesystem::remove(genericSegmentContinuationPreferencePath);
    }

    type(state, "nijixuzuo");
    require(!state.candidates().empty() && state.candidates()[0] != "你",
            "long pinyin keeps full candidate ahead of single-character fallback");
    require(candidateIndex(state, "你") < state.candidates().size(),
            "long pinyin still exposes first-syllable single character");
    action = state.select(candidateIndex(state, "你"));
    require(action.accepted && action.commitText == "你", "long pinyin can commit first character only");
    require(state.preedit() == "jixuzuo", "long pinyin keeps remaining syllables after first-character commit");
    require(!state.candidates().empty() && state.candidates()[0] == "继续做",
            "remaining long pinyin refreshes phrase candidates");
    state.reset();

    type(state, "nibunonghao");
    require(candidateIndex(state, "你不弄好") < state.candidates().size(),
            "observed long phrase nibunonghao remains selectable");
    require(candidateIndex(state, "你不") < 6, "two-syllable prefix candidate is inserted near the front");
    action = state.select(candidateIndex(state, "你不"));
    require(action.accepted && action.commitText == "你不", "long pinyin can commit a two-character prefix");
    require(state.preedit() == "nonghao", "two-character prefix commit keeps remaining pinyin");
    require(candidateIndex(state, "弄好") < state.candidates().size(), "remaining phrase after prefix commit refreshes candidates");
    state.reset();

    type(state, "nanizuobei");
    require(!state.candidates().empty() && state.candidates()[0] == "那你做呗",
            "spoken phrase nanizuobei has a sensible initial candidate");
    std::string packedBackfillCandidate;
    std::size_t packedBackfillConsumed = 0;
    {
        const auto visible = state.visibleCandidates();
        require(visible.size() == 4 && visible[0] == "那你做呗" && visible[1] == "那" &&
                    visible[2] == "那你坐北" && !visible[3].empty(),
                "collapsed visible candidates backfill first-row gaps with later short candidates");
        const auto request = state.modelRequestSnapshot();
        packedBackfillCandidate = visible[3];
        const auto backfillIndex = candidateIndex(state, packedBackfillCandidate);
        packedBackfillConsumed = state.candidateConsumedPrefixLength(backfillIndex);
        require(packedBackfillConsumed > 0 && packedBackfillConsumed < state.preedit().size(),
                "collapsed backfill candidate records a partial pinyin span");
        const auto visibleRow = "visible_candidates\t0:那你做呗\t1:那\t2:那你坐北\t" +
                                std::to_string(backfillIndex) + ":" + packedBackfillCandidate + "\n";
        const auto numberedRow = "numbered_candidates\t1:0:那你做呗\t2:1:那\t3:2:那你坐北\t4:" +
                                 std::to_string(backfillIndex) + ":" + packedBackfillCandidate + "\n";
        require(request.find(visibleRow) != std::string::npos,
                "model request visible candidates match the collapsed packed row");
        require(request.find(numberedRow) != std::string::npos,
                "model request numbered candidates match collapsed digit labels");
    }
    action = state.selectVisibleDigit(3, "4");
    require(action.accepted && action.commitText == packedBackfillCandidate,
            "collapsed digit selection follows the visible packed row instead of raw candidate indexes");
    require(state.preedit() == std::string_view("nanizuobei").substr(packedBackfillConsumed) &&
                tipe::isCompletePinyinSequence(state.preedit()),
            "collapsed packed prefix selection keeps a parseable remaining pinyin sequence");
    state.reset();

    type(state, "nanizuobei");
    action = state.select(candidateIndex(state, "那"));
    require(action.accepted && action.commitText == "那", "nanizuobei can commit the first particle");
    require(state.preedit() == "nizuobei", "nanizuobei keeps remaining pinyin after first particle");
    require(candidateIndex(state, "你做呗") < state.candidates().size(),
            "remaining nizuobei phrase stays selectable");
    state.reset();

    type(state, "jixuzuoa");
    require(!state.candidates().empty() && state.candidates()[0] == "继续做啊",
            "spoken phrase jixuzuoa has a sensible initial candidate");
    action = state.select(candidateIndex(state, "继续做"));
    require(action.accepted && action.commitText == "继续做", "jixuzuoa can commit a three-character prefix");
    require(state.preedit() == "a", "jixuzuoa keeps final particle after prefix commit");
    require(candidateIndex(state, "啊") < state.candidates().size(), "remaining final particle is selectable");
    state.reset();

    type(state, "jinrixiufu");
    require(candidateIndex(state, "今日") < 6, "common jin/ri syllables support two-character prefix candidates");
    action = state.select(candidateIndex(state, "今日"));
    require(action.accepted && action.commitText == "今日", "jin-ri prefix can be committed from a longer preedit");
    require(state.preedit() == "xiufu", "jin-ri prefix commit keeps xiu-fu as remaining pinyin");
    require(candidateIndex(state, "修复") < state.candidates().size(), "remaining xiu-fu candidate is still selectable");
    state.reset();

    type(state, "lveshi");
    require(!state.candidates().empty() && state.candidates()[0] == "略施",
            "typed lve pinyin prefers Chinese candidates over raw v text");
    require(candidateIndex(state, "略") < 6, "typed lve syllable exposes prefix candidates");
    action = state.select(candidateIndex(state, "略"));
    require(action.accepted && action.commitText == "略", "lve prefix can be committed from a longer preedit");
    require(state.preedit() == "shi", "lve prefix commit keeps following syllable");
    require(candidateIndex(state, "是") < state.candidates().size(), "remaining shi candidate is still selectable");
    state.reset();

    type(state, "nvedai");
    require(!state.candidates().empty() && state.candidates()[0] == "虐待",
            "typed nve pinyin prefers Chinese candidates over raw v text");
    require(candidateIndex(state, "虐") < 6, "typed nve syllable exposes prefix candidates");
    action = state.select(candidateIndex(state, "虐"));
    require(action.accepted && action.commitText == "虐", "nve prefix can be committed from a longer preedit");
    require(state.preedit() == "dai", "nve prefix commit keeps following syllable");
    state.reset();

    type(state, "zaidianyici");
    require(candidateIndex(state, "再点一次") < state.candidates().size(),
            "observed long phrase zaidianyici remains selectable");
    state.reset();

    type(state, "yianfangxiangjian");
    require(state.preedit() == "yianfangxiangjian", "long pinyin-like input remains in preedit");
    require(candidateIndex(state, "一按方向键") < state.candidates().size(),
            "long pinyin-like input can compose fallback phrase");
    require(state.candidates().size() >= 3, "composed fallback phrase exposes multiple candidates");
    require(!hasDuplicateCandidates(state), "composed fallback candidates are deduplicated");
    action = state.backspace();
    require(action.accepted && state.preedit() == "yianfangxiangjia", "long pinyin-like input can be edited");
    state.reset();

    type(state, "womeiyoufanying");
    require(candidateIndex(state, "我没有反应") < state.candidates().size(),
            "fallback phrase composer keeps common multi-word input selectable");
    require(!state.candidates().empty() && state.candidates()[0] == "我没有反应",
            "fallback phrase composer prefers longer known words over over-segmented text");
    state.reset();

    type(state, "shurufa");
    require(!state.candidates().empty() && state.candidates()[0] == "输入法", "known multi-character word is promoted");
    state.reset();

    type(state, "shenglue");
    require(!state.candidates().empty() && state.candidates()[0] == "省略",
            "known word shenglue is promoted ahead of libpinyin single-character guesses");
    state.reset();

    type(state, "shengluehao");
    require(!state.candidates().empty() && state.candidates()[0] == "省略号",
            "known word shengluehao is available as a direct phrase");
    state.reset();

    type(state, "shur");
    require(candidateIndex(state, "输入") < candidateIndex(state, "输入法"),
            "fallback prefix candidates keep shorter direct words ahead of longer completions");
    state.reset();

    type(state, "shurunihaodeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "输入你好的时候",
            "real trial phrase shurunihaodeshihou has a sensible initial candidate");
    state.reset();

    type(state, "yidingyaobanihaodawan");
    require(!state.candidates().empty() && state.candidates()[0] == "一定要把你好打完",
            "real trial phrase yidingyaobanihaodawan has a sensible initial candidate");
    state.reset();

    type(state, "buranmeiyouhouxuanchuang");
    require(!state.candidates().empty() && state.candidates()[0] == "不然没有候选窗",
            "real trial phrase buranmeiyouhouxuanchuang has a sensible initial candidate");
    state.reset();

    type(state, "dgithub");
    require(!state.candidates().empty() && state.candidates()[0] == "打github",
            "real trial phrase dgithub prefers the intended mixed Chinese-English candidate");
    state.reset();

    type(state, "dagithubdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "打github的时候",
            "real trial phrase dagithubdeshihou has a sensible mixed Chinese-English candidate");
    state.reset();

    type(state, "githubdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "github的时候",
            "mixed English-Chinese phrase githubdeshihou keeps github raw with Chinese suffix");
    state.reset();

    type(state, "dgithubdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "打github的时候",
            "mixed English-Chinese phrase dgithubdeshihou has a sensible initial candidate");
    require(candidateIndex(state, "打github") < state.candidates().size(),
            "mixed English-Chinese typo phrase exposes an English-token prefix candidate");
    action = state.select(candidateIndex(state, "打github"));
    require(action.accepted && action.commitText == "打github" && state.preedit() == "deshihou",
            "mixed English-token typo prefix commit keeps the remaining pinyin after the token");
    state.reset();

    type(state, "dagithubdeshihou");
    require(candidateIndex(state, "打github") < state.candidates().size(),
            "mixed English-Chinese phrase exposes an English-token prefix candidate");
    action = state.select(candidateIndex(state, "打github"));
    require(action.accepted && action.commitText == "打github",
            "mixed English-token prefix candidate can be committed");
    require(state.preedit() == "deshihou",
            "mixed English-token prefix commit keeps the remaining pinyin after the token");
    require(!state.candidates().empty() && state.candidates()[0] == "的时候",
            "mixed English-token prefix remainder refreshes candidates");
    state.reset();

    type(state, "dawanle");
    require(!state.candidates().empty() && state.candidates()[0] == "打完了",
            "real trial phrase dawanle has a sensible initial candidate");
    state.reset();

    type(state, "houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen");
    require(!state.candidates().empty() &&
                state.candidates()[0] == "候选窗显示的不是github这个英文而是中文",
            "real trial mixed English sentence keeps github raw inside Chinese phrase");
    state.reset();

    type(state, "andown");
    require(!state.candidates().empty() && state.candidates()[0] == "按down",
            "real navigation phrase andown prefers mixed Chinese-English command text");
    state.reset();

    type(state, "bianchenggengchangdeyilie");
    require(!state.candidates().empty() && state.candidates()[0] == "变成更长的一列",
            "real candidate-window phrase bianchenggengchangdeyilie has a sensible initial candidate");
    state.reset();

    type(state, "suoyine");
    require(!state.candidates().empty() && state.candidates()[0] == "所以呢",
            "real connector phrase suoyine has a sensible initial candidate");
    state.reset();

    type(state, "qingwenyixia");
    require(!state.candidates().empty() && state.candidates()[0] == "请问一下",
            "common phrase qingwenyixia prefers 一下 over 以下");
    action = state.select(candidateIndex(state, "请问"));
    require(action.accepted && action.commitText == "请问", "qingwenyixia can commit the polite prefix");
    require(state.preedit() == "yixia", "qingwen prefix commit keeps yixia");
    require(!state.candidates().empty() && state.candidates()[0] == "一下",
            "remaining yixia still prefers 一下");
    state.reset();

    type(state, "ruguo");
    require(!state.candidates().empty() && state.candidates()[0] == "如果",
            "short common word ruguo should not be overtaken by longer prefix completions");
    state.reset();

    type(state, "zhegebudui");
    require(!state.candidates().empty() && state.candidates()[0] == "这个不对",
            "common phrase zhegebudui prefers 不对 over 部队");
    state.reset();

    type(state, "zhegebuxing");
    require(!state.candidates().empty() && state.candidates()[0] == "这个不行",
            "common phrase zhegebuxing prefers 不行 over 不幸");
    state.reset();

    type(state, "dengyixia");
    require(!state.candidates().empty() && state.candidates()[0] == "等一下",
            "common phrase dengyixia prefers 一下 over 以下");
    state.reset();

    type(state, "shaodengyixia");
    require(!state.candidates().empty() && state.candidates()[0] == "稍等一下",
            "common phrase shaodengyixia prefers 稍等一下 over 少等以下");
    state.reset();

    type(state, "cikuzhexiekeyiyibudaoweia");
    require(!state.candidates().empty() && state.candidates()[0] == "词库这些可以一步到位啊",
            "real dictionary feedback phrase cikuzhexiekeyiyibudaoweia has a sensible initial candidate");
    state.reset();

    type(state, "haodewokanyixiahaiyoumeiyu");
    require(candidateIndex(state, "好的我看一下") < state.candidates().size(),
            "long sentence exposes a selectable prefix phrase");
    action = state.select(candidateIndex(state, "好的我看一下"));
    require(action.accepted && action.commitText == "好的我看一下",
            "long sentence can commit a prefix phrase without taking the rest");
    require(state.preedit() == "haiyoumeiyu",
            "long sentence prefix commit keeps remaining pinyin after the selected phrase");
    require(candidateIndex(state, "还有美誉") < state.candidates().size(),
            "remaining long-sentence pinyin refreshes candidates after prefix commit");
    state.reset();

    type(state, "nihaowoxiangwenyixia");
    require(!state.candidates().empty() && state.candidates()[0] == "你好我想问一下",
            "common long sentence nihaowoxiangwenyixia prefers the natural phrase");
    action = state.select(candidateIndex(state, "你好"));
    require(action.accepted && action.commitText == "你好",
            "common long sentence can commit the greeting prefix");
    require(state.preedit() == "woxiangwenyixia",
            "common long sentence keeps remaining pinyin after greeting prefix");
    require(!state.candidates().empty() && state.candidates()[0] == "我想问一下",
            "remaining common sentence prefers the natural continuation");
    action = state.select(candidateIndex(state, "我想"));
    require(action.accepted && action.commitText == "我想",
            "common long sentence can commit a second prefix chunk");
    require(state.preedit() == "wenyixia",
            "second common prefix commit keeps the final phrase pinyin");
    require(!state.candidates().empty() && state.candidates()[0] == "问一下",
            "remaining wenyixia prefers question phrase over awkward homophones");
    state.reset();

    type(state, "woxiangwenyigewenti");
    require(!state.candidates().empty() && state.candidates()[0] == "我想问一个问题",
            "common question sentence woxiangwenyigewenti has a sensible initial candidate");
    state.reset();

    type(state, "woxiangyaozuoshenme");
    require(!state.candidates().empty() && state.candidates()[0] == "我想要做什么",
            "common question sentence woxiangyaozuoshenme has a sensible initial candidate");
    action = state.select(candidateIndex(state, "我想"));
    require(action.accepted && action.commitText == "我想",
            "common question sentence can commit the first two-character prefix");
    require(state.preedit() == "yaozuoshenme",
            "common question sentence keeps remaining pinyin after first prefix commit");
    require(!state.candidates().empty() && state.candidates()[0] == "要做什么",
            "remaining question phrase refreshes after prefix commit");
    state.reset();

    type(state, "nihaowoxiangyaozuoshenme");
    require(!state.candidates().empty() && state.candidates()[0] == "你好我想要做什么",
            "common greeting plus question sentence has a sensible initial candidate");
    state.reset();

    type(state, "woxiangyonggithub");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用github",
            "Chinese prefix plus github suffix keeps raw English token");
    require(candidateIndex(state, "我想用") < state.candidates().size(),
            "Chinese prefix before github remains selectable");
    action = state.select(candidateIndex(state, "我想用"));
    require(action.accepted && action.commitText == "我想用",
            "Chinese prefix before github can be committed");
    require(state.preedit() == "github", "woxiangyong prefix commit keeps github raw token");
    require(!state.candidates().empty() && state.candidates()[0] == "github",
            "remaining github prefers raw English token");
    state.reset();

    type(state, "woxiangyo");
    require(std::find(state.candidates().begin(), state.candidates().end(), "我想用git") == state.candidates().end(),
            "incomplete yong should not prematurely suggest mixed English-token phrases");
    state.reset();

    type(state, "woxiangyongchatgpt");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用chatgpt",
            "Chinese prefix plus chatgpt suffix keeps raw English token");
    state.reset();

    type(state, "woxiangyongdocker");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用docker",
            "Chinese prefix plus docker suffix keeps raw English token instead of transliteration");
    state.reset();

    type(state, "woxiangyonggit");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用git",
            "Chinese prefix plus short git suffix should not expand to github");
    state.reset();

    type(state, "woxiangyongpython");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用python",
            "Chinese prefix plus python suffix keeps raw English token");
    state.reset();

    type(state, "woxiangyongollama");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用ollama",
            "Chinese prefix plus ollama suffix keeps raw English token");
    state.reset();

    type(state, "woxiangyongniri");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用niri",
            "Chinese prefix plus niri suffix keeps raw English token");
    state.reset();

    type(state, "woxiangyongqwen");
    action = state.inputAsciiDigit('2');
    require(action.accepted && state.preedit() == "woxiangyongqwen2",
            "mixed Chinese prefix can accept a known alphanumeric English-token suffix digit");
    require(!state.candidates().empty() && state.candidates()[0] == "我想用qwen2",
            "Chinese prefix plus qwen2 suffix keeps raw alphanumeric English token");
    state.reset();

    type(state, "githubshang");
    require(!state.candidates().empty() && state.candidates()[0] == "github上",
            "raw github token composes with Chinese shang suffix");
    state.reset();

    type(state, "dockerlimian");
    require(!state.candidates().empty() && state.candidates()[0] == "docker里面",
            "raw docker token composes with Chinese limian suffix");
    state.reset();

    type(state, "systemdfuwu");
    require(!state.candidates().empty() && state.candidates()[0] == "systemd服务",
            "raw systemd token composes with Chinese fuwu suffix");
    state.reset();

    type(state, "dbusxiaoxi");
    require(!state.candidates().empty() && state.candidates()[0] == "dbus消息",
            "raw dbus token composes with Chinese xiaoxi suffix");
    state.reset();

    type(state, "dakaigithub");
    require(!state.candidates().empty() && state.candidates()[0] == "打开github",
            "open command composes with github raw token");
    state.reset();

    type(state, "dakaigit");
    require(!state.candidates().empty() && state.candidates()[0] == "打开git",
            "open command composes with git raw token instead of github");
    state.reset();

    type(state, "dakaiollama");
    require(!state.candidates().empty() && state.candidates()[0] == "打开ollama",
            "open command composes with ollama raw token");
    state.reset();

    type(state, "dakainiri");
    require(!state.candidates().empty() && state.candidates()[0] == "打开niri",
            "open command composes with niri raw token");
    state.reset();

    type(state, "xiugaidocker");
    require(!state.candidates().empty() && state.candidates()[0] == "修改docker",
            "modify command composes with docker raw token");
    state.reset();

    type(state, "xiugaigit");
    require(!state.candidates().empty() && state.candidates()[0] == "修改git",
            "modify command composes with git raw token instead of github");
    state.reset();

    type(state, "xiugaipython");
    require(!state.candidates().empty() && state.candidates()[0] == "修改python",
            "modify command composes with python raw token");
    state.reset();

    type(state, "haodewokanyxiahaiyoumeiyu");
    require(candidateIndex(state, "好的我看一下") < state.candidates().size(),
            "long sentence typo exposes a selectable known prefix phrase");
    action = state.select(candidateIndex(state, "好的我看一下"));
    require(action.accepted && action.commitText == "好的我看一下",
            "long sentence typo can commit a known prefix phrase without taking the rest");
    require(state.preedit() == "haiyoumeiyu",
            "long sentence typo prefix commit keeps remaining pinyin after the selected phrase");
    require(candidateIndex(state, "还有美誉") < state.candidates().size(),
            "remaining pinyin after typo prefix commit refreshes candidates");
    state.reset();

    type(state, "changjuxianxuanqianbanduanhouhoumiandepinyinbubaoliu");
    require(!state.candidates().empty() && state.candidates()[0] == "长句先选前半段后后面的拼音不保留",
            "real partial-commit feedback sentence has a sensible initial candidate");
    action = state.select(candidateIndex(state, "长句先选前半段后"));
    require(action.accepted && action.commitText == "长句先选前半段后",
            "generated long-sentence prefix candidate can be committed");
    require(state.preedit() == "houmiandepinyinbubaoliu",
            "generated long-sentence prefix keeps remaining pinyin");
    require(!state.candidates().empty() && state.candidates()[0] == "后面的拼音不保留",
            "generated long-sentence remainder refreshes candidates");
    state.reset();

    type(state, "haodewokanyixiahaiyoumeiyu");
    action = state.moveCandidateCursorTo(candidateIndex(state, "好的我看一下"), "End");
    require(action.accepted, "long sentence can move cursor to prefix phrase candidate");
    action = state.space();
    require(action.accepted && action.commitText == "好的我看一下",
            "space on a highlighted prefix phrase commits that prefix");
    require(state.preedit() == "haiyoumeiyu",
            "space on a highlighted prefix phrase keeps remaining pinyin");
    require(candidateIndex(state, "还有美誉") < state.candidates().size(),
            "remaining pinyin refreshes after space commits a prefix phrase");
    state.reset();

    type(state, "haodewokanyixiahaiyoumeiyu");
    action = state.expandCandidates("Down");
    require(action.accepted && state.candidatesExpanded(), "first Down expands long-sentence candidates");
    action = state.selectVisibleDigit(1, "2");
    require(action.accepted && action.commitText == "好的我看一下",
            "digit 2 in the first expanded visual row commits the prefix phrase");
    require(state.preedit() == "haiyoumeiyu",
            "digit selection after expanded Down navigation keeps remaining pinyin");
    state.reset();

    type(state, "jixuzuo");
    require(state.candidates().size() >= 3 && state.candidates()[0] == "继续做" && state.candidates()[1] == "继续",
            "multi-character prefix candidates are shown before single-character fallbacks");
    state.reset();

    type(state, "changjuxianxuanqianbanduanhoumiandepinyinbubaoliu");
    require(!state.candidates().empty() && state.candidates()[0] == "长句先选前半段后",
            "typo-like long sentence promotes the known prefix phrase ahead of divergent backend guesses");
    require(optionalCandidateIndex(state, "长局限选前半段后面的拼音不保留") > 24,
            "divergent long sentence guesses are absent or stay out of the first expanded page");
    action = state.select(candidateIndex(state, "长句先选前半段后"));
    require(action.accepted && action.commitText == "长句先选前半段后",
            "typo-like long sentence can commit the known prefix phrase");
    require(state.preedit() == "miandepinyinbubaoliu",
            "typo-like long sentence prefix commit preserves the remaining pinyin");
    require(candidateIndex(state, "面的拼音不保留") < state.candidates().size(),
            "remaining typo-like long sentence pinyin still exposes a useful continuation");
    state.reset();

    type(state, "cikubuyinggaichushishezhiyigebijiaohaoyongdeshunxuma");
    require(!state.candidates().empty() &&
                state.candidates()[0] == "词库不应该初始设置一个比较好用的顺序吗",
            "real dictionary-order feedback phrase has a sensible initial candidate");
    state.reset();

    type(state, "erqiewoganjuemeiyougengxin");
    require(!state.candidates().empty() && state.candidates()[0] == "而且我感觉没有更新",
            "real learning feedback phrase erqiewoganjuemeiyougengxin has a sensible initial candidate");
    state.reset();

    type(state, "pinyinjiuquanbuxiaoshile");
    require(!state.candidates().empty() && state.candidates()[0] == "拼音就全部消失了",
            "real partial-commit feedback phrase pinyinjiuquanbuxiaoshile has a sensible initial candidate");
    state.reset();

    type(state, "pinyinjiu");
    require(!state.candidates().empty() && state.candidates()[0] == "拼音就",
            "short prefixes prefer a strong known prefix plus one complete syllable continuation");
    require(optionalCandidateIndex(state, "拼音就全部消失了") > 6,
            "short prefixes of long fallback feedback phrases do not prematurely enter the visible row");
    action = state.space();
    require(action.accepted && action.commitText == "拼音就" && state.empty(),
            "space on a strong prefix plus one-syllable continuation commits the whole typed preedit");
    state.reset();

    type(state, "bunengjiezhexuanhaozhegezi");
    require(!state.candidates().empty() && state.candidates()[0] == "不能接着选好这个字",
            "real partial-commit feedback phrase bunengjiezhexuanhaozhegezi has a sensible initial candidate");
    state.reset();

    type(state, "zhexiewentixianchengdeshurufazaojiujiejuele");
    require(!state.candidates().empty() && state.candidates()[0] == "这些问题现成的输入法早就解决了",
            "real expectation feedback phrase zhexiewentixianchengdeshurufazaojiujiejuele has a sensible initial candidate");
    state.reset();

    type(state, "butaixing");
    require(!state.candidates().empty() && state.candidates()[0] == "不太行",
            "real complaint phrase butaixing has a sensible initial candidate");
    state.reset();

    type(state, "shunxuyoudianwenti");
    require(!state.candidates().empty() && state.candidates()[0] == "顺序有点问题",
            "real complaint phrase shunxuyoudianwenti has a sensible initial candidate");
    action = state.select(candidateIndex(state, "顺序"));
    require(action.accepted && action.commitText == "顺序", "long complaint phrase can commit shunxu prefix");
    require(state.preedit() == "youdianwenti", "shunxu prefix commit leaves youdianwenti");
    require(!state.candidates().empty() && state.candidates()[0] == "有点问题",
            "recent context promotes continuation after shunxu");
    require(candidateIndex(state, "有点问题") < state.candidates().size(),
            "remaining youdianwenti phrase stays selectable");
    state.reset();

    type(state, "guangbiaogensuishibai");
    require(!state.candidates().empty() && state.candidates()[0] == "光标跟随失败",
            "cursor-follow complaint phrase has a sensible initial candidate");
    action = state.select(candidateIndex(state, "光标跟随"));
    require(action.accepted && action.commitText == "光标跟随", "cursor-follow phrase can commit a prefix");
    require(state.preedit() == "shibai", "guangbiaogensui prefix commit leaves shibai");
    require(!state.candidates().empty() && state.candidates()[0] == "失败",
            "recent context promotes continuation after guangbiaogensui");
    require(candidateIndex(state, "失败") < state.candidates().size(), "remaining shibai phrase stays selectable");
    state.reset();

    type(state, "jixuzuoba");
    require(!state.candidates().empty() && state.candidates()[0] == "继续做吧",
            "real continuation phrase jixuzuoba has a sensible initial candidate");
    action = state.select(candidateIndex(state, "继续做"));
    require(action.accepted && action.commitText == "继续做", "jixuzuoba can commit jixuzuo prefix");
    require(state.preedit() == "ba", "jixuzuo prefix commit leaves ba");
    require(candidateIndex(state, "吧") < state.candidates().size(), "remaining ba particle stays selectable");
    state.reset();

    type(state, "jixunonga");
    require(!state.candidates().empty() && state.candidates()[0] == "继续弄啊",
            "real continuation phrase jixunonga has a sensible initial candidate");
    state.reset();

    type(state, "houxuanchuangxianshi");
    require(!state.candidates().empty() && state.candidates()[0] == "候选窗显示",
            "real trial phrase houxuanchuangxianshi has a sensible initial candidate");
    state.reset();

    type(state, "houxuanch");
    require(candidateIndex(state, "候选窗") < candidateIndex(state, "候选窗口"),
            "fallback prefix candidates use deterministic shorter-key ordering");
    state.reset();

    type(state, "houxuanchuangkoubenlaishi");
    require(!state.candidates().empty() && state.candidates()[0] == "候选窗口本来是",
            "real trial phrase houxuanchuangkoubenlaishi has a sensible initial candidate");
    state.reset();

    type(state, "zhijiebunengyongle");
    require(!state.candidates().empty() && state.candidates()[0] == "直接不能用了",
            "real failure phrase zhijiebunengyongle has a sensible initial candidate");
    state.reset();

    type(state, "zhaohuanbuchulai");
    require(!state.candidates().empty() && state.candidates()[0] == "召唤不出来",
            "real failure phrase zhaohuanbuchulai has a sensible initial candidate");
    state.reset();

    type(state, "zuizhongchengpin");
    require(!state.candidates().empty() && state.candidates()[0] == "最终成品",
            "product-status phrase zuizhongchengpin has a sensible initial candidate");
    state.reset();

    type(state, "tesegongneng");
    require(!state.candidates().empty() && state.candidates()[0] == "特色功能",
            "feature phrase tesegongneng has a sensible initial candidate");
    state.reset();

    tipe::State basicCommitState({}, cleanPreferencePath);
    type(basicCommitState, "nihao");
    require(basicCommitState.preedit() == "nihao", "nihao preedit");
    require(!basicCommitState.candidates().empty() && basicCommitState.candidates()[0] == "你好", "nihao candidate");
    require(basicCommitState.candidateCursorIndex() == 0, "candidate cursor starts at first candidate");
    require(hasEvent(basicCommitState, tipe::InputEventType::Letter), "letter events are recorded");
    action = basicCommitState.space();
    require(action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "你好", "space commits first candidate");
    require(basicCommitState.empty(), "space clears state");
    require(hasEvent(basicCommitState, tipe::InputEventType::CandidateSelected), "candidate commit is recorded");
    {
        auto sessionContext = basicCommitState.sessionContext();
        require(sessionContext.recentCommits == std::vector<std::string>{"你好"},
                "completed composition exposes its bounded committed-text session context");
        tipe::State nextComposition({}, cleanPreferencePath);
        nextComposition.restoreSessionContext(std::move(sessionContext));
        type(nextComposition, "shijie");
        const auto request = nextComposition.modelRequestSnapshot();
        require(request.find("context\t你好\n") != std::string::npos,
                "a replacement composition state restores prior committed text for the next model request");
        require(request.find("context_features\tv1:ae29dad40e284cf7f2329b78cb420481\n") !=
                    std::string::npos,
                "C++ model requests use the same stable opaque context feature as offline export");
        require(request.find("correction_events\tletter:n") != std::string::npos &&
                    request.find("candidate-selected:你好") != std::string::npos,
                "a replacement composition state restores the bounded cross-composition correction trail");
    }
    {
        const auto completed = basicCommitState.completedSupervisionRequest("Alacritty", "前文", "后文");
        require(completed.has_value(), "full candidate commit preserves a terminal supervision request");
        require(completed->preedit == "nihao" && completed->candidateCount > 0,
                "terminal supervision request keeps the committed preedit and candidate list");
        require(completed->payload.find("preedit\tnihao\n") != std::string::npos &&
                    completed->payload.find("selected_candidate\t0\t你好\n") != std::string::npos &&
                    completed->payload.find("candidate-selected:你好") != std::string::npos,
                "terminal supervision request pairs the confirmed candidate with its original preedit");
        require(completed->payload.find("surrounding_before\t前文\n") != std::string::npos &&
                    completed->payload.find("surrounding_after\t后文\n") != std::string::npos,
                "terminal supervision request keeps commit-time surrounding context");
        require(completed->payload.find(
                    "surrounding_features\tbefore:v1:d2b3ee84ac59e5cdc564f1383eb8ee70"
                    "\tafter:v1:66a1dbc4b3e6e762a125e6892191654e\n") != std::string::npos,
                "terminal supervision includes opaque surrounding-context features for private offline export");
    }
    action = basicCommitState.cursorMove("Down");
    require(!action.accepted && action.type == tipe::ActionType::None, "post-commit cursor key should pass through");
    {
        const auto postCommitPassThrough = basicCommitState.modelRequestSnapshot();
        require(postCommitPassThrough.find("supervision_state\tmode\tpass-through-only") != std::string::npos,
                "post-commit pass-through snapshot is empty-preedit supervision");
        require(postCommitPassThrough.find("\nevents\tcursor-move:Down\n") != std::string::npos,
                "post-commit pass-through snapshot keeps only the current key event row");
        require(postCommitPassThrough.find("\nevent_counts\tcursor-move:1\n") != std::string::npos,
                "post-commit pass-through snapshot keeps the current key event");
        require(postCommitPassThrough.find("\nevents\tletter:") == std::string::npos,
                "post-commit pass-through snapshot does not inherit previous preedit letters");
        require(postCommitPassThrough.find("\nevents\tcandidate-selected:") == std::string::npos,
                "post-commit pass-through snapshot does not inherit previous candidate selection");
    }

    type(basicCommitState, "shijie");
    action = basicCommitState.select(0);
    require(action.accepted && action.commitText == "世界", "digit 1 commits first candidate");
    require(basicCommitState.empty(), "candidate selection clears state");

    {
        const auto terminalSelectionPreferencePath = preferenceBase.string() + "-terminal-selection.tsv";
        std::filesystem::remove(terminalSelectionPreferencePath);
        tipe::State directSelectionState({}, terminalSelectionPreferencePath);
        type(directSelectionState, "nihao");
        const auto selectedIndex = candidateIndex(directSelectionState, "你号");
        action = directSelectionState.select(selectedIndex);
        require(action.accepted && directSelectionState.empty(),
                "direct non-leading candidate selection completes composition");
        const auto completed = directSelectionState.completedSupervisionRequest();
        require(completed && completed->payload.find("selected_candidate\t" + std::to_string(selectedIndex) +
                                                     "\t你号\n") != std::string::npos,
                "terminal supervision uses the directly selected candidate index instead of the old highlight");
        std::filesystem::remove(terminalSelectionPreferencePath);
    }

    {
        tipe::State punctuationState({}, cleanPreferencePath);
        type(punctuationState, "nihao");
        action = punctuationState.punctuation(",");
        require(!action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "你好",
                "punctuation commits current candidate while leaving punctuation key unaccepted");
        require(punctuationState.empty(), "punctuation commit clears state");
    }

    {
        tipe::State prefixPunctuationState({}, cleanPreferencePath);
        type(prefixPunctuationState, "jixuzuo");
        action = prefixPunctuationState.moveCandidateCursorTo(candidateIndex(prefixPunctuationState, "继续"), "End");
        require(action.accepted, "punctuation prefix setup can highlight a prefix candidate");
        action = prefixPunctuationState.punctuation(",");
        require(!action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "继续",
                "punctuation commits highlighted prefix candidate while leaving punctuation key unaccepted");
        require(action.keepPreedit && prefixPunctuationState.preedit() == "zuo",
                "punctuation keeps remaining pinyin after committing a prefix candidate");
    }

    {
        tipe::State passthroughState({}, cleanPreferencePath);
        type(passthroughState, "nihao");
        action = passthroughState.commitCurrentPassthrough("7");
        require(action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "你好" &&
                    action.passthroughText == "7",
                "unmapped digit commits the current candidate and requests deterministic trailing text");
        require(passthroughState.empty(), "unmapped digit commit clears state");
        require(hasEventText(passthroughState, tipe::InputEventType::ObservedKey, "7"),
                "unmapped digit passthrough is observed for learning");
    }

    {
        tipe::State uppercaseRawState({}, cleanPreferencePath);
        type(uppercaseRawState, "nihao");
        action = uppercaseRawState.inputUppercaseAscii('A');
        require(action.accepted && action.type == tipe::ActionType::Update && action.commitText.empty() &&
                    action.passthroughText.empty(),
                "uppercase input keeps an active composition instead of committing Chinese");
        require(uppercaseRawState.preedit() == "nihaoA" && uppercaseRawState.rawPreeditMode() &&
                    !uppercaseRawState.candidates().empty() && uppercaseRawState.candidates().front() == "nihaoA" &&
                    uppercaseRawState.candidateSource(0) == "raw",
                "uppercase input converts the active composition into a case-preserving raw candidate");
        action = uppercaseRawState.inputAscii('b');
        require(action.accepted && uppercaseRawState.preedit() == "nihaoAb" && uppercaseRawState.rawPreeditMode() &&
                    uppercaseRawState.candidates().front() == "nihaoAb",
                "lowercase input continues a mixed-case raw composition without losing case");
        action = uppercaseRawState.space();
        require(action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "nihaoAb" &&
                    action.passthroughText.empty() && uppercaseRawState.empty(),
                "space commits the mixed-case raw candidate once without an extra passthrough letter");

        tipe::State emptyUppercaseState({}, cleanPreferencePath);
        action = emptyUppercaseState.inputUppercaseAscii('A');
        require(!action.accepted && action.type == tipe::ActionType::None && emptyUppercaseState.empty(),
                "uppercase input with no composition remains a normal application passthrough");
        require(hasEventText(emptyUppercaseState, tipe::InputEventType::Letter, "A"),
                "passthrough uppercase input remains available to supervision");
    }

    {
        tipe::State englishModeBoundaryState({}, cleanPreferencePath);
        type(englishModeBoundaryState, "nihao");
        action = englishModeBoundaryState.commitRawPreedit("InputModeEnglish");
        require(!action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "nihao",
                "the first English text key commits pending pinyin as raw text");
        require(englishModeBoundaryState.empty(), "raw mode-switch commit clears the completed composition");
        require(hasEventText(englishModeBoundaryState, tipe::InputEventType::ObservedKey, "InputModeEnglish"),
                "raw mode-switch commit records an explicit supervision boundary");
        require(hasEventText(englishModeBoundaryState, tipe::InputEventType::RawCommitted, "nihao"),
                "raw mode-switch commit remains available to learning");
    }

    type(basicCommitState, "nihao");
    action = basicCommitState.select(candidateIndex(basicCommitState, "你号"));
    require(action.accepted && action.commitText == "你号", "select commits requested candidate");

    type(basicCommitState, "nihao");
    action = basicCommitState.select(candidateIndex(basicCommitState, "你"));
    require(action.accepted && action.commitText == "你", "single-character selection commits the first syllable");
    require(basicCommitState.preedit() == "hao", "single-character selection keeps remaining pinyin");
    require(!basicCommitState.candidates().empty() && basicCommitState.candidates()[0] == "好",
            "recent context promotes natural continuation after ni");
    require(candidateIndex(basicCommitState, "好") < basicCommitState.candidates().size(),
            "remaining pinyin refreshes candidates");
    action = basicCommitState.select(candidateIndex(basicCommitState, "好"));
    require(action.accepted && action.commitText == "好", "remaining pinyin candidate can be committed next");
    basicCommitState.reset();

    {
        tipe::State restoreState({}, mainPreferencePath);
        type(restoreState, "nihao");
        action = restoreState.select(candidateIndex(restoreState, "你"));
        require(action.accepted && restoreState.preedit() == "hao",
                "partial commit leaves hao active for key boundary tests");
        action = restoreState.cursorMove("Down");
        require(action.accepted && action.type == tipe::ActionType::None && restoreState.preedit() == "hao",
                "cursor movement after partial commit keeps remaining pinyin active");
        action = restoreState.deleteKey();
        require(action.accepted && action.type == tipe::ActionType::Update && restoreState.preedit() == "nihao",
                "delete after a partial commit restores the original pinyin");
        require(restoreState.recentCommits().empty(),
                "restoring a partial commit removes the undone text from live context");
        require(restoreState.modelRequestSnapshot().find("pending_segment\t") == std::string::npos,
                "restoring a partial commit clears stale pending segment supervision");
        action = restoreState.backspace();
        require(action.accepted && restoreState.preedit() == "niha",
                "backspace after restored partial commit edits restored pinyin");
        action = restoreState.escape();
        require(action.accepted && action.type == tipe::ActionType::Clear && restoreState.empty(),
                "escape after partial commit clears remaining pinyin explicitly");
    }

    {
        tipe::State restoreState({}, mainPreferencePath);
        type(restoreState, "haodewokanyixiahaiyoumeiyu");
        action = restoreState.select(candidateIndex(restoreState, "好的我看一下"));
        require(action.accepted && action.keepPreedit && restoreState.preedit() == "haiyoumeiyu",
                "long prefix commit leaves remaining pinyin before undo");
        require(restoreState.modelRequestSnapshot().find("pending_segment\thaodewokanyixiahaiyoumeiyu") !=
                    std::string::npos,
                "long prefix commit exposes pending segment before undo");
        action = restoreState.deleteKey();
        require(action.accepted && restoreState.preedit() == "haodewokanyixiahaiyoumeiyu",
                "Delete after a long prefix commit restores the full original pinyin");
        require(!restoreState.candidates().empty() && restoreState.candidates()[0] == "好的我看一下还有美誉",
                "undoing a long prefix commit refreshes full-preedit candidates");
        require(restoreState.recentCommits().empty(),
                "undoing a long prefix commit removes the undone text from live supervision context");
        require(restoreState.modelRequestSnapshot().find("context\t0\t好的我看一下") == std::string::npos,
                "undoing a long prefix commit removes the stale context row from model requests");
        require(restoreState.modelRequestSnapshot().find("pending_segment\t") == std::string::npos,
                "undoing a long prefix commit clears stale pending segment supervision");
    }

    type(state, "zhexie");
    action = state.select(candidateIndex(state, "这"));
    require(action.accepted && action.commitText == "这", "zhe-xie can commit the first character");
    require(state.preedit() == "xie", "zhe-xie keeps remaining pinyin after first-character commit");
    action = state.select(candidateIndex(state, "些"));
    require(action.accepted && action.commitText == "些", "zhe-xie can commit the remaining character");
    state.reset();

    type(state, "shihou");
    action = state.select(candidateIndex(state, "是"));
    require(action.accepted && action.commitText == "是", "shi-hou uses shi as the first syllable");
    require(state.preedit() == "hou", "shi-hou keeps remaining pinyin after first-character commit");
    action = state.select(candidateIndex(state, "后"));
    require(action.accepted && action.commitText == "后", "shi-hou can commit the remaining character");
    state.reset();

    type(state, "dawan");
    action = state.select(candidateIndex(state, "打"));
    require(action.accepted && action.commitText == "打", "da-wan can commit the first character");
    require(state.preedit() == "wan", "da-wan keeps remaining pinyin after first-character commit");
    action = state.select(candidateIndex(state, "完"));
    require(action.accepted && action.commitText == "完", "da-wan can commit the remaining character");
    state.reset();

    type(state, "ceshi");
    action = state.select(state.candidates().size());
    require(action.accepted && action.type == tipe::ActionType::None, "out-of-range candidate selection is consumed");
    require(state.preedit() == "ceshi", "out-of-range candidate selection keeps preedit");
    state.reset();

    type(state, "zhongguo");
    action = state.enter();
    require(action.accepted && action.commitText == "zhongguo", "enter commits raw pinyin");
    require(hasEvent(state, tipe::InputEventType::RawCommitted), "raw commit is recorded");
    type(state, "zhongguo");
    require(!state.candidates().empty() && state.candidates()[0] == "中国",
            "explicit raw commit does not override known Chinese candidates");
    action = state.space();
    require(action.accepted && action.commitText == "中国", "space commits Chinese candidate after raw commit");
    type(state, "zhongguo");
    action = state.select(candidateIndex(state, "中国"));
    require(action.accepted && action.commitText == "中国", "selecting Chinese candidate after raw preference is still possible");
    type(state, "zhongguo");
    require(!state.candidates().empty() && state.candidates()[0] == "中国", "strong Chinese selection can override raw preference");
    state.reset();

    type(state, "ceshi");
    action = state.backspace();
    require(action.accepted && state.preedit() == "cesh", "backspace removes one byte ascii preedit");
    require(!state.candidates().empty() && state.candidates()[0] == "测试", "backspace refreshes prefix candidates after preedit changes");
    action = state.deleteKey();
    require(action.accepted && action.type == tipe::ActionType::None, "delete is recorded and consumed while composing");
    action = state.observeKey();
    require(!action.accepted && action.type == tipe::ActionType::None, "observed key is recorded but not consumed");
    action = state.observeKey("Tab");
    require(!action.accepted && hasEventText(state, tipe::InputEventType::ObservedKey, "Tab"),
            "named observed key is recorded but not consumed");
    action = state.escape();
    require(action.accepted && action.type == tipe::ActionType::Clear && state.empty(), "escape clears preedit");

    type(state, "nihao");
    action = state.cursorMove("Left");
    require(action.accepted && state.preeditCursorIndex() == 4, "left arrow moves the pinyin edit cursor left");
    action = state.cursorMove("Left");
    require(action.accepted && state.preeditCursorIndex() == 3, "left arrow can move inside the pinyin preedit");
    action = state.inputAscii('x');
    require(action.accepted && state.preedit() == "nihxao" && state.preeditCursorIndex() == 4,
            "typing after moving left inserts at the pinyin edit cursor");
    action = state.deleteKey();
    require(action.accepted && state.preedit() == "nihxo" && state.preeditCursorIndex() == 4,
            "delete removes the character after the pinyin edit cursor");
    action = state.backspace();
    require(action.accepted && state.preedit() == "niho" && state.preeditCursorIndex() == 3,
            "backspace removes the character before the pinyin edit cursor");
    action = state.cursorMove("Right");
    require(action.accepted && state.preeditCursorIndex() == 4, "right arrow moves the pinyin edit cursor right");
    state.reset();

    type(state, "xiangzuodehenchang");
    action = state.select(candidateIndex(state, "想"));
    require(action.accepted && action.commitText == "想", "long phrase can commit a short prefix candidate");
    require(state.preedit() == "zuodehenchang", "short prefix selection keeps the long remaining pinyin");
    state.reset();

    type(state, "woxiangyoabc");
    action = state.select(candidateIndex(state, "我"));
    require(action.accepted && action.commitText == "我",
            "long pinyin-like text with a noisy tail can commit a short prefix candidate");
    require(state.preedit() == "xiangyoabc",
            "short prefix selection keeps the remaining noisy pinyin-like tail");
    state.reset();

    type(state, "jibengongneg");
    require(!state.candidates().empty() && state.candidates()[0] == "基本功能",
            "local full-word typo repair outranks the noisy jibengongneg segmentation");
    action = state.space();
    require(action.accepted && action.commitText == "基本功能",
            "space commits the corrected full first candidate for jibengongneg");
    require(state.empty(), "full first-candidate commit must clear typo-like preedit");
    type(state, "jibengongneg");
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens typo correction candidates");
    require(!state.candidates().empty() && state.candidates()[0] == "基本功能",
            "explicit local rerank keeps the plausible typo correction candidate first");
    action = state.select(candidateIndex(state, "基本功能"));
    require(action.accepted && action.commitText == "基本功能",
            "selecting the explicit local correction candidate commits it");
    require(state.empty(), "explicit local correction commit clears the original typo preedit");
    type(state, "shurufa");
    const auto shurufaLeadingBeforeRerank = state.candidates().front();
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank expands a valid complete pinyin word");
    require(!state.candidates().empty() && state.candidates().front() == shurufaLeadingBeforeRerank &&
                state.candidates().front() == "输入法",
            "speculative suffix completions cannot replace a valid complete-pinyin first candidate");
    state.reset();
    type(state, "shurufang");
    const auto shurufangLeadingBeforeRerank = state.candidates().front();
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens suffix typo correction candidates");
    require(!state.candidates().empty() && state.candidates().front() == shurufangLeadingBeforeRerank &&
                std::find(state.candidates().begin(), state.candidates().end(), "输入法") != state.candidates().end(),
            "explicit local rerank offers a trailing-ng repair without replacing the dictionary first candidate");
    action = state.select(candidateIndex(state, "输入法"));
    require(action.accepted && action.commitText == "输入法",
            "selecting commits the suffix typo correction candidate");
    require(state.empty(), "suffix typo correction commit clears the original typo preedit");
    type(state, "woxiangyo");
    const auto woxiangyoLeadingBeforeRerank = state.candidates().front();
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens omitted-ng correction candidates");
    require(!state.candidates().empty() && state.candidates().front() == woxiangyoLeadingBeforeRerank &&
                std::find(state.candidates().begin(), state.candidates().end(), "我想用") != state.candidates().end(),
            "explicit local rerank offers a valid-pinyin omitted-ng repair without overriding the original first choice");
    action = state.select(candidateIndex(state, "我想用"));
    require(action.accepted && action.commitText == "我想用",
            "selecting the omitted-ng correction candidate commits it");
    require(state.empty(), "omitted-ng correction commit clears the original typo preedit");
    type(state, "ihao");
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens missing-letter correction candidates");
    require(std::find(state.candidates().begin(), state.candidates().end(), "你好") != state.candidates().end(),
            "explicit local rerank offers a missing initial letter repair");
    action = state.select(candidateIndex(state, "你好"));
    require(action.accepted && action.commitText == "你好",
            "selecting the missing-letter correction candidate commits it");
    require(state.empty(), "missing-letter correction commit clears the original typo preedit");
    type(state, "nhao");
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens valid-pinyin omission correction candidates");
    require(std::find(state.candidates().begin(), state.candidates().end(), "你好") != state.candidates().end(),
            "explicit local rerank offers a valid-pinyin missing-letter repair");
    action = state.select(candidateIndex(state, "你好"));
    require(action.accepted && action.commitText == "你好",
            "selecting the valid-pinyin omission correction candidate commits it");
    require(state.empty(), "valid-pinyin omission correction commit clears the original typo preedit");
    type(state, "nhihao");
    action = state.rerankCandidates();
    require(action.accepted && state.candidatesExpanded(), "explicit rerank opens extra-letter correction candidates");
    require(std::find(state.candidates().begin(), state.candidates().end(), "你好") != state.candidates().end(),
            "explicit local rerank offers an accidental extra-letter repair");
    action = state.select(candidateIndex(state, "你好"));
    require(action.accepted && action.commitText == "你好",
            "selecting the extra-letter correction candidate commits it");
    require(state.empty(), "extra-letter correction commit clears the original typo preedit");

    {
        const auto swallowDictionaryPath = preferenceBase.string() + "-swallow-dictionary.tsv";
        const auto swallowPreferencePath = preferenceBase.string() + "-swallow-preferences.tsv";
        std::filesystem::remove(swallowDictionaryPath);
        std::filesystem::remove(swallowPreferencePath);
        std::ofstream swallowDictionary(swallowDictionaryPath);
        swallowDictionary << "haishicunzai\t短\n";
        swallowDictionary << "qqhaishicunzai\t短\n";
        swallowDictionary << "shdfjshdkjfa\t占位完整候选\t水电费\n";
        swallowDictionary.close();
        setenv("TIPE_USER_DICTIONARY", swallowDictionaryPath.c_str(), 1);
        tipe::State swallowState({}, swallowPreferencePath);
        type(swallowState, "haishicunzai");
        action = swallowState.select(candidateIndex(swallowState, "短"));
        require(action.accepted && action.commitText == "短", "non-prefix short candidate can still be committed");
        require(!swallowState.preedit().empty(),
                "non-prefix short candidate must not swallow the remaining long pinyin");
        require(swallowState.preedit().size() < std::string("haishicunzai").size(),
                "non-prefix short candidate should consume a prefix-sized pinyin chunk");
        swallowState.reset();
        type(swallowState, "nihaoceshi");
        action = swallowState.select(candidateIndex(swallowState, "你好"));
        require(action.accepted && action.commitText == "你好",
                "explicitly selecting a leading phrase from a long sentence commits only that phrase");
        require(swallowState.preedit() == "ceshi",
                "explicit leading phrase selection keeps the remaining sentence pinyin");
        swallowState.reset();
        type(swallowState, "jiuhuixiaoshi");
        action = swallowState.moveCollapsedCandidateCursor(1, "Right");
        require(action.accepted && swallowState.candidateCursorIndex() == candidateIndex(swallowState, "就会"),
                "moving highlight to a shorter prefix phrase succeeds");
        action = swallowState.space();
        require(action.accepted && action.commitText == "就会",
                "space commits the highlighted prefix phrase");
        require(swallowState.preedit() == "xiaoshi",
                "highlighted prefix phrase consumes its source pinyin and keeps the remaining sentence");
        action = swallowState.select(candidateIndex(swallowState, "消失"));
        require(action.accepted && action.commitText == "消失" && swallowState.empty(),
                "selecting the remaining phrase after a prefix commit finishes the sentence");
        swallowState.reset();
        type(swallowState, "woxxx");
        action = swallowState.select(candidateIndex(swallowState, "我"));
        require(action.accepted && action.commitText == "我",
                "explicitly selecting a single-character prefix from a noisy long preedit commits it");
        require(swallowState.preedit() == "xxx",
                "single-character prefix selection keeps even a noisy unresolved tail");
        swallowState.reset();
        type(swallowState, "shdfjshdkjfa");
        action = swallowState.moveCandidateCursorTo(candidateIndex(swallowState, "水电费"));
        require(action.accepted, "moving highlight to a short noisy candidate succeeds");
        action = swallowState.space();
        require(action.accepted && action.commitText == "水电费",
                "space commits the highlighted short candidate");
        require(swallowState.preedit() == "jshdkjfa",
                "space on a highlighted short candidate consumes its approximate prefix and keeps the unresolved tail");
        swallowState.reset();
        type(swallowState, "qqhaishicunzai");
        action = swallowState.select(candidateIndex(swallowState, "短"));
        require(action.accepted && action.commitText == "短",
                "random non-pinyin leading text can still commit a short candidate");
        require(swallowState.empty(),
                "unresolved short selection must not leave unchanged stale pinyin that repeats the same candidate");
        swallowState.reset();
        type(swallowState, "qqhaishicunzai");
        action = swallowState.space();
        require(action.accepted && action.commitText == "短",
                "space can still commit an unresolved first candidate");
        require(swallowState.empty(),
                "space on an unresolved first candidate clears composing text instead of leaving stale pinyin");
        setenv("TIPE_USER_DICTIONARY", isolatedDefaultUserDictionaryPath.c_str(), 1);
        std::filesystem::remove(swallowDictionaryPath);
        std::filesystem::remove(swallowPreferencePath);
    }

    type(state, "unknown");
    action = state.space();
    require(action.accepted && action.commitText == "unknown", "space commits raw pinyin without candidates");

    type(state, "naninaninaninaninaninaninaninaninaninaninaninaninani");
    action = state.backspace();
    require(action.accepted, "very long ambiguous pinyin can still be edited without expensive splitting");
    action = state.enter();
    require(action.accepted && state.empty(), "very long ambiguous pinyin can commit raw text");

    type(state, "github");
    require(!state.candidates().empty() && state.candidates()[0] == "github", "english-like raw candidate is shown first");
    action = state.space();
    require(action.accepted && action.commitText == "github", "english-like input commits raw text ahead of candidates");

    type(state, "dgithub");
    require(!state.candidates().empty() && state.candidates()[0] == "打github",
            "mixed Chinese-English pinyin is not forced to raw english by github signal");
    state.reset();

    type(state, "githubdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "github的时候",
            "raw english token can be composed with a Chinese suffix");
    state.reset();

    type(state, "chatgptdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "chatgpt的时候",
            "extended raw english token can be composed with a Chinese suffix");
    require(candidateIndex(state, "chatgpt") < state.candidates().size(),
            "extended raw english token phrase exposes the raw token prefix");
    action = state.select(candidateIndex(state, "chatgpt"));
    require(action.accepted && action.commitText == "chatgpt",
            "extended raw english token prefix can be committed");
    require(state.preedit() == "deshihou",
            "extended raw english token prefix commit keeps the Chinese suffix pinyin");
    state.reset();

    type(state, "ollamadeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "ollama的时候",
            "local model raw english token can be composed with a Chinese suffix");
    state.reset();

    type(state, "dgithubdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "打github的时候",
            "Chinese prefix plus raw english token can be composed with a Chinese suffix");
    state.reset();

    type(state, "gitdeshihou");
    require(!state.candidates().empty() && state.candidates()[0] == "git的时候",
            "short raw git token can be composed with a Chinese suffix");
    state.reset();

    type(state, "cargobuild");
    require(!state.candidates().empty() && state.candidates()[0] == "cargobuild",
            "adjacent cargo/build letters stay raw instead of becoming awkward transliteration");
    state.reset();

    type(state, "cmakebuild");
    require(!state.candidates().empty() && state.candidates()[0] == "cmakebuild",
            "adjacent cmake/build letters stay raw instead of becoming awkward transliteration");
    state.reset();

    type(state, "docker");
    require(!state.candidates().empty() && state.candidates()[0] == "docker", "docker raw candidate is shown first");
    action = state.space();
    require(action.accepted && action.commitText == "docker", "docker commits raw text ahead of transliteration");

    for (const auto *englishToken : {"vscode", "cursor", "python", "wayland", "chatgpt", "javascript", "niri",
                                     "ollama", "waybar", "hyprland", "systemd", "gnome", "dbus", "git",
                                     "npm", "node", "rust", "cargo", "cmake", "build", "cmakebuild", "tipe"}) {
        type(state, englishToken);
        require(!state.candidates().empty() && state.candidates()[0] == englishToken,
                "known developer english token is shown as raw text first");
        action = state.space();
        require(action.accepted && action.commitText == englishToken,
                "known developer english token commits raw text ahead of awkward transliteration");
    }

    for (const auto &[prefix, digit, token] :
         {std::tuple{"qwen", '2', "qwen2"}, std::tuple{"gpt", '4', "gpt4"}, std::tuple{"ipv", '6', "ipv6"}}) {
        type(state, prefix);
        action = state.inputAsciiDigit(digit);
        require(action.accepted && state.preedit() == token,
                "known alphanumeric developer English token consumes its digit key");
        require(hasEventText(state, tipe::InputEventType::Digit, std::string(1, digit)),
                "known alphanumeric developer English token records the digit semantically");
        require(!state.candidates().empty() && state.candidates()[0] == token,
                "known alphanumeric developer English token is shown as raw text first");
        action = state.space();
        require(action.accepted && action.commitText == token,
                "known alphanumeric developer English token commits raw text");
    }

    {
        const auto learnedRawPreferencePath = preferenceBase.string() + "-learned-raw.tsv";
        std::filesystem::remove(learnedRawPreferencePath);
        tipe::State rawTrainer({}, learnedRawPreferencePath);
        type(rawTrainer, "ceshi");
        action = rawTrainer.enter();
        require(action.accepted && action.commitText == "ceshi", "enter can train an intentional raw commit");
        type(rawTrainer, "ceshi");
        require(!rawTrainer.candidates().empty() && rawTrainer.candidates()[0] == "测试",
                "one raw commit should not make normal pinyin prefer raw text");
        action = rawTrainer.enter();
        require(action.accepted && action.commitText == "ceshi", "second intentional raw commit is recorded");
        require(!fileContains(learnedRawPreferencePath, "ceshi\tceshi\t"),
                "normal pinyin raw commits remain supervision evidence instead of persistent raw-English preference");

        tipe::State rawRestored({}, learnedRawPreferencePath);
        type(rawRestored, "ceshi");
        require(!rawRestored.candidates().empty() && rawRestored.candidates()[0] == "测试",
                "repeated raw commits do not make normal pinyin prefer raw text");
        action = rawRestored.space();
        require(action.accepted && action.commitText == "测试",
                "normal pinyin keeps committing the Chinese candidate after raw learning");

        type(rawRestored, "ceshi");
        action = rawRestored.select(candidateIndex(rawRestored, "测试"));
        require(action.accepted && action.commitText == "测试",
                "explicit Chinese selection can push back against learned raw preference");
        type(rawRestored, "ceshi");
        action = rawRestored.select(candidateIndex(rawRestored, "测试"));
        require(action.accepted && action.commitText == "测试",
                "repeated Chinese selection outweighs learned raw preference");

        tipe::State chineseRestored({}, learnedRawPreferencePath);
        type(chineseRestored, "ceshi");
        require(!chineseRestored.candidates().empty() && chineseRestored.candidates()[0] == "测试",
                "candidate preference can recover normal Chinese ordering after raw learning");
        std::filesystem::remove(learnedRawPreferencePath);
    }

    {
        tipe::State correctionState({}, cleanPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(correctionState, "ihao");
            eraseAll(correctionState);
            type(correctionState, "nihao");
            action = correctionState.select(candidateIndex(correctionState, "你好"));
            require(action.accepted && action.commitText == "你好", "corrected nihao commit teaches omission pattern");
        }
        type(correctionState, "ihao");
        require(!correctionState.candidates().empty() && correctionState.candidates()[0] == "你好",
                "repeated omission correction lets ihao borrow nihao candidates");
        action = correctionState.select(candidateIndex(correctionState, "你好"));
        require(action.accepted && action.commitText == "你好" && correctionState.empty(),
                "selecting a learned correction candidate commits it as a full correction and clears typo preedit");
        type(correctionState, "ihao");
        action = correctionState.inputAsciiDigit('2');
        require(!action.accepted && correctionState.preedit() == "ihao",
                "learned correction candidates prevent typo preedit from becoming raw ihao2");
    }
    {
        tipe::State genericCorrectionState({}, genericCorrectionPreferencePath);
        type(genericCorrectionState, "cesi");
        require(!genericCorrectionState.candidates().empty() && genericCorrectionState.candidates()[0] != "测试",
                "untrained typo keeps its own default candidates before correction learning");
        for (int attempt = 0; attempt < 2; ++attempt) {
            eraseAll(genericCorrectionState);
            type(genericCorrectionState, "cesi");
            eraseAll(genericCorrectionState);
            type(genericCorrectionState, "ceshi");
            action = genericCorrectionState.select(candidateIndex(genericCorrectionState, "测试"));
            require(action.accepted && action.commitText == "测试",
                    "corrected ceshi commit teaches a generic missing-letter pattern");
        }
        type(genericCorrectionState, "cesi");
        require(!genericCorrectionState.candidates().empty() && genericCorrectionState.candidates()[0] == "测试",
                "generic repeated correction lets cesi borrow ceshi candidates");
    }
    {
        {
            std::ofstream preferences(correctionConflictPreferencePath);
            preferences << "__correction__\tong\tnong\t3\n";
            preferences << "__correction__\tong\tgong\t2\n";
        }
        tipe::InputModel uniqueCorrectionModel(correctionConflictPreferencePath);
        const auto uniqueCorrections = uniqueCorrectionModel.learnedCorrections("ong");
        require(uniqueCorrectionModel.hasExactLearnedCorrection("ong") && uniqueCorrections.size() == 1 &&
                    uniqueCorrections.front() == "nong",
                "a unique strongest exact correction is the only automatic correction");

        {
            std::ofstream preferences(correctionConflictPreferencePath, std::ios::trunc);
            preferences << "__correction__\tong\tnong\t3\n";
            preferences << "__correction__\tong\tgong\t3\n";
        }
        tipe::InputModel tiedCorrectionModel(correctionConflictPreferencePath);
        require(!tiedCorrectionModel.hasExactLearnedCorrection("ong") &&
                    tiedCorrectionModel.learnedCorrections("ong").empty(),
                "tied exact correction evidence stays ambiguous instead of guessing automatically");
    }
    {
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath);
            generalizedPreferences << "__correction__\tihao\tnihao\t1\n";
        }
        tipe::InputModel singleObservationModel(generalizedPatternPreferencePath);
        require(singleObservationModel.learnedCorrections("iren").empty(),
                "one missing-key observation stays inactive in the realtime path");

        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath, std::ios::trunc);
            generalizedPreferences << "__correction__\tihao\tnihao\t1\n";
            generalizedPreferences << "__correction__\timen\tnimen\t1\n";
        }
        tipe::InputModel aggregatedPatternModel(generalizedPatternPreferencePath);
        const auto aggregatedCorrections = aggregatedPatternModel.learnedCorrections("iren");
        require(aggregatedCorrections.size() == 1 && aggregatedCorrections.front() == "niren",
                "separate missing-key observations aggregate into one reusable realtime pattern");
    }
    {
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath);
            generalizedPreferences << "__correction__\tihao\tnihao\t2\n";
        }
        tipe::State generalizedPatternState({}, generalizedPatternPreferencePath);
        type(generalizedPatternState, "engli");
        require(!generalizedPatternState.candidates().empty() && generalizedPatternState.candidates()[0] == "能力",
                "learned missing-letter patterns generalize to longer typo candidates without an external model click");
        generalizedPatternState.reset();
        type(generalizedPatternState, "xuesh");
        require(!generalizedPatternState.candidates().empty() &&
                    (generalizedPatternState.candidates()[0] == "学生" ||
                     generalizedPatternState.candidates()[0] == "学士"),
                "normal local completion keeps a coherent high-frequency xuesh candidate first");
        require(std::find(generalizedPatternState.candidates().begin(), generalizedPatternState.candidates().end(),
                          "年学生") == generalizedPatternState.candidates().end(),
                "absolute missing-letter patterns do not invent unrelated leading corrections before suffix learning");
    }
    {
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath);
            generalizedPreferences << "__correction__\tcesh\tceshi\t2\n";
        }
        tipe::State suffixPatternState({}, generalizedPatternPreferencePath);
        type(suffixPatternState, "xuesh");
        require(!suffixPatternState.candidates().empty() && suffixPatternState.candidates()[0] == "学士",
                "suffix-relative missing-letter patterns generalize to same-ending typo candidates");
        suffixPatternState.reset();
        type(suffixPatternState, "ong");
        require(!suffixPatternState.candidates().empty() && suffixPatternState.candidates()[0] != "弄",
                "suffix-relative patterns do not behave like unrelated leading-letter corrections");
        suffixPatternState.reset();
        type(suffixPatternState, "haodewokanyxiahaiyoumeiyu");
        require(!suffixPatternState.candidates().empty() &&
                    suffixPatternState.candidates()[0] == "好的我看一下还有美誉",
                "suffix-relative typo patterns do not pollute long sentence candidates");
    }
    {
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath);
            generalizedPreferences << "__correction__\tgongnegn\tgongneng\t1\n";
        }
        tipe::InputModel inactiveTransposeModel(generalizedPatternPreferencePath);
        require(inactiveTransposeModel.learnedCorrections("jibengongnegn").empty(),
                "one adjacent transposition observation stays inactive in the realtime path");
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath, std::ios::trunc);
            generalizedPreferences << "__correction__\tgongnegn\tgongneng\t2\n";
        }
        tipe::InputModel transposePatternModel(generalizedPatternPreferencePath);
        const auto transposeCorrections = transposePatternModel.learnedCorrections("jibengongnegn");
        require(transposeCorrections.size() == 1 && transposeCorrections.front() == "jibengongneng",
                "repeated adjacent transposition learns a suffix-relative realtime correction pattern");
        require(transposePatternModel.learnedCorrections("stringn").empty(),
                "learned adjacent transposition does not rewrite an English-like identifier");

        tipe::State transposePatternState({}, generalizedPatternPreferencePath);
        type(transposePatternState, "jibengongnegn");
        require(!transposePatternState.candidates().empty() && transposePatternState.candidates()[0] == "基本功能",
                "learned adjacent transposition reaches realtime candidates without an external model click");
    }
    {
        {
            std::ofstream generalizedPreferences(generalizedPatternPreferencePath);
            generalizedPreferences << "__correction__\tihao\tnihao\t2\n";
        }
        tipe::State generalizedPatternState({}, generalizedPatternPreferencePath);
        type(generalizedPatternState, "haodewokanyxiahaiyoumeiyu");
        require(!generalizedPatternState.candidates().empty() &&
                    generalizedPatternState.candidates()[0] == "好的我看一下还有美誉",
                "realtime generalized typo patterns do not pollute long sentence candidates");
        generalizedPatternState.reset();
        type(generalizedPatternState, "react");
        require(!generalizedPatternState.candidates().empty() && generalizedPatternState.candidates()[0] == "react",
                "realtime generalized typo patterns do not pollute known raw English tokens");
    }
    {
        {
            std::ofstream distilledPreferences(generalizedPatternPreferencePath);
            distilledPreferences << "__correction_pattern__\tmissing\t\tn\t0\t0\t2\n";
            distilledPreferences << "__key_habit__\tmissing\t\tn\t6\n";
            distilledPreferences << "__key_habit__\tmissing\t\tg\t5\n";
            distilledPreferences << "__key_habit__\tmissing\t\ti\t5\n";
            distilledPreferences << "__key_habit__\tmissing\t\tu\t4\n";
            distilledPreferences << "__key_habit__\ttranspose\tgn\tng\t5\n";
            distilledPreferences << "__key_habit__\tmissing\tbad\tn\t999\n";
            distilledPreferences << "__correction_pattern__\tmissing\t\tn\t64\t0\t999\n";
        }
        tipe::InputModel distilledModel(generalizedPatternPreferencePath);
        const auto leadingHabitCorrections = distilledModel.learnedCorrections("ihao");
        require(std::find(leadingHabitCorrections.begin(), leadingHabitCorrections.end(), "nihao") !=
                    leadingHabitCorrections.end(),
                "distilled TiP missing-n evidence reaches an unseen normal-input correction");
        const auto movedHabitCorrections = distilledModel.learnedCorrections(
            "woxiangyog", [](std::string_view candidate) { return candidate == "woxiangyong"; });
        require(std::find(movedHabitCorrections.begin(), movedHabitCorrections.end(), "woxiangyong") !=
                    movedHabitCorrections.end(),
                "distilled TiP key habits apply at a new position instead of memorizing one typo pair");
        const auto twoEditCorrections = distilledModel.learnedCorrections(
            "woxiangyo", [](std::string_view candidate) { return candidate == "woxiangyong"; });
        require(std::find(twoEditCorrections.begin(), twoEditCorrections.end(), "woxiangyong") !=
                    twoEditCorrections.end(),
                "bounded TiP edit channel can combine two independently learned omissions");
        const auto parseableTwoEditCorrections = distilledModel.learnedCorrections(
            "woxiangyo", [](std::string_view candidate) {
                return tipe::isCompletePinyinSequence(candidate) ? 1 : 0;
            });
        require(!parseableTwoEditCorrections.empty() && parseableTwoEditCorrections.front() == "woxiangyong",
                "a long suffix repair outranks equally parseable habit combinations; first=" +
                    (parseableTwoEditCorrections.empty() ? std::string("<empty>")
                                                         : parseableTwoEditCorrections.front()));
        require(distilledModel.learnedCorrections("github").empty(),
                "distilled keyboard habits do not rewrite a known English token");
        distilledModel.recordCandidatePreference("nihao", "你好", 1);
        require(fileContains(generalizedPatternPreferencePath, "__key_habit__\tmissing\t\tn\t6") &&
                    fileContains(generalizedPatternPreferencePath,
                                 "__correction_pattern__\tmissing\t\tn\t0\t0\t2") &&
                    !fileContains(generalizedPatternPreferencePath, "__key_habit__\tmissing\tbad"),
                "runtime preference saves retain valid distilled habits and discard malformed rows");

        tipe::State distilledState({}, generalizedPatternPreferencePath);
        type(distilledState, "ihao");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "你好",
                "distilled TiP habits affect ordinary candidate generation without F9 or a model process");
        distilledState.reset();
        type(distilledState, "woxiangyo");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "我想用",
                "two strong TiP omission habits recover an unseen valid-pinyin typo during ordinary input");
        distilledState.reset();
        type(distilledState, "enggou");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "能够",
                "a known phrase outranks parseable noise when a missing-n habit moves to an unseen position");
        distilledState.reset();
        type(distilledState, "wanluo");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "网络",
                "a missing-g habit generalizes without appending an unrelated syllable");
        distilledState.reset();
        type(distilledState, "shjie");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "世界",
                "a missing-i habit prefers a known phrase over a suffix insertion");
        distilledState.reset();
        type(distilledState, "yigngai");
        require(!distilledState.candidates().empty() && distilledState.candidates()[0] == "应该",
                "an adjacent-transposition habit promotes the known corrected phrase");
    }
    {
        const auto segmentChainPreferencePath = preferenceBase.string() + "-segment-chain.tsv";
        std::filesystem::remove(segmentChainPreferencePath);
        tipe::State segmentChainState({}, segmentChainPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(segmentChainState, "woc");
            action = segmentChainState.select(candidateIndex(segmentChainState, "我"));
            require(action.accepted && action.commitText == "我" && segmentChainState.preedit() == "c",
                    "segment chain can commit the first character and keep the remaining typo");
            require(!segmentChainState.candidates().empty() && segmentChainState.candidates()[0] == "操",
                    "pending segment chain promotes the exact full-word suffix candidate");
            require(segmentChainState.modelRequestSnapshot().find("pending_segment\twoc\two\t我\tc") != std::string::npos,
                    "model requests expose the pending prefix selection before the suffix is confirmed");
            action = segmentChainState.select(candidateIndex(segmentChainState, "操"));
            require(action.accepted && action.commitText == "操",
                    "segment chain can commit the intended candidate from the remaining typo");
        }
        type(segmentChainState, "woc");
        require(!segmentChainState.candidates().empty() && segmentChainState.candidates()[0] == "我操",
                "repeated segmented selection teaches the original preedit as a full phrase correction");
        require(fileContains(segmentChainPreferencePath,
                             "__segment_chain__\twoc\two\t我\tc\twocao\t我操\t2"),
                "segment-selection chains are persisted as reusable supervised learning evidence");

        const auto storedSegmentChainScriptPath = preferenceBase.string() + "-stored-segment-chain.sh";
        {
            std::ofstream script(storedSegmentChainScriptPath);
            script << "#!/usr/bin/env bash\n";
            script << "set -euo pipefail\n";
            script << "input=$(cat)\n";
            script << "grep -q $'^segment_chain\\twoc\\two\\t我\\tc\\twocao\\t我操$' <<< \"$input\" || exit 1\n";
            script << "printf '%s\\n' $'candidate\\t我操'\n";
        }
        std::filesystem::permissions(storedSegmentChainScriptPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", storedSegmentChainScriptPath.c_str(), 1);
        tipe::State storedSegmentChainState({}, segmentChainPreferencePath);
        type(storedSegmentChainState, "woc");
        action = storedSegmentChainState.rerankCandidates();
        require(action.accepted && !storedSegmentChainState.candidates().empty() &&
                    storedSegmentChainState.candidates()[0] == "我操",
                "model requests include persisted segment-selection chains after a new state instance is created");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(storedSegmentChainScriptPath);

        tipe::State normalSegmentChainState({}, segmentChainPreferencePath);
        type(normalSegmentChainState, "jixuzuo");
        action = normalSegmentChainState.select(candidateIndex(normalSegmentChainState, "继续"));
        require(action.accepted && action.commitText == "继续" && normalSegmentChainState.preedit() == "zuo",
                "normal segment chain commits a multi-character prefix and keeps the suffix");
        action = normalSegmentChainState.select(candidateIndex(normalSegmentChainState, "做"));
        require(action.accepted && action.commitText == "做", "normal segment chain commits the suffix candidate");
        const auto &segmentContext = normalSegmentChainState.recentCommits();
        require(segmentContext.size() >= 2 && segmentContext[segmentContext.size() - 2] == "继续" &&
                    segmentContext.back() == "做",
                "segment-chain preference learning keeps real committed chunks in recent context");
        require(std::find(segmentContext.begin(), segmentContext.end(), "继续做") == segmentContext.end(),
                "segment-chain preference learning does not inject combined candidates into recent context");
        type(normalSegmentChainState, "jixuzuo");
        require(!normalSegmentChainState.candidates().empty() && normalSegmentChainState.candidates()[0] == "继续做",
                "normal segmented selection teaches the original long preedit as a full phrase preference");
        std::filesystem::remove(segmentChainPreferencePath);
    }
    {
        const auto escapedCorrectionPreferencePath = preferenceBase.string() + "-escaped-correction.tsv";
        std::filesystem::remove(escapedCorrectionPreferencePath);
        tipe::State escapedCorrectionState({}, escapedCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(escapedCorrectionState, "ihao");
            eraseAll(escapedCorrectionState);
            action = escapedCorrectionState.escape();
            require(!action.accepted && action.type == tipe::ActionType::None,
                    "escape after erased typo is recorded but still passes through");
            type(escapedCorrectionState, "nihao");
            action = escapedCorrectionState.select(candidateIndex(escapedCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "correct nihao commit still works after an escaped typo sequence");
        }
        type(escapedCorrectionState, "ihao");
        require(escapedCorrectionState.candidates().empty() || escapedCorrectionState.candidates()[0] != "你好",
                "escaped typo deletion should not teach a cross-composition omission correction");
        std::filesystem::remove(escapedCorrectionPreferencePath);
    }
    {
        const auto middleOmissionPreferencePath = preferenceBase.string() + "-middle-omission.tsv";
        std::filesystem::remove(middleOmissionPreferencePath);
        tipe::State middleOmissionState({}, middleOmissionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(middleOmissionState, "iho");
            eraseAll(middleOmissionState);
            type(middleOmissionState, "nihao");
            action = middleOmissionState.select(candidateIndex(middleOmissionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "corrected nihao commit teaches a middle omission pattern");
        }
        type(middleOmissionState, "iho");
        require(!middleOmissionState.candidates().empty() && middleOmissionState.candidates()[0] == "你好",
                "repeated middle omission correction lets iho borrow nihao candidates");
        std::filesystem::remove(middleOmissionPreferencePath);
    }
    {
        const auto deleteEraseCorrectionPreferencePath = preferenceBase.string() + "-delete-erase-correction.tsv";
        std::filesystem::remove(deleteEraseCorrectionPreferencePath);
        tipe::State deleteEraseCorrectionState({}, deleteEraseCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(deleteEraseCorrectionState, "ihao");
            for (int index = 0; index < 4; ++index) {
                action = deleteEraseCorrectionState.cursorMove("Left");
                require(action.accepted, "delete-erase correction can move to the start of the typo preedit");
            }
            for (int index = 0; index < 4; ++index) {
                action = deleteEraseCorrectionState.deleteKey();
                require(action.accepted, "delete-erase correction can remove typo bytes with Delete");
            }
            require(deleteEraseCorrectionState.empty(), "delete-erase correction removed the typo preedit");
            type(deleteEraseCorrectionState, "nihao");
            action = deleteEraseCorrectionState.select(candidateIndex(deleteEraseCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "Delete-erased typo then corrected commit teaches omission pattern");
        }
        type(deleteEraseCorrectionState, "ihao");
        require(!deleteEraseCorrectionState.candidates().empty() &&
                    deleteEraseCorrectionState.candidates()[0] == "你好",
                "Delete-erased typo correction lets ihao borrow nihao candidates");
        std::filesystem::remove(deleteEraseCorrectionPreferencePath);
    }
    {
        tipe::State enterCorrectionState({}, enterCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(enterCorrectionState, "ihao");
            eraseAll(enterCorrectionState);
            type(enterCorrectionState, "nihao");
            action = enterCorrectionState.enter();
            require(action.accepted && action.commitText == "nihao",
                    "raw enter commit can teach omission correction");
        }
        type(enterCorrectionState, "ihao");
        require(!enterCorrectionState.candidates().empty() && enterCorrectionState.candidates()[0] == "你好",
                "raw enter correction lets typo borrow corrected candidates");
    }
    {
        tipe::State noisyCorrectionState({}, noisyCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(noisyCorrectionState, "ihao");
            eraseAll(noisyCorrectionState);
            for (int index = 0; index < 80; ++index) {
                action = noisyCorrectionState.cursorMove(index % 2 == 0 ? "Down" : "Up");
                require(!action.accepted, "noisy cursor move should pass through without preedit");
                action = noisyCorrectionState.observeKey("Tab");
                require(!action.accepted, "noisy observed key should pass through without preedit");
            }
            type(noisyCorrectionState, "nihao");
            action = noisyCorrectionState.select(candidateIndex(noisyCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "correction learning should survive pass-through key noise");
        }
        type(noisyCorrectionState, "ihao");
        require(!noisyCorrectionState.candidates().empty() && noisyCorrectionState.candidates()[0] == "你好",
                "dedicated correction trail survives noisy model events");
    }
    {
        tipe::State partialRewriteCorrectionState({}, partialRewriteCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(partialRewriteCorrectionState, "nhao");
            action = partialRewriteCorrectionState.backspace();
            require(action.accepted && partialRewriteCorrectionState.preedit() == "nha",
                    "partial rewrite correction can erase last typo byte");
            action = partialRewriteCorrectionState.backspace();
            require(action.accepted && partialRewriteCorrectionState.preedit() == "nh",
                    "partial rewrite correction can erase second typo byte");
            action = partialRewriteCorrectionState.backspace();
            require(action.accepted && partialRewriteCorrectionState.preedit() == "n",
                    "partial rewrite correction can keep the shared prefix");
            type(partialRewriteCorrectionState, "ihao");
            action = partialRewriteCorrectionState.select(candidateIndex(partialRewriteCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "partial rewrite corrected commit teaches omission pattern");
        }
        type(partialRewriteCorrectionState, "nhao");
        require(!partialRewriteCorrectionState.candidates().empty() &&
                    partialRewriteCorrectionState.candidates()[0] == "你好",
                "partial rewrite omission correction lets nhao borrow nihao candidates");
    }
    {
        const auto cursorInsertCorrectionPreferencePath = preferenceBase.string() + "-cursor-insert-correction.tsv";
        std::filesystem::remove(cursorInsertCorrectionPreferencePath);
        tipe::State cursorInsertCorrectionState({}, cursorInsertCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(cursorInsertCorrectionState, "nhao");
            for (int index = 0; index < 3; ++index) {
                action = cursorInsertCorrectionState.cursorMove("Left");
                require(action.accepted, "cursor insertion correction can move inside the typo preedit");
            }
            action = cursorInsertCorrectionState.inputAscii('i');
            require(action.accepted && cursorInsertCorrectionState.preedit() == "nihao",
                    "cursor insertion correction can insert the missing letter in the middle");
            action = cursorInsertCorrectionState.select(candidateIndex(cursorInsertCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "cursor insertion corrected commit teaches omission pattern");
        }
        type(cursorInsertCorrectionState, "nhao");
        require(!cursorInsertCorrectionState.candidates().empty() &&
                    cursorInsertCorrectionState.candidates()[0] == "你好",
                "cursor insertion correction lets nhao borrow nihao candidates");
        std::filesystem::remove(cursorInsertCorrectionPreferencePath);
    }
    {
        const auto deleteCorrectionPreferencePath = preferenceBase.string() + "-delete-correction.tsv";
        std::filesystem::remove(deleteCorrectionPreferencePath);
        tipe::State deleteCorrectionState({}, deleteCorrectionPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(deleteCorrectionState, "niyhao");
            for (int index = 0; index < 4; ++index) {
                action = deleteCorrectionState.cursorMove("Left");
                require(action.accepted, "delete correction can move before the extra middle letter");
            }
            action = deleteCorrectionState.deleteKey();
            require(action.accepted && deleteCorrectionState.preedit() == "nihao",
                    "delete correction can remove the extra middle letter");
            action = deleteCorrectionState.select(candidateIndex(deleteCorrectionState, "你好"));
            require(action.accepted && action.commitText == "你好",
                    "delete corrected commit teaches extra-letter pattern");
        }
        type(deleteCorrectionState, "niyhao");
        require(!deleteCorrectionState.candidates().empty() && deleteCorrectionState.candidates()[0] == "你好",
                "delete-key correction lets niyhao borrow nihao candidates");
        std::filesystem::remove(deleteCorrectionPreferencePath);
    }

    type(state, "nihao");
    require(state.candidates().size() >= 5, "nihao has enough candidates for expansion");
    require(state.visibleCandidates().size() == std::min<std::size_t>(state.candidates().size(), 6),
            "candidate list is one horizontal row when collapsed");
    require(state.displayCandidates().size() == state.visibleCandidates().size(),
            "collapsed display shows one horizontal row of candidates");
    action = state.moveCollapsedCandidateCursor(1, "Right");
    require(action.accepted && !state.candidatesExpanded() && state.candidateCursorIndex() == 1,
            "right arrow moves within collapsed candidates without expanding");
    action = state.moveCollapsedCandidateCursor(-1, "Left");
    require(action.accepted && !state.candidatesExpanded() && state.candidateCursorIndex() == 0,
            "left arrow moves collapsed candidate selection back without expanding");
    action = state.cursorMove("Up");
    require(action.accepted && !state.candidatesExpanded() && state.candidateCursorIndex() == 0,
            "up arrow does not wrap collapsed candidate selection to the last expanded row");
    action = state.cursorMove("Left");
    require(action.accepted && !state.candidatesExpanded() && state.candidateCursorIndex() == 0,
            "left arrow edits preedit after collapsed candidate selection is already at the first candidate");
    action = state.expandCandidates();
    require(action.accepted && action.type == tipe::ActionType::Update, "down arrow expands candidates");
    require(state.candidatesExpanded(), "expanded candidate state is set");
    require(state.candidateCursorIndex() == 0, "expansion keeps current candidate selected");
    require(state.visibleCandidates().size() == state.candidates().size(), "expanded list shows all candidates");
    require(state.displayCandidates().size() == state.candidates().size(), "expanded display keeps one candidate per item");
    require(candidateIndex(state, "你好") < state.displayCandidates().size(), "expanded display includes real candidates");
    action = state.moveCandidateCursor(6);
    require(action.accepted && action.type == tipe::ActionType::Update, "candidate cursor can move down one grid row");
    require(state.candidateCursorIndex() == tipe::visualCandidateColumns, "candidate cursor keeps column while moving down");
    action = state.moveCandidateCursor(1);
    require(action.accepted && state.candidateCursorIndex() == 7, "candidate cursor can move right in grid like Tab");
    action = state.moveCandidateCursor(-1);
    require(action.accepted && state.candidateCursorIndex() == tipe::visualCandidateColumns,
            "candidate cursor can move left in grid like Shift+Tab");
    action = state.moveCandidateCursor(1);
    require(action.accepted && state.candidateCursorIndex() == 7, "candidate cursor can move back after Shift+Tab");
    const auto secondCandidate = state.candidates()[7];
    action = state.space();
    require(action.accepted && action.commitText == secondCandidate, "space commits highlighted candidate");
    state.reset();
    type(state, "nihao");
    action = state.expandCandidates();
    action = state.moveCandidateCursor(6, "Down");
    require(action.accepted && state.candidateCursorIndex() == tipe::visualCandidateColumns,
            "expanded digit selection test reaches row two");
    const auto rowTwoFirstCandidate = state.candidates()[6];
    action = state.selectVisibleDigit(0, "1");
    require(action.accepted && action.commitText == rowTwoFirstCandidate,
            "visible digit 1 selects the first candidate in the highlighted expanded row");
    {
        tipe::State visualRowState({}, cleanPreferencePath);
        type(visualRowState, "haodewokanyxiahaiyoumeiyu");
        action = visualRowState.expandCandidates("Down");
        require(action.accepted && visualRowState.candidateCursorIndex() == 0,
                "first Down expands typo candidates without moving selection");
        action = visualRowState.moveCandidateCursor(6, "Down");
        require(action.accepted &&
                    visualRowState.candidateCursorIndex() == candidateIndex(visualRowState, "好"),
                "second Down moves to the first candidate on the next visual row after long candidates");
        const auto expandedCells = tipe::visibleVisualCellsFor(
            visualRowState.candidates(), visualRowState.candidateCursorIndex(), true);
        const auto selectedCell = tipe::visualCellForIndex(expandedCells, visualRowState.candidateCursorIndex());
        require(selectedCell.has_value(), "selected expanded candidate has a visual cell");
        const auto selectedRowCells = tipe::cellsInVisualRow(expandedCells, selectedCell->row);
        require(selectedRowCells.size() >= 2, "selected expanded visual row exposes a second digit candidate");
        const auto expectedSecondRowCandidate = visualRowState.candidates()[selectedRowCells[1].index];
        action = visualRowState.selectVisibleDigit(1, "2");
        require(action.accepted && action.commitText == expectedSecondRowCandidate,
                "digit 2 selects the second candidate in the selected visual row after long candidates");
    }
    {
        const auto visualNavigationPreferencePath = preferenceBase.string() + "-visual-navigation.tsv";
        std::filesystem::remove(visualNavigationPreferencePath);
        tipe::State visualNavigationState({}, visualNavigationPreferencePath);
        type(visualNavigationState, "nihaowoainiwohhenxihuanniwendy");
        const auto rawCandidate = candidateIndex(visualNavigationState, visualNavigationState.preedit());
        const auto cells = tipe::visualCandidateCells(visualNavigationState.candidates());
        const auto firstCell = tipe::visualCellForIndex(cells, 0);
        const auto rawCell = tipe::visualCellForIndex(cells, rawCandidate);
        const auto firstRowCells = tipe::cellsInVisualRow(cells, firstCell ? firstCell->row : 0);
        require(rawCandidate == 1 && firstCell && rawCell && rawCell->row != firstCell->row && firstRowCells.size() > 1,
                "long raw candidate creates the visual backfill layout reported by real use");
        action = visualNavigationState.expandCandidates("Down");
        action = visualNavigationState.moveCandidateCursor(1, "Right");
        require(action.accepted && visualNavigationState.candidateCursorIndex() == firstRowCells[1].index &&
                    visualNavigationState.candidateCursorIndex() != rawCandidate,
                "expanded Right follows the next visible cell instead of jumping to a raw candidate on another row");
        action = visualNavigationState.moveCandidateCursor(-1, "Left");
        require(action.accepted && visualNavigationState.candidateCursorIndex() == 0,
                "expanded Left follows visual reading order across a backfilled row");
        std::filesystem::remove(visualNavigationPreferencePath);
    }
    type(state, "nihao");
    action = state.expandCandidates();
    const auto currentCandidateBeforeUnmappedDigit = state.candidates()[state.candidateCursorIndex()];
    action = state.selectVisibleDigit(6, "7");
    require(action.accepted && action.type == tipe::ActionType::Commit &&
                action.commitText == currentCandidateBeforeUnmappedDigit && action.passthroughText == "7",
            "digit outside the visible six-column row commits candidate before deterministic trailing text");
    {
        tipe::State prefixPassthroughState({}, cleanPreferencePath);
        type(prefixPassthroughState, "jixuzuo");
        action = prefixPassthroughState.moveCandidateCursorTo(candidateIndex(prefixPassthroughState, "继续"), "End");
        require(action.accepted, "unmapped digit prefix setup can highlight a prefix candidate");
        action = prefixPassthroughState.selectVisibleDigit(6, "7");
        require(action.accepted && action.type == tipe::ActionType::Commit && action.commitText == "继续" &&
                    action.passthroughText == "7",
                "digit outside the visible row commits highlighted prefix before deterministic trailing text");
        require(action.keepPreedit && prefixPassthroughState.preedit() == "zuo",
                "unmapped digit keeps remaining pinyin after committing a prefix candidate");
    }
    bool foundPartialFinalRow = false;
    for (const auto *input : {"nihao", "shijie", "ceshi", "zhongguo", "github"}) {
        typeFresh(state, input);
        const auto cells = tipe::visualCandidateCells(state.candidates());
        const auto rowCount = tipe::visualRowCount(cells);
        const auto finalRowCells = rowCount == 0 ? std::vector<tipe::VisualCandidateCell>{}
                                                 : tipe::cellsInVisualRow(cells, rowCount - 1);
        if (!finalRowCells.empty() && finalRowCells.size() < tipe::visualCandidateColumns) {
            foundPartialFinalRow = true;
            break;
        }
    }
    require(foundPartialFinalRow, "expanded digit gap test needs an input with a partial final row");
    action = state.expandCandidates();
    action = state.moveCandidateCursorTo(state.candidates().size() - 1, "End");
    require(action.accepted && state.candidatesExpanded(), "expanded digit gap test reaches the final row");
    const auto gapCells = tipe::visualCandidateCells(state.candidates());
    const auto selectedGapCell = tipe::visualCellForIndex(gapCells, state.candidateCursorIndex());
    require(selectedGapCell.has_value(), "expanded digit gap test selected candidate has a visual cell");
    const auto finalRowCount = tipe::cellsInVisualRow(gapCells, selectedGapCell->row).size();
    require(finalRowCount > 0 && finalRowCount < tipe::visualCandidateColumns,
            "expanded digit gap test needs a partial final row");
    const auto currentCandidateBeforeEmptyDigit = state.candidates()[state.candidateCursorIndex()];
    const auto emptySlotDigit = std::to_string(finalRowCount + 1);
    action = state.selectVisibleDigit(finalRowCount, emptySlotDigit);
    require(action.accepted && action.type == tipe::ActionType::Commit &&
                action.commitText == currentCandidateBeforeEmptyDigit && action.passthroughText == emptySlotDigit,
            "digit in an empty selected-row slot commits candidate before deterministic trailing text");
    state.reset();
    type(state, "nihao");
    const auto wrapCells = tipe::visualCandidateCells(state.candidates());
    const auto wrapRowCount = tipe::visualRowCount(wrapCells);
    const auto lastWrapRowCells = tipe::cellsInVisualRow(wrapCells, wrapRowCount - 1);
    require(wrapRowCount > 1 && !lastWrapRowCells.empty(), "candidate cursor wrap test has multiple visual rows");
    action = state.moveCandidateCursor(-6);
    require(action.accepted, "candidate cursor can wrap upward");
    require(state.candidateCursorIndex() == lastWrapRowCells.front().index,
            "candidate cursor wraps upward to the first cell on the last visual row");
    action = state.moveCandidateCursor(6);
    require(action.accepted && state.candidateCursorIndex() == 0, "candidate cursor wraps downward to first candidate");
    action = state.moveCandidateCursorTo(state.candidates().size() - 1, "End");
    require(action.accepted && state.candidatesExpanded() && state.candidateCursorIndex() == state.candidates().size() - 1,
            "End moves to the last candidate and keeps candidates expanded");
    action = state.moveCandidateCursorTo(0, "Home");
    require(action.accepted && state.candidateCursorIndex() == 0, "Home moves to the first candidate");
    action = state.moveCandidateCursorTo(state.candidates().size() + 100, "End");
    require(action.accepted && state.candidateCursorIndex() == state.candidates().size() - 1,
            "candidate cursor absolute movement clamps to the last candidate");
    action = state.select(candidateIndex(state, "你号"));
    require(action.accepted && action.commitText == "你号", "expanded display does not change numeric selection semantics");
    type(state, "nihao");
    action = state.expandCandidates();
    action = state.backspace();
    require(action.accepted && !state.candidatesExpanded(), "editing collapses candidates again");
    state.reset();

    const auto rerankCleanPreferencePath = preferenceBase.string() + "-rerank-clean.tsv";
    std::filesystem::remove(rerankCleanPreferencePath);
    tipe::State freshState({}, rerankCleanPreferencePath);
    type(freshState, "nihao");
    action = freshState.rerankCandidates();
    require(action.accepted && action.type == tipe::ActionType::Update, "rerank updates candidate order");
    require(freshState.candidatesExpanded(), "rerank expands candidates");
    require(freshState.candidates()[0] == "你好", "rerank keeps sensible default order without learned preferences");
    require(hasEvent(freshState, tipe::InputEventType::AiRerankRequested), "rerank request is recorded");

    {
        const auto shortPreeditPollutionPreferencePath = preferenceBase.string() + "-short-preedit-pollution.tsv";
        {
            std::ofstream output(shortPreeditPollutionPreferencePath);
            output << "d\t多克尔\t10\n";
        }
        tipe::State shortPreeditState({}, shortPreeditPollutionPreferencePath);
        type(shortPreeditState, "d");
        require(!shortPreeditState.candidates().empty() && shortPreeditState.candidates()[0] != "多克尔",
                "learned preferences cannot promote a long phrase over a one-letter common preedit");
        action = shortPreeditState.rerankCandidates();
        require(action.accepted && !shortPreeditState.candidates().empty() &&
                    shortPreeditState.candidates()[0] != "多克尔",
                "rerank also guards one-letter preedits from polluted learned long-phrase preferences");
    }

    {
        const auto selectableRawCandidatePreferencePath = preferenceBase.string() + "-selectable-raw-candidate.tsv";
        std::filesystem::remove(selectableRawCandidatePreferencePath);
        tipe::State selectableRawCandidate({}, selectableRawCandidatePreferencePath);
        type(selectableRawCandidate, "start");
        require(!selectableRawCandidate.candidates().empty() && selectableRawCandidate.candidates().front() != "start",
                "unlearned English-like input stays behind normal Chinese ranking by default");
        const auto rawCandidateIndex = candidateIndex(selectableRawCandidate, "start");
        require(rawCandidateIndex < selectableRawCandidate.candidates().size() && rawCandidateIndex <= 2,
                "unlearned English-like input still exposes a visible raw candidate for explicit selection");
        action = selectableRawCandidate.select(rawCandidateIndex);
        require(action.accepted && action.commitText == "start",
                "explicitly selecting the raw candidate commits the English text");

        tipe::State learnedRawFromSelection({}, selectableRawCandidatePreferencePath);
        type(learnedRawFromSelection, "start");
        require(!learnedRawFromSelection.candidates().empty() && learnedRawFromSelection.candidates().front() == "start",
                "one explicit raw selection is enough to activate the raw-English preference");
        require(learnedRawFromSelection.modelRequestSnapshot().find("\npreference\tstart\tstart\t3\n") !=
                    std::string::npos,
                "the learned raw-English preference from explicit selection is exposed to models");
        std::filesystem::remove(selectableRawCandidatePreferencePath);

        tipe::State genericRawOffer({}, cleanPreferencePath);
        type(genericRawOffer, "goal");
        const auto genericRawOfferIndex = candidateIndex(genericRawOffer, "goal");
        require(genericRawOfferIndex < genericRawOffer.candidates().size() && genericRawOfferIndex <= 2 &&
                    genericRawOffer.candidateSource(genericRawOfferIndex) == "raw-offer",
                "generic English-like terminal shapes expose an unlearned raw candidate");

        tipe::State normalPinyinOfferGuard({}, cleanPreferencePath);
        type(normalPinyinOfferGuard, "shuruf");
        require(std::find(normalPinyinOfferGuard.candidates().begin(), normalPinyinOfferGuard.candidates().end(),
                          "shuruf") == normalPinyinOfferGuard.candidates().end(),
                "unfinished normal pinyin does not expose an unlearned raw candidate");
    }

    {
        const auto learnedRawIdentifierPreferencePath = preferenceBase.string() + "-learned-raw-identifier.tsv";
        std::filesystem::remove(learnedRawIdentifierPreferencePath);
        tipe::State rawIdentifierTrainer({}, learnedRawIdentifierPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(rawIdentifierTrainer, "started");
            action = rawIdentifierTrainer.enter();
            require(action.accepted && action.commitText == "started",
                    "raw identifier learning records explicit raw commits");
        }

        tipe::State learnedRawIdentifier({}, learnedRawIdentifierPreferencePath);
        type(learnedRawIdentifier, "started");
        require(!learnedRawIdentifier.candidates().empty() && learnedRawIdentifier.candidates()[0] == "started",
                "repeated raw identifier commits promote the raw text without app-specific rules");
        require(learnedRawIdentifier.modelRequestSnapshot().find("\npreference\tstarted\tstarted\t4\n") !=
                    std::string::npos,
                "active raw identifier preference is exposed to clicked models");
        action = learnedRawIdentifier.inputAsciiDigit('1');
        require(action.accepted && learnedRawIdentifier.preedit() == "started1" &&
                    !learnedRawIdentifier.candidates().empty() && learnedRawIdentifier.candidates()[0] == "started1",
                "digit keys extend a learned raw English preedit instead of selecting candidates");
        action = learnedRawIdentifier.backspace();
        require(action.accepted && learnedRawIdentifier.preedit() == "started" &&
                    !learnedRawIdentifier.candidates().empty() && learnedRawIdentifier.candidates()[0] == "started",
                "backspace keeps learned raw English mode active while text remains");
        eraseAll(learnedRawIdentifier);
        type(learnedRawIdentifier, "nihao");
        action = learnedRawIdentifier.inputAsciiDigit('2');
        require(!action.accepted && learnedRawIdentifier.preedit() == "nihao",
                "emptying a learned raw English preedit clears raw digit mode for later pinyin");

        tipe::State unlearnedEnglish({}, cleanPreferencePath);
        type(unlearnedEnglish, "started");
        action = unlearnedEnglish.rerankCandidates("Alacritty", "const taskStatus = ", "");
        require(action.accepted && !unlearnedEnglish.candidates().empty() &&
                    unlearnedEnglish.candidates()[0] != "started",
                "raw English promotion is not app- or surrounding-specific without user learning");

        tipe::State terminalPinyin({}, cleanPreferencePath);
        type(terminalPinyin, "shurufa");
        action = terminalPinyin.rerankCandidates("Alacritty");
        require(action.accepted && !terminalPinyin.candidates().empty() && terminalPinyin.candidates()[0] != "shurufa",
                "normal pinyin is not promoted as raw English by app context");

        for (const std::string token : {"goal", "ok"}) {
            tipe::State genericRawTrainer({}, learnedRawIdentifierPreferencePath);
            for (int attempt = 0; attempt < 2; ++attempt) {
                type(genericRawTrainer, token);
                action = genericRawTrainer.enter();
                require(action.accepted && action.commitText == token,
                        "generic raw identifier learning records explicit commits: " + token);
            }
            tipe::State learnedGenericRaw({}, learnedRawIdentifierPreferencePath);
            type(learnedGenericRaw, token);
            require(!learnedGenericRaw.candidates().empty() && learnedGenericRaw.candidates()[0] == token,
                    "generic terminal-shape learning promotes non-hardcoded English: " + token);
        }

        tipe::State pinyinVTrainer({}, learnedRawIdentifierPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(pinyinVTrainer, "lv");
            action = pinyinVTrainer.enter();
            require(action.accepted && action.commitText == "lv", "v-form pinyin raw commit remains supervised");
        }
        tipe::State learnedPinyinV({}, learnedRawIdentifierPreferencePath);
        type(learnedPinyinV, "lv");
        require(!learnedPinyinV.candidates().empty() && learnedPinyinV.candidates()[0] != "lv",
                "valid v-form pinyin is not misclassified as raw English");

        std::filesystem::remove(learnedRawIdentifierPreferencePath);

        tipe::State knownRawEnglish({}, cleanPreferencePath);
        type(knownRawEnglish, "react");
        require(!knownRawEnglish.candidates().empty() && knownRawEnglish.candidates()[0] == "react",
                "known raw English token is available before a digit suffix");
        action = knownRawEnglish.inputAsciiDigit('1');
        require(action.accepted && knownRawEnglish.preedit() == "react1" &&
                    !knownRawEnglish.candidates().empty() && knownRawEnglish.candidates()[0] == "react1",
                "digit keys extend a known raw English token preedit");
        action = knownRawEnglish.inputRawTokenSymbol('-');
        require(action.accepted && knownRawEnglish.preedit() == "react1-" &&
                    !knownRawEnglish.candidates().empty() && knownRawEnglish.candidates()[0] == "react1-",
                "hyphen extends a raw English token preedit");
        action = knownRawEnglish.inputAscii('a');
        require(action.accepted && knownRawEnglish.preedit() == "react1-a" &&
                    !knownRawEnglish.candidates().empty() && knownRawEnglish.candidates()[0] == "react1-a",
                "letters continue after a raw English token symbol");
        action = knownRawEnglish.inputRawTokenSymbol('_');
        require(action.accepted && knownRawEnglish.preedit() == "react1-a_" &&
                    !knownRawEnglish.candidates().empty() && knownRawEnglish.candidates()[0] == "react1-a_",
                "underscore extends a raw English token preedit");
        action = knownRawEnglish.inputRawTokenSymbol('.');
        require(action.accepted && knownRawEnglish.preedit() == "react1-a_.",
                "period extends a raw English token preedit");
        action = knownRawEnglish.inputRawTokenSymbol('/');
        require(action.accepted && knownRawEnglish.preedit() == "react1-a_./",
                "slash extends a raw English token preedit");

        tipe::State normalPunctuation({}, cleanPreferencePath);
        type(normalPunctuation, "nihao");
        action = normalPunctuation.inputRawTokenSymbol('-');
        require(!action.accepted && normalPunctuation.preedit() == "nihao",
                "raw token symbols do not edit normal pinyin preedit");
    }

    type(state, "nihao");
    action = state.select(candidateIndex(state, "你号"));
    require(action.accepted && action.commitText == "你号", "selecting a candidate records preference");
    state.reset();
    type(state, "nihao");
    require(state.candidates()[0] == "你号", "learned preference automatically promotes recently selected candidate");
    action = state.rerankCandidates();
    require(action.accepted && state.candidates()[0] == "你号", "rerank preserves learned preference");
    const auto snapshot = state.debugSnapshot();
    require(snapshot.preedit == "nihao", "debug snapshot includes preedit");
    require(snapshot.candidateCount >= 5, "debug snapshot includes candidate count");
    require(snapshot.displayCandidateCount == snapshot.candidateCount, "debug snapshot includes display candidate count");
    require(snapshot.candidatesExpanded, "debug snapshot includes expansion state");
    require(snapshot.eventCounts.rerankRequests >= 1, "debug snapshot includes rerank count");
    const auto modelRequestSnapshot =
        state.modelRequestSnapshot("Alacritty", "left\tcontext", "right\ncontext", true);
    require(modelRequestSnapshot.find("protocol\t1\n") != std::string::npos,
            "model request snapshot uses the model protocol header");
    require(modelRequestSnapshot.find("preedit\tnihao\n") != std::string::npos,
            "model request snapshot includes current preedit");
    require(modelRequestSnapshot.find("application\tAlacritty\n") != std::string::npos,
            "model request snapshot includes application");
    require(modelRequestSnapshot.find("surrounding_before\tleft\\tcontext\n") != std::string::npos &&
                modelRequestSnapshot.find("surrounding_after\tright\\ncontext\n") != std::string::npos,
            "model request snapshot escapes surrounding context");
    require(modelRequestSnapshot.find("candidates\t") != std::string::npos &&
                modelRequestSnapshot.find("candidate_metadata\t0\t") != std::string::npos &&
                modelRequestSnapshot.find("visible_candidates\t") != std::string::npos &&
                modelRequestSnapshot.find("numbered_candidates\t") != std::string::npos,
            "model request snapshot includes candidate UI state and candidate metadata");
    require(modelRequestSnapshot.find("runtime_state\tcontinuous\t1\n") != std::string::npos,
            "model request snapshot includes continuous-mode runtime state");
    require(modelRequestSnapshot.find("supervision_state\tmode\tactive-preedit\tactive_preedit\t1") !=
                std::string::npos,
            "model request snapshot declares active-preedit supervision mode");
    require(modelRequestSnapshot.find("events\t") != std::string::npos &&
                modelRequestSnapshot.find("correction_events\t") != std::string::npos,
            "model request snapshot includes supervised key trails");
    require(modelRequestSnapshot.find("event_counts\t") != std::string::npos &&
                modelRequestSnapshot.find("correction_event_counts\t") != std::string::npos,
            "model request snapshot includes supervised key count summaries");
    state.reset();

    {
        tipe::State prefixMetadataRequest({}, cleanPreferencePath);
        type(prefixMetadataRequest, "jixuzuo");
        const auto request = prefixMetadataRequest.modelRequestSnapshot();
        require(request.find("candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\t") != std::string::npos,
                "model request exposes prefix candidate consumed-pinyin metadata");
    }

    {
        const auto currentCorrectionPriorityPath = preferenceBase.string() + "-current-correction-priority.tsv";
        {
            std::ofstream preferences(currentCorrectionPriorityPath);
            for (int index = 0; index < 40; ++index) {
                preferences << "__correction__\tmiss" << index << "\tmissn" << index << "\t99\n";
            }
            preferences << "__correction__\tnhao\tnihao\t1\n";
        }
        tipe::State currentCorrectionPriority({}, currentCorrectionPriorityPath);
        type(currentCorrectionPriority, "nhao");
        const auto request = currentCorrectionPriority.modelRequestSnapshot();
        require(request.find("\ncorrection\tnhao\tnihao\t1\n") != std::string::npos,
                "model request keeps current-preedit correction evidence even when many stronger unrelated rows exist");
        const auto currentCorrectionOffset = request.find("\ncorrection\tnhao\tnihao\t1\n");
        const auto unrelatedCorrectionOffset = request.find("\ncorrection\tmiss");
        require(currentCorrectionOffset != std::string::npos &&
                    (unrelatedCorrectionOffset == std::string::npos || currentCorrectionOffset < unrelatedCorrectionOffset),
                "model request orders current-preedit correction evidence before unrelated correction rows");
        std::filesystem::remove(currentCorrectionPriorityPath);
    }

    {
        const auto inactivePreferencePath = preferenceBase.string() + "-inactive-model-preferences.tsv";
        {
            std::ofstream preferences(inactivePreferencePath);
            preferences << "nihao\t你号\t1\n";
            preferences << "nihao\tnihao\t9\n";
            preferences << "started\tstarted\t2\n";
            preferences << "d\t多克尔\t10\n";
            preferences << "jixuzuo\t继续\t10\n";
        }
        tipe::State inactivePreferenceState({}, inactivePreferencePath);
        type(inactivePreferenceState, "nihao");
        auto request = inactivePreferenceState.modelRequestSnapshot();
        require(request.find("\npreference\tnihao\t你号\t1\n") == std::string::npos,
                "one-count candidate evidence is not exposed to a model as an active preference");
        require(request.find("\npreference\tnihao\tnihao\t9\n") == std::string::npos,
                "normal pinyin raw evidence is not exposed to a model as raw-English preference");
        inactivePreferenceState.reset();
        type(inactivePreferenceState, "started");
        request = inactivePreferenceState.modelRequestSnapshot();
        require(request.find("\npreference\tstarted\tstarted\t2\n") == std::string::npos,
                "raw preference evidence below its three-count threshold is not exposed as active");
        inactivePreferenceState.reset();
        type(inactivePreferenceState, "d");
        request = inactivePreferenceState.modelRequestSnapshot();
        require(request.find("\npreference\td\t多克尔\t10\n") == std::string::npos,
                "shape-incompatible legacy preferences are not exposed to the model");
        inactivePreferenceState.reset();
        type(inactivePreferenceState, "jixuzuo");
        request = inactivePreferenceState.modelRequestSnapshot();
        require(request.find("\npreference\tjixuzuo\t继续\t10\n") == std::string::npos,
                "prefix-only preferences are not exposed as full-preedit model evidence");
        const auto learnedPrefixIndex = candidateIndex(inactivePreferenceState, "继续");
        require(inactivePreferenceState.candidateConsumedPrefixLength(learnedPrefixIndex) == 4,
                "active legacy preference cannot erase prefix-consumption metadata");
        const auto learnedPrefixAction = inactivePreferenceState.select(learnedPrefixIndex);
        require(learnedPrefixAction.accepted && learnedPrefixAction.keepPreedit &&
                    inactivePreferenceState.preedit() == "zuo",
                "selecting a prefix candidate with legacy full-preedit evidence still preserves the suffix");
        std::filesystem::remove(inactivePreferencePath);
    }

    {
        const auto rawThresholdPreferencePath = preferenceBase.string() + "-raw-threshold.tsv";
        {
            std::ofstream preferences(rawThresholdPreferencePath);
            preferences << "started\tstarted\t2\n";
        }
        tipe::State inactiveRawThreshold({}, rawThresholdPreferencePath);
        type(inactiveRawThreshold, "started");
        require(!inactiveRawThreshold.candidates().empty() && inactiveRawThreshold.candidates().front() != "started",
                "raw English preference stays inactive below the three-count threshold in local ranking");
        const auto inactiveRerankAction = inactiveRawThreshold.rerankCandidates();
        require(inactiveRerankAction.accepted && !inactiveRawThreshold.candidates().empty() &&
                    inactiveRawThreshold.candidates().front() != "started",
                "local rerank honors the raw-English activation threshold");

        {
            std::ofstream preferences(rawThresholdPreferencePath);
            preferences << "started\tstarted\t3\n";
        }
        tipe::State activeRawThreshold({}, rawThresholdPreferencePath);
        type(activeRawThreshold, "started");
        require(!activeRawThreshold.candidates().empty() && activeRawThreshold.candidates().front() == "started",
                "raw English preference activates once the three-count threshold is met");
        std::filesystem::remove(rawThresholdPreferencePath);
    }

    {
        const auto supervisedRawTokenPath = preferenceBase.string() + "-supervised-raw-token.tsv";
        {
            std::ofstream preferences(supervisedRawTokenPath);
            preferences << "__raw_token__\tto\t2\n";
            preferences << "__raw_token__\tUPPER\t9\n";
            preferences << "nihao\tnihao\t9\n";
        }
        tipe::State inactiveSupervisedRaw({}, supervisedRawTokenPath);
        type(inactiveSupervisedRaw, "to");
        require(!inactiveSupervisedRaw.candidates().empty() && inactiveSupervisedRaw.candidates().front() != "to",
                "English-mode exact token evidence stays inactive below count three");
        action = inactiveSupervisedRaw.escape();
        require(action.accepted, "inactive supervised raw-token probe can clear its preedit");
        type(inactiveSupervisedRaw, "nihao");
        require(!inactiveSupervisedRaw.candidates().empty() && inactiveSupervisedRaw.candidates().front() == "你好",
                "ordinary inactive raw-pinyin rows do not become trusted English-mode evidence");

        {
            std::ofstream preferences(supervisedRawTokenPath);
            preferences << "__raw_token__\tto\t3\n";
        }
        tipe::State activeSupervisedRaw({}, supervisedRawTokenPath);
        type(activeSupervisedRaw, "to");
        require(!activeSupervisedRaw.candidates().empty() && activeSupervisedRaw.candidates().front() == "to",
                "three confirmed English-mode commits activate an exact short English token");
        const auto chineseCandidateIter = std::find_if(
            activeSupervisedRaw.candidates().begin(), activeSupervisedRaw.candidates().end(),
            [](const auto &candidate) { return candidate != "to"; });
        require(chineseCandidateIter != activeSupervisedRaw.candidates().end(),
                "supervised raw token keeps a Chinese alternative available");
        const auto chineseCandidate = *chineseCandidateIter;
        for (int attempt = 0; attempt < 2; ++attempt) {
            action = activeSupervisedRaw.select(candidateIndex(activeSupervisedRaw, chineseCandidate));
            require(action.accepted && action.commitText == chineseCandidate,
                    "an explicit Chinese choice can correct supervised English-token ordering");
            type(activeSupervisedRaw, "to");
        }
        require(activeSupervisedRaw.candidates().front() == chineseCandidate,
                "repeated explicit Chinese choices override supervised English-token evidence");
        require(fileContains(supervisedRawTokenPath, "__raw_token__\tto\t3"),
                "normal preference writes preserve supervised raw-token records");
        std::filesystem::remove(supervisedRawTokenPath);
    }

    {
        const auto malformedPreferenceShapePath = preferenceBase.string() + "-malformed-shape.tsv";
        {
            std::ofstream preferences(malformedPreferenceShapePath);
            preferences << "nihao\t你号\t3\textra\n";
            preferences << "started\tstarted\t2\textra\n";
            preferences << "nihao\t你好\t2\n";
        }
        tipe::State malformedShapeState({}, malformedPreferenceShapePath);
        type(malformedShapeState, "nihao");
        require(!malformedShapeState.candidates().empty() && malformedShapeState.candidates().front() == "你好",
                "malformed four-field preference rows are ignored instead of polluting learned ranking");
        std::filesystem::remove(malformedPreferenceShapePath);
    }

    {
        const auto passiveSelectionPath = preferenceBase.string() + "-passive-selection-write.tsv";
        {
            std::ofstream preferences(passiveSelectionPath);
            preferences << "nihao\t你号\t2\n";
        }
        std::error_code timeError;
        const auto oldWriteTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
        std::filesystem::last_write_time(passiveSelectionPath, oldWriteTime, timeError);
        require(!timeError, "passive selection persistence test can set a stable file timestamp");
        tipe::InputModel passiveSelectionModel(passiveSelectionPath);
        const auto writeTimeBefore = std::filesystem::last_write_time(passiveSelectionPath);
        passiveSelectionModel.recordCandidateSelection("nihao", "你好", 0);
        require(std::filesystem::last_write_time(passiveSelectionPath) == writeTimeBefore,
                "passive default confirmation does not rewrite an unchanged preference file");
        std::filesystem::remove(passiveSelectionPath);
    }

    {
        const auto correctiveSelectionPath = preferenceBase.string() + "-corrective-selection.tsv";
        {
            std::ofstream preferences(correctiveSelectionPath);
            preferences << "nihao\t你好\t20\n";
            preferences << "nihao\t你号\t2\n";
        }
        tipe::InputModel correctiveSelectionModel(correctiveSelectionPath);
        correctiveSelectionModel.recordCandidateSelection("nihao", "你号", 3);
        tipe::InputModel correctedSelectionModel(correctiveSelectionPath);
        const auto corrected = correctedSelectionModel.applyLearnedPreferences("nihao", {"你好", "你号"});
        require(corrected.size() == 2 && corrected.front() == "你号",
                "one explicit alternative selection overrides stale stronger preference evidence");
        require(fileContains(correctiveSelectionPath, "nihao\t你号\t21\n"),
                "explicit alternative selection persists a learning strength above its strongest competitor");
        std::filesystem::remove(correctiveSelectionPath);
    }

    {
        const auto passiveCorrectionPath = preferenceBase.string() + "-passive-correction-write.tsv";
        std::filesystem::remove(passiveCorrectionPath);
        tipe::InputModel passiveCorrectionModel(passiveCorrectionPath);
        for (const char ch : std::string("ihao")) {
            passiveCorrectionModel.record(tipe::InputEventType::Letter, std::string(1, ch));
        }
        for (int index = 0; index < 4; ++index) {
            passiveCorrectionModel.record(tipe::InputEventType::Backspace);
        }
        for (const char ch : std::string("nihao")) {
            passiveCorrectionModel.record(tipe::InputEventType::Letter, std::string(1, ch));
        }
        passiveCorrectionModel.recordCandidateSelection("nihao", "你好", 0);
        require(fileContains(passiveCorrectionPath, "__correction__\tihao\tnihao\t1"),
                "passive confirmation still persists a correction inferred from supervised key history");
        std::filesystem::remove(passiveCorrectionPath);
    }

    type(state, "nihao");
    action = state.cursorMove("Left");
    require(action.accepted && action.type == tipe::ActionType::Update && state.preeditCursorIndex() == 4,
            "cursor move edits the pinyin cursor while composing");
    require(hasEvent(state, tipe::InputEventType::CursorMove), "cursor move is recorded");
    require(hasEventText(state, tipe::InputEventType::CursorMove, "Left"), "named cursor move is recorded");

    {
        tipe::State trained({}, persistentPreferencePath);
        type(trained, "nihao");
        action = trained.select(candidateIndex(trained, "你号"));
        require(action.accepted && action.commitText == "你号", "persistent preference test selects candidate");
    }
    {
        tipe::State restored({}, persistentPreferencePath);
        type(restored, "nihao");
        require(restored.candidates()[0] == "你号", "candidate preference survives a new state instance");
        require(!std::filesystem::exists(persistentPreferencePath + ".tmp"),
                "candidate preference save does not leave a temporary file behind");
    }
    {
        tipe::State correctionTrainer({}, persistentPreferencePath);
        for (int attempt = 0; attempt < 2; ++attempt) {
            type(correctionTrainer, "ihao");
            eraseAll(correctionTrainer);
            type(correctionTrainer, "nihao");
            action = correctionTrainer.select(candidateIndex(correctionTrainer, "你好"));
            require(action.accepted && action.commitText == "你好", "persistent correction test selects corrected candidate");
        }
    }
    {
        tipe::State correctionRestored({}, persistentPreferencePath);
        type(correctionRestored, "ihao");
        require(!correctionRestored.candidates().empty() && correctionRestored.candidates()[0] == "你好",
                "correction learning survives a new state instance");
    }
    {
        {
            std::ofstream preferences(hotReloadPreferencePath);
            preferences << "nihao\t你号\t8\n";
        }
        tipe::State hotReloaded({}, hotReloadPreferencePath);
        type(hotReloaded, "nihao");
        require(!hotReloaded.candidates().empty() && hotReloaded.candidates()[0] == "你号",
                "initial preference is loaded before an external learning update");

        const auto previousWriteTime = std::filesystem::last_write_time(hotReloadPreferencePath);
        {
            std::ofstream preferences(hotReloadPreferencePath);
            preferences << "nihao\t你好\t8\n";
            preferences << "__correction__\tihao\tnihao\t2\n";
        }
        std::filesystem::last_write_time(hotReloadPreferencePath, previousWriteTime + std::chrono::seconds(2));
        action = hotReloaded.rerankCandidates({}, {}, {}, false, false);
        require(action.accepted && !hotReloaded.candidates().empty() && hotReloaded.candidates()[0] == "你好",
                "an active state reloads preference rows written by the analysis window");
        hotReloaded.reset();
        type(hotReloaded, "ihao");
        require(!hotReloaded.candidates().empty() && hotReloaded.candidates()[0] == "你好",
                "an active state reloads correction rows written by the analysis window");

        hotReloaded.reset();
        type(hotReloaded, "nihao");
        action = hotReloaded.select(candidateIndex(hotReloaded, "你号"));
        require(action.accepted && fileContains(hotReloadPreferencePath, "nihao\t你好\t8\n") &&
                    fileContains(hotReloadPreferencePath, "nihao\t你号\t9\n"),
                "local corrective selection merges with external rows and wins against their latest strength");
    }
    {
        std::ofstream malformedPreferences(malformedPreferencePath);
        malformedPreferences << "nihao\t\t999\n";
        malformedPreferences << "nihao\t泥豪\t999\textra\n";
        malformedPreferences << "__correction__\tihao\tzzzz\t999\n";
        malformedPreferences << "__correction__\tnhao\tnihao\t999\textra\n";
        malformedPreferences << "nihao\t你号\t3\n";
        malformedPreferences << "__correction__\tihao\tnihao\t2\n";
        malformedPreferences.close();

        tipe::State malformedRestored({}, malformedPreferencePath);
        type(malformedRestored, "nihao");
        require(!malformedRestored.candidates().empty() && malformedRestored.candidates()[0] == "你号",
                "valid preference rows still load from a mixed preference file without accepting malformed counts");
        malformedRestored.reset();
        type(malformedRestored, "ihao");
        require(!malformedRestored.candidates().empty() && malformedRestored.candidates()[0] == "你好",
                "valid correction rows still load from a mixed preference file");
        require(std::find(malformedRestored.candidates().begin(), malformedRestored.candidates().end(), "zzzz") ==
                    malformedRestored.candidates().end(),
                "malformed correction rows are ignored while loading preferences");
        malformedRestored.reset();
        type(malformedRestored, "nhao");
        const auto malformedRequest = malformedRestored.modelRequestSnapshot();
        require(malformedRequest.find("correction\tihao\tnihao\t2\n") != std::string::npos &&
                    malformedRequest.find("correction\tnhao\tnihao\t999\n") == std::string::npos,
                "malformed correction rows with trailing fields are ignored while loading preferences");
    }
    {
        std::ofstream crlfPreferences(crlfPreferencePath, std::ios::binary);
        crlfPreferences << "nihao\t你号\t3\r\n";
        crlfPreferences << "__correction__\tihao\tnihao\t2\r\n";
        crlfPreferences.close();

        tipe::State crlfRestored({}, crlfPreferencePath);
        type(crlfRestored, "nihao");
        require(!crlfRestored.candidates().empty() && crlfRestored.candidates()[0] == "你号",
                "CRLF preference rows load candidate ordering at runtime");
        crlfRestored.reset();
        type(crlfRestored, "ihao");
        require(!crlfRestored.candidates().empty() && crlfRestored.candidates()[0] == "你好",
                "CRLF correction rows load typo correction at runtime");
    }
    {
        const auto pollutedPrefixPreferencePath = preferenceBase.string() + "-polluted-prefix.tsv";
        std::filesystem::remove(pollutedPrefixPreferencePath);
        std::ofstream pollutedPreferences(pollutedPrefixPreferencePath);
        pollutedPreferences << "yixia\t一\t99\n";
        pollutedPreferences << "yixia\t以\t88\n";
        pollutedPreferences.close();

        tipe::State pollutedRestored({}, pollutedPrefixPreferencePath);
        type(pollutedRestored, "yixia");
        require(!pollutedRestored.candidates().empty() && pollutedRestored.candidates()[0] == "一下",
                "old learned prefix preferences should not outrank a known full phrase");
        require(candidateIndex(pollutedRestored, "一") < pollutedRestored.candidates().size(),
                "old learned prefix preferences remain selectable behind the full phrase");
        std::filesystem::remove(pollutedPrefixPreferencePath);
    }
    {
        const auto rimeDictionaryPath = preferenceBase.string() + "-rime.dict.yaml";
        std::ofstream rimeDictionary(rimeDictionaryPath);
        rimeDictionary << "# Rime dictionary\n";
        rimeDictionary << "# encoding: utf-8\n";
        rimeDictionary << "---\n";
        rimeDictionary << "name: tipe_test\n";
        rimeDictionary << "version: \"1\"\n";
        rimeDictionary << "sort: by_weight\n";
        rimeDictionary << "...\n";
        rimeDictionary << "歪库\tjia ku\t1\n";
        rimeDictionary << "甲库\tjia ku\t900\n";
        rimeDictionary << "乙表\tyi biao\t800\n";
        rimeDictionary.close();

        setenv("TIPE_SYSTEM_RIME_DICTIONARY", rimeDictionaryPath.c_str(), 1);
        tipe::State rimeDictionaryState({}, userDictionaryPreferencePath);
        type(rimeDictionaryState, "jiaku");
        require(!rimeDictionaryState.candidates().empty() && rimeDictionaryState.candidates()[0] == "甲库",
                "system Rime dictionary entries load and sort by weight");
        require(candidateIndex(rimeDictionaryState, "歪库") > candidateIndex(rimeDictionaryState, "甲库"),
                "lower-weight Rime entries remain behind higher-weight entries");
        rimeDictionaryState.reset();
        type(rimeDictionaryState, "jiakuyibiao");
        require(candidateIndex(rimeDictionaryState, "甲库乙表") < rimeDictionaryState.candidates().size(),
                "system Rime dictionary entries can compose a longer preedit");
        {
            std::ofstream updatedRimeDictionary(rimeDictionaryPath, std::ios::app);
            updatedRimeDictionary << "新库\txin ku\t950\n";
        }
        tipe::State refreshedRimeDictionaryState({}, userDictionaryPreferencePath);
        type(refreshedRimeDictionaryState, "xinku");
        require(candidateIndex(refreshedRimeDictionaryState, "新库") <
                    refreshedRimeDictionaryState.candidates().size(),
                "shared system Rime cache reloads when the dictionary file version changes");
        unsetenv("TIPE_SYSTEM_RIME_DICTIONARY");
        std::filesystem::remove(rimeDictionaryPath);
    }
    {
        setenv("TIPE_USER_DICTIONARY", automaticDictionaryPath.c_str(), 1);
        {
            std::ofstream dictionary(automaticDictionaryPath);
            dictionary << "# preserved manual note\n";
            dictionary << "zidingyi\t旧词\t新词\n";
        }
        tipe::Dictionary automaticDictionary;
        require(!automaticDictionary.learnUserEntry("", "新词") &&
                    !automaticDictionary.learnUserEntry(std::string(129, 'a'), "新词") &&
                    !automaticDictionary.learnUserEntry("zidingyi", "plain-ascii") &&
                    !automaticDictionary.learnUserEntry("zidingyi", "坏\t词") &&
                    !automaticDictionary.learnUserEntry("zidingyi", std::string("\xf0\x28\x8c\x28", 4)) &&
                    !automaticDictionary.learnUserEntry("zidingyi", std::string(257, 'x') + "词"),
                "automatic dictionary learning rejects malformed, ASCII-only, and oversized entries");
        require(automaticDictionary.learnUserEntry("zidingyi", "新词"),
                "automatic dictionary learning accepts a safe Chinese candidate");
        const auto learnedCandidates = automaticDictionary.exactUserCandidates("zidingyi");
        require(learnedCandidates.size() == 2 && learnedCandidates[0] == "新词" && learnedCandidates[1] == "旧词",
                "automatic dictionary learning promotes an existing candidate without duplicating it");
        require(fileContains(automaticDictionaryPath, "# preserved manual note\n") &&
                    lineCount(automaticDictionaryPath) == 2,
                "automatic dictionary learning preserves comments and existing rows");
        require(fileIsPrivate(automaticDictionaryPath) && fileIsPrivate(automaticDictionaryPath + ".lock") &&
                    !hasTemporarySibling(automaticDictionaryPath),
                "automatic dictionary writes and locks are private and leave no temporary files");
        require(automaticDictionary.learnUserEntry("zidingyi", "新词") &&
                    lineCount(automaticDictionaryPath) == 2,
                "relearning an existing user candidate is idempotent");
    }
#ifdef TIPE_TEST_HAVE_LIBIME
    {
        const char *previousFallbackFixture = std::getenv("TIPE_TEST_FALLBACK_DICTIONARY");
        const std::string previousFallbackFixtureValue = previousFallbackFixture ? previousFallbackFixture : "";
        unsetenv("TIPE_TEST_FALLBACK_DICTIONARY");
        unsetenv("TIPE_DISABLE_LIBIME_LEARNING");
        setenv("TIPE_LIBIME_USER_HISTORY", learnedLibIMEHistoryPath.c_str(), 1);
        tipe::Dictionary learningDictionary;
        const auto beforeLearning = learningDictionary.lookup("gai");
        require(std::find(beforeLearning.begin(), beforeLearning.end(), "盖") != beforeLearning.end(),
                "LibIME learning test candidate is available before training");
        require(learningDictionary.learnLanguageModelSelection("gai", "盖"),
                "LibIME selection learning persists a valid decoder candidate");
        require(fileIsPrivate(learnedLibIMEHistoryPath) && fileIsPrivate(learnedLibIMEHistoryPath + ".lock") &&
                    !hasTemporarySibling(learnedLibIMEHistoryPath),
                "LibIME history writes and locks are private and atomic");

        std::filesystem::copy_file(learnedLibIMEHistoryPath, reloadedLibIMEHistoryPath,
                                   std::filesystem::copy_options::overwrite_existing);
        setenv("TIPE_LIBIME_USER_HISTORY", reloadedLibIMEHistoryPath.c_str(), 1);
        tipe::Dictionary reloadedLearningDictionary;
        const auto afterReload = reloadedLearningDictionary.lookup("gai");
        require(!afterReload.empty() && afterReload.front() == "盖",
                "persisted LibIME history changes ranking after a fresh backend load; first=" +
                    (afterReload.empty() ? std::string("<none>") : afterReload.front()));

        setenv("TIPE_DISABLE_LIBIME_LEARNING", "1", 1);
        setenv("TIPE_LIBIME_USER_HISTORY", isolatedLibIMEHistoryPath.c_str(), 1);
        if (previousFallbackFixture) {
            setenv("TIPE_TEST_FALLBACK_DICTIONARY", previousFallbackFixtureValue.c_str(), 1);
        }
    }
#endif
    {
        {
            std::ofstream dictionary(rareWordDictionaryPath);
            dictionary << "da\t龘\n";
            dictionary << "bing\t靐\n";
            dictionary << "nang\t齉\n";
        }
        setenv("TIPE_USER_DICTIONARY", rareWordDictionaryPath.c_str(), 1);
        tipe::State rareWordState({}, rareWordPreferencePath);
        const auto chooseRareWordInThreeSegments = [&rareWordState] {
            type(rareWordState, "dabingnang");
            auto selection = rareWordState.select(candidateIndex(rareWordState, "龘"));
            require(selection.accepted && selection.commitText == "龘" && rareWordState.preedit() == "bingnang",
                    "rare-word learning keeps the suffix after the first selected segment");
            selection = rareWordState.select(candidateIndex(rareWordState, "靐"));
            require(selection.accepted && selection.commitText == "靐" && rareWordState.preedit() == "nang",
                    "rare-word learning accumulates more than two selected segments");
            selection = rareWordState.select(candidateIndex(rareWordState, "齉"));
            require(selection.accepted && selection.commitText == "齉" && rareWordState.empty(),
                    "rare-word learning completes the final selected segment");
        };

        chooseRareWordInThreeSegments();
        tipe::Dictionary stagedDictionary;
        require(stagedDictionary.exactUserCandidates("dabingnang").empty(),
                "one segmented selection remains staged instead of polluting the user dictionary");
        chooseRareWordInThreeSegments();
        require(fileContains(rareWordDictionaryPath, "dabingnang\t龘靐齉\n") &&
                    fileIsPrivate(rareWordDictionaryPath) && !hasTemporarySibling(rareWordDictionaryPath),
                "the second identical segmented selection atomically learns the complete rare word");

        type(rareWordState, "dabingnang");
        require(!rareWordState.candidates().empty() && rareWordState.candidates()[0] == "龘靐齉",
                "a newly learned rare word is available immediately without model reranking");
        rareWordState.reset();
        tipe::State reloadedRareWordState({}, rareWordReloadPreferencePath);
        type(reloadedRareWordState, "dabingnang");
        require(!reloadedRareWordState.candidates().empty() && reloadedRareWordState.candidates()[0] == "龘靐齉",
                "a fresh state loads the automatically learned rare word first");
    }
    {
        setenv("TIPE_USER_DICTIONARY", isolatedDefaultUserDictionaryPath.c_str(), 1);
        {
            std::ofstream preferences(knownPinyinHabitPreferencePath);
            preferences << "__key_habit__\tmissing\t\tn\t6\n";
            preferences << "__key_habit__\tmissing\t\tg\t5\n";
        }
        tipe::State protectedKnownPinyin({}, knownPinyinHabitPreferencePath);
        type(protectedKnownPinyin, "shurufa");
        require(!protectedKnownPinyin.candidates().empty() && protectedKnownPinyin.candidates()[0] == "输入法",
                "global missing-key habits do not override an exact known pinyin entry");
        require(optionalCandidateIndex(protectedKnownPinyin, "输入反") == protectedKnownPinyin.candidates().size(),
                "global missing-key habits do not inject an unrelated completion for exact known pinyin");
        protectedKnownPinyin.reset();
        type(protectedKnownPinyin, "woxiangyo");
        require(!protectedKnownPinyin.candidates().empty() && protectedKnownPinyin.candidates()[0] == "我想用",
                "strong missing-key habits still repair a composable phrase without an exact dictionary entry");
    }
    {
        std::ofstream userDictionary(userDictionaryPath);
        userDictionary << "nihao\t你号\t你好啊\n";
        userDictionary << "zidingyi\t自定义\t字定义\n";
        userDictionary << "NiHao\t不应该出现\n";
        userDictionary << "abc123\t也不应该出现\n";
        userDictionary << "konghouxuan\t\t空候选不应该出现\n";
        userDictionary << "chongfu\t重复不应该出现\t重复不应该出现\n";
        userDictionary << "youxiahang\t有效行\n";
        userDictionary << "abc\t自搓\n";
        userDictionary << "abcnihao\t自搓你好\n";
        userDictionary.close();
        setenv("TIPE_USER_DICTIONARY", userDictionaryPath.c_str(), 1);
        tipe::State userDictionaryState({}, userDictionaryPreferencePath);
        type(userDictionaryState, "nihao");
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "你号",
                "user dictionary entries can define initial candidate order");
        require(userDictionaryState.candidates().size() >= 3 && userDictionaryState.candidates()[1] == "你好啊",
                "prefix candidates do not split the exact user dictionary candidate order");
        require(candidateIndex(userDictionaryState, "你") > candidateIndex(userDictionaryState, "你好啊"),
                "single-character prefix candidates stay behind exact user dictionary candidates");
        require(candidateIndex(userDictionaryState, "你好") < userDictionaryState.candidates().size(),
                "built-in candidates remain available behind user dictionary entries");
        require(std::find(userDictionaryState.candidates().begin(), userDictionaryState.candidates().end(), "不应该出现") ==
                    userDictionaryState.candidates().end(),
                "invalid user dictionary pinyin rows are ignored");
        userDictionaryState.reset();
        type(userDictionaryState, "konghouxuan");
        require(std::find(userDictionaryState.candidates().begin(), userDictionaryState.candidates().end(),
                          "空候选不应该出现") == userDictionaryState.candidates().end(),
                "user dictionary rows with empty candidate fields are ignored");
        userDictionaryState.reset();
        type(userDictionaryState, "chongfu");
        require(std::find(userDictionaryState.candidates().begin(), userDictionaryState.candidates().end(),
                          "重复不应该出现") == userDictionaryState.candidates().end(),
                "user dictionary rows with duplicate candidates are ignored");
        userDictionaryState.reset();
        type(userDictionaryState, "youxiahang");
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "有效行",
                "valid user dictionary rows after invalid rows still load");
        userDictionaryState.reset();
        type(userDictionaryState, "zidingyi");
        if (userDictionaryState.candidates().empty()) {
            std::fprintf(stderr, "user dictionary zidingyi: no candidates\n");
        } else if (userDictionaryState.candidates()[0] != "自定义") {
            std::fprintf(stderr, "user dictionary zidingyi first=%s size=%zu\n",
                         userDictionaryState.candidates()[0].c_str(), userDictionaryState.candidates().size());
        }
        std::fflush(stderr);
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "自定义",
                "user dictionary can add a new phrase");
        userDictionaryState.reset();
        type(userDictionaryState, "abcnihao");
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "自搓你好",
                "user dictionary can define a full phrase with a nonstandard prefix");
        require(candidateIndex(userDictionaryState, "自搓") < userDictionaryState.candidates().size(),
                "user dictionary exact prefixes are inserted into longer custom phrases");
        action = userDictionaryState.select(candidateIndex(userDictionaryState, "自搓"));
        require(action.accepted && action.commitText == "自搓",
                "user dictionary exact prefix can be committed from a longer custom phrase");
        require(userDictionaryState.preedit() == "nihao",
                "user dictionary exact prefix commit keeps the standard pinyin suffix");
        userDictionaryState.reset();
        std::ofstream updatedUserDictionary(userDictionaryPath);
        updatedUserDictionary << "nihao\t你好啊\t你号\n";
        updatedUserDictionary.close();
        const auto previousWriteTime = std::filesystem::last_write_time(userDictionaryPath);
        std::filesystem::last_write_time(userDictionaryPath, previousWriteTime + std::chrono::seconds(2));
        type(userDictionaryState, "nihao");
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "你好啊",
                "user dictionary reloads after the file changes");
        userDictionaryState.reset();
        std::ofstream crlfUserDictionary(userDictionaryPath, std::ios::binary);
        crlfUserDictionary << "nihao\t你号\r\n";
        crlfUserDictionary.close();
        const auto updatedWriteTime = std::filesystem::last_write_time(userDictionaryPath);
        std::filesystem::last_write_time(userDictionaryPath, updatedWriteTime + std::chrono::seconds(2));
        type(userDictionaryState, "nihao");
        require(!userDictionaryState.candidates().empty() && userDictionaryState.candidates()[0] == "你号",
                "user dictionary accepts CRLF lines without keeping carriage returns");
        setenv("TIPE_USER_DICTIONARY", isolatedDefaultUserDictionaryPath.c_str(), 1);
    }

    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'protocol\\t1' <<< \"$input\" || exit 1\n";
        script << "grep -q $'events\\t' <<< \"$input\" || exit 2\n";
        script << "grep -q 'letter:n' <<< \"$input\" || exit 3\n";
        script << "grep -q 'cursor-move:Down' <<< \"$input\" || exit 4\n";
        script << "grep -q 'observed:Tab' <<< \"$input\" || exit 5\n";
        script << "grep -q $'^state\\tpreedit_cursor\\t5\\tcandidate_cursor\\t6\\texpanded\\t1$' <<< \"$input\" || exit 6\n";
        script << "grep -q $'^selected_candidate\\t6\\t' <<< \"$input\" || exit 7\n";
        script << "grep -q $'^visible_candidates\\t' <<< \"$input\" || exit 8\n";
        script << "grep -q $'\\t6:' <<< \"$input\" || exit 9\n";
        script << "grep -q $'^numbered_candidates\\t1:6:' <<< \"$input\" || exit 10\n";
        script << "printf '%s\\n' '你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State externallyRanked({}, modelPreferencePath);
        action = externallyRanked.cursorMove("Down");
        require(!action.accepted, "empty cursor move before model request should pass through");
        type(externallyRanked, "nihao");
        action = externallyRanked.expandCandidates("Down");
        require(action.accepted, "model state payload test expands candidates");
        action = externallyRanked.moveCandidateCursor(static_cast<int>(tipe::visualCandidateColumns), "Down");
        require(action.accepted, "model state payload test moves candidate cursor");
        action = externallyRanked.observeKey("Tab");
        require(!action.accepted, "observed key before model request should pass through");
        action = externallyRanked.rerankCandidates("Alacritty");
        require(action.accepted && externallyRanked.candidates()[0] == "你号", "external rerank command can reorder candidates");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        const auto asyncPreferencePath = preferenceBase.string() + "-async-model.tsv";
        std::filesystem::remove(asyncPreferencePath);
        tipe::State asyncRanked({}, asyncPreferencePath);
        type(asyncRanked, "nihao");
        const auto request = asyncRanked.externalModelRequest("Alacritty");
        require(request && request->payload.find("protocol\t1") != std::string::npos &&
                    request->payload.find("state\tpreedit_cursor\t5\tcandidate_cursor\t0\texpanded\t0") !=
                        std::string::npos,
                "async model request captures the pre-rerank state and complete protocol payload");
        const auto preModelCandidates = asyncRanked.candidates();
        action = asyncRanked.beginExternalModelRerank();
        require(action.accepted && asyncRanked.candidatesExpanded() &&
                    asyncRanked.candidates() == preModelCandidates &&
                    asyncRanked.debugSnapshot().eventCounts.rerankRequests == 1,
                "async model request expands once without showing an interim candidate order");
        auto armedRequest = *request;
        asyncRanked.armExternalModelRequest(armedRequest);
        action = asyncRanked.applyExternalModelResponse(armedRequest, "candidate\t你号\n");
        require(action.accepted && asyncRanked.candidates()[0] == "你号",
                "async model response applies while the composition is unchanged");

        const auto stalePreferencePath = preferenceBase.string() + "-async-stale.tsv";
        std::filesystem::remove(stalePreferencePath);
        tipe::State staleAsyncRanked({}, stalePreferencePath);
        type(staleAsyncRanked, "nihao");
        const auto staleRequest = staleAsyncRanked.externalModelRequest("Alacritty");
        require(staleRequest.has_value(), "stale async model request is captured");
        action = staleAsyncRanked.beginExternalModelRerank();
        require(action.accepted, "stale async model test starts without an interim candidate reorder");
        auto armedStaleRequest = *staleRequest;
        staleAsyncRanked.armExternalModelRequest(armedStaleRequest);
        action = staleAsyncRanked.inputAscii('x');
        require(action.accepted && staleAsyncRanked.preedit() == "nihaox",
                "typing can continue while the model request is pending");
        action = staleAsyncRanked.applyExternalModelResponse(armedStaleRequest, "candidate\t你号\n");
        require(!action.accepted && staleAsyncRanked.preedit() == "nihaox",
                "stale async model response cannot overwrite later typing");
        std::filesystem::remove(asyncPreferencePath);
        std::filesystem::remove(stalePreferencePath);
    }
    {
        const auto continuousMarkerPath = preferenceBase.string() + "-continuous-called";
        std::filesystem::remove(continuousMarkerPath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "touch '" << continuousMarkerPath << "'\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State continuousRanked({}, modelPreferencePath);
        type(continuousRanked, "nihao");
        action = continuousRanked.rerankCandidates("Alacritty", {}, {}, false, false);
        require(action.accepted && !continuousRanked.candidatesExpanded(),
                "continuous rerank keeps the candidate panel collapsed");
        require(!std::filesystem::exists(continuousMarkerPath),
                "continuous rerank does not invoke the configured external model command");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(continuousMarkerPath);
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\tvite'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State externallyRawEnglish({}, modelPreferencePath);
        type(externallyRawEnglish, "vite");
        action = externallyRawEnglish.rerankCandidates("Alacritty");
        require(action.accepted && !externallyRawEnglish.candidates().empty() &&
                    externallyRawEnglish.candidates()[0] == "vite",
                "external model can add the current non-pinyin raw English-like preedit as a candidate");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\tnihao'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State externallyRawPinyin({}, modelPreferencePath);
        type(externallyRawPinyin, "nihao");
        action = externallyRawPinyin.rerankCandidates("Alacritty");
        require(action.accepted && !externallyRawPinyin.candidates().empty() &&
                    externallyRawPinyin.candidates()[0] != "nihao",
                "external model cannot add normal pinyin as a raw candidate");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "expected=$'application\\tCode\\\\tOSS'\n";
        script << "[[ \"$input\" == *\"$expected\"* ]] || exit 1\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State applicationRanked({}, modelPreferencePath);
        type(applicationRanked, "nihao");
        action = applicationRanked.rerankCandidates("Code\tOSS");
        require(action.accepted && applicationRanked.candidates()[0] == "你号",
                "external model request includes escaped current application context");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "expected_before=$'surrounding_before\\t刚才\\\\tPath\\\\\\\\Name'\n";
        script << "expected_after=$'surrounding_after\\t后面\\\\nText'\n";
        script << "[[ \"$input\" == *\"$expected_before\"* ]] || exit 1\n";
        script << "[[ \"$input\" == *\"$expected_after\"* ]] || exit 2\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State surroundingRanked({}, modelPreferencePath);
        type(surroundingRanked, "nihao");
        action = surroundingRanked.rerankCandidates("Alacritty", "刚才\tPath\\Name", "后面\nText");
        require(action.accepted && surroundingRanked.candidates()[0] == "你号",
                "external model request includes escaped surrounding text context");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^candidate_metadata\\t1\\tconsumed_prefix\\t4\\tsource\\tprefix\\tscore\\t' <<< \"$input\" || exit 1\n";
        script << "printf '%s\\n' $'candidate\\t继续'\n";
        script << "printf '%s\\n' $'preference\\tjixuzuo\\t继续\\t5'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State externallyRankedPrefix({}, modelPreferencePath);
        type(externallyRankedPrefix, "jixuzuo");
        action = externallyRankedPrefix.rerankCandidates();
        require(action.accepted && !externallyRankedPrefix.candidates().empty() &&
                    externallyRankedPrefix.candidates()[0] == "继续",
                "external model can promote a prefix candidate");
        require(externallyRankedPrefix.candidateConsumedPrefixLength(0) == 4,
                "external model rerank preserves prefix-consumption metadata");
        require(externallyRankedPrefix.candidateSource(0).find("prefix") != std::string::npos,
                "external model rerank preserves candidate semantic source");
        action = externallyRankedPrefix.space();
        require(action.accepted && action.commitText == "继续",
                "space can commit an externally ranked first-position prefix candidate");
        require(externallyRankedPrefix.preedit() == "zuo",
                "externally ranked prefix candidate keeps remaining pinyin after space");
        require(!fileContains(modelPreferencePath, "jixuzuo\t继续\t"),
                "external prefix-only candidate rerank must not persist a full-preedit preference");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'correction\\tihao\\tnihao'\n";
        script << "printf '%s\\n' $'candidate\\t继续'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State externallyCorrectedPrefix({}, modelPreferencePath);
        type(externallyCorrectedPrefix, "jixuzuo");
        action = externallyCorrectedPrefix.rerankCandidates();
        const auto correctedPrefixIndex = candidateIndex(externallyCorrectedPrefix, "继续");
        require(externallyCorrectedPrefix.candidateConsumedPrefixLength(correctedPrefixIndex) == 4,
                "external correction refresh preserves existing prefix-consumption metadata");
        require(externallyCorrectedPrefix.candidateSource(correctedPrefixIndex).find("prefix") != std::string::npos,
                "external correction refresh preserves existing prefix source");
        action = externallyCorrectedPrefix.select(correctedPrefixIndex);
        require(action.accepted && action.commitText == "继续" && externallyCorrectedPrefix.preedit() == "zuo",
                "external correction refresh still lets prefix selection keep remaining pinyin");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^context\\t你好$' <<< \"$input\" || exit 1\n";
        script << "printf '%s\\n' $'candidate\\t世界'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State contextualRanked({}, modelPreferencePath);
        type(contextualRanked, "nihao");
        action = contextualRanked.select(candidateIndex(contextualRanked, "你好"));
        require(action.accepted && action.commitText == "你好", "context test first commits nihao");
        type(contextualRanked, "shijie");
        action = contextualRanked.rerankCandidates();
        require(action.accepted && contextualRanked.candidates()[0] == "世界",
                "external model request includes recent committed text context");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        const auto modelPreferencePayloadPath = preferenceBase.string() + "-model-preference-payload.tsv";
        std::filesystem::remove(modelPreferencePayloadPath);
        {
            tipe::State preferenceTrainer({}, modelPreferencePayloadPath);
            type(preferenceTrainer, "shijie");
            action = preferenceTrainer.select(candidateIndex(preferenceTrainer, "时节"));
            require(action.accepted && action.commitText == "时节", "model preference payload test trains shijie");
            type(preferenceTrainer, "nihao");
            action = preferenceTrainer.select(candidateIndex(preferenceTrainer, "你号"));
            require(action.accepted && action.commitText == "你号", "model preference payload test trains unrelated nihao");
        }
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^preference\\tshijie\\t时节\\t3$' <<< \"$input\" || exit 1\n";
        script << "! grep -q $'^preference\\tnihao\\t你号' <<< \"$input\" || exit 2\n";
        script << "printf '%s\\n' $'candidate\\t世界'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State preferencePayloadRanked({}, modelPreferencePayloadPath);
        type(preferencePayloadRanked, "shijie");
        action = preferencePayloadRanked.rerankCandidates();
        require(action.accepted && preferencePayloadRanked.candidates()[0] == "世界",
                "external model request includes bounded current-preedit preference rows");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(modelPreferencePayloadPath);
    }
    {
        const auto modelSegmentChainPayloadPath = preferenceBase.string() + "-model-segment-chain-payload.tsv";
        std::filesystem::remove(modelSegmentChainPayloadPath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^segment_chain\\tjixuzuo\\tjixu\\t继续\\tzuo\\tjixuzuo\\t继续做$' <<< \"$input\" || exit 1\n";
        script << "grep -q $'^context\\t继续\\t做$' <<< \"$input\" || exit 2\n";
        script << "! grep -q $'^context.*继续做' <<< \"$input\" || exit 3\n";
        script << "printf '%s\\n' $'candidate\\t继续做'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State segmentChainPayloadRanked({}, modelSegmentChainPayloadPath);
        type(segmentChainPayloadRanked, "jixuzuo");
        action = segmentChainPayloadRanked.select(candidateIndex(segmentChainPayloadRanked, "继续"));
        require(action.accepted && action.commitText == "继续",
                "model segment-chain payload test commits the prefix candidate");
        action = segmentChainPayloadRanked.select(candidateIndex(segmentChainPayloadRanked, "做"));
        require(action.accepted && action.commitText == "做",
                "model segment-chain payload test commits the suffix candidate");
        type(segmentChainPayloadRanked, "jixuzuo");
        action = segmentChainPayloadRanked.rerankCandidates();
        require(action.accepted && !segmentChainPayloadRanked.candidates().empty() &&
                    segmentChainPayloadRanked.candidates()[0] == "继续做",
                "external model request includes segment chains without context pollution");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(modelSegmentChainPayloadPath);
    }
    {
        const auto modelEditCorrectionPayloadPath = preferenceBase.string() + "-model-edit-correction-payload.tsv";
        std::filesystem::remove(modelEditCorrectionPayloadPath);
        {
            tipe::State editCorrectionTrainer({}, modelEditCorrectionPayloadPath);
            for (int attempt = 0; attempt < 2; ++attempt) {
                type(editCorrectionTrainer, "nhao");
                for (int index = 0; index < 3; ++index) {
                    action = editCorrectionTrainer.cursorMove("Left");
                    require(action.accepted, "model edit-correction payload test can move to insert point");
                }
                action = editCorrectionTrainer.inputAscii('i');
                require(action.accepted && editCorrectionTrainer.preedit() == "nihao",
                        "model edit-correction payload test inserts missing letter");
                action = editCorrectionTrainer.select(candidateIndex(editCorrectionTrainer, "你好"));
                require(action.accepted && action.commitText == "你好",
                        "model edit-correction payload test commits inserted correction");

                type(editCorrectionTrainer, "niyhao");
                for (int index = 0; index < 4; ++index) {
                    action = editCorrectionTrainer.cursorMove("Left");
                    require(action.accepted, "model edit-correction payload test can move to delete point");
                }
                action = editCorrectionTrainer.deleteKey();
                require(action.accepted && editCorrectionTrainer.preedit() == "nihao",
                        "model edit-correction payload test deletes extra middle letter");
                action = editCorrectionTrainer.select(candidateIndex(editCorrectionTrainer, "你好"));
                require(action.accepted && action.commitText == "你好",
                        "model edit-correction payload test commits delete correction");
            }
        }
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^correction\\tnhao\\tnihao\\t' <<< \"$input\" || exit 1\n";
        script << "grep -q $'^correction\\tniyhao\\tnihao\\t' <<< \"$input\" || exit 2\n";
        script << "printf '%s\\n' $'candidate\\t你好'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State editCorrectionPayloadRanked({}, modelEditCorrectionPayloadPath);
        type(editCorrectionPayloadRanked, "nhao");
        action = editCorrectionPayloadRanked.rerankCandidates();
        require(action.accepted && editCorrectionPayloadRanked.candidates()[0] == "你好",
                "external model request includes cursor-edit and delete learned correction rows");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(modelEditCorrectionPayloadPath);
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "events_line_count=$(grep -c $'^events\\t' <<< \"$input\")\n";
        script << "[ \"$events_line_count\" = 1 ] || exit 1\n";
        script << "expected=$'observed:Tab\\\\tLine\\\\nSlash\\\\\\\\'\n";
        script << "[[ \"$input\" == *\"$expected\"* ]] || exit 2\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State escapedPayload({}, modelPreferencePath);
        type(escapedPayload, "nihao");
        action = escapedPayload.observeKey("Tab\tLine\nSlash\\");
        require(!action.accepted, "observed key with control characters should pass through");
        action = escapedPayload.rerankCandidates();
        require(action.accepted && escapedPayload.candidates()[0] == "你号",
                "external model payload escapes event text control characters");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "correction_events_line_count=$(grep -c $'^correction_events\\t' <<< \"$input\")\n";
        script << "[ \"$correction_events_line_count\" = 1 ] || exit 1\n";
        script << "expected=$'letter:i\\tletter:h\\tletter:a\\tletter:o\\tbackspace:\\tbackspace:\\tbackspace:\\tbackspace:\\tletter:n\\tletter:i\\tletter:h\\tletter:a\\tletter:o'\n";
        script << "[[ \"$input\" == *\"$expected\"* ]] || exit 2\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State correctionTrailPayload({}, modelPreferencePath);
        type(correctionTrailPayload, "ihao");
        for (int index = 0; index < 4; ++index) {
            action = correctionTrailPayload.backspace();
            require(action.accepted, "correction trail payload test deletes typo preedit");
        }
        type(correctionTrailPayload, "nihao");
        action = correctionTrailPayload.rerankCandidates();
        require(action.accepted && correctionTrailPayload.candidates()[0] == "你号",
                "external model request includes long correction-event trail");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "correction_events_line_count=$(grep -c $'^correction_events\\t' <<< \"$input\")\n";
        script << "[ \"$correction_events_line_count\" = 1 ] || exit 1\n";
        script << "expected=$'space:\\tdelete:\\tcursor-move:Down\\tobserved:Tab\\tletter:n\\tletter:i\\tletter:h\\tletter:a\\tletter:o\\trerank-requested:nihao'\n";
        script << "[[ \"$input\" == *\"$expected\"* ]] || exit 2\n";
        script << "grep -q $'^event_counts\\tspace:1\\tdelete:1\\tcursor-move:1\\tobserved:1\\tletter:5\\trerank-requested:1$' <<< \"$input\" || exit 3\n";
        script << "grep -q $'^correction_event_counts\\tspace:1\\tdelete:1\\tcursor-move:1\\tobserved:1\\tletter:5\\trerank-requested:1$' <<< \"$input\" || exit 4\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State broaderKeyTrailPayload({}, modelPreferencePath);
        action = broaderKeyTrailPayload.space();
        require(!action.accepted, "empty space before model request should pass through");
        action = broaderKeyTrailPayload.deleteKey();
        require(!action.accepted, "empty delete before model request should pass through");
        action = broaderKeyTrailPayload.cursorMove("Down");
        require(!action.accepted, "empty cursor move before broader key trail request should pass through");
        action = broaderKeyTrailPayload.observeKey("Tab");
        require(!action.accepted, "empty observed key before broader key trail request should pass through");
        type(broaderKeyTrailPayload, "nihao");
        action = broaderKeyTrailPayload.rerankCandidates();
        require(action.accepted && broaderKeyTrailPayload.candidates()[0] == "你号",
                "external model request includes broader key trail");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State explicitlyRanked({}, modelPreferencePath);
        type(explicitlyRanked, "nihao");
        action = explicitlyRanked.rerankCandidates();
        require(action.accepted && explicitlyRanked.candidates()[0] == "你号",
                "external model protocol accepts explicit candidate lines");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelSuggestion({}, modelCandidateLearningPreferencePath);
        type(modelSuggestion, "nihao");
        action = modelSuggestion.rerankCandidates();
        require(action.accepted && modelSuggestion.candidates()[0] == "你号",
                "external model candidate suggestion applies immediately");
        unsetenv("TIPE_MODEL_COMMAND");

        tipe::State learnedModelSuggestion({}, modelCandidateLearningPreferencePath);
        type(learnedModelSuggestion, "nihao");
        require(!learnedModelSuggestion.candidates().empty() && learnedModelSuggestion.candidates()[0] == "你号",
                "external model candidate suggestion persists as a lightweight local preference");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你号' $'candidate\\t你毫' $'candidate\\t泥好'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State fullModelOrdering({}, modelCandidateLearningPreferencePath);
        type(fullModelOrdering, "nihao");
        action = fullModelOrdering.rerankCandidates();
        require(action.accepted && fullModelOrdering.candidates()[0] == "你号",
                "external model can return a complete candidate ordering");
        unsetenv("TIPE_MODEL_COMMAND");
        require(fileContains(modelCandidateLearningPreferencePath, "nihao\t你号\t2") &&
                    !fileContains(modelCandidateLearningPreferencePath, "nihao\t你毫") &&
                    !fileContains(modelCandidateLearningPreferencePath, "nihao\t泥好"),
                "complete model ordering learns only the candidate promoted to first place");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        {
            std::ofstream preferences(modelCandidateLearningPreferencePath);
            preferences << "nihao\t你号\t18446744073709551615\n";
        }
        tipe::State corruptPreferenceCount({}, modelCandidateLearningPreferencePath);
        type(corruptPreferenceCount, "nihao");
        require(!corruptPreferenceCount.candidates().empty() && corruptPreferenceCount.candidates()[0] == "你好",
                "out-of-range persisted counts cannot override normal candidate ranking");
        action = corruptPreferenceCount.select(0);
        require(action.accepted && !fileContains(modelCandidateLearningPreferencePath, "18446744073709551615"),
                "the next valid learning write drops an out-of-range persisted count");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        {
            std::ofstream preferences(modelCandidateLearningPreferencePath);
            preferences << "nihao\t你号\t1\n";
        }
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State staleCandidateEvidence({}, modelCandidateLearningPreferencePath);
        type(staleCandidateEvidence, "nihao");
        action = staleCandidateEvidence.rerankCandidates();
        require(action.accepted && staleCandidateEvidence.candidates()[0] == "你号",
                "external model can promote a candidate despite retained one-count evidence");
        unsetenv("TIPE_MODEL_COMMAND");
        require(fileContains(modelCandidateLearningPreferencePath, "nihao\t你号\t3"),
                "external model activation adds to inactive candidate evidence instead of being blocked by it");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        {
            std::ofstream preferences(modelCandidateLearningPreferencePath);
            preferences << "vite\tvite\t2\n";
        }
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\tvite'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State staleRawEvidence({}, modelCandidateLearningPreferencePath);
        type(staleRawEvidence, "vite");
        action = staleRawEvidence.rerankCandidates();
        require(action.accepted && staleRawEvidence.candidates()[0] == "vite",
                "external model can promote raw English despite retained two-count raw evidence");
        unsetenv("TIPE_MODEL_COMMAND");
        require(fileContains(modelCandidateLearningPreferencePath, "vite\tvite\t5"),
                "external model activation adds to inactive raw evidence instead of being blocked by it");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你好' $'candidate\\t你号' $'candidate\\t你毫'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelTopEcho({}, modelCandidateLearningPreferencePath);
        type(modelTopEcho, "nihao");
        action = modelTopEcho.rerankCandidates();
        require(action.accepted && !modelTopEcho.candidates().empty() && modelTopEcho.candidates()[0] == "你好",
                "external model can echo the current top candidate without changing ranking");
        unsetenv("TIPE_MODEL_COMMAND");
        require(!std::filesystem::exists(modelCandidateLearningPreferencePath) ||
                    std::filesystem::file_size(modelCandidateLearningPreferencePath) == 0,
                "external model top-candidate echo and its tail are not persisted as learned preferences");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'preference\\tnihao\\t你号\\t6'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelPreferenceAction({}, modelCandidateLearningPreferencePath);
        type(modelPreferenceAction, "nihao");
        action = modelPreferenceAction.rerankCandidates();
        require(action.accepted, "external model preference action is accepted");
        unsetenv("TIPE_MODEL_COMMAND");

        tipe::State learnedModelPreferenceAction({}, modelCandidateLearningPreferencePath);
        type(learnedModelPreferenceAction, "nihao");
        require(!learnedModelPreferenceAction.candidates().empty() &&
                    learnedModelPreferenceAction.candidates()[0] == "你号",
                "external model preference action persists as local candidate learning");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'segment_chain\\twoc\\two\\t我\\tc\\twocao\\t我操\\t5'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelSegmentChainAction({}, modelCandidateLearningPreferencePath);
        type(modelSegmentChainAction, "woc");
        action = modelSegmentChainAction.rerankCandidates();
        require(action.accepted && !modelSegmentChainAction.candidates().empty() &&
                    modelSegmentChainAction.candidates()[0] == "我操",
                "external model segment-chain action applies immediately");
        unsetenv("TIPE_MODEL_COMMAND");
        require(fileContains(modelCandidateLearningPreferencePath,
                             "__segment_chain__\twoc\two\t我\tc\twocao\t我操\t5"),
                "external model segment-chain action persists reusable chain evidence");
        require(fileContains(modelCandidateLearningPreferencePath, "woc\t我操\t5"),
                "external model segment-chain action persists a full-preedit preference");
        require(fileContains(modelCandidateLearningPreferencePath, "__correction__\twoc\twocao\t2"),
                "external model segment-chain action persists its implied typo correction");

        tipe::State learnedModelSegmentChain({}, modelCandidateLearningPreferencePath);
        type(learnedModelSegmentChain, "woc");
        require(!learnedModelSegmentChain.candidates().empty() &&
                    learnedModelSegmentChain.candidates()[0] == "我操",
                "persisted external segment-chain learning affects later local ranking");
        learnedModelSegmentChain.reset();
        type(learnedModelSegmentChain, "wo");
        action = learnedModelSegmentChain.select(candidateIndex(learnedModelSegmentChain, "我"));
        require(action.accepted && action.commitText == "我", "segment-chain continuation test commits prefix");
        type(learnedModelSegmentChain, "c");
        require(!learnedModelSegmentChain.candidates().empty() &&
                    learnedModelSegmentChain.candidates()[0] == "操",
                "persisted external segment-chain learning boosts the learned suffix after the prefix");
        const auto suffixModelRequest = learnedModelSegmentChain.modelRequestSnapshot();
        require(suffixModelRequest.find("segment_chain\twoc\two\t我\tc\twocao\t我操") != std::string::npos,
                "model requests for a remaining suffix include stored segment chains that match the recent prefix commit");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "input=$(cat)\n";
        script << "grep -q $'^pending_segment\\twoc\\two\\t我\\tc$' <<< \"$input\" || exit 1\n";
        script << "printf '%s\\n' $'segment_chain\\twoc\\two\\t我\\tc\\twoc\\t我操\\t5'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State passivePendingSegmentChainAction({}, modelCandidateLearningPreferencePath);
        type(passivePendingSegmentChainAction, "woc");
        action = passivePendingSegmentChainAction.select(candidateIndex(passivePendingSegmentChainAction, "我"));
        require(action.accepted && action.commitText == "我" && passivePendingSegmentChainAction.preedit() == "c",
                "passive pending segment-chain test commits prefix and keeps suffix");
        action = passivePendingSegmentChainAction.rerankCandidates();
        require(action.accepted, "external model output is allowed to run while passive suffix remains pending");
        unsetenv("TIPE_MODEL_COMMAND");
        require(!fileContains(modelCandidateLearningPreferencePath, "__segment_chain__\twoc\two\t我\tc\twoc\t我操"),
                "external pending segment-chain action does not persist from passive top suffix highlight");
        require(!fileContains(modelCandidateLearningPreferencePath, "woc\t我操"),
                "external pending segment-chain action does not persist full-preedit preference from passive top suffix");
        std::filesystem::remove(modelCandidateLearningPreferencePath);

        std::ofstream explicitScript(rerankScriptPath);
        explicitScript << "#!/usr/bin/env bash\n";
        explicitScript << "input=$(cat)\n";
        explicitScript << "grep -q $'^pending_segment\\twoc\\two\\t我\\tc$' <<< \"$input\" || exit 1\n";
        explicitScript << "printf '%s\\n' $'segment_chain\\twoc\\two\\t我\\tc\\twoc\\t我从\\t5'\n";
        explicitScript.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelPendingSegmentChainAction({}, modelCandidateLearningPreferencePath);
        type(modelPendingSegmentChainAction, "woc");
        action = modelPendingSegmentChainAction.select(candidateIndex(modelPendingSegmentChainAction, "我"));
        require(action.accepted && action.commitText == "我" && modelPendingSegmentChainAction.preedit() == "c",
                "external pending segment-chain test commits prefix and keeps suffix");
        action = modelPendingSegmentChainAction.moveCandidateCursorTo(candidateIndex(modelPendingSegmentChainAction, "从"),
                                                                      "End");
        require(action.accepted && modelPendingSegmentChainAction.candidateCursorIndex() ==
                                      candidateIndex(modelPendingSegmentChainAction, "从"),
                "external pending segment-chain test explicitly selects a non-leading suffix");
        action = modelPendingSegmentChainAction.rerankCandidates();
        require(action.accepted, "external model segment-chain action from pending suffix is accepted");
        unsetenv("TIPE_MODEL_COMMAND");
        require(fileContains(modelCandidateLearningPreferencePath, "__segment_chain__\twoc\two\t我\tc\twoc\t我从\t5"),
                "external model segment-chain action from pending suffix persists reusable chain evidence");
        require(fileContains(modelCandidateLearningPreferencePath, "woc\t我从\t5"),
                "external model segment-chain action from pending suffix persists a full-preedit preference");

        tipe::State learnedPendingSegmentChain({}, modelCandidateLearningPreferencePath);
        type(learnedPendingSegmentChain, "woc");
        require(!learnedPendingSegmentChain.candidates().empty() && learnedPendingSegmentChain.candidates()[0] == "我从",
                "persisted pending suffix segment-chain learning affects later full-preedit ranking");
        learnedPendingSegmentChain.reset();
        type(learnedPendingSegmentChain, "wo");
        action = learnedPendingSegmentChain.select(candidateIndex(learnedPendingSegmentChain, "我"));
        require(action.accepted && action.commitText == "我", "pending suffix continuation test commits prefix");
        type(learnedPendingSegmentChain, "c");
        require(!learnedPendingSegmentChain.candidates().empty() && learnedPendingSegmentChain.candidates()[0] == "从",
                "persisted pending suffix segment-chain learning boosts the learned suffix after the prefix");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'segment_chain\\twoc\\two\\t我\\tx\\twocao\\t我操\\t5'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State invalidModelSegmentChain({}, modelCandidateLearningPreferencePath);
        type(invalidModelSegmentChain, "woc");
        invalidModelSegmentChain.rerankCandidates();
        unsetenv("TIPE_MODEL_COMMAND");
        require(!fileContains(modelCandidateLearningPreferencePath, "__segment_chain__\twoc\two\t我\tx\twocao\t我操"),
                "external model segment-chain learning rejects impossible consumed/remaining shape");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\tvite'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelRawSuggestion({}, modelCandidateLearningPreferencePath);
        type(modelRawSuggestion, "vite");
        action = modelRawSuggestion.rerankCandidates();
        require(action.accepted && !modelRawSuggestion.candidates().empty() &&
                    modelRawSuggestion.candidates()[0] == "vite",
                "external model raw-English suggestion for a non-pinyin token applies immediately");
        unsetenv("TIPE_MODEL_COMMAND");

        tipe::State learnedRawSuggestion({}, modelCandidateLearningPreferencePath);
        type(learnedRawSuggestion, "vite");
        require(!learnedRawSuggestion.candidates().empty() && learnedRawSuggestion.candidates()[0] == "vite",
                "external model raw-English suggestion for a non-pinyin token persists as a local raw preference");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'preference\\tvite\\tvite\\t4'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelRawPreference({}, modelCandidateLearningPreferencePath);
        type(modelRawPreference, "vite");
        action = modelRawPreference.rerankCandidates();
        require(action.accepted && !modelRawPreference.candidates().empty() &&
                    modelRawPreference.candidates()[0] == "vite",
                "external model raw-English preference for a non-pinyin token applies immediately");
        unsetenv("TIPE_MODEL_COMMAND");

        tipe::State learnedRawPreference({}, modelCandidateLearningPreferencePath);
        type(learnedRawPreference, "vite");
        require(!learnedRawPreference.candidates().empty() && learnedRawPreference.candidates()[0] == "vite",
                "external model raw-English preference for a non-pinyin token persists as a local raw preference");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::filesystem::remove(modelCandidateLearningPreferencePath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'preference\\tnihao\\tnihao\\t9'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelRawPinyinPreference({}, modelCandidateLearningPreferencePath);
        type(modelRawPinyinPreference, "nihao");
        action = modelRawPinyinPreference.rerankCandidates();
        require(action.accepted && !modelRawPinyinPreference.candidates().empty() &&
                    modelRawPinyinPreference.candidates()[0] != "nihao",
                "external model raw preference action cannot teach normal pinyin as raw English");
        unsetenv("TIPE_MODEL_COMMAND");
        std::filesystem::remove(modelCandidateLearningPreferencePath);
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf 'candidate\\t你号\\r\\n'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State crlfRanked({}, modelPreferencePath);
        type(crlfRanked, "nihao");
        action = crlfRanked.rerankCandidates();
        require(action.accepted && crlfRanked.candidates()[0] == "你号",
                "external model protocol accepts CRLF candidate lines");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf 'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State noNewlineRanked({}, modelPreferencePath);
        type(noNewlineRanked, "nihao");
        action = noNewlineRanked.rerankCandidates();
        require(action.accepted && noNewlineRanked.candidates()[0] == "你号",
                "external model protocol accepts final candidate line without trailing newline");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "[ \"${TIPE_TEST_RERANK:-}\" = yes ] || exit 6\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        const auto command = "TIPE_TEST_RERANK=yes " + rerankScriptPath;
        setenv("TIPE_MODEL_COMMAND", command.c_str(), 1);
        tipe::State envRanked({}, modelPreferencePath);
        type(envRanked, "nihao");
        action = envRanked.rerankCandidates();
        require(action.accepted && envRanked.candidates()[0] == "你号",
                "external model command supports simple environment assignments without shell");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "[ \"${TIPE_TEST_RERANK:-}\" = yes ] || exit 6\n";
        script << "[ \"${1:-}\" = --prefer ] || exit 7\n";
        script << "[ \"${2:-}\" = second ] || exit 8\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        const auto command = "TIPE_TEST_RERANK=yes " + rerankScriptPath + " --prefer second";
        setenv("TIPE_MODEL_COMMAND", command.c_str(), 1);
        tipe::State argumentRanked({}, modelPreferencePath);
        type(argumentRanked, "nihao");
        action = argumentRanked.rerankCandidates();
        require(action.accepted && argumentRanked.candidates()[0] == "你号",
                "external model command supports safe arguments without shell");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'correction\\tihao\\tnihao'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelCorrected({}, modelPreferencePath);
        type(modelCorrected, "ihao");
        action = modelCorrected.rerankCandidates();
        require(action.accepted && !modelCorrected.candidates().empty() && modelCorrected.candidates()[0] == "你好",
                "external model correction can refresh candidates for typo preedit");
        modelCorrected.reset();
        type(modelCorrected, "ihao");
        action = modelCorrected.rerankCandidates();
        require(action.accepted && !modelCorrected.candidates().empty() && modelCorrected.candidates()[0] == "你好",
                "repeated external model correction still refreshes typo candidates");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf 'correction\\tihao\\tnihao\\r\\n'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State crlfModelCorrected({}, modelPreferencePath);
        type(crlfModelCorrected, "ihao");
        action = crlfModelCorrected.rerankCandidates();
        require(action.accepted && !crlfModelCorrected.candidates().empty() &&
                    crlfModelCorrected.candidates()[0] == "你好",
                "external model protocol accepts CRLF correction lines");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf 'correction\\tihao\\tnihao'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State noNewlineModelCorrected({}, modelPreferencePath);
        type(noNewlineModelCorrected, "ihao");
        action = noNewlineModelCorrected.rerankCandidates();
        require(action.accepted && !noNewlineModelCorrected.candidates().empty() &&
                    noNewlineModelCorrected.candidates()[0] == "你好",
                "external model protocol accepts final correction line without trailing newline");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'correction\\tihao\\tnihao\\t2'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State modelCorrectedWithCount({}, modelPreferencePath);
        type(modelCorrectedWithCount, "ihao");
        action = modelCorrectedWithCount.rerankCandidates();
        require(action.accepted && !modelCorrectedWithCount.candidates().empty() &&
                    modelCorrectedWithCount.candidates()[0] == "你好",
                "external model correction ignores optional trailing count fields");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'correction\\tihao\\tzzzz'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State invalidModelCorrection({}, invalidModelPreferencePath);
        type(invalidModelCorrection, "ihao");
        action = invalidModelCorrection.rerankCandidates();
        require(action.accepted &&
                    std::find(invalidModelCorrection.candidates().begin(), invalidModelCorrection.candidates().end(),
                              "zzzz") == invalidModelCorrection.candidates().end(),
                "implausible external model corrections are ignored by the core model");
        invalidModelCorrection.reset();
        type(invalidModelCorrection, "ihao");
        require(std::find(invalidModelCorrection.candidates().begin(), invalidModelCorrection.candidates().end(),
                          "你好") == invalidModelCorrection.candidates().end(),
                "implausible external model corrections are not persisted");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'correction\\tnhao\\tnihao'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        tipe::State unrelatedModelCorrection({}, invalidModelPreferencePath);
        type(unrelatedModelCorrection, "ihao");
        action = unrelatedModelCorrection.rerankCandidates();
        require(action.accepted, "unrelated external model correction still leaves rerank action handled");
        require(!fileContains(invalidModelPreferencePath, "__correction__\tnhao\tnihao"),
                "external model corrections unrelated to the current request are not persisted");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "sleep 2\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        setenv("TIPE_MODEL_TIMEOUT_SECONDS", "1", 1);
        std::filesystem::remove(modelTimeoutPreferencePath);
        tipe::State timedOutModel({}, modelTimeoutPreferencePath);
        type(timedOutModel, "nihao");
        action = timedOutModel.rerankCandidates();
        require(action.accepted && timedOutModel.candidates()[0] == "你好",
                "external model timeout falls back to built-in candidate order");
        unsetenv("TIPE_MODEL_COMMAND");
        unsetenv("TIPE_MODEL_TIMEOUT_SECONDS");
    }
    {
        std::filesystem::remove(modelDescendantMarkerPath);
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "(sleep 1.2; printf survived >'" << modelDescendantMarkerPath << "') &\n";
        script << "sleep 5\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        setenv("TIPE_MODEL_TIMEOUT_SECONDS", "1", 1);
        tipe::State timedOutModelWithDescendant({}, modelTimeoutPreferencePath);
        type(timedOutModelWithDescendant, "nihao");
        action = timedOutModelWithDescendant.rerankCandidates();
        require(action.accepted && timedOutModelWithDescendant.candidates()[0] == "你好",
                "timed-out model with descendants falls back to built-in candidate order");
        usleep(500000);
        require(!std::filesystem::exists(modelDescendantMarkerPath),
                "timing out a model command terminates its descendant process group");
        unsetenv("TIPE_MODEL_COMMAND");
        unsetenv("TIPE_MODEL_TIMEOUT_SECONDS");
    }
    {
        std::ofstream script(rerankScriptPath);
        script << "#!/usr/bin/env bash\n";
        script << "printf '%s\\n' $'candidate\\t你号'\n";
        script << "exec >/dev/null\n";
        script << "sleep 2\n";
        script.close();
        std::filesystem::permissions(rerankScriptPath, std::filesystem::perms::owner_exec |
                                                          std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_write);
        setenv("TIPE_MODEL_COMMAND", rerankScriptPath.c_str(), 1);
        setenv("TIPE_MODEL_TIMEOUT_SECONDS", "1", 1);
        std::filesystem::remove(modelTimeoutPreferencePath);
        tipe::State closedStdoutButStillRunningModel({}, modelTimeoutPreferencePath);
        type(closedStdoutButStillRunningModel, "nihao");
        action = closedStdoutButStillRunningModel.rerankCandidates();
        require(action.accepted && closedStdoutButStillRunningModel.candidates()[0] == "你好",
                "external model timeout still applies after stdout closes");
        unsetenv("TIPE_MODEL_COMMAND");
        unsetenv("TIPE_MODEL_TIMEOUT_SECONDS");
    }
    {
        setenv("TIPE_MODEL_COMMAND", "printf injected; printf bad", 1);
        std::filesystem::remove(modelTimeoutPreferencePath);
        tipe::State unsafeCommand({}, modelTimeoutPreferencePath);
        type(unsafeCommand, "nihao");
        action = unsafeCommand.rerankCandidates();
        require(action.accepted && unsafeCommand.candidates()[0] == "你好",
                "unsafe model command is ignored and falls back to built-in order");
        unsetenv("TIPE_MODEL_COMMAND");
    }
    {
        {
            std::ofstream oversizedPreferences(trimPreferencePath);
            for (int index = 0; index < 2200; ++index) {
                const auto number = std::to_string(index);
                const auto suffix = std::string(4 - number.size(), '0') + number;
                oversizedPreferences << "p" << suffix << '\t' << "c" << suffix << "\t2\n";
            }
        }
        tipe::InputModel trimModel(trimPreferencePath);
        const auto retainedRequest = trimModel.modelRequest("p0000", {"base", "c0000"});
        const auto droppedRequest = trimModel.modelRequest("p2199", {"base", "c2199"});
        require(retainedRequest.find("preference\tp0000\tc0000\t2") != std::string::npos &&
                    droppedRequest.find("preference\tp2199\tc2199\t2") == std::string::npos,
                "loading an oversized preference file applies the same deterministic in-memory row cap");
        trimModel.recordCandidateSelection("latest", "最新", 3);
        require(lineCount(trimPreferencePath) == 2048, "saved candidate preferences are capped");
        require(fileContains(trimPreferencePath, "latest\t最新\t3"),
                "new candidate learning survives trimming a full preference file");
    }
    {
        const auto unsafePreferencePath = preferenceBase.string() + "-unsafe.tsv";
        std::filesystem::remove(unsafePreferencePath);
        tipe::InputModel unsafeModel(unsafePreferencePath);
        unsafeModel.recordCandidateSelection("nihao", "你\t号");
        unsafeModel.recordRawCommit("bad\nraw");
        require(!std::filesystem::exists(unsafePreferencePath) || lineCount(unsafePreferencePath) == 0,
                "unsafe stored text is not persisted into preference TSV");
        std::filesystem::remove(unsafePreferencePath);
    }

    std::filesystem::remove(mainPreferencePath);
    std::filesystem::remove(cleanPreferencePath);
    std::filesystem::remove(enterCorrectionPreferencePath);
    std::filesystem::remove(noisyCorrectionPreferencePath);
    std::filesystem::remove(genericCorrectionPreferencePath);
    std::filesystem::remove(generalizedPatternPreferencePath);
    std::filesystem::remove(correctionConflictPreferencePath);
    std::filesystem::remove(partialRewriteCorrectionPreferencePath);
    std::filesystem::remove(hotReloadPreferencePath);
    std::filesystem::remove(genericSegmentContinuationPreferencePath);
    std::filesystem::remove(trimPreferencePath);
    std::filesystem::remove(modelPreferencePath);
    std::filesystem::remove(modelCandidateLearningPreferencePath);
    std::filesystem::remove(modelTimeoutPreferencePath);
    std::filesystem::remove(invalidModelPreferencePath);
    std::filesystem::remove(persistentPreferencePath);
    std::filesystem::remove(passiveSelectionPreferencePath);
    std::filesystem::remove(userDictionaryPreferencePath);
    std::filesystem::remove(rerankScriptPath);
    std::filesystem::remove(modelDescendantMarkerPath);
    std::filesystem::remove(userDictionaryPath);
    std::filesystem::remove(automaticDictionaryPath);
    std::filesystem::remove(automaticDictionaryPath + ".lock");
    std::filesystem::remove(rareWordDictionaryPath);
    std::filesystem::remove(rareWordDictionaryPath + ".lock");
    std::filesystem::remove(rareWordPreferencePath);
    std::filesystem::remove(rareWordPreferencePath + ".lock");
    std::filesystem::remove(rareWordReloadPreferencePath);
    std::filesystem::remove(knownPinyinHabitPreferencePath);
    std::filesystem::remove(isolatedLibIMEHistoryPath);
    std::filesystem::remove(isolatedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(learnedLibIMEHistoryPath);
    std::filesystem::remove(learnedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(reloadedLibIMEHistoryPath);
    std::filesystem::remove(reloadedLibIMEHistoryPath + ".lock");
    std::filesystem::remove(boundedSupervisionHistoryTestPath);
    std::filesystem::remove(passThroughPreferencePath);

    std::cout << "state machine ok\n";
    return 0;
}
