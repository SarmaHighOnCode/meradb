"""
Syntax highlighting for MeraDB queries (used by the terminal UI).

Why not reuse tokenizer.py? The tokenizer is STRICT: it raises an error on
`'unterminated string`, which is exactly what the text looks like while you are
still typing. A highlighter must be LENIENT: colour what it can, skip the rest.
So this is a small regex-based scanner that shares the keyword list with the
real tokenizer, so the two can never disagree about what a keyword is.

Returns (start, end, kind) spans; `kind` names match the style names used by
Textual's editor themes ("keyword", "string", "number", ...).
"""

import re

from .aggregates import ALIASES as AGGREGATE_NAMES
from .datatypes import TYPE_ALIASES
from .tokenizer import KEYWORDS

_TOKEN_RE = re.compile(
    r"""
      (?P<comment>--.*)
    | (?P<string>'(?:[^']|'')*'?)             # the closing quote is optional while typing
    | (?P<number>\b\d+(?:\.\d+)?\b)
    | (?P<word>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<operator><=|>=|!=|<>|[=<>+\-*/%])
    | (?P<bracket>[()])
    """,
    re.VERBOSE,
)


def spans(line: str) -> list[tuple[int, int, str]]:
    """Highlight spans for ONE line of text, as character positions."""
    out = []
    for m in _TOKEN_RE.finditer(line):
        kind = m.lastgroup
        if kind == "word":
            word = m.group().upper()
            if word in ("SACH", "JHOOTH"):
                kind = "boolean"
            elif word == "KHALI":
                kind = "constant.builtin"
            elif word in KEYWORDS:
                kind = "keyword"
            elif word in TYPE_ALIASES:
                kind = "type"
            elif word in AGGREGATE_NAMES and line[m.end() :].lstrip().startswith("("):
                kind = "function"
            else:
                continue  # table / column names stay uncoloured
        elif kind == "bracket":
            kind = "punctuation.bracket"
        out.append((m.start(), m.end(), kind))
    return out


# Rich styles for the same kinds -- used when echoing queries into the log.
RICH_STYLES = {
    "keyword": "bold #ff79c6",
    "string": "#f1fa8c",
    "number": "#bd93f9",
    "comment": "italic #6272a4",
    "operator": "#ff79c6",
    "type": "#8be9fd",
    "boolean": "#bd93f9",
    "constant.builtin": "italic #bd93f9",
    "function": "#50fa7b",
    "punctuation.bracket": "",
}
