"""Lexer for the pubsub_itc_fw DSL code generator."""

from __future__ import annotations
from dataclasses import dataclass
from typing import Optional
from .errors import LexError


@dataclass
class Token:
    """A single lexical token with kind, value, and source location."""
    kind: str
    value: Optional[str]
    line: int
    column: int

    def __repr__(self):
        return f"Token({self.kind}, {self.value}, {self.line}:{self.column})"


class Lexer:  # pylint: disable=too-few-public-methods
    """Tokenises DSL source text into a stream of Token objects."""

    KEYWORDS = {
        "enum",
        "message",
        "optional",
        "list",
        "string",
        "char",
        "i8",
        "i16",
        "i32",
        "i64",
        "bool",
        "datetime_ns",
        "end",
    }

    SINGLE = {
        "(": "LPAREN",
        ")": "RPAREN",
        "{": "LBRACE",
        "}": "RBRACE",
        "<": "LT",
        ">": "GT",
        ",": "COMMA",
        "=": "EQUAL",
        ":": "COLON",
        "[": "LBRACKET",
        "]": "RBRACKET",
        ".": "DOT",
    }

    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.line = 1
        self.col = 1

    def _peek(self) -> str:
        return self.text[self.pos] if self.pos < len(self.text) else ""

    def _advance(self) -> str:
        ch = self._peek()
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def next_token(self) -> Token:
        """Return the next token from the input, skipping whitespace and comments."""
        while True:
            ch = self._peek()

            if ch == "":
                return Token("EOF", None, self.line, self.col)

            if ch.isspace():
                self._advance()
                continue

            if ch == "#":
                self._skip_comment()
                continue

            if ch in self.SINGLE:
                tok = Token(self.SINGLE[ch], ch, self.line, self.col)
                self._advance()
                return tok

            if ch == "'":
                return self._lex_char_lit()

            if ch.isalpha() or ch == "_":
                return self._lex_ident()

            if ch.isdigit() or (ch == "-" and self._peek_next_digit()):
                return self._lex_int()

            raise LexError(f"Unexpected character '{ch}' at {self.line}:{self.col}")

    def _peek_next_digit(self) -> bool:
        return (
            self.pos + 1 < len(self.text)
            and self.text[self.pos + 1].isdigit()
        )

    def _skip_comment(self):
        while self._peek() not in ("", "\n"):
            self._advance()
        if self._peek() == "\n":
            self._advance()

    def _lex_char_lit(self) -> Token:
        """Lex a single-quoted character literal e.g. '1', 'B'.

        The token value is the decimal ASCII code of the character as a string,
        so it can be treated identically to an INT token by the parser.
        """
        line, col = self.line, self.col
        self._advance()  # consume opening quote
        ch = self._peek()
        if ch in ("", "\n"):
            raise LexError(f"Unterminated character literal at {line}:{col}")
        self._advance()  # consume the character
        closing = self._peek()
        if closing != "'":
            raise LexError(
                f"Expected closing quote for character literal at {line}:{col}, "
                f"got '{closing}'"
            )
        self._advance()  # consume closing quote
        return Token("CHAR_LIT", str(ord(ch)), line, col)

    def _lex_ident(self) -> Token:
        line, col = self.line, self.col
        start = self.pos
        while True:
            ch = self._peek()
            if not (ch.isalnum() or ch == "_"):
                break
            self._advance()
        value = self.text[start:self.pos]
        kind = "KEYWORD" if value in self.KEYWORDS else "IDENT"
        return Token(kind, value, line, col)

    def _lex_int(self) -> Token:
        line, col = self.line, self.col
        start = self.pos
        if self._peek() == "-":
            self._advance()
        while self._peek().isdigit():
            self._advance()
        return Token("INT", self.text[start:self.pos], line, col)
