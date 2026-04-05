from __future__ import annotations
from typing import List, Dict

from .lexer import Lexer, Token
from .errors import ParseError
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
            raise ParseError(
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

    def _expect_keyword(self, value: str) -> int:
        """Consume the named keyword and return its line number."""
        if self.current.kind != "KEYWORD" or self.current.value != value:
            raise ParseError(
                f"Expected keyword '{value}', got {self.current.value} "
                f"at {self.current.line}:{self.current.column}"
            )
        line = self.current.line
        self.current = self.lexer.next_token()
        return line

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

        raise ParseError(
            f"Unexpected token '{self.current.value}' at "
            f"{self.current.line}:{self.current.column}"
        )

    # -----------------------------
    # Enum
    # -----------------------------

    def _parse_enum(self) -> EnumDecl:
        enum_line = self._expect_keyword("enum")

        name_tok = self._eat("IDENT")
        self._eat("COLON")

        if self.current.kind != "KEYWORD":
            raise ParseError(
                f"Expected type after ':', got {self.current.value} "
                f"at {self.current.line}:{self.current.column}"
            )
        underlying = self.current.value
        self.current = self.lexer.next_token()

        self._eat("LBRACE")

        entries: List[EnumEntry] = []
        while self.current.kind == "IDENT":
            entry_tok = self._eat("IDENT")
            self._eat("EQUAL")
            value = int(self._eat("INT").value)
            entries.append(EnumEntry(
                name=entry_tok.value,
                value=value,
                line=entry_tok.line,
            ))

        self._eat("RBRACE")

        return EnumDecl(
            name=name_tok.value,
            underlying_type=underlying,
            entries=entries,
            line=enum_line,
        )

    # -----------------------------
    # Message
    # -----------------------------

    def _parse_message(self) -> MessageDecl:
        message_line = self._expect_keyword("message")

        name_tok = self._eat("IDENT")

        self._eat("LPAREN")
        metadata = self._parse_metadata()
        self._eat("RPAREN")

        fields: List[Field] = []

        while not (self.current.kind == "KEYWORD" and self.current.value == "end"):
            if self.current.kind == "EOF":
                raise ParseError(
                    f"Unexpected EOF while parsing fields for message '{name_tok.value}'"
                )
            fields.append(self._parse_field())

        self._expect_keyword("end")

        return MessageDecl(
            name=name_tok.value,
            metadata=metadata,
            fields=fields,
            line=message_line,
        )

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

        field_type = self._parse_type()
        name_tok = self._eat("IDENT")

        return Field(
            name=name_tok.value,
            type=field_type,
            optional=optional,
            line=name_tok.line,
        )

    # -----------------------------
    # Type
    # -----------------------------

    def _parse_type(self):
        tok = self.current

        if tok.kind == "KEYWORD" and tok.value in {
            "i8", "i16", "i32", "i64", "bool", "datetime_ns"
        }:
            self.current = self.lexer.next_token()
            base = PrimitiveType(tok.value)

        elif tok.kind == "KEYWORD" and tok.value == "string":
            self.current = self.lexer.next_token()
            base = StringType()

        elif tok.kind == "KEYWORD" and tok.value == "list":
            self.current = self.lexer.next_token()
            self._eat("LT")
            element_type = self._parse_type()
            self._eat("GT")
            base = ListType(element_type)

        elif tok.kind == "IDENT":
            self.current = self.lexer.next_token()
            base = ReferenceType(name=tok.value, line=tok.line)

        else:
            raise ParseError(
                f"Unexpected type token '{tok.value}' at "
                f"{tok.line}:{tok.column}"
            )

        if self.current.kind == "LBRACKET":
            bracket_line = self.current.line
            self.current = self.lexer.next_token()
            length = int(self._eat("INT").value)
            self._eat("RBRACKET")
            base = ArrayType(element_type=base, length=length, line=bracket_line)

        return base
