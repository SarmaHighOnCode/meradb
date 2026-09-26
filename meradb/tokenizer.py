"""
STAGE 1 of a query's life: the TOKENIZER (also called lexer / scanner).

It turns raw text into a flat list of "tokens" (words with a label):

    "DIKHAO naam SE students JAHAN umar > 18;"
        |
        v
    [KEYWORD DIKHAO] [IDENT naam] [KEYWORD SE] [IDENT students]
    [KEYWORD JAHAN] [IDENT umar] [SYMBOL >] [NUMBER 18] [SYMBOL ;] [EOF]

The tokenizer does NOT care whether the words make sense together. That is the
parser's job. It only answers: "what kind of word is this?"

Learn more: Crafting Interpreters, chapter 4 "Scanning"
https://craftinginterpreters.com/scanning.html
"""

from dataclasses import dataclass
from enum import Enum, auto

from .errors import TokenizerError


class TokenType(Enum):
    KEYWORD = auto()  # reserved words of the Hinglish language: DIKHAO, JAHAN ...
    IDENT = auto()  # names chosen by the user: students, naam, umar ...
    NUMBER = auto()  # 42, 3.14
    STRING = auto()  # 'Ravi'
    SYMBOL = auto()  # ( ) , ; * = < > <= >= != + - / %
    EOF = auto()  # end of input -- makes the parser simpler


# Every reserved word in the MeraDB language. See docs/LANGUAGE.md for the
# SQL equivalent of each one. Keywords are case-insensitive.
KEYWORDS = {
    # --- DDL (Data Definition Language): change the *structure* ---
    "BANAO",  # CREATE
    "HATAO",  # DROP
    "SUDHARO",  # ALTER
    "JODO",  # ADD (inside ALTER)
    "SAAF",  # TRUNCATE
    "TABLE",
    "TABLES",
    "DATABASE",
    "COLUMN",
    "ISTEMAL",  # USE <database>
    "BATAO",  # DESCRIBE <table>
    "SIKODO",  # VACUUM / compact a table file
    # constraints
    "MUKHYA",  # PRIMARY   (MUKHYA KUNJI = PRIMARY KEY)
    "KUNJI",  # KEY
    "ZAROORI",  # NOT NULL
    "ANOKHA",  # UNIQUE
    "WARNA",  # DEFAULT   (umar INT WARNA 18 = "otherwise 18")
    "SANDARBH",  # REFERENCES (cid INT SANDARBH courses(id) = FOREIGN KEY)
    "SHART",  # CHECK      (umar INT SHART (umar >= 0) = CHECK (umar >= 0))
    "NAYA_NAAM",  # RENAME TO / RENAME COLUMN ... TO (naya_naam = "new name")
    # --- DML (Data Manipulation Language): change / read the *data* ---
    "DAALO",  # INSERT
    "MEIN",  # INTO
    "MAAN",  # VALUES
    "DIKHAO",  # SELECT
    "SE",  # FROM
    "JAHAN",  # WHERE
    "BADLO",  # UPDATE
    "RAKHO",  # SET
    "MITAO",  # DELETE
    "KRAM",  # ORDER BY
    "SEEDHA",  # ASC
    "ULTA",  # DESC
    "SIRF",  # LIMIT
    "ALAG",  # DISTINCT
    "SAMOOH",  # GROUP BY
    "JINKA",  # HAVING    (SAMOOH shehar JINKA GINO(*) > 1)
    "MILAO",  # JOIN
    "BAAYAN",  # LEFT      (BAAYAN MILAO = LEFT JOIN)
    "PAR",  # ON        (MILAO courses c PAR s.cid = c.id)
    "KAHO",  # AS        (output column alias: GINO(*) KAHO total)
    "SAMJHAO",  # EXPLAIN   (show the query plan)
    # --- transactions ---
    "SHURU",  # BEGIN
    "PAKKA",  # COMMIT
    "WAPAS",  # ROLLBACK
    # --- logic & literals ---
    "JAISA",  # LIKE      (naam JAISA 'R%')
    "BEECH",  # BETWEEN   (umar BEECH 18 AUR 25)
    "AUR",  # AND
    "YA",  # OR
    "NAHI",  # NOT
    "HAI",  # IS   (umar HAI KHALI = umar IS NULL)
    "KHALI",  # NULL
    "SACH",  # TRUE
    "JHOOTH",  # FALSE
    # --- Phase A gap features (subqueries, views, set ops, joins, CASE, upsert) ---
    "VIEW",  # BANAO VIEW / HATAO VIEW / DIKHAO VIEWS
    "VIEWS",
    "SANYUKT",  # UNION
    "SAAJHA",  # INTERSECT
    "CHHODKAR",  # EXCEPT / MINUS
    "SAMAAN",  # NATURAL      (SAMAAN MILAO = NATURAL JOIN)
    "DAHINA",  # RIGHT        (DAHINA MILAO = RIGHT JOIN)
    "DONO",  # FULL          (DONO MILAO = FULL OUTER JOIN)
    # NOTE: PEHLA/COALESCE are NOT reserved keywords -- they're ordinary
    # identifiers special-cased in parser._parse_primary (like a function
    # name), so `PEHLA(a, b)` parses via the normal IDENT "(" path.
    "AGAR",  # CASE ... WHEN   (IF)
    "TAB",  # ... THEN
    "KHATAM",  # ... END (also used by Phase B for trigger/procedure blocks)
    "TAKRAAV",  # CONFLICT     (TAKRAAV PAR BADLO = ON CONFLICT DO UPDATE)
}

# Two-character symbols must be checked BEFORE one-character ones,
# otherwise "<=" would be read as "<" followed by "=".
TWO_CHAR_SYMBOLS = {"<=", ">=", "!=", "<>"}
ONE_CHAR_SYMBOLS = set("(),;*=<>+-/%.")  # '.' as in s.naam (numbers like 3.14 are read first)


@dataclass
class Token:
    type: TokenType
    value: object  # str for most tokens, int/float for NUMBER
    line: int
    col: int
    # Character offsets into the original source text (defaults keep every
    # existing `Token(type, value, line, col)` call working unchanged).
    # SHART (CHECK) needs these to slice out an expression's source text,
    # because an AST can't be saved to catalog.json.
    start: int = -1
    end: int = -1

    def __repr__(self) -> str:
        return f"{self.type.name}({self.value!r})"


class Tokenizer:
    def __init__(self, text: str):
        self.text = text
        self.pos = 0  # index of the character we are looking at
        self.line = 1
        self.col = 1

    # --- small helpers -------------------------------------------------
    def _peek(self, offset: int = 0) -> str:
        i = self.pos + offset
        return self.text[i] if i < len(self.text) else ""

    def _advance(self) -> str:
        ch = self.text[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _error(self, msg: str) -> TokenizerError:
        return TokenizerError(f"{msg} (line {self.line}, col {self.col})")

    # --- main loop ------------------------------------------------------
    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        while self.pos < len(self.text):
            ch = self._peek()

            # "﻿" is the invisible byte-order mark some Windows tools put at
            # the start of text (Notepad, PowerShell pipes) -- treat it as space
            if ch.isspace() or ch == "﻿":
                self._advance()
                continue

            # comments: "-- anything till end of line"
            if ch == "-" and self._peek(1) == "-":
                while self.pos < len(self.text) and self._peek() != "\n":
                    self._advance()
                continue

            start, line, col = self.pos, self.line, self.col

            if ch.isalpha() or ch == "_":
                tok = self._read_word(line, col)
            elif ch.isdigit():
                tok = self._read_number(line, col)
            elif ch == "'":
                tok = self._read_string(line, col)
            elif ch + self._peek(1) in TWO_CHAR_SYMBOLS:
                sym = self._advance() + self._advance()
                tok = Token(TokenType.SYMBOL, "!=" if sym == "<>" else sym, line, col)
            elif ch in ONE_CHAR_SYMBOLS:
                tok = Token(TokenType.SYMBOL, self._advance(), line, col)
            else:
                raise self._error(f"Ye character samajh nahi aaya: {ch!r}")

            tok.start, tok.end = start, self.pos
            tokens.append(tok)

        tokens.append(Token(TokenType.EOF, None, self.line, self.col, start=self.pos, end=self.pos))
        return tokens

    def _read_word(self, line: int, col: int) -> Token:
        start = self.pos
        while self._peek().isalnum() or self._peek() == "_":
            self._advance()
        word = self.text[start : self.pos]
        if word.upper() in KEYWORDS:
            return Token(TokenType.KEYWORD, word.upper(), line, col)
        # identifiers are stored lower-case so `Students` and `students` are the same table
        return Token(TokenType.IDENT, word.lower(), line, col)

    def _read_number(self, line: int, col: int) -> Token:
        start = self.pos
        while self._peek().isdigit():
            self._advance()
        is_float = False
        if self._peek() == "." and self._peek(1).isdigit():
            is_float = True
            self._advance()
            while self._peek().isdigit():
                self._advance()
        text = self.text[start : self.pos]
        return Token(TokenType.NUMBER, float(text) if is_float else int(text), line, col)

    def _read_string(self, line: int, col: int) -> Token:
        self._advance()  # opening quote
        chars = []
        while True:
            if self.pos >= len(self.text):
                raise self._error("String band nahi hui -- closing ' missing hai")
            ch = self._advance()
            if ch == "'":
                # SQL-style escape: two quotes '' inside a string mean one quote
                if self._peek() == "'":
                    chars.append(self._advance())
                    continue
                break
            chars.append(ch)
        return Token(TokenType.STRING, "".join(chars), line, col)


def tokenize(text: str) -> list[Token]:
    return Tokenizer(text).tokenize()
