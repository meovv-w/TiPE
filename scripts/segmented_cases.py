"""Actual desktop segmented selection contracts, used by the owned runner."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def key(name):
    return ('-k', name)


def selection(raw, text, consumed, numeric=False):
    """Discover a fixture choice, then navigate via actual desktop keys.

    The probe only supplies an index: success requires independently observed
    client text and preedit to match the literal contracts below. Its state is
    private and fresh; it cannot clear or rewrite the desktop's learning.
    """
    with tempfile.TemporaryDirectory(prefix='tipe-segment-choice-') as state:
        result = subprocess.run(
            [str(ROOT/'build/tipe-demo'), '--json-lines', '--state-dir', state],
            input=' '.join(raw)+' Tab\n', text=True, capture_output=True, check=True,
            env=dict(os.environ, TIPE_CLASSIC_GRID='1'))
    view = json.loads(result.stdout.splitlines()[-1])
    index = next(i for i, candidate in enumerate(view['candidates'])
                 if candidate['text'] == text and candidate['consumed'] == consumed)
    sequence = [*key('Tab'), *key('Home')]
    # The real delivered grid holds forty candidates per page. Right moves
    # through visual cells; numeric selection is local to the current ten.
    for _ in range(index//40):
        sequence.extend(key('Page_Down'))
    for _ in range(index%40):
        sequence.extend(key('Right'))
    sequence.extend((str((index%10+1)%10),) if numeric else key('space'))
    return sequence, dict(raw=raw, text=text, consumed=consumed, index=index, numeric=numeric)


def execute(toolkit, keys, clear, checkpoint, finish, record):
    cases = [
        ('Partial selection retains suffix', 'nihao', [('你', 2)], 'space', '你好'),
        ('Numeric prefix after first ten candidates', 'nihao', [('你', 2)], 'numeric', '你好'),
        ('Long phrase preserves remaining syllables', 'nihaoshijie', [('你好', 5)], 'shift', '你好世界'),
        ('Edit remaining pinyin after partial choice', 'nihao', [('你', 2)], 'edit', '你ao'),
        ('Cancel remaining pinyin keeps selected prefix', 'nihao', [('你', 2)], 'cancel', '你'),
        ('Abbreviation selection chain woc', 'woc', [('我', 2), ('操', 1)], 'none', '我操'),
        ('Three successive phrase segments', 'zhonghuarenmingongheguo',
         [('中华', 8), ('人民', 6), ('共和国', 9)], 'none', '中华人民共和国'),
        ('Long composition crosses decoder window without losing suffix', 'nihao'*12+'zhongguo',
         [('你好'*12,60)], 'shift', '你好'*12+'中国'),
    ]
    if toolkit in ('gtk', 'qt', 'browser'):
        cases.append(('Restore prefix after deleting all suffix pinyin', 'nihao',
                      [('你', 2)], 'restore', 'ni'))
    if selected := os.environ.get('TIPE_SEGMENT_CASE'):
        cases = [case for case in cases if selected.lower() in case[0].lower()]
        assert cases, 'No matching segmented case'
    for name, raw, choices, ending, expected in cases:
        clear()
        keys((raw,))
        committed = ''
        remaining = raw
        checks = []
        navigation = []
        for text, consumed in choices:
            sequence, chosen = selection(remaining, text, consumed, ending == 'numeric')
            navigation.append(chosen)
            keys(sequence)
            committed += text
            remaining = remaining[consumed:]
            checks.append(checkpoint(committed, remaining))
        if ending == 'shift':
            keys((*key('Shift_L'), *key('Shift_L')))
            checks.append(checkpoint(committed, remaining))
        if ending == 'edit':
            keys((*key('Home'), *key('Delete')))
            checks.append(checkpoint(committed, 'ao'))
            keys(key('Return'))
        elif ending == 'cancel':
            keys(key('Escape'))
        elif ending == 'restore':
            keys(key('BackSpace')*len(remaining))
            checks.append(checkpoint(committed, ''))
            keys(key('BackSpace'))
            checks.append(checkpoint('', 'ni'))
            keys(key('Return'))
        elif remaining:
            keys(key('space'))
        actual = finish()
        result = dict(toolkit=toolkit, name=name, expected=expected, actual=actual,
                      passed=actual == expected and all(c['passed'] for c in checks),
                      navigation=navigation, checkpoints=checks)
        record(result)
        assert result['passed'], result


def execute_learning(toolkit, context, keys, clear, checkpoint, finish, record):
    """Prove an abbreviation chain is learned and loaded without a restart."""
    state = Path(os.environ.get('XDG_STATE_HOME') or Path.home()/'.local/state')/'tipe'

    def count(pinyin):
        model = state/'model.current'
        if not model.exists():
            return 0
        for line in model.read_text().splitlines():
            fields = line.split('\t')
            if len(fields) == 4 and fields[0] == pinyin and fields[1] == '我操' and fields[3] == context:
                return int(fields[2])
        return 0

    assert count('woc') == 0, 'Learning fixture must have a fresh application identity'
    checks = []
    for repetition in range(3):
        clear()
        keys(('woc',))
        choose, _ = selection('woc', '我', 2)
        keys(choose)
        checks.append(checkpoint('我', 'c'))
        choose, _ = selection('c', '操', 1)
        keys(choose)
        checks.append(checkpoint('我操', ''))
        keys(key('space'))
        checks.append(checkpoint('我操 ', ''))
        time.sleep(.5)
    for _ in range(100):
        if count('woc') >= 3 and count("wo'cao") >= 3:
            break
        time.sleep(.1)
    observed = dict(abbreviation=count('woc'), canonical=count("wo'cao"), context=context)
    assert observed['abbreviation'] == 3 and observed['canonical'] == 3, observed
    clear()
    keys(('woc', *key('space')))
    actual = finish()
    result = dict(toolkit=toolkit, name='Retained abbreviation chain publishes and loads automatically',
                  expected='我操', actual=actual, published=observed,
                  passed=actual == '我操' and all(c['passed'] for c in checks), checkpoints=checks)
    record(result)
    assert result['passed'], result
