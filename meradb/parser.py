"""
STAGE 2: the PARSER. Turns a list of tokens into an AST (see ast_nodes.py).

Technique: *recursive descent*. Each grammar rule becomes one Python method.
The grammar itself is written out in docs/LANGUAGE.md -- read it side by side
with this file.

Expression precedence (lowest -> highest), one method per level:

    _parse_or          a YA b
    _parse_and         a AUR b
    _parse_not         NAHI a
    _parse_comparison  a = b, a < b, a HAI KHALI ...
    _parse_additive    a + b, a - b
    _parse_term        a * b, a / b, a % b
    _parse_unary       -a
    _parse_primary     42, 'Ravi', naam, (a + b)

Because `_parse_and` calls `_parse_not`, which calls `_parse_comparison`, AUR
binds tighter than YA, and comparisons bind tighter than AUR -- exactly like SQL.

Learn more: Crafting Interpreters, chapter 6 "Parsing Expressions"
https://craftinginterpreters.com/parsing-expressions.html
"""

from functools import lru_cache
from typing import Optional

from . import ast_nodes as ast
from .datatypes import normalize_type
from .errors import ParseError
from .tokenizer import Token, TokenType, tokenize


class Parser:
    def __init__(self, tokens: list[Token], text: str = ""):
        self.tokens = tokens
        self.pos = 0
        # kept only so SHART (CHECK) can slice out an expression's SOURCE TEXT
        # -- an AST can't be saved to catalog.json, but text can.
        self.text = text

    # ------------------------------------------------------------------
    # Token helpers. Almost every parser has these four.
    # ------------------------------------------------------------------
    def _peek(self, offset: int = 0) -> Token:
        return self.tokens[min(self.pos + offset, len(self.tokens) - 1)]

    def _advance(self) -> Token:
        tok = self.tokens[self.pos]
        if tok.type != TokenType.EOF:
            self.pos += 1
        return tok

    def _check(self, type_: TokenType, value=None) -> bool:
        tok = self._peek()
        return tok.type == type_ and (value is None or tok.value == value)

    def _match(self, type_: TokenType, value=None) -> bool:
        """If the next token matches, consume it and return True."""
        if self._check(type_, value):
            self._advance()
            return True
        return False

    def _check_kw(self, word: str) -> bool:
        return self._check(TokenType.KEYWORD, word)

    def _match_kw(self, word: str) -> bool:
        return self._match(TokenType.KEYWORD, word)

    def _match_sym(self, sym: str) -> bool:
        return self._match(TokenType.SYMBOL, sym)

    def _expect_kw(self, word: str) -> None:
        if not self._match_kw(word):
            raise self._error(f"'{word}' expected tha")

    def _expect_sym(self, sym: str) -> None:
        if not self._match_sym(sym):
            raise self._error(f"'{sym}' expected tha")

    def _expect_ident(self, what: str = "naam") -> str:
        tok = self._peek()
        if tok.type != TokenType.IDENT:
            raise self._error(f"{what} expected tha")
        self._advance()
        return tok.value

    def _error(self, msg: str) -> ParseError:
        tok = self._peek()
        found = "end of query" if tok.type == TokenType.EOF else repr(tok.value)
        return ParseError(f"{msg}, par {found} mila (line {tok.line}, col {tok.col})")

    # ------------------------------------------------------------------
    # Entry point: a script is statements separated by ';'
    # ------------------------------------------------------------------
    def parse_script(self) -> list[ast.Statement]:
        statements = []
        while not self._check(TokenType.EOF):
            if self._match_sym(";"):  # allow empty statements like ';;'
                continue
            statements.append(self._parse_statement())
            if not self._check(TokenType.EOF):
                self._expect_sym(";")
        return statements

    def _parse_statement(self) -> ast.Statement:
        tok = self._peek()
        if tok.type != TokenType.KEYWORD:
            raise self._error("Query kisi command se shuru honi chahiye (jaise DIKHAO, DAALO, BANAO)")

        dispatch = {
            "BANAO": self._parse_banao,
            "HATAO": self._parse_hatao,
            "SUDHARO": self._parse_sudharo,
            "SAAF": self._parse_saaf,
            "SIKODO": self._parse_sikodo,
            "ISTEMAL": self._parse_istemal,
            "BATAO": self._parse_batao,
            "DAALO": self._parse_insert,
            "DIKHAO": self._parse_dikhao_stmt,
            "BADLO": self._parse_update,
            "MITAO": self._parse_delete,
            "SHURU": ast.Begin,
            "PAKKA": ast.Commit,
            "WAPAS": ast.Rollback,
            # SAMJHAO <statement>: the thing to explain is itself a whole statement
            "SAMJHAO": lambda: ast.Explain(self._parse_statement()),
        }
        handler = dispatch.get(tok.value)
        if handler is None:
            raise self._error("Ye command nahi pata")
        self._advance()  # consume the command keyword
        return handler()

    # ------------------------------------------------------------------
    # DDL
    # ------------------------------------------------------------------
    def _parse_banao(self) -> ast.Statement:
        # BANAO DATABASE name
        if self._match_kw("DATABASE"):
            return ast.CreateDatabase(self._expect_ident("database ka naam"))

        # BANAO VIEW naam KAHO DIKHAO ...
        if self._match_kw("VIEW"):
            return self._parse_create_view()

        # BANAO TABLE name ( coldef | table-constraint, ... )
        self._expect_kw("TABLE")
        name = self._expect_ident("table ka naam")
        self._expect_sym("(")
        stmt = ast.CreateTable(name, [])
        self._parse_table_item(stmt)
        while self._match_sym(","):
            self._parse_table_item(stmt)
        self._expect_sym(")")
        return stmt

    def _parse_table_item(self, stmt: ast.CreateTable) -> None:
        """One comma-separated item inside `BANAO TABLE (...)`: either a normal
        column definition, or a table-level composite constraint -- ANOKHA
        (a, b) or MUKHYA KUNJI (a, b). Distinguished by lookahead: a bare
        ANOKHA/MUKHYA keyword here (not following an IDENT column name) can
        only be the table-level form, since a column def always starts with
        an IDENT (the column's own name)."""
        if self._check_kw("ANOKHA"):
            self._advance()
            stmt.composite_unique.append(self._parse_composite_columns())
            return
        if self._check_kw("MUKHYA") and self._peek(1).type == TokenType.KEYWORD and self._peek(1).value == "KUNJI":
            self._advance()
            self._advance()
            if stmt.composite_pk is not None:
                raise self._error("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai (composite bhi sirf ek)")
            stmt.composite_pk = self._parse_composite_columns()
            return
        stmt.columns.append(self._parse_column_def())

    def _parse_composite_columns(self) -> list[str]:
        self._expect_sym("(")
        cols = [self._expect_ident("column ka naam")]
        while self._match_sym(","):
            cols.append(self._expect_ident("column ka naam"))
        self._expect_sym(")")
        if len(cols) < 2:
            raise self._error("Composite constraint mein kam se kam 2 columns chahiye (1 column ke liye normal ANOKHA/MUKHYA KUNJI use karo)")
        return cols

    def _parse_create_view(self) -> ast.CreateView:
        name = self._expect_ident("view ka naam")
        self._expect_kw("KAHO")
        start = self._peek().start
        if not self._check_kw("DIKHAO"):
            raise self._error("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai")
        # capture the raw source text of the SELECT, the same way SHART does,
        # so the view always re-binds against the CURRENT schema when it is used
        self._advance()  # consume DIKHAO
        select_stmt = self._parse_select_body()
        if self._peek().type == TokenType.KEYWORD and self._peek().value in self.SET_OPS:
            raise self._error("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai")
        end = self.tokens[self.pos - 1].end
        query_text = self.text[start:end]
        if not isinstance(select_stmt, ast.Select):
            raise self._error("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai")
        return ast.CreateView(name, query_text)

    def _parse_column_def(self) -> ast.ColumnDef:
        # coldef := name TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]
        #           [MUKHYA KUNJI | ZAROORI | ANOKHA | WARNA literal | SANDARBH ref | SHART "(" expr ")"]*
        name = self._expect_ident("column ka naam")
        type_tok = self._peek()
        type_name = normalize_type(str(type_tok.value)) if type_tok.type == TokenType.IDENT else None
        if type_name is None:
            raise self._error(
                f"Column '{name}' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)"
            )
        self._advance()

        col = ast.ColumnDef(name, type_name, max_length=self._parse_type_length(type_name, name))
        while True:
            if self._match_kw("MUKHYA"):
                self._expect_kw("KUNJI")
                col.primary_key = True
            elif self._match_kw("ZAROORI"):
                col.not_null = True
            elif self._match_kw("ANOKHA"):
                col.unique = True
            elif self._match_kw("WARNA"):
                value = self._parse_unary()  # handles -5 as well as 5
                if not isinstance(value, ast.Literal):
                    raise self._error("WARNA ke baad ek fixed value (jaise 18 ya 'Delhi') expected thi")
                col.default = value.value
            elif self._match_kw("SANDARBH"):
                col.ref_table = self._expect_ident("parent table ka naam")
                self._expect_sym("(")
                col.ref_column = self._expect_ident("parent column ka naam")
                self._expect_sym(")")
            elif self._match_kw("SHART"):
                col.check = self._parse_check_source()
            else:
                break
        return col

    def _parse_check_source(self) -> str:
        """SHART "(" expr ")" -- returns the expression's SOURCE TEXT (not an
        AST: that can't be saved to catalog.json). We parse it here too, only
        to validate the syntax immediately and to find where it ends."""
        self._expect_sym("(")
        start = self._peek().start
        self._parse_expr()
        end = self._peek().start  # position of the ")" we're about to consume
        self._expect_sym(")")
        return self.text[start:end].rstrip()

    def _parse_type_length(self, type_name: str, column: str) -> Optional[int]:
        """`VARCHAR(20)`, `NUMBER(5,2)`, `INT(11)`: an optional `(n [, n])` after
        the type name. Only TEXT types keep the first number, as `max_length`;
        everywhere else (NUMBER/NUMERIC/DECIMAL precision+scale, INT(n) ...) both
        numbers are accepted for familiarity and then simply ignored."""
        if not self._match_sym("("):
            return None
        first = self._expect_length_number(column)
        if self._match_sym(","):
            self._expect_length_number(column)
        self._expect_sym(")")
        return first if type_name == "TEXT" else None

    def _expect_length_number(self, column: str) -> int:
        tok = self._peek()
        if tok.type != TokenType.NUMBER or not isinstance(tok.value, int):
            raise self._error(f"Column '{column}': type ke baad ek whole number (length) expected tha")
        self._advance()
        return tok.value

    def _parse_hatao(self) -> ast.Statement:
        if self._match_kw("DATABASE"):
            return ast.DropDatabase(self._expect_ident("database ka naam"))
        if self._match_kw("VIEW"):
            return ast.DropView(self._expect_ident("view ka naam"))
        self._expect_kw("TABLE")
        return ast.DropTable(self._expect_ident("table ka naam"))

    def _parse_sudharo(self) -> ast.Statement:
        # SUDHARO TABLE name JODO [COLUMN] coldef
        # SUDHARO TABLE name HATAO [COLUMN] colname
        # SUDHARO TABLE name NAYA_NAAM new_name              -- rename the table
        # SUDHARO TABLE name COLUMN old_name NAYA_NAAM new_name  -- rename a column
        self._expect_kw("TABLE")
        table = self._expect_ident("table ka naam")
        if self._match_kw("JODO"):
            if self._match_kw("ANOKHA"):
                return ast.AlterAddComposite(table, "ANOKHA", self._parse_composite_columns())
            if self._check_kw("MUKHYA") and self._peek(1).type == TokenType.KEYWORD and self._peek(1).value == "KUNJI":
                self._advance()
                self._advance()
                return ast.AlterAddComposite(table, "MUKHYA", self._parse_composite_columns())
            self._match_kw("COLUMN")
            return ast.AlterAddColumn(table, self._parse_column_def())
        if self._match_kw("HATAO"):
            self._match_kw("COLUMN")
            return ast.AlterDropColumn(table, self._expect_ident("column ka naam"))
        if self._match_kw("NAYA_NAAM"):
            return ast.RenameTable(table, self._expect_ident("naya table naam"))
        if self._match_kw("COLUMN"):
            column = self._expect_ident("column ka naam")
            self._expect_kw("NAYA_NAAM")
            return ast.RenameColumn(table, column, self._expect_ident("naya column naam"))
        raise self._error("SUDHARO TABLE ke baad JODO, HATAO, NAYA_NAAM ya COLUMN expected tha")

    def _parse_saaf(self) -> ast.Statement:
        self._expect_kw("TABLE")
        return ast.TruncateTable(self._expect_ident("table ka naam"))

    def _parse_sikodo(self) -> ast.Statement:
        self._expect_kw("TABLE")
        return ast.CompactTable(self._expect_ident("table ka naam"))

    def _parse_istemal(self) -> ast.Statement:
        self._match_kw("DATABASE")  # optional: ISTEMAL DATABASE college
        return ast.UseDatabase(self._expect_ident("database ka naam"))

    def _parse_batao(self) -> ast.Statement:
        self._match_kw("TABLE")  # optional: BATAO TABLE students
        return ast.Describe(self._expect_ident("table ka naam"))

    # ------------------------------------------------------------------
    # DML
    # ------------------------------------------------------------------
    def _parse_insert(self) -> ast.Statement:
        # DAALO MEIN table [(col, col)] MAAN (expr, expr), (expr, expr) ...
        #                                | DIKHAO ...                     -- INSERT ... SELECT
        #      [TAKRAAV PAR BADLO col = expr, ...]                        -- simplified upsert
        self._expect_kw("MEIN")
        table = self._expect_ident("table ka naam")

        columns = None
        if self._match_sym("("):
            columns = [self._expect_ident("column ka naam")]
            while self._match_sym(","):
                columns.append(self._expect_ident("column ka naam"))
            self._expect_sym(")")

        rows, select = None, None
        if self._match_kw("MAAN"):
            rows = [self._parse_value_tuple()]
            while self._match_sym(","):
                rows.append(self._parse_value_tuple())
        elif self._match_kw("DIKHAO"):
            select = self._parse_select_body()
        else:
            raise self._error("DAALO ke baad MAAN ya DIKHAO expected tha")

        on_conflict_update = None
        if self._match_kw("TAKRAAV"):
            self._expect_kw("PAR")
            self._expect_kw("BADLO")
            on_conflict_update = self._parse_assignment_list()

        return ast.Insert(table, columns, rows=rows, select=select, on_conflict_update=on_conflict_update)

    def _parse_value_tuple(self) -> list[ast.Expr]:
        self._expect_sym("(")
        values = [self._parse_expr()]
        while self._match_sym(","):
            values.append(self._parse_expr())
        self._expect_sym(")")
        return values

    SET_OPS = ("SANYUKT", "SAAJHA", "CHHODKAR")  # UNION, INTERSECT, EXCEPT

    def _parse_dikhao_stmt(self) -> ast.Statement:
        """DIKHAO already consumed. Handles DIKHAO TABLES / VIEWS, or a full
        select (optionally chained with SANYUKT/SAAJHA/CHHODKAR into a SetOp,
        left-associatively: `a SANYUKT b SANYUKT c` -> SetOp(SetOp(a, b), c))."""
        if self._match_kw("TABLES"):
            return ast.ShowTables()
        if self._match_kw("VIEWS"):
            return ast.ShowViews()

        result: ast.Statement = self._parse_select_body()
        while self._peek().type == TokenType.KEYWORD and self._peek().value in self.SET_OPS:
            op = self._advance().value
            self._expect_kw("DIKHAO")
            result = ast.SetOp(op, result, self._parse_select_body())
        return result

    def _parse_select_body(self) -> ast.Select:
        # select body := [ALAG] cols SE table [alias] { join } [JAHAN expr]
        #                [SAMOOH expr, ...] [JINKA expr] [KRAM expr [ULTA|SEEDHA], ...] [SIRF n]
        # (assumes the leading DIKHAO keyword was already consumed by the caller)
        distinct = self._match_kw("ALAG")
        columns, aliases = [], []
        expr, alias = self._parse_select_item()
        columns.append(expr)
        aliases.append(alias)
        while self._match_sym(","):
            expr, alias = self._parse_select_item()
            columns.append(expr)
            aliases.append(alias)

        self._expect_kw("SE")
        table = self._expect_ident("table ka naam")
        select = ast.Select(columns, table, alias=self._parse_alias(), distinct=distinct, aliases=aliases)

        while True:
            # exactly one of BAAYAN/DAHINA/DONO/SAMAAN, or a plain MILAO (INNER)
            if self._match_kw("BAAYAN"):
                self._expect_kw("MILAO")
                kind = "LEFT"
            elif self._match_kw("DAHINA"):
                self._expect_kw("MILAO")
                kind = "RIGHT"
            elif self._match_kw("DONO"):
                self._expect_kw("MILAO")
                kind = "FULL"
            elif self._match_kw("SAMAAN"):
                self._expect_kw("MILAO")
                kind = "NATURAL"
            elif self._match_kw("MILAO"):
                kind = "INNER"
            else:
                break
            join_table = self._expect_ident("table ka naam")
            join_alias = self._parse_alias() or join_table
            if kind == "NATURAL":
                # no PAR at all -- the ON condition is synthesised at bind time
                # from the shared column names (see planner._plan_select)
                on = None
            else:
                self._expect_kw("PAR")
                on = self._parse_expr()
            select.joins.append(ast.Join(join_table, join_alias, on, kind))

        if self._match_kw("JAHAN"):
            select.where = self._parse_expr()

        if self._match_kw("SAMOOH"):
            select.group_by.append(self._parse_expr())
            while self._match_sym(","):
                select.group_by.append(self._parse_expr())

        if self._match_kw("JINKA"):
            select.having = self._parse_expr()

        if self._match_kw("KRAM"):
            select.order_by.append(self._parse_order_item())
            while self._match_sym(","):
                select.order_by.append(self._parse_order_item())

        if self._match_kw("SIRF"):
            tok = self._peek()
            if tok.type != TokenType.NUMBER or not isinstance(tok.value, int):
                raise self._error("SIRF ke baad ek whole number expected tha")
            self._advance()
            select.limit = tok.value

        return select

    def _parse_select_item(self) -> tuple[ast.Expr, Optional[str]]:
        # item = "*" | IDENT "." "*" | expr [ "KAHO" IDENT ]
        # `*` can't be parsed as an expression (it would look like multiplication)
        expr = ast.Star() if self._match_sym("*") else self._parse_expr()
        if isinstance(expr, ast.Star):  # bare `*` or `alias.*` -- KAHO makes no sense on either
            if self._check_kw("KAHO"):
                raise self._error("'*' ke baad KAHO (alias) nahi laga sakte")
            return expr, None
        if self._match_kw("KAHO"):
            return expr, self._expect_ident("alias ka naam")
        return expr, None

    def _parse_alias(self):
        """`SE students s` -- a name right after the table name is its short alias."""
        tok = self._peek()
        if tok.type == TokenType.IDENT:
            self._advance()
            return tok.value
        return None

    def _parse_order_item(self) -> ast.OrderItem:
        expr = self._parse_expr()
        if self._match_kw("ULTA"):
            return ast.OrderItem(expr, descending=True)
        self._match_kw("SEEDHA")
        return ast.OrderItem(expr, descending=False)

    def _parse_update(self) -> ast.Statement:
        # BADLO table RAKHO col = expr, col = expr [JAHAN expr]
        table = self._expect_ident("table ka naam")
        self._expect_kw("RAKHO")
        assignments = self._parse_assignment_list()
        where = self._parse_expr() if self._match_kw("JAHAN") else None
        return ast.Update(table, assignments, where)

    def _parse_assignment_list(self) -> list[tuple[str, ast.Expr]]:
        """`col = expr, col = expr, ...` -- shared by BADLO ... RAKHO and
        DAALO ... TAKRAAV PAR BADLO."""
        assignments = [self._parse_assignment()]
        while self._match_sym(","):
            assignments.append(self._parse_assignment())
        return assignments

    def _parse_assignment(self) -> tuple[str, ast.Expr]:
        col = self._expect_ident("column ka naam")
        self._expect_sym("=")
        return col, self._parse_expr()

    def _parse_delete(self) -> ast.Statement:
        # MITAO SE table [JAHAN expr]
        self._expect_kw("SE")
        table = self._expect_ident("table ka naam")
        where = self._parse_expr() if self._match_kw("JAHAN") else None
        return ast.Delete(table, where)

    # ------------------------------------------------------------------
    # Expressions (see precedence table at the top of the file)
    # ------------------------------------------------------------------
    def _parse_expr(self) -> ast.Expr:
        return self._parse_or()

    def _parse_or(self) -> ast.Expr:
        left = self._parse_and()
        while self._match_kw("YA"):
            left = ast.BinaryOp("YA", left, self._parse_and())
        return left

    def _parse_and(self) -> ast.Expr:
        left = self._parse_not()
        while self._match_kw("AUR"):
            left = ast.BinaryOp("AUR", left, self._parse_not())
        return left

    def _parse_not(self) -> ast.Expr:
        if self._match_kw("NAHI"):
            return ast.UnaryOp("NAHI", self._parse_not())
        return self._parse_comparison()

    def _parse_comparison(self) -> ast.Expr:
        left = self._parse_additive()

        # x HAI KHALI  /  x HAI NAHI KHALI
        if self._match_kw("HAI"):
            negated = self._match_kw("NAHI")
            self._expect_kw("KHALI")
            return ast.IsNull(left, negated)

        # x [NAHI] JAISA / BEECH / MEIN ...   -- the NAHI here is *infix*,
        # different from the prefix NAHI handled in _parse_not.
        negated = False
        nxt = self._peek(1)
        if self._check_kw("NAHI") and nxt.type == TokenType.KEYWORD and nxt.value in ("JAISA", "BEECH", "MEIN"):
            self._advance()
            negated = True

        node = self._parse_pattern_range_or_list(left)
        if node is not None:
            return ast.UnaryOp("NAHI", node) if negated else node

        for op in ("=", "!=", "<", "<=", ">", ">="):
            if self._match_sym(op):
                return ast.BinaryOp(op, left, self._parse_additive())
        return left

    def _parse_pattern_range_or_list(self, left: ast.Expr):
        """
        JAISA, BEECH and MEIN. Two of them are *syntactic sugar*: the parser
        rewrites them into nodes the evaluator already understands, so the
        evaluator needs no new code for them.

            x BEECH a AUR b    ->  x >= a AUR x <= b
            x MEIN (a, b, c)   ->  x = a YA x = b YA x = c
        """
        if self._match_kw("JAISA"):
            return ast.BinaryOp("JAISA", left, self._parse_additive())

        if self._match_kw("BEECH"):
            low = self._parse_additive()  # additive stops before AUR, so this is safe
            self._expect_kw("AUR")
            high = self._parse_additive()
            return ast.BinaryOp("AUR", ast.BinaryOp(">=", left, low), ast.BinaryOp("<=", left, high))

        if self._match_kw("MEIN"):
            self._expect_sym("(")
            if self._check_kw("DIKHAO"):
                subquery = self._parse_subquery()
                self._expect_sym(")")
                return ast.InSubquery(left, subquery, negated=False)
            node = ast.BinaryOp("=", left, self._parse_expr())
            while self._match_sym(","):
                node = ast.BinaryOp("YA", node, ast.BinaryOp("=", left, self._parse_expr()))
            self._expect_sym(")")
            return node

        return None

    def _parse_subquery(self) -> ast.Subquery:
        """Assumes the opening '(' was already consumed and the next token is
        DIKHAO. Does NOT consume the closing ')' -- the caller does, since the
        two call sites (bare parens, MEIN list) close it slightly differently."""
        self._expect_kw("DIKHAO")
        select_stmt = self._parse_select_body()
        return ast.Subquery(select_stmt)

    def _parse_additive(self) -> ast.Expr:
        left = self._parse_term()
        while True:
            if self._match_sym("+"):
                left = ast.BinaryOp("+", left, self._parse_term())
            elif self._match_sym("-"):
                left = ast.BinaryOp("-", left, self._parse_term())
            else:
                return left

    def _parse_term(self) -> ast.Expr:
        left = self._parse_unary()
        while True:
            op = next((s for s in ("*", "/", "%") if self._check(TokenType.SYMBOL, s)), None)
            if op is None:
                return left
            self._advance()
            left = ast.BinaryOp(op, left, self._parse_unary())

    def _parse_unary(self) -> ast.Expr:
        if self._match_sym("-"):
            operand = self._parse_unary()
            # fold "-5" into a single literal so INSERT values stay simple
            if isinstance(operand, ast.Literal) and isinstance(operand.value, (int, float)):
                return ast.Literal(-operand.value)
            return ast.UnaryOp("-", operand)
        return self._parse_primary()

    def _parse_primary(self) -> ast.Expr:
        tok = self._peek()
        if tok.type in (TokenType.NUMBER, TokenType.STRING):
            self._advance()
            return ast.Literal(tok.value)
        if self._match_kw("SACH"):
            return ast.Literal(True)
        if self._match_kw("JHOOTH"):
            return ast.Literal(False)
        if self._match_kw("KHALI"):
            return ast.Literal(None)
        if self._match_kw("AGAR"):
            return self._parse_case()
        if tok.type == TokenType.IDENT:
            self._advance()
            # name followed by "(" is a function call: GINO(*), AUSAT(cgpa) --
            # except PEHLA/COALESCE, a scalar construct with any number of args
            if self._match_sym("("):
                if tok.value.upper() in ("PEHLA", "COALESCE"):
                    args = [self._parse_expr()]
                    while self._match_sym(","):
                        args.append(self._parse_expr())
                    self._expect_sym(")")
                    return ast.Coalesce(args)
                arg = ast.Star() if self._match_sym("*") else self._parse_expr()
                self._expect_sym(")")
                return ast.FuncCall(tok.value.upper(), arg)
            # name.column  or  name.*   (qualified by a table name or alias)
            if self._match_sym("."):
                if self._match_sym("*"):
                    return ast.Star(table=tok.value)
                return ast.ColumnRef(self._expect_ident("column ka naam"), table=tok.value)
            return ast.ColumnRef(tok.value)
        if self._match_sym("("):
            if self._check_kw("DIKHAO"):
                subquery = self._parse_subquery()
                self._expect_sym(")")
                return subquery
            expr = self._parse_expr()
            self._expect_sym(")")
            return expr
        raise self._error("Value ya column ka naam expected tha")

    def _parse_case(self) -> ast.CaseWhen:
        """Assumes AGAR was already consumed.
        case := "AGAR" expr "TAB" expr { "AGAR" expr "TAB" expr } ["WARNA" expr] "KHATAM" """
        branches = []
        while True:
            cond = self._parse_expr()
            self._expect_kw("TAB")
            value = self._parse_expr()
            branches.append((cond, value))
            if not self._match_kw("AGAR"):
                break
        else_ = self._parse_expr() if self._match_kw("WARNA") else None
        self._expect_kw("KHATAM")
        return ast.CaseWhen(branches, else_)


def parse(text: str) -> list[ast.Statement]:
    """Text -> list of statements. The one function the rest of the engine calls."""
    return Parser(tokenize(text), text).parse_script()


@lru_cache(maxsize=256)
def parse_expression(text: str) -> ast.Expr:
    """
    Text -> one expression. Used to re-parse a SHART (CHECK) constraint's saved
    source text back into an AST whenever it needs to be evaluated or
    validated. Cached: the same CHECK text is parsed on every INSERT/UPDATE,
    so re-parsing it every single row would be wasteful.
    """
    parser = Parser(tokenize(text), text)
    expr = parser._parse_expr()
    if not parser._check(TokenType.EOF):
        raise ParseError(f"SHART expression adhoori parse hui: {text!r}")
    return expr
