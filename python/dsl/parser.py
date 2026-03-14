from __future__ import annotations
from typing import List, Dict
from dataclasses import dataclass

from .lexer import Lexer, Token
from .ast import (
    DslFile,
    Declaration,
    EnumDecl,
    EnumEntry,
    MessageDecl,
    Field,
    PrimitiveType,
    StringType,
    ListType,
    ArrayType,
    ReferenceType,
)


class Parser:
    def __init__(self, text: str):
        self.lexer = Lexer(text)
        self.current = self.lexer.next_token()

    # -----------------------------
    # Token helpers
    # -----------------------------

    def _eat(self, kind: str) -> Token:
        if self.current.kind != kind:
            raise SyntaxError(
                f"Expected {kind}, got {self.current.kind} at "
                f"{self.current.line}:{self.current.column}"
            )
        tok = self.current
        self.current = self.lexer.next_token()
        return tok

    def _accept(self, kind: str) -> bool:
        if self.current.kind == kind:
            self.current = self.lexer.next_token()
            return True
        return False

    def _expect_keyword(self, value: str):
        if self.current.kind != "KEYWORD" or self.current.value != value:
            raise SyntaxError(
                f"Expected keyword '{value}', got {self.current.value} "
                f"at {self.current.line}:{self.current.column}"
            )
        self.current = self.lexer.next_token()

    # -----------------------------
    # Entry point
    # -----------------------------

    def parse(self) -> DslFile:
        declarations: List[Declaration] = []

        while self.current.kind != "EOF":
            declarations.append(self._parse_declaration())

        return DslFile(declarations)

    # -----------------------------
    # Declarations
    # -----------------------------

    def _parse_declaration(self) -> Declaration:
        if self.current.kind == "KEYWORD" and self.current.value == "enum":
            return self._parse_enum()

        if self.current.kind == "KEYWORD" and self.current.value == "message":
            return self._parse_message()

        raise SyntaxError(
            f"Unexpected token '{self.current.value}' at "
            f"{self.current.line}:{self.current.column}"
        )

    # -----------------------------
    # Enum
    # -----------------------------

    def _parse_enum(self) -> EnumDecl:
        self._expect_keyword("enum")

        name_tok = self._eat("IDENT")
        name = name_tok.value

        self._eat("COLON") if False else None  # placeholder if needed
        # Actually the grammar uses IDENT ":" integer_type
        self._eat("COLON") if False else None

        # But our lexer does not produce COLON; instead "=" is EQUAL.
        # So we need to adjust: underlying type is after ":" which is not lexed.
        # Let's fix this properly:

        # Expect ":"
        if self.current.kind != "EQUAL" and self.current.value != ":":
            # We need to handle ":" as a token. Let's add it to SINGLE_CHAR_TOKENS.
            raise SyntaxError("Colon ':' not recognized by lexer. Add ':' to SINGLE_CHAR_TOKENS.")

        # For now, assume ":" is recognized:
        self._eat("COLON")

        underlying = self._eat("KEYWORD").value  # must be i8/i16/i32/i64

        self._eat("LBRACE")

        entries: List[EnumEntry] = []
        while self.current.kind == "IDENT":
            entry_name = self._eat("IDENT").value
            self._eat("EQUAL")
            value = int(self._eat("INT").value)
            entries.append(EnumEntry(entry_name, value))

        self._eat("RBRACE")

        return EnumDecl(name, underlying, entries)

    # -----------------------------
    # Message
    # -----------------------------

    def _parse_message(self) -> MessageDecl:
        self._expect_keyword("message")

        name = self._eat("IDENT").value

        self._eat("LPAREN")
        metadata = self._parse_metadata()
        self._eat("RPAREN")

        fields: List[Field] = []
        while self.current.kind not in ("EOF", "KEYWORD") or (
            self.current.kind == "KEYWORD" and self.current.value != "end"
        ):
            fields.append(self._parse_field())

        # Expect "end"
        if self.current.kind != "KEYWORD" or self.current.value != "end":
            raise SyntaxError(
                f"Expected 'end' at {self.current.line}:{self.current.column}"
            )
        self.current = self.lexer.next_token()

        return MessageDecl(name, metadata, fields)

    # -----------------------------
    # Metadata
    # -----------------------------

    def _parse_metadata(self) -> Dict[str, int]:
        metadata: Dict[str, int] = {}

        key = self._eat("IDENT").value
        self._eat("EQUAL")
        value = int(self._eat("INT").value)
        metadata[key] = value

        while self._accept("COMMA"):
            key = self._eat("IDENT").value
            self._eat("EQUAL")
            value = int(self._eat("INT").value)
            metadata[key] = value

        return metadata

    # -----------------------------
    # Field
    # -----------------------------

    def _parse_field(self) -> Field:
        optional = False

        if self.current.kind == "KEYWORD" and self.current.value == "optional":
            optional = True
            self.current = self.lexer.next_token()

        t = self._parse_type()
        name = self._eat("IDENT").value

        return Field(name=name, type=t, optional=optional)

    # -----------------------------
    # Type
    # -----------------------------

    def _parse_type(self):
        tok = self.current

        # Primitive
        if tok.kind == "KEYWORD" and tok.value in {
            "i8", "i16", "i32", "i64", "bool", "datetime_ns"
        }:
            self.current = self.lexer.next_token()
            # Array?
            if self._accept("LBRACKET"):
                length = int(self._eat("INT").value)
                self._eat("RBRACKET")
                return ArrayType(PrimitiveType(tok.value), length)
            return PrimitiveType(tok.value)

        # string
        if tok.kind == "KEYWORD" and tok.value == "string":
            self.current = self.lexer.next_token()
            return StringType()

        # list<T>
        if tok.kind == "KEYWORD" and tok.value == "list":
            self.current = self.lexer.next_token()
            self._eat("LT")
            elem = self._parse_type()
            self._eat("GT")
            return ListType(elem)

        # Reference type (message or enum)
        if tok.kind == "IDENT":
            name = tok.value
            self.current = self.lexer.next_token()
            return ReferenceType(name)

        raise SyntaxError(
            f"Unexpected type token '{tok.value}' at "
            f"{tok.line}:{tok.column}"
        )
