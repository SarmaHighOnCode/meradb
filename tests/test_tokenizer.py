import unittest

from meradb.errors import TokenizerError
from meradb.tokenizer import TokenType, tokenize


def kinds(text):
    return [(t.type, t.value) for t in tokenize(text)]


class TokenizerTest(unittest.TestCase):
    def test_simple_select(self):
        self.assertEqual(
            kinds("DIKHAO naam SE students;"),
            [
                (TokenType.KEYWORD, "DIKHAO"),
                (TokenType.IDENT, "naam"),
                (TokenType.KEYWORD, "SE"),
                (TokenType.IDENT, "students"),
                (TokenType.SYMBOL, ";"),
                (TokenType.EOF, None),
            ],
        )

    def test_keywords_are_case_insensitive_and_idents_lowercased(self):
        self.assertEqual(kinds("dikhao Naam")[:2], [(TokenType.KEYWORD, "DIKHAO"), (TokenType.IDENT, "naam")])

    def test_numbers(self):
        self.assertEqual(kinds("42 3.14")[:2], [(TokenType.NUMBER, 42), (TokenType.NUMBER, 3.14)])

    def test_string_with_escaped_quote(self):
        self.assertEqual(kinds("'Ravi''s'")[0], (TokenType.STRING, "Ravi's"))

    def test_two_char_symbols(self):
        values = [t.value for t in tokenize("<= >= != <> < >")][:-1]
        self.assertEqual(values, ["<=", ">=", "!=", "!=", "<", ">"])

    def test_comments_are_skipped(self):
        self.assertEqual(kinds("-- hello\nSE")[0], (TokenType.KEYWORD, "SE"))

    def test_byte_order_mark_is_ignored(self):
        self.assertEqual(kinds("﻿DIKHAO")[0], (TokenType.KEYWORD, "DIKHAO"))

    def test_unterminated_string(self):
        with self.assertRaises(TokenizerError):
            tokenize("'oops")

    def test_unknown_character(self):
        with self.assertRaises(TokenizerError):
            tokenize("DIKHAO @")


if __name__ == "__main__":
    unittest.main()
