"""Arrow contracts for an isolated, untrained LibIME input context."""

def key(name):
    return ('-k', name)

SHIFT_TAB = ('-M', 'shift', '-k', 'Tab', '-m', 'shift')
CASES = [
    ('Compact Right selects second candidate', ('s', *key('Right'), *key('space')), '上'),
    ('Left returns to first candidate', ('s', *key('Right'), *key('Left'), *key('space')), '是'),
    ('Collapsed selection survives Space', ('s', *key('Right'), *SHIFT_TAB, *key('space')), '上'),
    ('Compact Left edits spelling before Right reopens candidates',
     ('s', *key('Left'), *key('Right'), *key('Right'), *key('space')), '上'),
    ('Arrow navigation preserves literal Enter', ('s', *key('Right'), *key('Return')), 's'),
    ('Spelling cursor still inserts in the middle',
     ('nihao', *key('Home'), *key('Right'), 'x', *key('Return')), 'nxihao'),
    ('Collapsed selection allows Home and spelling edit',
     ('nihao', *key('Right'), *SHIFT_TAB, *key('Home'), *key('Right'), 'x', *key('Return')), 'nxihao'),
    ('Collapsed Left navigates candidates before spelling',
     ('s', *key('Right'), *SHIFT_TAB, *key('Left'), *key('space')), '是'),
]
