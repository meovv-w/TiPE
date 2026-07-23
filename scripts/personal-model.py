#!/usr/bin/env python3

import argparse
import fcntl
import hashlib
import json
import math
import os
import random
import sys
from collections import Counter
from functools import lru_cache
from pathlib import Path


SCHEMA = "tipe.personal-reranker.v1"
TRAINING_SCHEMA = "tipe.training.v1"
MODEL_NAME = "TiP"
ARCHITECTURE = (
    "hashed-pairwise-ranker+personal-edit-channel+raw-token-memory+raw-offer-profile+pinyin-prior"
)
LEGACY_ARCHITECTURES = {
    "hashed-pairwise-ranker+personal-edit-channel+raw-offer-profile+pinyin-prior",
    "hashed-pairwise-ranker+personal-edit-channel+pinyin-prior",
}
DEFAULT_DIMENSION = 262144
DEFAULT_PROMOTION_MARGIN = 0.5
CURRENT_FEATURE_VERSION = 4
MAX_MODEL_BYTES = 16 * 1024 * 1024
MAX_CORRECTION_PATTERNS = 512
MAX_KEY_HABITS = 128
MAX_PAIR_EVIDENCE = 65536
MAX_RAW_TOKEN_EVIDENCE = 4096
MAX_RUNTIME_PREFERENCE_ROWS = 2048
MAX_RUNTIME_RAW_TOKEN_ROWS = 512
MAX_SAVED_LEARNING_COUNT = 1000000
MAX_PINYIN_PRIOR_ENTRIES = 65536
MIN_PINYIN_PRIOR_MARGIN = 2
CORRECTION_PATTERN_ACTIVATION_COUNTS = {
    "missing": 2,
    "transpose": 2,
    "extra": 3,
    "replace": 4,
}
KEY_HABIT_ACTIVATION_COUNTS = {
    "transpose": 3,
    "missing": 5,
    "extra": 6,
    "replace": 6,
}
MIN_CANDIDATE_PREFERENCE_COUNT = 2
MIN_RAW_PREFERENCE_COUNT = 3
MIN_GENERIC_VALIDATION_NON_LEADING = 5
MIN_RAW_PROFILE_ACCEPTED_SAMPLES = 4
MIN_RAW_PROFILE_REJECTED_SAMPLES = 4
MIN_RAW_PROFILE_VALIDATION_PER_CLASS = 2
MAX_RAW_PROFILE_AUXILIARY_SAMPLES = 512
RAW_PROFILE_AUXILIARY_WEIGHT = 0.25
GENERIC_VALIDATION_STRATEGY = "capability-isolated-temporal-v4"
LEGACY_GENERIC_VALIDATION_STRATEGIES = frozenset({"capability-isolated-temporal-v3"})
RAW_PROFILE_VALIDATION_STRATEGY = "balanced-temporal-v1"
HASH_PERSON = b"TiPErank"
PAIR_PERSON = b"TiPEpair"
RAW_TOKEN_PERSON = b"TiPEraw"
MAX_CONTEXT_ITEMS = 16
MAX_CONTEXT_BYTES = 1024
CONTEXT_FINGERPRINT_PREFIX = "v1:"
CONTEXT_FINGERPRINT_HEX_LENGTH = 32
MASK64 = (1 << 64) - 1


def correction_pattern_activation_count(kind):
    return CORRECTION_PATTERN_ACTIVATION_COUNTS.get(kind, MAX_SAVED_LEARNING_COUNT + 1)


def key_habit_activation_count(kind):
    return KEY_HABIT_ACTIVATION_COUNTS.get(kind, MAX_SAVED_LEARNING_COUNT + 1)


def active_correction_pattern(row):
    return row["count"] >= correction_pattern_activation_count(row["kind"])


def active_key_habit(row):
    return row["count"] >= key_habit_activation_count(row["kind"])

PINYIN_SYLLABLES = frozenset("""
chuang shuang zhuang chang cheng chong chuan chuai jiang liang niang qiang shang sheng shuan shuai
xiang xiong zhang zheng zhong zhuan zhuai ang bai ban bang bao bei ben beng bian biao bie bin bing
cai can cang cao cei cen ceng chai chan chao che chen chi chou chu chua chui chun chuo ci cong cou
cu cuan cui cun cuo dai dan dang dao dei den deng di dian diao die ding diu dong dou du duan dui dun
duo eng er fan fang fei fen feng fo fou fu gai gan gang gao ge gei gen geng gong gou gu gua guai guan
guang gui gun guo hai han hang hao hei hen heng hong hou hu hua huai huan huang hui hun huo ji jia
jian jiao jie jin jing jiong jiu ju juan jue jun kai kan kang kao ke kei ken keng kong kou ku kua kuai
kuan kuang kui kun kuo lai lan lang lao lei leng li lia lian liao lie lin ling liu lo long lou lu luan
lue lun luo lv lve mai man mang mao mei men meng mi mian miao mie min ming miu mo mou mu nai nan nang
nao nei nen neng ni nian niao nie nin ning niu nong nou nu nuan nue nuo nv nve pai pan pang pao pei
pen peng pi pian piao pie pin ping po pou pu qi qia qian qiao qie qin qing qiong qiu qu quan que qun
ran rang rao re ren reng ri rong rou ru ruan rui run ruo sai san sang sao sei sen seng sha shai shan
shao she shen shi shou shu shua shui shun shuo si song sou su suan sui sun suo tai tan tang tao tei
teng ti tian tiao tie ting tong tou tu tuan tui tun tuo wai wan wang wei wen weng wo wu xi xia xian
xiao xie xin xing xiu xu xuan xue xun yan yang yao ye yi yin ying yo yong you yu yuan yue yun zai zan
zang zao zei zen zeng zha zhai zhan zhao zhe zhei zhen zhi zhou zhu zhua zhui zhun zhuo zi zong zou
zu zuan zui zun zuo ai an ao ba bo bi bu ca ce ch da de ei en fa ga ha he ka la le ma me na ne ng ou
pa sa se sh ta te wa ya za ze zh a e m n o
""".split())


def default_model_path():
    data_home = os.environ.get("XDG_DATA_HOME")
    if data_home:
        return Path(data_home) / "tipe" / "personal-reranker.json"
    home = os.environ.get("HOME")
    if not home:
        raise ValueError("HOME and XDG_DATA_HOME are both unset")
    return Path(home) / ".local" / "share" / "tipe" / "personal-reranker.json"


def default_preferences_path():
    data_home = os.environ.get("XDG_DATA_HOME")
    if data_home:
        return Path(data_home) / "tipe" / "candidate-preferences.tsv"
    home = os.environ.get("HOME")
    if not home:
        raise ValueError("HOME and XDG_DATA_HOME are both unset")
    return Path(home) / ".local" / "share" / "tipe" / "candidate-preferences.tsv"


def default_pinyin_dictionary_paths():
    override = os.environ.get("TIPE_PINYIN_PRIOR_DICTIONARY", "")
    if override:
        return [Path(value) for value in override.split(os.pathsep) if value]
    return [
        Path("/usr/share/rime-data/pinyin_simp.dict.yaml"),
        Path("/usr/share/rime-data/luna_pinyin.dict.yaml"),
    ]


def load_pinyin_prior(paths):
    prior = {}
    readable_sources = 0
    for path in paths:
        try:
            stream = path.open("r", encoding="utf-8", errors="replace")
        except OSError:
            continue
        readable_sources += 1
        in_body = False
        with stream:
            for raw_line in stream:
                line = raw_line.rstrip("\r\n")
                if not in_body:
                    in_body = line == "..."
                    continue
                if not line or line.startswith("#"):
                    continue
                fields = line.split("\t")
                if len(fields) < 2:
                    continue
                pinyin = fields[1].replace(" ", "").lower()
                if not 1 <= len(pinyin) <= 64 or not pinyin.isascii() or not pinyin.isalpha():
                    continue
                try:
                    weight = int(fields[2]) if len(fields) >= 3 else 1
                except ValueError:
                    weight = 1
                bucket = min(31, max(1, int(math.log2(max(1, weight))) + 1))
                prior[pinyin] = max(prior.get(pinyin, 0), bucket)
    if len(prior) > MAX_PINYIN_PRIOR_ENTRIES:
        ranked = sorted(prior.items(), key=lambda item: (-item[1], item[0]))[:MAX_PINYIN_PRIOR_ENTRIES]
        prior = dict(sorted(ranked))
    return prior, readable_sources


def feature_hash(name, dimension):
    digest = hashlib.blake2b(name.encode("utf-8"), digest_size=9, person=HASH_PERSON).digest()
    index = int.from_bytes(digest[:8], "little") % dimension
    sign = 1.0 if digest[8] & 1 else -1.0
    return index, sign


def pair_evidence_key(preedit, candidate):
    payload = f"{preedit}\x1f{candidate}".encode("utf-8")
    return hashlib.blake2b(payload, digest_size=12, person=PAIR_PERSON).hexdigest()


def raw_token_evidence_key(token):
    return hashlib.blake2b(
        token.lower().encode("utf-8"), digest_size=12, person=RAW_TOKEN_PERSON
    ).hexdigest()


def context_fingerprint_part(text, seed):
    value = seed
    for byte in b"TiPE-context-v1" + text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & MASK64
    value ^= value >> 33
    value = (value * 0xFF51AFD7ED558CCD) & MASK64
    value ^= value >> 33
    value = (value * 0xC4CEB9FE1A85EC53) & MASK64
    value ^= value >> 33
    return value


def context_fingerprint(text):
    if not isinstance(text, str) or not text or len(text.encode("utf-8")) > MAX_CONTEXT_BYTES:
        return None
    first = context_fingerprint_part(text, 14695981039346656037)
    second = context_fingerprint_part(text, 0x84222325CBF29CE4)
    return f"{CONTEXT_FINGERPRINT_PREFIX}{first:016x}{second:016x}"


def valid_context_fingerprint(value):
    if not isinstance(value, str) or not value.startswith(CONTEXT_FINGERPRINT_PREFIX):
        return False
    digest = value[len(CONTEXT_FINGERPRINT_PREFIX):]
    return len(digest) == CONTEXT_FINGERPRINT_HEX_LENGTH and all(
        character in "0123456789abcdef" for character in digest
    )


def unescape_protocol_text(text):
    result = []
    index = 0
    replacements = {"t": "\t", "r": "\r", "n": "\n", "\\": "\\"}
    while index < len(text):
        if text[index] == "\\" and index + 1 < len(text) and text[index + 1] in replacements:
            result.append(replacements[text[index + 1]])
            index += 2
        else:
            result.append(text[index])
            index += 1
    return "".join(result)


def add_feature(features, name, value, dimension):
    if not value:
        return
    index, sign = feature_hash(name, dimension)
    features[index] = features.get(index, 0.0) + float(value) * sign


def source_class(source):
    lowered = str(source).lower()
    if "correction" in lowered:
        return "correction"
    if "prefix" in lowered or "partial" in lowered:
        return "prefix"
    if "raw" in lowered:
        return "raw"
    if "user" in lowered or "exact" in lowered:
        return "exact"
    return "lookup"


def raw_candidate_index(observation, strict_offer=False):
    preedit = observation.get("preedit", "")
    candidates = observation.get("candidates", [])
    metadata = observation.get("candidate_metadata", [])
    if not isinstance(preedit, str) or not isinstance(candidates, list) or not isinstance(metadata, list):
        return None
    for index, candidate in enumerate(candidates):
        row = metadata[index] if index < len(metadata) and isinstance(metadata[index], dict) else {}
        source = str(row.get("source", "")).lower()
        if (
            isinstance(candidate, str)
            and candidate.lower() == preedit.lower()
            and ((source == "raw-offer") if strict_offer else source_class(source) == "raw")
        ):
            return index
    return None


def has_raw_candidate(observation):
    return raw_candidate_index(observation) is not None


def raw_pass_through_observation(observation):
    if not isinstance(observation, dict) or observation.get("supervision_mode") != "pass-through-only":
        return False
    preedit = observation.get("preedit")
    candidates = observation.get("candidates")
    metadata = observation.get("candidate_metadata")
    if (
        not safe_input_text(preedit)
        or len(preedit) < 2
        or not isinstance(candidates, list)
        or not isinstance(metadata, list)
    ):
        return False
    for index, candidate in enumerate(candidates):
        row = metadata[index] if index < len(metadata) and isinstance(metadata[index], dict) else {}
        if candidate == preedit and str(row.get("source", "")).lower() == "raw-pass-through":
            return True
    return False


def raw_pass_through_positive(sample):
    if not isinstance(sample, dict):
        return False
    observation = sample.get("input")
    target = sample.get("target")
    return (
        raw_pass_through_observation(observation)
        and isinstance(target, dict)
        and target.get("action") == "raw-committed"
        and target.get("text") == observation.get("preedit")
    )


def length_bucket(value):
    if value <= 1:
        return str(value)
    if value <= 3:
        return "2-3"
    if value <= 6:
        return "4-6"
    if value <= 12:
        return "7-12"
    return "13+"


def count_events(events):
    counts = Counter()
    for event in events or []:
        if isinstance(event, dict) and isinstance(event.get("type"), str):
            counts[event["type"]] += 1
    return counts


def event_feature_token(event, detailed=False):
    if not isinstance(event, dict):
        return None
    kind = event.get("type")
    text = event.get("text", "")
    if not isinstance(kind, str) or not kind or len(kind) > 32:
        return None
    if (
        kind == "cursor-move"
        and isinstance(text, str)
        and text.isascii()
        and 0 < len(text) <= 24
    ):
        return f"{kind}:{text}"
    if kind == "observed" and isinstance(text, str) and text.isascii() and 0 < len(text) <= 24:
        return f"{kind}:{text}"
    if kind in {"letter", "digit", "symbol"}:
        if detailed and isinstance(text, str) and text.isascii() and len(text) == 1 and text.isprintable():
            return f"{kind}:{text.lower()}"
        return "typing"
    if kind == "candidate-selected":
        return "commit"
    return kind


def event_feature_tokens(events, limit=12, detailed=False):
    tokens = []
    for event in events or []:
        token = event_feature_token(event, detailed=detailed)
        if token and (detailed or not tokens or tokens[-1] != token):
            tokens.append(token)
    return tokens[-limit:]


def evidence_count_bucket(count):
    if count <= 1:
        return "1"
    if count == 2:
        return "2"
    if count <= 4:
        return "3-4"
    if count <= 8:
        return "5-8"
    return "9+"


def bounded_positive_count(value, default=1):
    if isinstance(value, int) and not isinstance(value, bool):
        return max(0, min(value, 100))
    return default


def known_preference_count(observation, preedit, candidate):
    known_evidence = observation.get("known_evidence", {})
    if not isinstance(known_evidence, dict):
        return 0
    preferences = known_evidence.get("preferences", [])
    if not isinstance(preferences, list):
        return 0
    strongest = 0
    for preference in preferences:
        if not isinstance(preference, dict):
            continue
        if preference.get("preedit") == preedit and preference.get("candidate") == candidate:
            strongest = max(strongest, bounded_positive_count(preference.get("count")))
    return strongest


def matching_segment_chain_count(observation, preedit, candidate):
    chains = observation.get("segment_chains", [])
    if not isinstance(chains, list):
        return 0
    strongest = 0
    for chain in chains:
        if not isinstance(chain, dict) or chain.get("combined_candidate") != candidate:
            continue
        if chain.get("original_preedit") == preedit or chain.get("corrected_full_preedit") == preedit:
            strongest = max(strongest, bounded_positive_count(chain.get("count")))
    return strongest


def supervised_evidence_prior(observation, candidate_index):
    preedit = observation.get("preedit", "")
    candidates = observation.get("candidates", [])
    if candidate_index < 0 or candidate_index >= len(candidates):
        return 0.0
    candidate = candidates[candidate_index]
    raw_match = bool(preedit) and candidate.lower() == preedit.lower()
    preference_count = known_preference_count(observation, preedit, candidate)
    preference_threshold = MIN_RAW_PREFERENCE_COUNT if raw_match else MIN_CANDIDATE_PREFERENCE_COUNT
    prior = 0.0
    if preference_count >= preference_threshold:
        prior += min(4.0, 0.75 + 0.5 * preference_count)
    segment_count = matching_segment_chain_count(observation, preedit, candidate)
    if segment_count:
        prior += min(4.0, 1.0 + 0.5 * segment_count)
    return prior


def has_direct_promotion_support(observation, candidate_index):
    if supervised_evidence_prior(observation, candidate_index) > 0:
        return True
    metadata = observation.get("candidate_metadata", [])
    row = metadata[candidate_index] if candidate_index < len(metadata) else {}
    return isinstance(row, dict) and source_class(row.get("source", "")) == "correction"


def safe_input_text(value):
    return (
        isinstance(value, str)
        and 0 < len(value) <= 64
        and value.isascii()
        and value.isalnum()
    )


def edit_distance_at_most(left, right, limit=2):
    if abs(len(left) - len(right)) > limit:
        return False
    previous = list(range(len(right) + 1))
    for left_index, left_character in enumerate(left, 1):
        current = [left_index] + [0] * len(right)
        row_best = current[0]
        for right_index, right_character in enumerate(right, 1):
            cost = 0 if left_character == right_character else 1
            current[right_index] = min(
                previous[right_index] + 1,
                current[right_index - 1] + 1,
                previous[right_index - 1] + cost,
            )
            row_best = min(row_best, current[right_index])
        if row_best > limit:
            return False
        previous = current
    return previous[len(right)] <= limit


def plausible_correction(typo, corrected):
    return (
        typo != corrected
        and safe_input_text(typo)
        and safe_input_text(corrected)
        and edit_distance_at_most(typo, corrected)
    )


def complete_pinyin(value):
    if not isinstance(value, str) or not value or not value.isascii() or not value.isalpha():
        return False
    reachable = [False] * (len(value) + 1)
    reachable[0] = True
    for start in range(len(value)):
        if not reachable[start]:
            continue
        for end in range(start + 1, min(len(value), start + 6) + 1):
            if value[start:end] in PINYIN_SYLLABLES:
                reachable[end] = True
    return reachable[-1]


def possible_corrections_from_events(events, corrected_preedit):
    current = ""
    cursor = 0
    erased_original = ""
    last_fully_erased = None
    last_edited_original = None
    middle_edit_original = None
    erasing = False
    result = []
    seen = set()

    def remember_middle_edit():
        nonlocal middle_edit_original
        if current and middle_edit_original is None:
            middle_edit_original = current

    def collect_matching_corrections():
        if current != corrected_preedit:
            return
        for typo in (last_fully_erased, last_edited_original, middle_edit_original):
            if typo and typo not in seen and plausible_correction(typo, corrected_preedit):
                result.append((typo, corrected_preedit))
                seen.add(typo)

    for event in events or []:
        if not isinstance(event, dict):
            continue
        kind = event.get("type", "")
        text = event.get("text", "")
        if not isinstance(text, str):
            text = ""
        if kind in {"letter", "digit", "symbol"}:
            if erasing and erased_original:
                last_edited_original = erased_original
            erasing = False
            cursor = min(cursor, len(current))
            if cursor < len(current):
                remember_middle_edit()
            current = current[:cursor] + text + current[cursor:]
            cursor += len(text)
        elif kind == "backspace":
            cursor = min(cursor, len(current))
            if not current or cursor == 0:
                erasing = False
                erased_original = ""
                continue
            if cursor < len(current):
                remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor - 1] + current[cursor:]
            cursor -= 1
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "delete":
            cursor = min(cursor, len(current))
            if cursor >= len(current):
                continue
            remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor] + current[cursor + 1:]
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "cursor-move":
            if text in {"Left", "KP_Left"}:
                cursor = max(0, cursor - 1)
            elif text in {"Right", "KP_Right"}:
                cursor = min(cursor + 1, len(current))
            erasing = False
            erased_original = ""
        elif kind in {"candidate-selected", "raw-committed", "escape"}:
            if kind != "escape":
                collect_matching_corrections()
            current = ""
            cursor = 0
            erased_original = ""
            last_fully_erased = None
            last_edited_original = None
            middle_edit_original = None
            erasing = False

    collect_matching_corrections()
    return result


def correction_pattern(typo, corrected):
    if len(corrected) == len(typo) + 1:
        for index, character in enumerate(corrected):
            if corrected[:index] + corrected[index + 1:] != typo:
                continue
            offset = len(typo) - index
            return ("missing", "", character, offset if offset <= 2 else index, offset <= 2)
    elif len(typo) == len(corrected) + 1:
        for index, character in enumerate(typo):
            if typo[:index] + typo[index + 1:] != corrected:
                continue
            offset = len(typo) - index - 1
            return ("extra", character, "", offset if offset <= 1 else index, offset <= 1)
    elif len(typo) == len(corrected):
        differences = [index for index, pair in enumerate(zip(typo, corrected)) if pair[0] != pair[1]]
        if len(differences) == 1:
            index = differences[0]
            offset = len(typo) - index - 1
            return (
                "replace",
                typo[index],
                corrected[index],
                offset if offset <= 1 else index,
                offset <= 1,
            )
        if (
            len(differences) == 2
            and differences[1] == differences[0] + 1
            and typo[differences[0]:differences[1] + 1] == corrected[differences[0]:differences[1] + 1][::-1]
        ):
            index = differences[0]
            offset = len(typo) - index - 2
            return (
                "transpose",
                typo[index:index + 2],
                corrected[index:index + 2],
                offset if offset <= 1 else index,
                offset <= 1,
            )
    return None


def learn_correction_patterns(samples):
    pair_counts = Counter()
    persisted_pair_counts = {}
    for observation, _ in samples:
        corrected = observation.get("preedit", "")
        events = observation.get("correction_events", [])
        pair_counts.update(set(possible_corrections_from_events(events, corrected)))
        known_evidence = observation.get("known_evidence", {})
        if not isinstance(known_evidence, dict):
            continue
        corrections = known_evidence.get("corrections", [])
        if not isinstance(corrections, list):
            continue
        for correction in corrections:
            if not isinstance(correction, dict):
                continue
            typo = correction.get("typo")
            corrected_preedit = correction.get("corrected_preedit")
            count = correction.get("count")
            if (
                plausible_correction(typo, corrected_preedit)
                and isinstance(count, int)
                and not isinstance(count, bool)
                and count > 0
            ):
                pair = (typo, corrected_preedit)
                persisted_pair_counts[pair] = max(persisted_pair_counts.get(pair, 0), min(count, 20))
    for pair, count in persisted_pair_counts.items():
        pair_counts[pair] = max(pair_counts[pair], count)

    counts = Counter()
    habit_counts = Counter()
    for (typo, corrected), count in pair_counts.items():
        pattern = correction_pattern(typo, corrected)
        if pattern:
            counts[pattern] += count
            habit_counts[pattern[:3]] += count
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:MAX_CORRECTION_PATTERNS]
    patterns = [
        {
            "kind": key[0],
            "typed": key[1],
            "replacement": key[2],
            "position": key[3],
            "relative_to_end": key[4],
            "count": count,
        }
        for key, count in ranked
    ]
    ranked_habits = sorted(habit_counts.items(), key=lambda item: (-item[1], item[0]))[:MAX_KEY_HABITS]
    habits = [
        {
            "kind": key[0],
            "typed": key[1],
            "replacement": key[2],
            "count": count,
        }
        for key, count in ranked_habits
    ]
    return patterns, habits, len(pair_counts), sum(pair_counts.values())


def pinyin_ngrams(preedit):
    grams = set()
    lowered = preedit.lower()
    for size in (1, 2, 3, 4):
        if len(lowered) < size:
            continue
        for index in range(len(lowered) - size + 1):
            grams.add(lowered[index:index + size])
    return grams


def han_text(value):
    if not isinstance(value, str) or not value or len(value) > 16:
        return False
    return all(
        0x3400 <= ord(character) <= 0x4DBF
        or 0x4E00 <= ord(character) <= 0x9FFF
        or 0xF900 <= ord(character) <= 0xFAFF
        or 0x20000 <= ord(character) <= 0x323AF
        for character in value
    )


@lru_cache(maxsize=8192)
def aligned_pinyin_syllables(preedit, character_count):
    if (
        not isinstance(preedit, str)
        or not preedit
        or not preedit.isascii()
        or not preedit.isalpha()
        or len(preedit) > 64
        or not isinstance(character_count, int)
        or isinstance(character_count, bool)
        or character_count < 1
        or character_count > 16
    ):
        return ()

    lowered = preedit.lower()
    memo = {}

    def visit(offset, remaining):
        key = (offset, remaining)
        if key in memo:
            return memo[key]
        if remaining == 0:
            result = () if offset == len(lowered) else None
            memo[key] = result
            return result
        available = len(lowered) - offset
        if available < remaining or available > remaining * 6:
            memo[key] = None
            return None

        best = None
        best_score = None
        maximum_end = min(len(lowered), offset + 6)
        for end in range(offset + 1, maximum_end + 1):
            syllable = lowered[offset:end]
            if syllable not in PINYIN_SYLLABLES:
                continue
            suffix = visit(end, remaining - 1)
            if suffix is None:
                continue
            result = (syllable, *suffix)
            # Avoid an artificial one-letter split when a normal syllable path
            # exists, then prefer the longest valid syllable at each boundary.
            score = (-sum(len(item) == 1 for item in result), tuple(len(item) for item in result))
            if best_score is None or score > best_score:
                best = result
                best_score = score
        memo[key] = best
        return best

    return visit(0, character_count) or ()


def add_phonetic_transfer_features(features, preedit, candidate, dimension):
    if not han_text(candidate):
        return
    syllables = aligned_pinyin_syllables(preedit, len(candidate))
    if not syllables:
        return
    last_index = len(candidate) - 1
    for index, (syllable, character) in enumerate(zip(syllables, candidate)):
        if last_index == 0:
            position = "single"
        elif index == 0:
            position = "first"
        elif index == last_index:
            position = "last"
        else:
            position = "middle"
        add_feature(features, f"phonetic-v4:{syllable}\x1f{character}", 1.25, dimension)
        add_feature(
            features,
            f"phonetic-v4:{position}:{syllable}\x1f{character}",
            0.35,
            dimension,
        )
    for index in range(last_index):
        add_feature(
            features,
            f"phonetic-v4:pair:{syllables[index]}+{syllables[index + 1]}\x1f{candidate[index:index + 2]}",
            0.25,
            dimension,
        )


def pair_features(observation, candidate_index, dimension, feature_version=1):
    preedit = observation.get("preedit", "")
    candidates = observation.get("candidates", [])
    if candidate_index < 0 or candidate_index >= len(candidates):
        return {}
    candidate = candidates[candidate_index]
    metadata_rows = observation.get("candidate_metadata", [])
    metadata = metadata_rows[candidate_index] if candidate_index < len(metadata_rows) else {}
    consumed = int(metadata.get("consumed_prefix", 0) or 0)
    source = source_class(metadata.get("source", ""))
    is_partial = 0 < consumed < len(preedit)
    raw_match = candidate.lower() == preedit.lower() and bool(preedit)
    events = count_events(observation.get("events", []))
    correction_events = count_events(observation.get("correction_events", []))
    derived_prefix_selection = observation.get("derived_prefix_selection") is True

    features = {}
    add_feature(features, "bias", 0.1, dimension)
    add_feature(features, f"exact:{preedit}\x1f{candidate}", 4.0, dimension)
    if feature_version >= 4:
        add_phonetic_transfer_features(features, preedit, candidate, dimension)
    if derived_prefix_selection:
        return features
    add_feature(features, f"rank:{min(candidate_index, 9)}", 0.25, dimension)
    add_feature(features, f"candidate:{candidate}", 0.25, dimension)
    for gram in pinyin_ngrams(preedit):
        add_feature(features, f"pgram:{gram}\x1f{candidate}", 0.75, dimension)
    add_feature(features, f"source-candidate:{source}\x1f{candidate}", 0.5, dimension)
    add_feature(features, f"source:{source}", 1.0, dimension)
    add_feature(features, f"partial:{int(is_partial)}", 0.5, dimension)
    add_feature(features, f"raw-match:{int(raw_match)}", 0.75, dimension)
    if feature_version >= 3 and raw_match and source == "raw":
        lowered_preedit = preedit.lower()
        add_feature(features, "raw-profile:offer", 1.0, dimension)
        add_feature(features, f"raw-profile:length:{length_bucket(len(lowered_preedit))}", 0.5, dimension)
        add_feature(features, f"raw-profile:complete-pinyin:{int(complete_pinyin(lowered_preedit))}", 0.75, dimension)
        add_feature(features, f"raw-profile:has-digit:{int(any(ch.isdigit() for ch in lowered_preedit))}", 0.5, dimension)
        for width in (2, 3):
            if len(lowered_preedit) >= width:
                add_feature(features, f"raw-profile:prefix:{lowered_preedit[:width]}", 0.25, dimension)
                add_feature(features, f"raw-profile:suffix:{lowered_preedit[-width:]}", 0.25, dimension)
    add_feature(features, f"preedit-length:{length_bucket(len(preedit))}", 0.1, dimension)
    add_feature(features, f"candidate-length:{length_bucket(len(candidate))}", 0.1, dimension)
    preference_count = known_preference_count(observation, preedit, candidate)
    if preference_count:
        bucket = evidence_count_bucket(preference_count)
        add_feature(features, "evidence:preference:present", 1.0, dimension)
        add_feature(features, f"evidence:preference:count:{bucket}", min(preference_count, 8) * 0.25, dimension)
        add_feature(features, f"evidence:preference:source:{source}", 0.5, dimension)
    segment_chain_count = matching_segment_chain_count(observation, preedit, candidate)
    if segment_chain_count:
        bucket = evidence_count_bucket(segment_chain_count)
        add_feature(features, "evidence:segment-chain:present", 1.0, dimension)
        add_feature(features, f"evidence:segment-chain:count:{bucket}", min(segment_chain_count, 8) * 0.25, dimension)
        add_feature(features, f"evidence:segment-chain:source:{source}", 0.5, dimension)

    for event_type, count in events.items():
        bucket = min(count, 4)
        add_feature(features, f"event:{event_type}:{bucket}\x1fsource:{source}", 0.3, dimension)
    for event_type in ("backspace", "delete", "cursor-move", "observed"):
        count = correction_events.get(event_type, 0)
        if count:
            bucket = min(count, 8)
            add_feature(features, f"trail:{event_type}:{bucket}\x1fsource:{source}", 0.5, dimension)
    recent_tokens = event_feature_tokens(observation.get("events", []))
    for offset, token in enumerate(reversed(recent_tokens[-6:]), 1):
        add_feature(features, f"sequence:recent:last:{offset}:{token}\x1fsource:{source}", 0.25, dimension)
        add_feature(features, f"sequence:recent:last:{offset}:{token}\x1fcandidate:{candidate}", 0.25, dimension)
    for left, right in zip(recent_tokens[-8:], recent_tokens[-7:]):
        transition = f"{left}>{right}"
        add_feature(features, f"sequence:recent:transition:{transition}\x1fsource:{source}", 0.2, dimension)
        add_feature(features, f"sequence:recent:transition:{transition}\x1fcandidate:{candidate}", 0.2, dimension)
    correction_tokens = event_feature_tokens(observation.get("correction_events", []), 24)
    for offset, token in enumerate(reversed(correction_tokens[-8:]), 1):
        add_feature(features, f"sequence:correction:last:{offset}:{token}\x1fsource:{source}", 0.2, dimension)
        add_feature(features, f"sequence:correction:last:{offset}:{token}\x1fcandidate:{candidate}", 0.2, dimension)
    if feature_version >= 2:
        detailed_recent = event_feature_tokens(observation.get("events", []), 32, detailed=True)
        for offset, token in enumerate(reversed(detailed_recent[-12:]), 1):
            add_feature(features, f"sequence-v2:recent:last:{offset}:{token}\x1fsource:{source}", 0.2, dimension)
            add_feature(features, f"sequence-v2:recent:last:{offset}:{token}\x1fcandidate:{candidate}", 0.15, dimension)
        for left, right in zip(detailed_recent[-16:], detailed_recent[-15:]):
            add_feature(features, f"sequence-v2:recent:pair:{left}>{right}\x1fsource:{source}", 0.15, dimension)
        detailed_correction = event_feature_tokens(
            observation.get("correction_events", []), 64, detailed=True
        )
        for offset, token in enumerate(reversed(detailed_correction[-16:]), 1):
            add_feature(features, f"sequence-v2:correction:last:{offset}:{token}\x1fsource:{source}", 0.15, dimension)
        for left, right in zip(detailed_correction[-24:], detailed_correction[-23:]):
            add_feature(features, f"sequence-v2:correction:pair:{left}>{right}\x1fsource:{source}", 0.1, dimension)
        if feature_version >= 4:
            for offset, token in enumerate(reversed(detailed_correction[-24:]), 1):
                add_feature(
                    features,
                    f"sequence-v4:correction:last:{offset}:{token}\x1fcandidate:{candidate}",
                    0.12,
                    dimension,
                )
            for left, right in zip(detailed_correction[-32:], detailed_correction[-31:]):
                add_feature(
                    features,
                    f"sequence-v4:correction:pair:{left}>{right}\x1fcandidate:{candidate}",
                    0.08,
                    dimension,
                )
    if feature_version >= 3:
        context_features = observation.get("context_features", [])
        if isinstance(context_features, list):
            valid_context = [value for value in context_features if valid_context_fingerprint(value)][-MAX_CONTEXT_ITEMS:]
            for offset, fingerprint in enumerate(reversed(valid_context), 1):
                add_feature(
                    features,
                    f"context-v3:last:{offset}:{fingerprint}\x1fcandidate:{candidate}",
                    0.5,
                    dimension,
                )
                add_feature(
                    features,
                    f"context-v3:last:{offset}:{fingerprint}\x1fsource:{source}",
                    0.25,
                    dimension,
                )
        surrounding_features = observation.get("surrounding_context_features", {})
        if isinstance(surrounding_features, dict):
            for side in ("before", "after"):
                fingerprint = surrounding_features.get(side)
                if valid_context_fingerprint(fingerprint):
                    add_feature(
                        features,
                        f"surrounding-v3:{side}:{fingerprint}\x1fcandidate:{candidate}",
                        0.25,
                        dimension,
                    )
    application = observation.get("application")
    if isinstance(application, str) and application:
        add_feature(features, f"application:{application}\x1fsource:{source}", 0.25, dimension)
    return features


def raw_profile_features(
    observation, dimension, feature_version=CURRENT_FEATURE_VERSION, allow_pass_through=False
):
    offer_index = raw_candidate_index(observation, strict_offer=True)
    preedit = observation.get("preedit", "")
    pass_through = allow_pass_through and raw_pass_through_observation(observation)
    if (offer_index is None or offer_index == 0) and not pass_through:
        return {}
    if not isinstance(preedit, str):
        return {}
    lowered = preedit.lower()
    features = {}
    add_feature(features, "raw-profile-binary:bias", 1.0, dimension)
    add_feature(features, f"raw-profile-binary:length:{length_bucket(len(lowered))}", 0.75, dimension)
    add_feature(features, f"raw-profile-binary:complete-pinyin:{int(complete_pinyin(lowered))}", 1.0, dimension)
    add_feature(features, f"raw-profile-binary:has-digit:{int(any(ch.isdigit() for ch in lowered))}", 1.0, dimension)
    for width in (2, 3):
        if len(lowered) >= width:
            add_feature(features, f"raw-profile-binary:prefix:{lowered[:width]}", 0.35, dimension)
            add_feature(features, f"raw-profile-binary:suffix:{lowered[-width:]}", 0.35, dimension)
    for offset, token in enumerate(reversed(event_feature_tokens(observation.get("events", []), 8)), 1):
        add_feature(features, f"raw-profile-binary:event:{offset}:{token}", 0.25, dimension)
    if feature_version >= 3:
        context_features = observation.get("context_features", [])
        if isinstance(context_features, list):
            valid_context = [value for value in context_features if valid_context_fingerprint(value)][-4:]
            for offset, fingerprint in enumerate(reversed(valid_context), 1):
                add_feature(features, f"raw-profile-binary:context:{offset}:{fingerprint}", 0.35, dimension)
    return features


def train_raw_profile(rows, dimension, epochs, learning_rate, seed, feature_version=CURRENT_FEATURE_VERSION):
    weights = {}
    order = list(range(len(rows)))
    randomizer = random.Random(seed ^ 0x54495045)
    for _ in range(epochs):
        randomizer.shuffle(order)
        for row_index in order:
            observation, accepted, auxiliary = rows[row_index]
            features = raw_profile_features(
                observation, dimension, feature_version, allow_pass_through=auxiliary
            )
            if not features:
                continue
            label = 1.0 if accepted else -1.0
            if label * dot(weights, features) >= 1.0:
                continue
            sample_weight = RAW_PROFILE_AUXILIARY_WEIGHT if auxiliary else 1.0
            for index, value in features.items():
                updated = weights.get(index, 0.0) + learning_rate * label * value * sample_weight
                if abs(updated) < 1e-9:
                    weights.pop(index, None)
                else:
                    weights[index] = updated
    return weights


def dot(weights, features):
    return sum(weights.get(index, 0.0) * value for index, value in features.items())


def candidate_scores(observation, weights, dimension, baseline_weight, feature_version=1):
    candidates = observation.get("candidates", [])
    scores = []
    feature_rows = []
    for index in range(len(candidates)):
        features = pair_features(observation, index, dimension, feature_version)
        score = dot(weights, features) - baseline_weight * index + supervised_evidence_prior(observation, index)
        scores.append(score)
        feature_rows.append(features)
    return scores, feature_rows


def selected_index(sample):
    target = sample.get("target")
    observation = sample.get("input")
    if not isinstance(target, dict) or not isinstance(observation, dict):
        return None
    candidates = observation.get("candidates")
    if not isinstance(candidates, list) or len(candidates) < 2 or len(candidates) > 256:
        return None
    action = target.get("action")
    if action == "raw-committed":
        text = target.get("text")
        preedit = observation.get("preedit", "")
        if not isinstance(text, str) or text != preedit or not safe_input_text(text):
            return None
        metadata = observation.get("candidate_metadata", [])
        if not isinstance(metadata, list):
            return None
        for index, candidate in enumerate(candidates):
            source = metadata[index].get("source", "") if index < len(metadata) and isinstance(metadata[index], dict) else ""
            if candidate == text and source_class(source) == "raw":
                return index
        return None
    if action != "candidate-selected":
        return None
    index = target.get("candidate_index")
    if isinstance(index, int) and 0 <= index < len(candidates):
        selected = index
    else:
        text = target.get("text")
        if not isinstance(text, str) or text not in candidates:
            return None
        selected = candidates.index(text)
    preedit = observation.get("preedit", "")
    metadata = observation.get("candidate_metadata", [])
    selected_metadata = metadata[selected] if selected < len(metadata) and isinstance(metadata[selected], dict) else {}
    consumed = selected_metadata.get("consumed_prefix", target.get("consumed_prefix", 0))
    if (
        isinstance(preedit, str)
        and isinstance(consumed, int)
        and not isinstance(consumed, bool)
        and 0 < consumed < len(preedit)
    ):
        comparable = []
        if isinstance(metadata, list):
            for index, row in enumerate(metadata):
                if index >= len(candidates) or not isinstance(row, dict):
                    continue
                row_consumed = row.get("consumed_prefix", 0)
                if isinstance(row_consumed, int) and not isinstance(row_consumed, bool) and row_consumed == consumed:
                    comparable.append(index)
        if selected not in comparable:
            comparable.append(selected)
        comparable.sort()
        if len(comparable) < 2:
            return None
        observation["candidates"] = [candidates[index] for index in comparable]
        observation["candidate_metadata"] = [metadata[index] for index in comparable]
        selected = comparable.index(selected)
        observation["preedit"] = preedit[:consumed]
        try:
            cursor = int(observation.get("preedit_cursor", consumed) or consumed)
        except (TypeError, ValueError):
            cursor = consumed
        observation["preedit_cursor"] = min(cursor, consumed)
        observation["derived_prefix_selection"] = True
        for row in observation["candidate_metadata"]:
            if isinstance(row, dict):
                row["consumed_prefix"] = 0
        target["consumed_prefix"] = 0
        target["remaining_preedit"] = ""
        return selected
    if target.get("remaining_preedit"):
        return None
    return selected


def load_training_samples(path):
    stream = sys.stdin if str(path) == "-" else Path(path).open("r", encoding="utf-8", errors="replace")
    ranking_samples = []
    supervision_observations = []
    raw_profile_auxiliary_observations = []
    skipped = 0
    try:
        for line in stream:
            if not line.strip():
                continue
            try:
                sample = json.loads(line)
            except json.JSONDecodeError:
                skipped += 1
                continue
            if not isinstance(sample, dict) or sample.get("schema") != TRAINING_SCHEMA:
                skipped += 1
                continue
            observation = sample.get("input")
            target = sample.get("target")
            if (
                not isinstance(observation, dict)
                or not isinstance(observation.get("preedit"), str)
                or not isinstance(target, dict)
                or target.get("action") not in {"candidate-selected", "raw-committed", "escape"}
            ):
                skipped += 1
                continue
            index = selected_index(sample)
            supervision_observations.append(observation)
            if raw_pass_through_positive(sample):
                raw_profile_auxiliary_observations.append(observation)
            if index is None:
                continue
            ranking_samples.append((observation, index))
    finally:
        if stream is not sys.stdin:
            stream.close()
    return (
        ranking_samples,
        supervision_observations,
        raw_profile_auxiliary_observations[-MAX_RAW_PROFILE_AUXILIARY_SAMPLES:],
        skipped,
    )


def learn_pair_evidence(samples):
    counts = Counter()
    for observation, selected in samples:
        candidates = observation.get("candidates", [])
        preedit = observation.get("preedit", "")
        if (
            isinstance(preedit, str)
            and 0 <= selected < len(candidates)
            and isinstance(candidates[selected], str)
        ):
            counts[pair_evidence_key(preedit, candidates[selected])] += 1
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:MAX_PAIR_EVIDENCE]
    return {key: count for key, count in sorted(ranked)}


def learn_raw_token_evidence(observations):
    counts = Counter()
    for observation in observations:
        preedit = observation.get("preedit", "")
        if safe_input_text(preedit) and len(preedit) >= 2:
            counts[raw_token_evidence_key(preedit)] += 1
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:MAX_RAW_TOKEN_EVIDENCE]
    return {key: count for key, count in sorted(ranked)}


def train_model(
    samples, dimension, epochs, learning_rate, baseline_weight, seed, feature_version=CURRENT_FEATURE_VERSION
):
    weights = {}
    updates = 0
    order = list(range(len(samples)))
    randomizer = random.Random(seed)
    for _ in range(epochs):
        randomizer.shuffle(order)
        for sample_index in order:
            observation, gold = samples[sample_index]
            scores, feature_rows = candidate_scores(
                observation, weights, dimension, baseline_weight, feature_version
            )
            wrong = max((index for index in range(len(scores)) if index != gold), key=lambda index: scores[index])
            if scores[gold] >= scores[wrong] + 1.0:
                continue
            delta = Counter(feature_rows[gold])
            delta.subtract(feature_rows[wrong])
            for index, value in delta.items():
                updated = weights.get(index, 0.0) + learning_rate * value
                if abs(updated) < 1e-9:
                    weights.pop(index, None)
                else:
                    weights[index] = updated
            updates += 1
    correct, _, _, _, margins = evaluate_ranking(
        samples, weights, dimension, baseline_weight, feature_version=feature_version
    )
    return weights, updates, correct, margins


def evaluate_ranking(
    samples, weights, dimension, baseline_weight, promotion_margin=0.0,
    feature_version=CURRENT_FEATURE_VERSION,
):
    correct = 0
    baseline_correct = 0
    non_leading = 0
    non_leading_correct = 0
    margins = []
    for observation, gold in samples:
        scores, _ = candidate_scores(observation, weights, dimension, baseline_weight, feature_version)
        scored_prediction = max(range(len(scores)), key=lambda index: scores[index])
        predicted = (
            scored_prediction
            if scored_prediction == 0 or scores[scored_prediction] >= scores[0] + promotion_margin
            else 0
        )
        correct += int(predicted == gold)
        baseline_correct += int(gold == 0)
        if gold != 0:
            non_leading += 1
            non_leading_correct += int(predicted == gold)
        wrong_score = max(score for index, score in enumerate(scores) if index != gold)
        margins.append(scores[gold] - wrong_score)
    return correct, baseline_correct, non_leading, non_leading_correct, margins


def stratified_temporal_split(samples, validation_count):
    if validation_count <= 0:
        return samples, []

    leading_indices = [index for index, (_, gold) in enumerate(samples) if gold == 0]
    non_leading_indices = [index for index, (_, gold) in enumerate(samples) if gold != 0]
    preedit_counts = Counter(observation.get("preedit", "") for observation, _ in samples)
    generic_non_leading_indices = [
        index for index in non_leading_indices
        if preedit_counts[samples[index][0].get("preedit", "")] == 1
        and not has_direct_promotion_support(samples[index][0], samples[index][1])
        and not has_raw_candidate(samples[index][0])
        and samples[index][0].get("derived_prefix_selection") is not True
    ]
    non_leading_target = (
        validation_count * len(non_leading_indices) + len(samples) // 2
    ) // len(samples)

    minimum_non_leading = max(0, validation_count - len(leading_indices))
    maximum_non_leading = min(validation_count, len(non_leading_indices))
    if leading_indices and non_leading_indices and validation_count >= 2:
        minimum_non_leading = max(minimum_non_leading, 1)
        maximum_non_leading = min(maximum_non_leading, validation_count - 1)

    # Generic ranking needs a real correction test, not a tail containing only
    # accepted defaults. Keep at least as many non-leading examples in training
    # as in validation so repeated pair evidence alone cannot pass this gate.
    if (
        validation_count >= MIN_GENERIC_VALIDATION_NON_LEADING
        and len(non_leading_indices) >= 2 * MIN_GENERIC_VALIDATION_NON_LEADING
    ):
        minimum_non_leading = max(minimum_non_leading, MIN_GENERIC_VALIDATION_NON_LEADING)
        maximum_non_leading = min(
            maximum_non_leading,
            len(non_leading_indices) - MIN_GENERIC_VALIDATION_NON_LEADING,
        )

    if minimum_non_leading > maximum_non_leading:
        minimum_non_leading = maximum_non_leading
    non_leading_target = min(max(non_leading_target, minimum_non_leading), maximum_non_leading)
    leading_target = validation_count - non_leading_target

    validation_indices = set(generic_non_leading_indices[-non_leading_target:]) if non_leading_target else set()
    if len(validation_indices) < non_leading_target:
        for index in reversed(non_leading_indices):
            validation_indices.add(index)
            if len(validation_indices) >= non_leading_target:
                break
    if leading_target:
        validation_indices.update(leading_indices[-leading_target:])
    training_samples = [sample for index, sample in enumerate(samples) if index not in validation_indices]
    validation_samples = [sample for index, sample in enumerate(samples) if index in validation_indices]
    return training_samples, validation_samples


def temporal_validation(
    samples, dimension, epochs, learning_rate, baseline_weight, promotion_margin, seed, percent,
    feature_version=CURRENT_FEATURE_VERSION,
):
    if percent == 0 or len(samples) < 10:
        return {
            "samples": 0,
            "correct": 0,
            "accuracy": None,
            "baseline_correct": 0,
            "baseline_accuracy": None,
            "gain": 0,
            "non_leading_samples": 0,
            "non_leading_correct": 0,
            "non_leading_accuracy": None,
            "leading_samples": 0,
            "leading_correct": 0,
            "generic_non_leading_samples": 0,
            "generic_non_leading_correct": 0,
            "generic_non_leading_accuracy": None,
            "generic_excluded_direct_evidence": 0,
            "generic_excluded_seen_preedit": 0,
            "generic_excluded_raw_candidate": 0,
            "generic_excluded_derived_prefix": 0,
            "recommendation": "collect-more-data",
        }
    validation_count = max(2, len(samples) * percent // 100)
    validation_count = min(validation_count, len(samples) - 4)
    training_samples, validation_samples = stratified_temporal_split(samples, validation_count)
    validation_weights, _, _, _ = train_model(
        training_samples, dimension, epochs, learning_rate, baseline_weight, seed, feature_version
    )
    correct, baseline_correct, non_leading, non_leading_correct, _ = evaluate_ranking(
        validation_samples, validation_weights, dimension, baseline_weight, promotion_margin, feature_version
    )
    training_preedits = {observation.get("preedit", "") for observation, _ in training_samples}
    generic_samples = []
    generic_excluded_direct_evidence = 0
    generic_excluded_seen_preedit = 0
    generic_excluded_raw_candidate = 0
    generic_excluded_derived_prefix = 0
    for observation, gold in validation_samples:
        if gold == 0:
            continue
        direct_evidence = has_direct_promotion_support(observation, gold)
        seen_preedit = observation.get("preedit", "") in training_preedits
        raw_candidate = has_raw_candidate(observation)
        derived_prefix = observation.get("derived_prefix_selection") is True
        generic_excluded_direct_evidence += int(direct_evidence)
        generic_excluded_seen_preedit += int(seen_preedit)
        generic_excluded_raw_candidate += int(raw_candidate)
        generic_excluded_derived_prefix += int(derived_prefix)
        if not direct_evidence and not seen_preedit and not raw_candidate and not derived_prefix:
            generic_samples.append((observation, gold))
    generic_non_leading_correct, _, generic_non_leading, _, _ = evaluate_ranking(
        generic_samples, validation_weights, dimension, baseline_weight, promotion_margin, feature_version
    ) if generic_samples else (0, 0, 0, 0, [])
    if correct > baseline_correct:
        recommendation = "ready"
    elif correct < baseline_correct:
        recommendation = "keep-heuristic"
    else:
        recommendation = "collect-more-data"
    return {
        "samples": validation_count,
        "correct": correct,
        "accuracy": round(correct / validation_count, 6),
        "baseline_correct": baseline_correct,
        "baseline_accuracy": round(baseline_correct / validation_count, 6),
        "gain": correct - baseline_correct,
        "non_leading_samples": non_leading,
        "non_leading_correct": non_leading_correct,
        "non_leading_accuracy": round(non_leading_correct / non_leading, 6) if non_leading else None,
        "leading_samples": baseline_correct,
        "leading_correct": correct - non_leading_correct,
        "generic_non_leading_samples": generic_non_leading,
        "generic_non_leading_correct": generic_non_leading_correct,
        "generic_non_leading_accuracy": (
            round(generic_non_leading_correct / generic_non_leading, 6) if generic_non_leading else None
        ),
        "generic_excluded_direct_evidence": generic_excluded_direct_evidence,
        "generic_excluded_seen_preedit": generic_excluded_seen_preedit,
        "generic_excluded_raw_candidate": generic_excluded_raw_candidate,
        "generic_excluded_derived_prefix": generic_excluded_derived_prefix,
        "recommendation": recommendation,
    }


def raw_profile_validation(
    samples, dimension, epochs, learning_rate, baseline_weight, promotion_margin, seed, percent,
    feature_version=CURRENT_FEATURE_VERSION, auxiliary_positive_observations=None,
):
    auxiliary_positive_observations = auxiliary_positive_observations or []
    profile_rows = []
    for sample_index, (observation, gold) in enumerate(samples):
        offer_index = raw_candidate_index(observation, strict_offer=True)
        if offer_index is None or offer_index == 0:
            continue
        profile_rows.append((sample_index, observation, gold, offer_index, gold == offer_index))
    accepted_rows = [row for row in profile_rows if row[4]]
    rejected_rows = [row for row in profile_rows if not row[4]]
    result = {
        "strategy": RAW_PROFILE_VALIDATION_STRATEGY,
        "samples": len(profile_rows),
        "accepted_samples": len(accepted_rows),
        "rejected_samples": len(rejected_rows),
        "auxiliary_positive_samples": len(auxiliary_positive_observations),
        "validation_samples": 0,
        "validation_accepted_samples": 0,
        "validation_rejected_samples": 0,
        "validation_correct": 0,
        "validation_accepted_correct": 0,
        "validation_rejected_correct": 0,
        "validation_baseline_correct": 0,
        "validation_false_promotions": 0,
        "validation_accuracy": None,
        "safe": False,
        "recommendation": "collect-more-data",
    }
    if (
        percent == 0
        or len(accepted_rows) < MIN_RAW_PROFILE_ACCEPTED_SAMPLES
        or len(rejected_rows) < MIN_RAW_PROFILE_REJECTED_SAMPLES
    ):
        return result

    def validation_count(rows):
        requested = max(
            MIN_RAW_PROFILE_VALIDATION_PER_CLASS,
            (len(rows) * percent + 99) // 100,
        )
        return min(requested, len(rows) - MIN_RAW_PROFILE_VALIDATION_PER_CLASS)

    accepted_validation_count = validation_count(accepted_rows)
    rejected_validation_count = validation_count(rejected_rows)
    if (
        accepted_validation_count < MIN_RAW_PROFILE_VALIDATION_PER_CLASS
        or rejected_validation_count < MIN_RAW_PROFILE_VALIDATION_PER_CLASS
    ):
        return result
    validation_indices = {
        row[0] for row in accepted_rows[-accepted_validation_count:] + rejected_rows[-rejected_validation_count:]
    }
    training_rows = [
        (observation, accepted, False)
        for sample_index, observation, _, _, accepted in profile_rows
        if sample_index not in validation_indices
    ]
    training_rows.extend(
        (observation, True, True) for observation in auxiliary_positive_observations
    )
    validation_rows = [row for row in profile_rows if row[0] in validation_indices]
    weights = train_raw_profile(training_rows, dimension, epochs, learning_rate, seed, feature_version)
    correct = 0
    accepted_correct = 0
    rejected_correct = 0
    false_promotions = 0
    for _, observation, _, _, accepted in validation_rows:
        profile_score = dot(weights, raw_profile_features(observation, dimension, feature_version))
        predicted_raw = profile_score >= promotion_margin
        row_correct = predicted_raw == accepted
        correct += int(row_correct)
        accepted_correct += int(accepted and row_correct)
        rejected_correct += int(not accepted and row_correct)
        false_promotions += int(not accepted and predicted_raw)
    validation_samples = len(validation_rows)
    safe = (
        validation_samples > 0
        and correct == validation_samples
        and accepted_correct == accepted_validation_count
        and rejected_correct == rejected_validation_count
        and false_promotions == 0
    )
    if safe:
        recommendation = "ready"
    elif correct < rejected_validation_count or false_promotions:
        recommendation = "keep-heuristic"
    else:
        recommendation = "collect-more-data"
    result.update({
        "validation_samples": validation_samples,
        "validation_accepted_samples": accepted_validation_count,
        "validation_rejected_samples": rejected_validation_count,
        "validation_correct": correct,
        "validation_accepted_correct": accepted_correct,
        "validation_rejected_correct": rejected_correct,
        "validation_baseline_correct": rejected_validation_count,
        "validation_false_promotions": false_promotions,
        "validation_accuracy": round(correct / validation_samples, 6),
        "safe": safe,
        "recommendation": recommendation,
    })
    return result


def effective_training_recommendation(training):
    stored = training.get("recommendation")
    validation_samples = training.get("validation_samples")
    correct = training.get("validation_correct")
    baseline_correct = training.get("validation_baseline_correct")
    metrics = (validation_samples, correct, baseline_correct)
    if all(isinstance(value, int) and not isinstance(value, bool) for value in metrics):
        if validation_samples > 0 and 0 <= correct <= validation_samples and 0 <= baseline_correct <= validation_samples:
            if correct > baseline_correct:
                return "ready"
            if correct < baseline_correct:
                return "keep-heuristic"
            return "collect-more-data"
    return stored if stored in {"ready", "keep-heuristic", "collect-more-data"} else None


def generic_ranking_safe(training):
    if not isinstance(training, dict):
        return False
    if training.get("validation_strategy") not in {
        GENERIC_VALIDATION_STRATEGY, *LEGACY_GENERIC_VALIDATION_STRATEGIES
    }:
        return False
    keys = (
        "validation_samples",
        "validation_correct",
        "validation_baseline_correct",
        "validation_non_leading_samples",
        "validation_non_leading_correct",
    )
    values = [training.get(key) for key in keys]
    if any(not isinstance(value, int) or isinstance(value, bool) for value in values):
        return False
    validation_samples, correct, baseline_correct, non_leading, non_leading_correct = values
    leading = baseline_correct
    leading_correct = correct - non_leading_correct
    generic_values = (
        training.get("validation_generic_non_leading_samples"),
        training.get("validation_generic_non_leading_correct"),
    )
    if any(not isinstance(value, int) or isinstance(value, bool) for value in generic_values):
        return False
    generic_non_leading, generic_non_leading_correct = generic_values
    return (
        validation_samples == leading + non_leading
        and validation_samples > 0
        and 0 <= non_leading_correct <= non_leading
        and 0 <= generic_non_leading_correct <= generic_non_leading <= non_leading
        and 0 <= leading_correct <= leading
        and correct > baseline_correct
        and leading_correct == leading
        and generic_non_leading >= MIN_GENERIC_VALIDATION_NON_LEADING
        and generic_non_leading_correct * 5 >= generic_non_leading * 3
    )


def raw_profile_safe(training):
    if not isinstance(training, dict) or training.get("raw_profile_strategy") != RAW_PROFILE_VALIDATION_STRATEGY:
        return False
    keys = (
        "raw_profile_samples",
        "raw_profile_accepted_samples",
        "raw_profile_rejected_samples",
        "raw_profile_validation_samples",
        "raw_profile_validation_accepted_samples",
        "raw_profile_validation_rejected_samples",
        "raw_profile_validation_correct",
        "raw_profile_validation_accepted_correct",
        "raw_profile_validation_rejected_correct",
        "raw_profile_validation_false_promotions",
    )
    values = [training.get(key) for key in keys]
    if any(not isinstance(value, int) or isinstance(value, bool) for value in values):
        return False
    (
        samples, accepted, rejected, validation_samples, validation_accepted, validation_rejected,
        correct, accepted_correct, rejected_correct, false_promotions,
    ) = values
    return (
        samples == accepted + rejected
        and accepted >= MIN_RAW_PROFILE_ACCEPTED_SAMPLES
        and rejected >= MIN_RAW_PROFILE_REJECTED_SAMPLES
        and validation_samples == validation_accepted + validation_rejected
        and validation_accepted >= MIN_RAW_PROFILE_VALIDATION_PER_CLASS
        and validation_rejected >= MIN_RAW_PROFILE_VALIDATION_PER_CLASS
        and correct == validation_samples
        and accepted_correct == validation_accepted
        and rejected_correct == validation_rejected
        and false_promotions == 0
    )


def keyboard_correction_safe(training, patterns, habits, pinyin_prior):
    if not pinyin_prior:
        return False
    if isinstance(training, dict) and training.get("keyboard_correction_safe") is False:
        return False
    return any(active_correction_pattern(pattern) for pattern in patterns) or any(
        active_key_habit(habit) for habit in habits
    )


def atomic_write_model(path, model):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    payload = (json.dumps(model, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )
    if len(payload) > MAX_MODEL_BYTES:
        raise ValueError("model payload exceeds size limit")
    try:
        temporary.write_bytes(payload)
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def merge_pair_evidence(existing, candidate):
    merged = dict(candidate)
    for key, count in existing.items():
        merged[key] = max(count, merged.get(key, 0))
    ranked = sorted(merged.items(), key=lambda item: (-item[1], item[0]))[:MAX_PAIR_EVIDENCE]
    merged = dict(sorted(ranked))
    existing_active = {
        key for key, count in existing.items() if count >= MIN_CANDIDATE_PREFERENCE_COUNT
    }
    candidate_active = {
        key for key, count in candidate.items() if count >= MIN_CANDIDATE_PREFERENCE_COUNT
    }
    if any(merged.get(key, 0) < existing[key] for key in existing_active):
        raise ValueError("safe merge would drop active pair evidence")
    restored_active = sum(
        key not in candidate_active or candidate.get(key, 0) < existing[key]
        for key in existing_active
    )
    return merged, len(existing_active), restored_active


def merge_raw_token_evidence(existing, candidate):
    merged = dict(candidate)
    for key, count in existing.items():
        merged[key] = max(count, merged.get(key, 0))
    ranked = sorted(merged.items(), key=lambda item: (-item[1], item[0]))[:MAX_RAW_TOKEN_EVIDENCE]
    merged = dict(sorted(ranked))
    existing_active = {
        key for key, count in existing.items() if count >= MIN_RAW_PREFERENCE_COUNT
    }
    candidate_active = {
        key for key, count in candidate.items() if count >= MIN_RAW_PREFERENCE_COUNT
    }
    if any(merged.get(key, 0) < existing[key] for key in existing_active):
        raise ValueError("safe merge would drop active raw token evidence")
    restored_active = sum(
        key not in candidate_active or candidate.get(key, 0) < existing[key]
        for key in existing_active
    )
    return merged, len(existing_active), restored_active


def merge_counted_rows(existing, candidate, key_function, limit, is_active, label):
    merged = {}
    for row in [*candidate, *existing]:
        key = key_function(row)
        previous = merged.get(key)
        if previous is None or row["count"] > previous["count"]:
            merged[key] = dict(row)
    ranked = sorted(merged.values(), key=lambda row: (-row["count"], key_function(row)))[:limit]
    merged_keys = {key_function(row): row["count"] for row in ranked}
    existing_active = {
        key_function(row): row["count"] for row in existing if is_active(row)
    }
    candidate_counts = {key_function(row): row["count"] for row in candidate}
    if any(merged_keys.get(key, 0) < count for key, count in existing_active.items()):
        raise ValueError(f"safe merge would drop active {label}")
    restored_active = sum(candidate_counts.get(key, 0) < count for key, count in existing_active.items())
    return ranked, len(existing_active), restored_active


def merge_pinyin_prior(existing, candidate):
    merged = dict(candidate)
    for pinyin, score in existing.items():
        merged[pinyin] = max(score, merged.get(pinyin, 0))
    ranked = sorted(merged.items(), key=lambda item: (-item[1], item[0]))[:MAX_PINYIN_PRIOR_ENTRIES]
    return dict(sorted(ranked))


def validate_correction_patterns(raw_patterns):
    if raw_patterns is None:
        return []
    if not isinstance(raw_patterns, list) or len(raw_patterns) > MAX_CORRECTION_PATTERNS:
        raise ValueError("invalid correction patterns")
    patterns = []
    for raw_pattern in raw_patterns:
        if not isinstance(raw_pattern, dict):
            raise ValueError("invalid correction pattern")
        kind = raw_pattern.get("kind")
        typed = raw_pattern.get("typed")
        replacement = raw_pattern.get("replacement")
        position = raw_pattern.get("position")
        relative_to_end = raw_pattern.get("relative_to_end")
        count = raw_pattern.get("count")
        if kind not in {"missing", "extra", "replace", "transpose"}:
            raise ValueError("invalid correction pattern kind")
        if not isinstance(typed, str) or not isinstance(replacement, str):
            raise ValueError("invalid correction pattern text")
        if any(not value.isascii() or (value and not value.isalnum()) for value in (typed, replacement)):
            raise ValueError("invalid correction pattern text")
        expected_lengths = {
            "missing": (0, 1),
            "extra": (1, 0),
            "replace": (1, 1),
            "transpose": (2, 2),
        }
        if (len(typed), len(replacement)) != expected_lengths[kind] or typed == replacement:
            raise ValueError("invalid correction pattern edit")
        if (
            not isinstance(position, int)
            or isinstance(position, bool)
            or position < 0
            or position > 63
            or not isinstance(relative_to_end, bool)
            or not isinstance(count, int)
            or isinstance(count, bool)
            or count < 1
            or count > 1000000
        ):
            raise ValueError("invalid correction pattern metadata")
        patterns.append({
            "kind": kind,
            "typed": typed,
            "replacement": replacement,
            "position": position,
            "relative_to_end": relative_to_end,
            "count": count,
        })
    return patterns


def validate_key_habits(raw_habits):
    if raw_habits is None:
        return []
    if not isinstance(raw_habits, list) or len(raw_habits) > MAX_KEY_HABITS:
        raise ValueError("invalid key habits")
    habits = []
    expected_lengths = {
        "missing": (0, 1),
        "extra": (1, 0),
        "replace": (1, 1),
        "transpose": (2, 2),
    }
    for raw_habit in raw_habits:
        if not isinstance(raw_habit, dict):
            raise ValueError("invalid key habit")
        kind = raw_habit.get("kind")
        typed = raw_habit.get("typed")
        replacement = raw_habit.get("replacement")
        count = raw_habit.get("count")
        if kind not in expected_lengths or not isinstance(typed, str) or not isinstance(replacement, str):
            raise ValueError("invalid key habit edit")
        if any(not value.isascii() or (value and not value.isalnum()) for value in (typed, replacement)):
            raise ValueError("invalid key habit text")
        if (len(typed), len(replacement)) != expected_lengths[kind] or typed == replacement:
            raise ValueError("invalid key habit edit")
        if not isinstance(count, int) or isinstance(count, bool) or count < 1 or count > 1000000:
            raise ValueError("invalid key habit count")
        habits.append({"kind": kind, "typed": typed, "replacement": replacement, "count": count})
    return habits


def validate_pair_evidence(raw_evidence):
    if raw_evidence is None:
        return {}
    if not isinstance(raw_evidence, dict) or len(raw_evidence) > MAX_PAIR_EVIDENCE:
        raise ValueError("invalid pair evidence")
    evidence = {}
    for key, count in raw_evidence.items():
        if (
            not isinstance(key, str)
            or len(key) != 24
            or any(character not in "0123456789abcdef" for character in key)
            or not isinstance(count, int)
            or isinstance(count, bool)
            or count < 1
            or count > 1000000
        ):
            raise ValueError("invalid pair evidence")
        evidence[key] = count
    return evidence


def validate_raw_token_evidence(raw_evidence):
    if raw_evidence is None:
        return {}
    if not isinstance(raw_evidence, dict) or len(raw_evidence) > MAX_RAW_TOKEN_EVIDENCE:
        raise ValueError("invalid raw token evidence")
    evidence = {}
    for key, count in raw_evidence.items():
        if (
            not isinstance(key, str)
            or len(key) != 24
            or any(character not in "0123456789abcdef" for character in key)
            or not isinstance(count, int)
            or isinstance(count, bool)
            or count < 1
            or count > 1000000
        ):
            raise ValueError("invalid raw token evidence")
        evidence[key] = count
    return evidence


def validate_pinyin_prior(raw_prior):
    if raw_prior is None:
        return {}
    if not isinstance(raw_prior, dict) or len(raw_prior) > MAX_PINYIN_PRIOR_ENTRIES:
        raise ValueError("invalid pinyin prior")
    prior = {}
    for pinyin, score in raw_prior.items():
        if (
            not isinstance(pinyin, str)
            or not 1 <= len(pinyin) <= 64
            or pinyin != pinyin.lower()
            or not pinyin.isascii()
            or not pinyin.isalpha()
            or not isinstance(score, int)
            or isinstance(score, bool)
            or not 1 <= score <= 31
        ):
            raise ValueError("invalid pinyin prior row")
        prior[pinyin] = score
    return prior


def validate_sparse_weights(raw_weights, dimension, label, optional=False):
    if raw_weights is None and optional:
        return {}
    if not isinstance(raw_weights, dict) or len(raw_weights) > dimension:
        raise ValueError(f"invalid {label} weights")
    weights = {}
    for key, value in raw_weights.items():
        if not isinstance(key, str) or not key.isdigit():
            raise ValueError(f"invalid {label} weight index")
        index = int(key)
        if (
            index < 0
            or index >= dimension
            or not isinstance(value, (int, float))
            or isinstance(value, bool)
            or not math.isfinite(value)
        ):
            raise ValueError(f"invalid {label} weight")
        weights[index] = float(value)
    return weights


def validate_model(model):
    if not isinstance(model, dict) or model.get("schema") != SCHEMA:
        raise ValueError("unsupported model schema")
    model_name = model.get("name")
    if model_name is not None and model_name != MODEL_NAME:
        raise ValueError("invalid model name")
    dimension = model.get("dimension")
    if not isinstance(dimension, int) or dimension < 1024 or dimension > 1048576:
        raise ValueError("invalid model dimension")
    feature_version = model.get("feature_version", 1)
    if (
        not isinstance(feature_version, int)
        or isinstance(feature_version, bool)
        or not 1 <= feature_version <= CURRENT_FEATURE_VERSION
    ):
        raise ValueError("invalid feature version")
    architecture = model.get("architecture")
    if architecture is not None and architecture != ARCHITECTURE and architecture not in LEGACY_ARCHITECTURES:
        raise ValueError("invalid model architecture")
    baseline_weight = model.get("baseline_weight")
    if (
        not isinstance(baseline_weight, (int, float))
        or isinstance(baseline_weight, bool)
        or not math.isfinite(baseline_weight)
    ):
        raise ValueError("invalid baseline weight")
    promotion_margin = model.get("promotion_margin", DEFAULT_PROMOTION_MARGIN)
    if (
        not isinstance(promotion_margin, (int, float))
        or isinstance(promotion_margin, bool)
        or not math.isfinite(promotion_margin)
        or promotion_margin < 0
        or promotion_margin > 10
    ):
        raise ValueError("invalid promotion margin")
    weights = validate_sparse_weights(model.get("weights"), dimension, "model")
    validate_sparse_weights(model.get("raw_profile_weights"), dimension, "raw profile", optional=True)
    patterns = validate_correction_patterns(model.get("correction_patterns"))
    habits = validate_key_habits(model.get("key_habits"))
    validate_pair_evidence(model.get("pair_evidence"))
    validate_raw_token_evidence(model.get("raw_token_evidence"))
    pinyin_prior = validate_pinyin_prior(model.get("pinyin_prior"))
    return (
        dimension, float(baseline_weight), float(promotion_margin), feature_version,
        weights, patterns, habits, pinyin_prior,
    )


def load_model(path):
    if not path.is_file():
        raise ValueError(f"model file not found: {path}")
    if path.stat().st_size > MAX_MODEL_BYTES:
        raise ValueError("model file exceeds size limit")
    try:
        model = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read model: {error}") from error
    validated = validate_model(model)
    return (model, *validated)


def command_merge_safe(args):
    try:
        (
            existing_model, _, _, _, existing_feature_version, _, existing_patterns,
            existing_habits, existing_prior,
        ) = load_model(args.existing)
        (
            candidate_model, _, _, _, candidate_feature_version, _, candidate_patterns,
            candidate_habits, candidate_prior,
        ) = load_model(args.candidate)
        if candidate_feature_version < existing_feature_version:
            raise ValueError("candidate feature version is older than the existing model")
        existing_training = existing_model.get("training", {})
        candidate_training = candidate_model.get("training", {})
        if not isinstance(existing_training, dict) or not isinstance(candidate_training, dict):
            raise ValueError("safe merge requires model training metadata")
        if generic_ranking_safe(existing_training) and not generic_ranking_safe(candidate_training):
            raise ValueError("candidate model would disable safe generic ranking")
        if raw_profile_safe(existing_training) and not raw_profile_safe(candidate_training):
            raise ValueError("candidate model would disable the safe raw-English profile")

        existing_evidence = validate_pair_evidence(existing_model.get("pair_evidence"))
        candidate_evidence = validate_pair_evidence(candidate_model.get("pair_evidence"))
        merged_evidence, retained_pairs, restored_pairs = merge_pair_evidence(
            existing_evidence, candidate_evidence
        )
        existing_raw_token_evidence = validate_raw_token_evidence(
            existing_model.get("raw_token_evidence")
        )
        candidate_raw_token_evidence = validate_raw_token_evidence(
            candidate_model.get("raw_token_evidence")
        )
        merged_raw_token_evidence, retained_raw_tokens, restored_raw_tokens = merge_raw_token_evidence(
            existing_raw_token_evidence, candidate_raw_token_evidence
        )
        merged_patterns, retained_patterns, restored_patterns = merge_counted_rows(
            existing_patterns,
            candidate_patterns,
            lambda row: (
                row["kind"], row["typed"], row["replacement"], row["position"],
                row["relative_to_end"],
            ),
            MAX_CORRECTION_PATTERNS,
            active_correction_pattern,
            "correction patterns",
        )
        merged_habits, retained_habits, restored_habits = merge_counted_rows(
            existing_habits,
            candidate_habits,
            lambda row: (row["kind"], row["typed"], row["replacement"]),
            MAX_KEY_HABITS,
            active_key_habit,
            "key habits",
        )
        merged_prior = merge_pinyin_prior(existing_prior, candidate_prior)
        merged_keyboard_safe = bool(merged_prior) and bool(
            any(active_correction_pattern(row) for row in merged_patterns)
            or any(active_key_habit(row) for row in merged_habits)
        )
        existing_keyboard_safe = keyboard_correction_safe(
            existing_training, existing_patterns, existing_habits, existing_prior
        )
        if existing_keyboard_safe and not merged_keyboard_safe:
            raise ValueError("candidate model would disable safe keyboard correction")

        candidate_model["name"] = MODEL_NAME
        candidate_model["pair_evidence"] = merged_evidence
        candidate_model["raw_token_evidence"] = merged_raw_token_evidence
        candidate_model["correction_patterns"] = merged_patterns
        candidate_model["key_habits"] = merged_habits
        candidate_model["pinyin_prior"] = merged_prior
        candidate_training["active_correction_patterns"] = sum(
            active_correction_pattern(row) for row in merged_patterns
        )
        candidate_training["active_key_habits"] = sum(
            active_key_habit(row) for row in merged_habits
        )
        candidate_training["pinyin_prior_entries"] = len(merged_prior)
        candidate_training["raw_token_evidence_entries"] = len(merged_raw_token_evidence)
        candidate_training["active_raw_token_evidence"] = sum(
            count >= MIN_RAW_PREFERENCE_COUNT for count in merged_raw_token_evidence.values()
        )
        candidate_training["keyboard_correction_safe"] = merged_keyboard_safe
        candidate_training["evidence_merge_strategy"] = "max-count-monotonic-v1"
        candidate_model["training"] = candidate_training
        atomic_write_model(args.output, candidate_model)
    except (OSError, ValueError) as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 1

    print("merge-strategy\tmax-count-monotonic-v1")
    print(f"retained-active-pair-evidence\t{retained_pairs}")
    print(f"restored-active-pair-evidence\t{restored_pairs}")
    print(f"retained-active-raw-token-evidence\t{retained_raw_tokens}")
    print(f"restored-active-raw-token-evidence\t{restored_raw_tokens}")
    print(f"retained-active-correction-patterns\t{retained_patterns}")
    print(f"restored-active-correction-patterns\t{restored_patterns}")
    print(f"retained-active-key-habits\t{retained_habits}")
    print(f"restored-active-key-habits\t{restored_habits}")
    return 0


def parse_request(stream):
    rows = {}
    for raw_line in stream:
        line = raw_line.rstrip("\r\n")
        if not line:
            continue
        fields = line.split("\t")
        rows.setdefault(fields[0], []).append(fields)
    if rows.get("protocol", [[None, None]])[0][1:2] != ["1"]:
        raise ValueError("request is missing protocol 1")
    preedit_rows = rows.get("preedit", [])
    candidate_rows = rows.get("candidates", [])
    if not preedit_rows or len(preedit_rows[0]) < 2 or not candidate_rows:
        raise ValueError("request is missing preedit or candidates")
    preedit = preedit_rows[0][1]
    candidates = candidate_rows[0][1:]
    if not preedit or not candidates or len(candidates) > 256 or any(not candidate for candidate in candidates):
        raise ValueError("request has invalid preedit or candidates")
    metadata = [
        {"index": index, "consumed_prefix": 0, "source": "", "score": 0}
        for index in range(len(candidates))
    ]
    for fields in rows.get("candidate_metadata", []):
        if len(fields) < 2:
            continue
        try:
            index = int(fields[1])
        except ValueError:
            continue
        if index < 0 or index >= len(metadata):
            continue
        values = dict(zip(fields[2::2], fields[3::2]))
        try:
            consumed = int(values.get("consumed_prefix", "0"))
            score = int(values.get("score", "0"))
        except ValueError:
            consumed = 0
            score = 0
        metadata[index] = {
            "index": index,
            "consumed_prefix": consumed,
            "source": values.get("source", ""),
            "score": score,
        }

    def events_for(name):
        result = []
        fields_list = rows.get(name, [])
        if not fields_list:
            return result
        for value in fields_list[0][1:]:
            kind, delimiter, text = value.partition(":")
            if delimiter and kind:
                result.append({"type": kind, "text": text})
        return result

    known_corrections = set()
    for fields in rows.get("correction", []):
        if len(fields) >= 3 and plausible_correction(fields[1], fields[2]):
            known_corrections.add((fields[1], fields[2]))
    known_preferences = []
    for fields in rows.get("preference", []):
        if len(fields) < 4:
            continue
        try:
            count = int(fields[3])
        except ValueError:
            count = 1
        known_preferences.append({
            "preedit": fields[1],
            "candidate": fields[2],
            "count": bounded_positive_count(count),
        })
    segment_chains = []
    for fields in rows.get("segment_chain", []):
        if len(fields) < 7:
            continue
        count = 1
        if len(fields) >= 8:
            try:
                count = int(fields[7])
            except ValueError:
                count = 1
        segment_chains.append({
            "original_preedit": fields[1],
            "consumed_preedit": fields[2],
            "committed_text": fields[3],
            "remaining_preedit": fields[4],
            "corrected_full_preedit": fields[5],
            "combined_candidate": fields[6],
            "count": bounded_positive_count(count),
        })
    application_rows = rows.get("application", [])
    context_features = []
    for fields in rows.get("context_features", []):
        context_features.extend(value for value in fields[1:] if valid_context_fingerprint(value))
    if not context_features:
        for fields in rows.get("context", []):
            for value in fields[1:]:
                fingerprint = context_fingerprint(unescape_protocol_text(value))
                if fingerprint:
                    context_features.append(fingerprint)
    surrounding_features = {}
    for fields in rows.get("surrounding_features", []):
        for value in fields[1:]:
            side, delimiter, fingerprint = value.partition(":")
            if delimiter and side in {"before", "after"} and valid_context_fingerprint(fingerprint):
                surrounding_features[side] = fingerprint
    for row_name, side in (("surrounding_before", "before"), ("surrounding_after", "after")):
        if side in surrounding_features:
            continue
        fields_list = rows.get(row_name, [])
        if fields_list and len(fields_list[0]) > 1:
            fingerprint = context_fingerprint(unescape_protocol_text(fields_list[0][1]))
            if fingerprint:
                surrounding_features[side] = fingerprint
    return {
        "preedit": preedit,
        "candidates": candidates,
        "candidate_metadata": metadata,
        "events": events_for("events"),
        "correction_events": events_for("correction_events"),
        "application": application_rows[0][1] if application_rows and len(application_rows[0]) > 1 else "",
        "context_features": context_features[-MAX_CONTEXT_ITEMS:],
        "surrounding_context_features": surrounding_features,
        "known_corrections": known_corrections,
        "known_evidence": {"preferences": known_preferences, "corrections": []},
        "segment_chains": segment_chains,
    }


def raw_english_active(observation):
    preedit = observation.get("preedit", "").lower()
    for index, candidate in enumerate(observation.get("candidates", [])):
        metadata = observation.get("candidate_metadata", [])
        source = metadata[index].get("source", "") if index < len(metadata) else ""
        if candidate.lower() == preedit and source_class(source) == "raw":
            return True
    return False


def apply_correction_pattern(preedit, pattern):
    kind = pattern["kind"]
    typed = pattern["typed"]
    replacement = pattern["replacement"]
    position = pattern["position"]
    if pattern["relative_to_end"]:
        if kind == "missing":
            if position > len(preedit):
                return None
            position = len(preedit) - position
        else:
            if position + len(typed) > len(preedit):
                return None
            position = len(preedit) - position - len(typed)
    if position < 0 or position > len(preedit):
        return None
    if kind == "missing":
        if position < len(preedit) and preedit[position:position + len(replacement)] == replacement:
            return None
        corrected = preedit[:position] + replacement + preedit[position:]
    else:
        if preedit[position:position + len(typed)] != typed:
            return None
        corrected = preedit[:position] + replacement + preedit[position + len(typed):]
    return corrected if plausible_correction(preedit, corrected) else None


def apply_key_habit(preedit, habit):
    kind = habit["kind"]
    typed = habit["typed"]
    replacement = habit["replacement"]
    corrections = set()
    if kind == "missing":
        for position in range(len(preedit) + 1):
            if position < len(preedit) and preedit[position:position + len(replacement)] == replacement:
                continue
            corrections.add(preedit[:position] + replacement + preedit[position:])
    else:
        width = len(typed)
        for position in range(len(preedit) - width + 1):
            if preedit[position:position + width] != typed:
                continue
            corrections.add(preedit[:position] + replacement + preedit[position + width:])
    return {
        corrected for corrected in corrections
        if plausible_correction(preedit, corrected) and complete_pinyin(corrected)
    }


def correction_suggestion(observation, patterns, habits=None, pinyin_prior=None):
    preedit = observation.get("preedit", "")
    known_corrections = observation.get("known_corrections", set())
    prior = pinyin_prior or {}
    if (
        not 2 <= len(preedit) <= 16
        or not safe_input_text(preedit)
        or complete_pinyin(preedit)
        or raw_english_active(observation)
        or any(typo == preedit for typo, _ in known_corrections)
    ):
        return None
    suggestions = {}
    for pattern in patterns:
        if not active_correction_pattern(pattern):
            continue
        corrected = apply_correction_pattern(preedit, pattern)
        if corrected and complete_pinyin(corrected) and (not prior or corrected in prior):
            suggestions[corrected] = max(suggestions.get(corrected, 0), pattern["count"] * 4)
    for habit in habits or []:
        if not active_key_habit(habit):
            continue
        for corrected in apply_key_habit(preedit, habit):
            if prior and corrected not in prior:
                continue
            suggestions[corrected] = max(suggestions.get(corrected, 0), habit["count"])
    ranked = sorted(
        suggestions.items(),
        key=lambda item: (-item[1], -prior.get(item[0], 0), item[0]),
    )
    if not ranked:
        return None
    if len(ranked) > 1 and ranked[0][1] == ranked[1][1]:
        best_prior = prior.get(ranked[0][0], 0)
        next_prior = prior.get(ranked[1][0], 0)
        if best_prior < next_prior + MIN_PINYIN_PRIOR_MARGIN:
            return None
    pair = (preedit, ranked[0][0])
    return None if pair in known_corrections else pair


def promotion_support(model, observation, candidate_index):
    candidates = observation.get("candidates", [])
    if candidate_index <= 0 or candidate_index >= len(candidates):
        return None
    if supervised_evidence_prior(observation, candidate_index) > 0:
        return "supervised-evidence"

    metadata = observation.get("candidate_metadata", [])
    candidate_metadata = metadata[candidate_index] if candidate_index < len(metadata) else {}
    if source_class(candidate_metadata.get("source", "")) == "correction":
        return "correction-source"

    pair_evidence = validate_pair_evidence(model.get("pair_evidence"))
    preedit = observation.get("preedit", "")
    candidate = candidates[candidate_index]
    count = pair_evidence.get(pair_evidence_key(preedit, candidate), 0)
    raw_match = bool(preedit) and candidate.lower() == preedit.lower()
    threshold = MIN_RAW_PREFERENCE_COUNT if raw_match else MIN_CANDIDATE_PREFERENCE_COUNT
    if count >= threshold:
        return "repeated-pair"

    if source_class(candidate_metadata.get("source", "")) != "raw" and generic_ranking_safe(model.get("training", {})):
        return "validated-generic"
    return None


def raw_token_promotion_count(model, observation, offer_index):
    candidates = observation.get("candidates", [])
    if offer_index is None or offer_index <= 0 or offer_index >= len(candidates):
        return 0
    preedit = observation.get("preedit", "")
    if not safe_input_text(preedit) or candidates[offer_index].lower() != preedit.lower():
        return 0
    raw_evidence = validate_raw_token_evidence(model.get("raw_token_evidence"))
    count = raw_evidence.get(raw_token_evidence_key(preedit), 0)
    if count < MIN_RAW_PREFERENCE_COUNT:
        return 0

    # An explicit repeated Chinese choice is stronger than passive English-mode
    # token frequency and gives the user a deterministic way to reverse learning.
    pair_evidence = validate_pair_evidence(model.get("pair_evidence"))
    metadata = observation.get("candidate_metadata", [])
    for index, candidate in enumerate(candidates):
        row = metadata[index] if index < len(metadata) and isinstance(metadata[index], dict) else {}
        if source_class(row.get("source", "")) == "raw":
            continue
        if pair_evidence.get(pair_evidence_key(preedit, candidate), 0) >= MIN_CANDIDATE_PREFERENCE_COUNT:
            return 0
    return count


def command_distill_runtime(args):
    try:
        _, _, raw_observations, skipped = load_training_samples(args.input)
        preferences_path = args.preferences or default_preferences_path()
        model_path = getattr(args, "model", None)
        published_patterns = []
        published_habits = []
        keyboard_safe = False
        if model_path is not None:
            (
                model, _, _, _, _, _, correction_patterns, key_habits, pinyin_prior,
            ) = load_model(model_path)
            training = model.get("training", {})
            keyboard_safe = keyboard_correction_safe(
                training, correction_patterns, key_habits, pinyin_prior
            )
            if keyboard_safe:
                published_patterns = [
                    pattern for pattern in correction_patterns
                    if active_correction_pattern(pattern)
                ]
                published_habits = [
                    habit for habit in key_habits
                    if active_key_habit(habit)
                ]
    except (OSError, ValueError) as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 1

    token_counts = Counter()
    for observation in raw_observations:
        token = observation.get("preedit", "").lower()
        if (
            safe_input_text(token)
            and len(token) >= 2
            and token.isascii()
            and token.isalpha()
            and not complete_pinyin(token)
        ):
            token_counts[token] += 1
    active_tokens = {
        token for token, count in token_counts.items() if count >= MIN_RAW_PREFERENCE_COUNT
    }
    if args.dry_run:
        print(f"runtime-preferences\t{preferences_path}")
        print(f"runtime-raw-token-candidates\t{len(active_tokens)}")
        print(f"runtime-keyboard-safe\t{int(keyboard_safe)}")
        print(f"runtime-correction-pattern-candidates\t{len(published_patterns)}")
        print(f"runtime-key-habit-candidates\t{len(published_habits)}")
        print("runtime-raw-token-updated\t0")
        print("runtime-correction-pattern-updated\t0")
        print("runtime-key-habit-updated\t0")
        print(f"skipped\t{skipped}")
        return 0

    try:
        preferences_path.parent.mkdir(parents=True, exist_ok=True)
        lock_path = preferences_path.with_name(preferences_path.name + ".lock")
        with lock_path.open("a+b") as lock_file:
            os.chmod(lock_path, 0o600)
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            existing_text = (
                preferences_path.read_text(encoding="utf-8", errors="surrogateescape")
                if preferences_path.exists()
                else ""
            )
            selected = {}
            raw_tokens = {}
            correction_patterns = {}
            key_habits = {}
            other_lines = []
            for line in existing_text.splitlines():
                fields = line.removesuffix("\r").split("\t")
                if (
                    len(fields) == 3
                    and fields[0] == "__raw_token__"
                    and safe_input_text(fields[1])
                    and len(fields[1]) >= 2
                    and fields[1].isascii()
                    and fields[1].isalpha()
                    and fields[1] == fields[1].lower()
                    and fields[2].isdigit()
                    and 1 <= int(fields[2]) <= MAX_SAVED_LEARNING_COUNT
                ):
                    raw_tokens[fields[1]] = max(raw_tokens.get(fields[1], 0), int(fields[2]))
                elif (
                    len(fields) == 3
                    and not fields[0].startswith("__")
                    and safe_input_text(fields[0])
                    and safe_input_text(fields[1])
                    and fields[2].isdigit()
                    and 1 <= int(fields[2]) <= MAX_SAVED_LEARNING_COUNT
                ):
                    key = (fields[0], fields[1])
                    selected[key] = max(selected.get(key, 0), int(fields[2]))
                elif fields and fields[0] == "__correction_pattern__":
                    if len(fields) != 7 or not fields[6].isdigit():
                        continue
                    try:
                        row = validate_correction_patterns([{
                            "kind": fields[1],
                            "typed": fields[2],
                            "replacement": fields[3],
                            "position": int(fields[4]),
                            "relative_to_end": fields[5] == "1",
                            "count": int(fields[6]),
                        }])[0]
                    except (ValueError, IndexError):
                        continue
                    if fields[5] not in {"0", "1"}:
                        continue
                    key = (
                        row["kind"], row["typed"], row["replacement"], row["position"],
                        row["relative_to_end"],
                    )
                    correction_patterns[key] = max(correction_patterns.get(key, 0), row["count"])
                elif fields and fields[0] == "__key_habit__":
                    if len(fields) != 5 or not fields[4].isdigit():
                        continue
                    try:
                        row = validate_key_habits([{
                            "kind": fields[1],
                            "typed": fields[2],
                            "replacement": fields[3],
                            "count": int(fields[4]),
                        }])[0]
                    except (ValueError, IndexError):
                        continue
                    key = (row["kind"], row["typed"], row["replacement"])
                    key_habits[key] = max(key_habits.get(key, 0), row["count"])
                else:
                    other_lines.append(line)
            old_raw_tokens = dict(raw_tokens)
            old_correction_patterns = dict(correction_patterns)
            old_key_habits = dict(key_habits)
            migrated = 0
            for token in active_tokens:
                key = (token, token)
                if 0 < selected.get(key, 0) <= MIN_RAW_PREFERENCE_COUNT:
                    selected.pop(key)
                    migrated += 1
                raw_tokens[token] = max(raw_tokens.get(token, 0), MIN_RAW_PREFERENCE_COUNT)
            for pattern in published_patterns:
                key = (
                    pattern["kind"], pattern["typed"], pattern["replacement"], pattern["position"],
                    pattern["relative_to_end"],
                )
                correction_patterns[key] = max(correction_patterns.get(key, 0), pattern["count"])
            for habit in published_habits:
                key = (habit["kind"], habit["typed"], habit["replacement"])
                key_habits[key] = max(key_habits.get(key, 0), habit["count"])
            selected_rows = sorted(
                selected.items(), key=lambda item: (-item[1], item[0][0], item[0][1])
            )[:MAX_RUNTIME_PREFERENCE_ROWS]
            raw_token_rows = sorted(
                raw_tokens.items(), key=lambda item: (-item[1], item[0])
            )[:MAX_RUNTIME_RAW_TOKEN_ROWS]
            correction_pattern_rows = sorted(
                correction_patterns.items(), key=lambda item: (-item[1], item[0])
            )[:MAX_CORRECTION_PATTERNS]
            key_habit_rows = sorted(
                key_habits.items(), key=lambda item: (-item[1], item[0])
            )[:MAX_KEY_HABITS]
            retained_raw_tokens = dict(raw_token_rows)
            retained_correction_patterns = dict(correction_pattern_rows)
            retained_key_habits = dict(key_habit_rows)
            updated = sum(
                old_raw_tokens.get(token, 0) < MIN_RAW_PREFERENCE_COUNT
                and retained_raw_tokens.get(token, 0) >= MIN_RAW_PREFERENCE_COUNT
                for token in active_tokens
            )
            pattern_updated = sum(
                old_correction_patterns.get(key, 0) < count
                and retained_correction_patterns.get(key, 0) >= count
                for key, count in ((
                    (
                        pattern["kind"], pattern["typed"], pattern["replacement"], pattern["position"],
                        pattern["relative_to_end"],
                    ),
                    pattern["count"],
                ) for pattern in published_patterns)
            )
            habit_updated = sum(
                old_key_habits.get(key, 0) < count and retained_key_habits.get(key, 0) >= count
                for key, count in ((
                    (habit["kind"], habit["typed"], habit["replacement"]), habit["count"],
                ) for habit in published_habits)
            )
            output_lines = [
                f"{preedit}\t{candidate}\t{count}"
                for (preedit, candidate), count in selected_rows
            ]
            output_lines.extend(
                f"__raw_token__\t{token}\t{count}" for token, count in raw_token_rows
            )
            output_lines.extend(
                f"__correction_pattern__\t{kind}\t{typed}\t{replacement}\t{position}\t{int(relative)}\t{count}"
                for (kind, typed, replacement, position, relative), count in correction_pattern_rows
            )
            output_lines.extend(
                f"__key_habit__\t{kind}\t{typed}\t{replacement}\t{count}"
                for (kind, typed, replacement), count in key_habit_rows
            )
            output_lines.extend(other_lines)
            payload = "\n".join(output_lines) + ("\n" if output_lines else "")
            if payload != existing_text:
                temporary = preferences_path.with_name(
                    preferences_path.name + f".tmp.{os.getpid()}"
                )
                try:
                    temporary.write_text(payload, encoding="utf-8", errors="surrogateescape")
                    os.chmod(temporary, 0o600)
                    os.replace(temporary, preferences_path)
                except OSError:
                    try:
                        temporary.unlink()
                    except OSError:
                        pass
                    raise
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
    except OSError as error:
        print(f"tipe-personal-model: cannot update runtime preferences: {error}", file=sys.stderr)
        return 1

    print(f"runtime-preferences\t{preferences_path}")
    print(f"runtime-raw-token-candidates\t{len(active_tokens)}")
    print(f"runtime-raw-token-active\t{sum(count >= MIN_RAW_PREFERENCE_COUNT for count in retained_raw_tokens.values())}")
    print(f"runtime-raw-token-updated\t{updated}")
    print(f"runtime-keyboard-safe\t{int(keyboard_safe)}")
    print(f"runtime-correction-pattern-candidates\t{len(published_patterns)}")
    print(
        "runtime-correction-pattern-active\t"
        f"{sum(count >= correction_pattern_activation_count(key[0]) for key, count in retained_correction_patterns.items())}"
    )
    print(f"runtime-correction-pattern-updated\t{pattern_updated}")
    print(f"runtime-key-habit-candidates\t{len(published_habits)}")
    print(
        "runtime-key-habit-active\t"
        f"{sum(count >= key_habit_activation_count(key[0]) for key, count in retained_key_habits.items())}"
    )
    print(f"runtime-key-habit-updated\t{habit_updated}")
    print(f"skipped\t{skipped}")
    return 0


def command_distill_raw(args):
    return command_distill_runtime(args)


def command_train(args):
    samples, supervision_observations, raw_profile_auxiliary_observations, skipped = load_training_samples(
        args.input
    )
    if len(supervision_observations) < args.min_samples:
        print(
            f"tipe-personal-model: need at least {args.min_samples} supervision samples, "
            f"found {len(supervision_observations)}",
            file=sys.stderr,
        )
        return 1
    pinyin_paths = [] if args.no_pinyin_prior else (args.pinyin_dictionary or default_pinyin_dictionary_paths())
    pinyin_prior, pinyin_prior_sources = load_pinyin_prior(pinyin_paths)
    chinese_ranking_samples = [sample for sample in samples if not has_raw_candidate(sample[0])]
    validation = temporal_validation(
        chinese_ranking_samples, args.dimension, args.epochs, args.learning_rate, args.baseline_weight,
        args.promotion_margin, args.seed,
        args.validation_percent, CURRENT_FEATURE_VERSION,
    )
    raw_validation = raw_profile_validation(
        samples, args.dimension, args.epochs, args.learning_rate, args.baseline_weight,
        args.promotion_margin, args.seed, args.validation_percent, CURRENT_FEATURE_VERSION,
        raw_profile_auxiliary_observations,
    )
    if samples:
        weights, updates, correct, margins = train_model(
            samples, args.dimension, args.epochs, args.learning_rate, args.baseline_weight, args.seed,
            CURRENT_FEATURE_VERSION,
        )
    else:
        weights, updates, correct, margins = {}, 0, 0, []
    raw_profile_rows = []
    for observation, gold in samples:
        offer_index = raw_candidate_index(observation, strict_offer=True)
        if offer_index is not None and offer_index > 0:
            raw_profile_rows.append((observation, gold == offer_index, False))
    raw_profile_rows.extend(
        (observation, True, True) for observation in raw_profile_auxiliary_observations
    )
    raw_profile_weights = train_raw_profile(
        raw_profile_rows, args.dimension, args.epochs, args.learning_rate, args.seed, CURRENT_FEATURE_VERSION
    )
    correction_samples = [(observation, 0) for observation in supervision_observations]
    correction_patterns, key_habits, correction_pairs, correction_observations = learn_correction_patterns(
        correction_samples
    )
    pair_evidence = learn_pair_evidence(samples)
    raw_token_evidence = learn_raw_token_evidence(raw_profile_auxiliary_observations)
    active_correction_patterns = sum(
        active_correction_pattern(pattern) for pattern in correction_patterns
    )
    active_key_habits = sum(active_key_habit(habit) for habit in key_habits)
    keyboard_safe = bool(pinyin_prior) and bool(active_correction_patterns or active_key_habits)
    if not samples and (active_correction_patterns or active_key_habits):
        validation["recommendation"] = "ready"
    non_leading_samples = sum(1 for _, gold in chinese_ranking_samples if gold != 0)
    model = {
        "schema": SCHEMA,
        "name": MODEL_NAME,
        "architecture": ARCHITECTURE,
        "feature_version": CURRENT_FEATURE_VERSION,
        "dimension": args.dimension,
        "baseline_weight": args.baseline_weight,
        "promotion_margin": args.promotion_margin,
        "weights": {str(index): round(value, 8) for index, value in sorted(weights.items())},
        "raw_profile_weights": {
            str(index): round(value, 8) for index, value in sorted(raw_profile_weights.items())
        },
        "pair_evidence": pair_evidence,
        "raw_token_evidence": raw_token_evidence,
        "correction_patterns": correction_patterns,
        "key_habits": key_habits,
        "pinyin_prior": pinyin_prior,
        "training": {
            "samples": len(supervision_observations),
            "ranking_samples": len(samples),
            "chinese_ranking_samples": len(chinese_ranking_samples),
            "correction_only_samples": len(supervision_observations) - len(samples),
            "skipped": skipped,
            "epochs": args.epochs,
            "updates": updates,
            "accuracy": round(correct / len(samples), 6) if samples else None,
            "minimum_margin": round(min(margins), 6) if margins else None,
            "seed": args.seed,
            "correction_pairs": correction_pairs,
            "correction_observations": correction_observations,
            "active_correction_patterns": active_correction_patterns,
            "active_key_habits": active_key_habits,
            "pinyin_prior_entries": len(pinyin_prior),
            "pinyin_prior_sources": pinyin_prior_sources,
            "validation_strategy": GENERIC_VALIDATION_STRATEGY,
            "validation_samples": validation["samples"],
            "validation_correct": validation["correct"],
            "validation_accuracy": validation["accuracy"],
            "validation_baseline_correct": validation["baseline_correct"],
            "validation_baseline_accuracy": validation["baseline_accuracy"],
            "validation_gain": validation["gain"],
            "non_leading_samples": non_leading_samples,
            "validation_non_leading_samples": validation["non_leading_samples"],
            "validation_non_leading_correct": validation["non_leading_correct"],
            "validation_non_leading_accuracy": validation["non_leading_accuracy"],
            "validation_leading_samples": validation["leading_samples"],
            "validation_leading_correct": validation["leading_correct"],
            "validation_generic_non_leading_samples": validation["generic_non_leading_samples"],
            "validation_generic_non_leading_correct": validation["generic_non_leading_correct"],
            "validation_generic_non_leading_accuracy": validation["generic_non_leading_accuracy"],
            "validation_generic_excluded_direct_evidence": validation["generic_excluded_direct_evidence"],
            "validation_generic_excluded_seen_preedit": validation["generic_excluded_seen_preedit"],
            "validation_generic_excluded_raw_candidate": validation["generic_excluded_raw_candidate"],
            "validation_generic_excluded_derived_prefix": validation["generic_excluded_derived_prefix"],
            "raw_profile_strategy": raw_validation["strategy"],
            "raw_profile_samples": raw_validation["samples"],
            "raw_profile_accepted_samples": raw_validation["accepted_samples"],
            "raw_profile_rejected_samples": raw_validation["rejected_samples"],
            "raw_profile_auxiliary_positive_samples": raw_validation["auxiliary_positive_samples"],
            "raw_token_evidence_entries": len(raw_token_evidence),
            "active_raw_token_evidence": sum(
                count >= MIN_RAW_PREFERENCE_COUNT for count in raw_token_evidence.values()
            ),
            "raw_profile_validation_samples": raw_validation["validation_samples"],
            "raw_profile_validation_accepted_samples": raw_validation["validation_accepted_samples"],
            "raw_profile_validation_rejected_samples": raw_validation["validation_rejected_samples"],
            "raw_profile_validation_correct": raw_validation["validation_correct"],
            "raw_profile_validation_accepted_correct": raw_validation["validation_accepted_correct"],
            "raw_profile_validation_rejected_correct": raw_validation["validation_rejected_correct"],
            "raw_profile_validation_baseline_correct": raw_validation["validation_baseline_correct"],
            "raw_profile_validation_false_promotions": raw_validation["validation_false_promotions"],
            "raw_profile_validation_accuracy": raw_validation["validation_accuracy"],
            "raw_profile_recommendation": raw_validation["recommendation"],
            "keyboard_correction_safe": keyboard_safe,
            "recommendation": validation["recommendation"],
        },
    }
    model["training"]["generic_ranking_safe"] = generic_ranking_safe(model["training"])
    model["training"]["raw_profile_safe"] = raw_profile_safe(model["training"])
    # Generic ranking remains inert until its own gate passes. A weak generic
    # holdout must not block independently validated keyboard or raw components.
    component_update_safe = (
        not model["training"]["generic_ranking_safe"]
        and (keyboard_safe or model["training"]["raw_profile_safe"])
    )
    model["training"]["component_update_safe"] = component_update_safe
    try:
        atomic_write_model(args.output, model)
    except (OSError, ValueError) as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 1
    print(f"model\t{args.output}")
    print(f"name\t{MODEL_NAME}")
    print(f"samples\t{len(supervision_observations)}")
    print(f"ranking-samples\t{len(samples)}")
    print(f"chinese-ranking-samples\t{len(chinese_ranking_samples)}")
    print(f"correction-only-samples\t{len(supervision_observations) - len(samples)}")
    print(f"skipped\t{skipped}")
    print(f"features\t{len(weights)}")
    print(f"raw-profile-features\t{len(raw_profile_weights)}")
    print(f"feature-version\t{CURRENT_FEATURE_VERSION}")
    print(f"pinyin-prior-entries\t{len(pinyin_prior)}")
    print(f"pinyin-prior-sources\t{pinyin_prior_sources}")
    print(f"pair-evidence\t{len(pair_evidence)}")
    print(f"active-pair-evidence\t{sum(count >= MIN_CANDIDATE_PREFERENCE_COUNT for count in pair_evidence.values())}")
    print(f"raw-token-evidence\t{len(raw_token_evidence)}")
    print(
        "active-raw-token-evidence\t"
        f"{sum(count >= MIN_RAW_PREFERENCE_COUNT for count in raw_token_evidence.values())}"
    )
    print(f"correction-pairs\t{correction_pairs}")
    print(f"correction-observations\t{correction_observations}")
    print(f"correction-patterns\t{len(correction_patterns)}")
    print(
        "active-correction-patterns\t"
        f"{active_correction_patterns}"
    )
    print(f"key-habits\t{len(key_habits)}")
    print(f"active-key-habits\t{active_key_habits}")
    print(f"non-leading-samples\t{non_leading_samples}")
    print(f"updates\t{updates}")
    print(f"accuracy\t{correct}/{len(samples)}")
    print(f"validation-strategy\t{GENERIC_VALIDATION_STRATEGY}")
    if validation["samples"]:
        print(f"validation-accuracy\t{validation['correct']}/{validation['samples']}")
        print(f"validation-baseline-accuracy\t{validation['baseline_correct']}/{validation['samples']}")
        print(f"validation-gain\t{validation['gain']}")
        print(
            "validation-non-leading-accuracy\t"
            f"{validation['non_leading_correct']}/{validation['non_leading_samples']}"
        )
        print(
            "validation-generic-non-leading-accuracy\t"
            f"{validation['generic_non_leading_correct']}/{validation['generic_non_leading_samples']}"
        )
        print(
            "validation-generic-excluded\t"
            f"direct-evidence:{validation['generic_excluded_direct_evidence']}\t"
            f"seen-preedit:{validation['generic_excluded_seen_preedit']}\t"
            f"raw-candidate:{validation['generic_excluded_raw_candidate']}\t"
            f"derived-prefix:{validation['generic_excluded_derived_prefix']}"
        )
    else:
        print("validation-accuracy\tunavailable")
        print("validation-baseline-accuracy\tunavailable")
        print("validation-gain\tunavailable")
        print("validation-non-leading-accuracy\tunavailable")
        print("validation-generic-non-leading-accuracy\tunavailable")
        print("validation-generic-excluded\tunavailable")
    print(f"recommendation\t{validation['recommendation']}")
    print(f"generic-ranking-safe\t{int(model['training']['generic_ranking_safe'])}")
    print(f"raw-profile-samples\t{raw_validation['samples']}")
    print(f"raw-profile-accepted\t{raw_validation['accepted_samples']}")
    print(f"raw-profile-rejected\t{raw_validation['rejected_samples']}")
    print(f"raw-profile-auxiliary-positive\t{raw_validation['auxiliary_positive_samples']}")
    if raw_validation["validation_samples"]:
        print(
            "raw-profile-validation-accuracy\t"
            f"{raw_validation['validation_correct']}/{raw_validation['validation_samples']}"
        )
        print(
            "raw-profile-validation-false-promotions\t"
            f"{raw_validation['validation_false_promotions']}"
        )
    else:
        print("raw-profile-validation-accuracy\tunavailable")
        print("raw-profile-validation-false-promotions\tunavailable")
    print(f"raw-profile-recommendation\t{raw_validation['recommendation']}")
    print(f"raw-profile-safe\t{int(model['training']['raw_profile_safe'])}")
    print(f"keyboard-correction-safe\t{int(keyboard_safe)}")
    print(f"component-update-safe\t{int(component_update_safe)}")
    return 0


def command_predict(args):
    try:
        (
            model, dimension, baseline_weight, promotion_margin, feature_version,
            weights, patterns, habits, pinyin_prior,
        ) = load_model(args.model)
        observation = parse_request(sys.stdin)
    except ValueError as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 1
    emitted_corrections = set()
    for pair in possible_corrections_from_events(
        observation.get("correction_events", []), observation["preedit"]
    ):
        if pair not in observation.get("known_corrections", set()) and pair not in emitted_corrections:
            print(f"correction\t{pair[0]}\t{pair[1]}")
            emitted_corrections.add(pair)
    training = model.get("training", {})
    suggestion = (
        correction_suggestion(observation, patterns, habits, pinyin_prior)
        if keyboard_correction_safe(training, patterns, habits, pinyin_prior)
        else None
    )
    if suggestion and suggestion not in emitted_corrections:
        print(f"correction\t{suggestion[0]}\t{suggestion[1]}")
        emitted_corrections.add(suggestion)

    scores, _ = candidate_scores(observation, weights, dimension, baseline_weight, feature_version)
    raw_weights = validate_sparse_weights(
        model.get("raw_profile_weights"), dimension, "raw profile", optional=True
    )
    offer_index = raw_candidate_index(observation, strict_offer=True)
    raw_token_count = raw_token_promotion_count(model, observation, offer_index)
    raw_token_promoted = raw_token_count >= MIN_RAW_PREFERENCE_COUNT
    raw_profile_score = None
    raw_profile_promoted = False
    if (
        not raw_token_promoted
        and offer_index is not None
        and offer_index > 0
        and raw_weights
        and raw_profile_safe(training)
    ):
        raw_profile_score = dot(
            raw_weights, raw_profile_features(observation, dimension, feature_version)
        )
        raw_profile_promoted = raw_profile_score >= promotion_margin
    if raw_token_promoted:
        support = "repeated-english-token"
        promoted = True
        order = [offer_index] + [index for index in range(len(scores)) if index != offer_index]
    elif raw_profile_promoted:
        support = "validated-raw-profile"
        promoted = True
        order = [offer_index] + [index for index in range(len(scores)) if index != offer_index]
    else:
        scored_order = sorted(range(len(scores)), key=lambda index: (-scores[index], index))
        support = promotion_support(model, observation, scored_order[0])
        promoted = (
            scored_order[0] != 0
            and scores[scored_order[0]] >= scores[0] + promotion_margin
            and support is not None
        )
        order = scored_order if promoted else list(range(len(scores)))
    for index in order:
        print(f"candidate\t{observation['candidates'][index]}")
    if args.explain:
        decision = "promote" if promoted else "preserve"
        print(
            f"decision\t{decision}\tmargin\t{promotion_margin:.6f}\tsupport\t{support or 'insufficient'}",
            file=sys.stderr,
        )
        if raw_profile_score is not None:
            print(f"raw-profile-score\t{raw_profile_score:.6f}", file=sys.stderr)
        if raw_token_count:
            print(f"raw-token-evidence\t{raw_token_count}", file=sys.stderr)
        for rank, index in enumerate(order, 1):
            print(
                f"score\t{rank}\t{index}\t{scores[index]:.6f}\t{observation['candidates'][index]}",
                file=sys.stderr,
            )
    return 0


def command_inspect(args):
    try:
        (
            model, _, _, promotion_margin, feature_version,
            weights, patterns, habits, pinyin_prior,
        ) = load_model(args.model)
    except ValueError as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 1
    training = model.get("training", {})
    if not isinstance(training, dict):
        training = {}
    print(f"model\t{args.model}")
    print(f"name\t{MODEL_NAME}")
    print(f"schema\t{model['schema']}")
    print(f"architecture\t{model.get('architecture', 'legacy-hashed-pairwise-ranker')}")
    print(f"feature-version\t{feature_version}")
    print(f"dimension\t{model['dimension']}")
    print(f"promotion-margin\t{promotion_margin}")
    print(f"features\t{len(weights)}")
    raw_weights = validate_sparse_weights(
        model.get("raw_profile_weights"), model["dimension"], "raw profile", optional=True
    )
    print(f"raw-profile-features\t{len(raw_weights)}")
    pair_evidence = validate_pair_evidence(model.get("pair_evidence"))
    print(f"pair-evidence\t{len(pair_evidence)}")
    print(f"active-pair-evidence\t{sum(count >= MIN_CANDIDATE_PREFERENCE_COUNT for count in pair_evidence.values())}")
    raw_token_evidence = validate_raw_token_evidence(model.get("raw_token_evidence"))
    print(f"raw-token-evidence\t{len(raw_token_evidence)}")
    print(
        "active-raw-token-evidence\t"
        f"{sum(count >= MIN_RAW_PREFERENCE_COUNT for count in raw_token_evidence.values())}"
    )
    print(f"generic-ranking-safe\t{int(generic_ranking_safe(training))}")
    print(f"raw-profile-safe\t{int(raw_profile_safe(training))}")
    print(f"keyboard-correction-safe\t{int(keyboard_correction_safe(training, patterns, habits, pinyin_prior))}")
    print(f"component-update-safe\t{int(training.get('component_update_safe') is True)}")
    if training.get("evidence_merge_strategy") == "max-count-monotonic-v1":
        print("evidence-merge-strategy\tmax-count-monotonic-v1")
    print(f"correction-patterns\t{len(patterns)}")
    print(
        "active-correction-patterns\t"
        f"{sum(active_correction_pattern(pattern) for pattern in patterns)}"
    )
    print(f"key-habits\t{len(habits)}")
    print(f"active-key-habits\t{sum(active_key_habit(habit) for habit in habits)}")
    print(f"pinyin-prior-entries\t{len(pinyin_prior)}")
    validation_strategy = training.get("validation_strategy")
    if validation_strategy in {
        GENERIC_VALIDATION_STRATEGY, *LEGACY_GENERIC_VALIDATION_STRATEGIES,
        "evidence-isolated-temporal-v2", "stratified-temporal-v1", "temporal-v1"
    }:
        print(f"training-validation-strategy\t{validation_strategy}")
    for key in (
        "samples", "ranking_samples", "chinese_ranking_samples", "correction_only_samples", "skipped", "epochs",
        "updates", "accuracy",
        "minimum_margin", "seed",
        "correction_pairs", "correction_observations", "active_correction_patterns", "active_key_habits",
        "pinyin_prior_entries", "pinyin_prior_sources",
        "validation_samples", "validation_correct", "validation_accuracy", "validation_baseline_correct",
        "validation_baseline_accuracy", "validation_gain", "non_leading_samples",
        "validation_non_leading_samples", "validation_non_leading_correct",
        "validation_non_leading_accuracy", "validation_leading_samples", "validation_leading_correct",
        "validation_generic_non_leading_samples", "validation_generic_non_leading_correct",
        "validation_generic_non_leading_accuracy", "validation_generic_excluded_direct_evidence",
        "validation_generic_excluded_seen_preedit", "validation_generic_excluded_raw_candidate",
        "validation_generic_excluded_derived_prefix",
        "raw_profile_samples", "raw_profile_accepted_samples", "raw_profile_rejected_samples",
        "raw_profile_auxiliary_positive_samples",
        "raw_token_evidence_entries", "active_raw_token_evidence",
        "raw_profile_validation_samples", "raw_profile_validation_accepted_samples",
        "raw_profile_validation_rejected_samples", "raw_profile_validation_correct",
        "raw_profile_validation_accepted_correct", "raw_profile_validation_rejected_correct",
        "raw_profile_validation_baseline_correct", "raw_profile_validation_false_promotions",
        "raw_profile_validation_accuracy",
        "recommendation",
    ):
        value = effective_training_recommendation(training) if key == "recommendation" else training.get(key)
        if key == "recommendation":
            if value:
                print(f"training-recommendation\t{value}")
        elif isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
            print(f"training-{key.replace('_', '-')}\t{value}")
    raw_recommendation = training.get("raw_profile_recommendation")
    if raw_recommendation in {"ready", "keep-heuristic", "collect-more-data"}:
        print(f"training-raw-profile-recommendation\t{raw_recommendation}")
    return 0


def command_self_test():
    repeated_keys = [
        {"type": "letter", "text": "l"},
        {"type": "letter", "text": "l"},
        {"type": "backspace", "text": ""},
    ]
    if event_feature_tokens(repeated_keys, detailed=True) != ["letter:l", "letter:l", "backspace"]:
        print("tipe-personal-model: detailed key sequence self-test failed", file=sys.stderr)
        return 1
    if (
        aligned_pinyin_syllables("chongkai", 2) != ("chong", "kai")
        or aligned_pinyin_syllables("xian", 2) != ("xi", "an")
        or aligned_pinyin_syllables("fangan", 2) != ("fang", "an")
    ):
        print("tipe-personal-model: pinyin alignment self-test failed", file=sys.stderr)
        return 1
    observation = {
        "preedit": "start",
        "candidates": ["开始", "start"],
        "candidate_metadata": [
            {"consumed_prefix": 0, "source": "lookup"},
            {"consumed_prefix": 0, "source": "raw"},
        ],
        "events": [{"type": "letter", "text": "s"}, {"type": "space", "text": ""}],
    }
    samples = [(observation, 1), (observation, 1)]
    weights, updates, correct, _ = train_model(samples, 4096, 6, 0.25, 0.05, 7)
    scores, _ = candidate_scores(observation, weights, 4096, 0.05, CURRENT_FEATURE_VERSION)
    correction_observation = {
        "preedit": "nihao",
        "correction_events": (
            [{"type": "letter", "text": character} for character in "ihao"]
            + [{"type": "backspace", "text": ""} for _ in "ihao"]
            + [{"type": "letter", "text": character} for character in "nihao"]
        ),
    }
    patterns, habits, _, _ = learn_correction_patterns([(correction_observation, 0), (correction_observation, 0)])
    correction = correction_suggestion(
        {
            "preedit": "imen",
            "candidates": ["一门", "你们"],
            "candidate_metadata": [
                {"source": "lookup"},
                {"source": "lookup"},
            ],
        },
        patterns,
        habits,
    )
    phonetic_training = {
        "preedit": "chong",
        "candidates": ["冲", "重"],
        "candidate_metadata": [{"source": "lookup"}, {"source": "lookup"}],
        "events": [{"type": "letter", "text": "c"}],
        "correction_events": [],
    }
    phonetic_weights, _, _, _ = train_model(
        [(phonetic_training, 1)] * 6, 4096, 16, 0.25, 0.05, 9
    )
    phonetic_transfer = {
        "preedit": "chongkai",
        "candidates": ["冲开", "重开"],
        "candidate_metadata": [{"source": "lookup"}, {"source": "lookup"}],
        "events": [{"type": "letter", "text": "c"}],
        "correction_events": [],
    }
    phonetic_scores, _ = candidate_scores(
        phonetic_transfer, phonetic_weights, 4096, 0.05, CURRENT_FEATURE_VERSION
    )

    def trail_observation(signal):
        return {
            "preedit": "ceshi",
            "candidates": ["甲", "乙"],
            "candidate_metadata": [{"source": "lookup"}, {"source": "lookup"}],
            "events": [{"type": "letter", "text": "c"}, {"type": "space", "text": ""}],
            "correction_events": (
                [{"type": "observed", "text": signal}]
                + [{"type": "cursor-move", "text": "Left"} for _ in range(23)]
            ),
        }

    trail_samples = []
    for _ in range(6):
        trail_samples.extend([(trail_observation("F13"), 0), (trail_observation("F14"), 1)])
    trail_weights, _, _, _ = train_model(trail_samples, 4096, 16, 0.25, 0.05, 9)
    first_trail_scores, _ = candidate_scores(
        trail_observation("F13"), trail_weights, 4096, 0.05, CURRENT_FEATURE_VERSION
    )
    second_trail_scores, _ = candidate_scores(
        trail_observation("F14"), trail_weights, 4096, 0.05, CURRENT_FEATURE_VERSION
    )
    if (
        updates == 0
        or correct != len(samples)
        or scores[1] <= scores[0]
        or correction != ("imen", "nimen")
        or phonetic_scores[1] < phonetic_scores[0] + DEFAULT_PROMOTION_MARGIN
        or first_trail_scores[0] <= first_trail_scores[1]
        or second_trail_scores[1] <= second_trail_scores[0]
    ):
        print("tipe-personal-model: self-test failed", file=sys.stderr)
        return 1
    print("TiP model ok")
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Train and run TiP, TiPE's click-triggered keyboard model.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    train = subparsers.add_parser("train", help="train TiP from tipe.training.v1 JSONL")
    train.add_argument("--input", type=Path, default=Path("-"), help="JSONL path, default stdin")
    train.add_argument("--output", type=Path, help="model output path")
    train.add_argument("--dimension", type=int, default=DEFAULT_DIMENSION)
    train.add_argument("--epochs", type=int, default=8)
    train.add_argument("--learning-rate", type=float, default=0.25)
    train.add_argument("--baseline-weight", type=float, default=0.05)
    train.add_argument("--promotion-margin", type=float, default=DEFAULT_PROMOTION_MARGIN)
    train.add_argument("--validation-percent", type=int, default=20)
    train.add_argument("--seed", type=int, default=17)
    train.add_argument("--min-samples", type=int, default=4)
    train.add_argument(
        "--pinyin-dictionary", type=Path, action="append", default=[],
        help="Rime pinyin dictionary used for the compact correction prior; repeatable",
    )
    train.add_argument(
        "--no-pinyin-prior", action="store_true",
        help="train without the system pinyin correction prior",
    )

    predict = subparsers.add_parser("predict", help="run TiP on a TiPE request TSV from stdin")
    predict.add_argument("--model", type=Path, help="model path")
    predict.add_argument("--explain", action="store_true", help="print candidate scores to stderr")

    inspect = subparsers.add_parser("inspect", help="show non-sensitive model metadata")
    inspect.add_argument("--model", type=Path, help="model path")

    merge_safe = subparsers.add_parser(
        "merge-safe", help="merge retained evidence into a validated candidate model"
    )
    merge_safe.add_argument("--existing", type=Path, required=True)
    merge_safe.add_argument("--candidate", type=Path, required=True)
    merge_safe.add_argument("--output", type=Path, required=True)

    distill_raw = subparsers.add_parser(
        "distill-raw", help="publish repeated English-mode tokens to lightweight runtime preferences"
    )
    distill_raw.add_argument("--input", type=Path, default=Path("-"), help="JSONL path, default stdin")
    distill_raw.add_argument("--preferences", type=Path, help="candidate preference TSV path")
    distill_raw.add_argument("--dry-run", action="store_true")

    distill_runtime = subparsers.add_parser(
        "distill-runtime",
        help="publish safe TiP keyboard habits and repeated English tokens to the lightweight runtime",
    )
    distill_runtime.add_argument("--input", type=Path, default=Path("-"), help="JSONL path, default stdin")
    distill_runtime.add_argument("--model", type=Path, required=True, help="validated TiP model path")
    distill_runtime.add_argument("--preferences", type=Path, help="candidate preference TSV path")
    distill_runtime.add_argument("--dry-run", action="store_true")

    subparsers.add_parser("self-test", help="run an in-memory trainer and predictor check")
    args = parser.parse_args(argv)
    if args.command == "train":
        if args.dimension < 1024 or args.dimension > 1048576:
            parser.error("--dimension must be between 1024 and 1048576")
        if args.epochs < 1 or args.epochs > 100:
            parser.error("--epochs must be between 1 and 100")
        if (
            not 0 < args.learning_rate <= 10
            or not 0 <= args.baseline_weight <= 10
            or not 0 <= args.promotion_margin <= 10
        ):
            parser.error("learning rate and baseline weight are out of range")
        if args.validation_percent < 0 or args.validation_percent > 50:
            parser.error("--validation-percent must be between 0 and 50")
        if args.min_samples < 1:
            parser.error("--min-samples must be positive")
        if args.no_pinyin_prior and args.pinyin_dictionary:
            parser.error("--no-pinyin-prior cannot be combined with --pinyin-dictionary")
        for dictionary in args.pinyin_dictionary:
            if not dictionary.is_file() or not os.access(dictionary, os.R_OK):
                parser.error(f"--pinyin-dictionary is not readable: {dictionary}")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        model_path = default_model_path()
    except ValueError as error:
        print(f"tipe-personal-model: {error}", file=sys.stderr)
        return 2
    if args.command == "train":
        args.output = args.output or model_path
        return command_train(args)
    if args.command == "predict":
        args.model = args.model or model_path
        return command_predict(args)
    if args.command == "inspect":
        args.model = args.model or model_path
        return command_inspect(args)
    if args.command == "merge-safe":
        return command_merge_safe(args)
    if args.command == "distill-raw":
        return command_distill_raw(args)
    if args.command == "distill-runtime":
        return command_distill_runtime(args)
    return command_self_test()


if __name__ == "__main__":
    raise SystemExit(main())
