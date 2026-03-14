from __future__ import annotations
from dataclasses import dataclass
from typing import Optional


# -----------------------------------
# Token definition
# -----------------------------------

@dataclass
class Token:
    kind: str
    value: Optional[str]
    line: int
    column: int

    def __repr__(self) -> str:
        return f"Token({self.kind}, {self.value}, {self.line}:{self.column})"


# -----------------------------------
# Lexer
# -----------------------------------

class Lexer:
    KEYWORDS = {
        "enum",
        "message",
        "optional",
        "list",
        "string",
        "i8",
        "i16",
        "i32",
        "i64",
        "bool",
        "datetime_ns",
    }

    SINGLE_CHAR_TOKENS = {
        "(": "LPAREN",
        ")": "RPAREN",
        "{": "LBRACE",
        "}": "RBRACE",
        "<": "LT",
        ">": "GT",
        ",": "COMMA",
        "=": "EQUAL",
        "[": "LBRACKET",
        "]": "RBRACKET",
    }

    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.line = 1
        self.column = 1

    # -----------------------------
    # Helpers
    # -----------------------------

    def _peek(self) -> str:
        if self.pos >= len(self.text):
            return ""
        return self.text[self.pos]

    def _advance(self) -> str:
        ch = self._peek()
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return ch

    def _make_token(self, kind: str, value: Optional[str], line: int, col: int) -> Token:
        return Token(kind, value, line, col)

    # -----------------------------
    # Main entry point
    # -----------------------------

    def next_token(self) -> Token:
        while True:
            ch = self._peek()

            # End of input
            if ch == "":
                return self._make_token("EOF", None, self.line, self.column)

            # Whitespace
            if ch.isspace():
                self._advance()
                continue

            # Comment
            if ch == "#":
                self._skip_comment()
                continue

            # Single-character punctuation
            if ch in self.SINGLE_CHAR_TOKENS:
                line, col = self.line, self.column
                self._advance()
                return self._make_token(self.SINGLE_CHAR_TOKENS[ch], ch, line, col)

            # Identifier or keyword
            if ch.isalpha() or ch == "_":
                return self._lex_identifier()

            # Integer literal (possibly negative)
            if ch.isdigit() or (ch == "-" and self._peek_next_is_digit()):
                return self._lex_int()

            # Unexpected character
            raise SyntaxError(f"Unexpected character '{ch}' at {self.line}:{self.column}")

    # -----------------------------
    # Lexing helpers
    # -----------------------------

    def _peek_next_is_digit(self) -> bool:
        if self.pos + 1 >= len(self.text):
            return False
        return self.text[self.pos + 1].isdigit()

    def _skip_comment(self):
        while self._peek() not in ("", "\n"):
            self._advance()
        # consume newline if present
        if self._peek() == "\n":
            self._advance()

    def _lex_identifier(self) -> Token:
        line, col = self.line, self.column
        start = self.pos

        while True:
            ch = self._peek()
            if not (ch.isalnum() or ch == "_"):
                break
            self._advance()

        value = self.text[start:self.pos]
        kind = "KEYWORD" if value in self.KEYWORDS else "IDENT"
        return self._make_token(kind, value, line, col)

    def _lex_int(self) -> Token:
        line, col = self.line, self.column
        start = self.pos

        if self._peek() == "-":
            self._advance()

        while self._peek().isdigit():
            self._advance()

        value = self.text[start:self.pos]
        return self._make_token("INT", value, line, col)

import pytest
from dsl.lexer import Lexer, Token
from dsl.errors import LexError


def lex_all(text):
    lx = Lexer(text)
    tokens = []
    while True:
        tok = lx.next_token()
        tokens.append(tok)
        if tok.kind == "EOF":
            break
    return tokens


def test_keywords_and_idents():
    tokens = lex_all("enum message optional list string i32 foo_bar")
    kinds = [t.kind for t in tokens]
    values = [t.value for t in tokens]

    assert values[:6] == ["enum", "message", "optional", "list", "string", "i32"]
    assert kinds[:6] == ["KEYWORD"] * 6
    assert values[6] == "foo_bar"
    assert kinds[6] == "IDENT"


def test_integers():
    tokens = lex_all("123 -42 0")
    vals = [t.value for t in tokens if t.kind == "INT"]
    assert vals == ["123", "-42", "0"]


def test_single_char_tokens():
    tokens = lex_all("(){}<>,=:[ ]")
    kinds = [t.kind for t in tokens if t.kind != "EOF"]
    assert kinds == [
        "LPAREN", "RPAREN",
        "LBRACE", "RBRACE",
        "LT", "GT",
        "COMMA",
        "EQUAL",
        "COLON",
        "LBRACKET",
        "RBRACKET",
    ]


def test_comments_are_skipped():
    tokens = lex_all("enum # this is a comment\n message")
    vals = [t.value for t in tokens if t.kind != "EOF"]
    assert vals == ["enum", "message"]


def test_unexpected_character():
    with pytest.raises(LexError):
        lex_all("@")  # not allowed


def test_multiline_positions():
    text = "enum\nmessage\nfoo"
    tokens = lex_all(text)
    # enum at line 1
    assert tokens[0].line == 1
    # message at line 2
    assert tokens[1].line == 2
    # foo at line 3
    assert tokens[2].line == 3


def test_realistic_snippet():
    text = """
        enum Venue : i16 {
            lse = 1
            chix = 2
        }
    """
    tokens = lex_all(text)
    kinds = [t.kind for t in tokens]
    assert "KEYWORD" in kinds
    assert "IDENT" in kinds
    assert "INT" in kinds
    assert "LBRACE" in kinds
    assert "RBRACE" in kinds
