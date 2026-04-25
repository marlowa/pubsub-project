"""Tests for char underlying type in enum declarations."""

import pytest
from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_cpp import CppGenerator
from dsl.errors import LexError, ParseError


def parse(text):
    return Parser(text).parse()


def validate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return ast


def generate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    gen = CppGenerator(namespace="test_ns")
    return gen.emit(ast)


def _raises_validation(text, *fragments):
    with pytest.raises(ValidationError) as exc_info:
        validate(text)
    message = str(exc_info.value)
    for fragment in fragments:
        assert fragment in message, (
            f"Expected '{fragment}' in error: {message}"
        )


# =============================================================================
# Lexer: CHAR_LIT tokens
# =============================================================================

def test_lexer_char_lit_produces_ascii_value():
    """'1' in an enum entry is lexed as ASCII 49."""
    from dsl.lexer import Lexer
    lexer = Lexer("'1'")
    tok = lexer.next_token()
    assert tok.kind == "CHAR_LIT"
    assert tok.value == "49"


def test_lexer_char_lit_letter():
    """'B' is lexed as ASCII 66."""
    from dsl.lexer import Lexer
    lexer = Lexer("'B'")
    tok = lexer.next_token()
    assert tok.kind == "CHAR_LIT"
    assert tok.value == "66"


def test_lexer_char_lit_rejects_unterminated():
    """An unterminated character literal raises LexError."""
    from dsl.lexer import Lexer
    with pytest.raises(LexError, match="Unterminated"):
        Lexer("'").next_token()


def test_lexer_char_lit_rejects_missing_closing_quote():
    """A character literal missing its closing quote raises LexError."""
    from dsl.lexer import Lexer
    with pytest.raises(LexError, match="closing quote"):
        Lexer("'12'").next_token()


# =============================================================================
# Parser: char underlying type and CHAR_LIT values
# =============================================================================

def test_parser_accepts_char_underlying_type():
    """enum E : char { ... } is accepted by the parser."""
    ast = parse("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    enum = ast.declarations[0]
    assert enum.underlying_type == "char"


def test_parser_char_entry_values_are_ascii():
    """'1' and '2' are stored as their ASCII values 49 and 50."""
    ast = parse("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    enum = ast.declarations[0]
    assert enum.entries[0].name == "Buy"
    assert enum.entries[0].value == ord('1')
    assert enum.entries[1].name == "Sell"
    assert enum.entries[1].value == ord('2')


def test_parser_mixed_char_and_int_values_in_same_enum():
    """An enum with char underlying type can mix char literals and integers."""
    ast = parse("""
        enum OrdType : char {
            Market = '1'
            Limit  = '2'
            Stop   = '3'
        }
    """)
    enum = ast.declarations[0]
    assert len(enum.entries) == 3


def test_parser_rejects_invalid_underlying_type():
    """An unrecognised underlying type raises ParseError."""
    with pytest.raises(ParseError):
        parse("""
            enum E : float {
                x = '1'
            }
        """)


def test_parser_rejects_comma_separated_enum_entries():
    """Comma-separated enum entries give a clear diagnostic message."""
    with pytest.raises(ParseError, match="do not use commas as separators"):
        parse("""
            enum Side : char {
                Buy  = '1',
                Sell = '2',
            }
        """)


# =============================================================================
# Validator: char underlying type
# =============================================================================

def test_validator_accepts_char_underlying_type():
    """Validator accepts char as a valid enum underlying type."""
    ast = validate("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    assert ast.declarations[0].underlying_type == "char"


def test_validator_rejects_duplicate_char_values():
    """Duplicate ASCII values in a char enum are rejected."""
    _raises_validation("""
        enum E : char {
            A = '1'
            B = '1'
        }
    """, "duplicate value")


def test_validator_accepts_char_enum_as_field_type():
    """A char enum can be used as a message field type."""
    validate("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
        message Order (id=1)
            Side side
            i64  quantity
        end
    """)


# =============================================================================
# Generator: char enum output
# =============================================================================

def test_generator_char_enum_uses_char():
    """A char enum is generated with char as the underlying C++ type."""
    code = generate("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    assert "enum Side : char" in code


def test_generator_char_enum_uses_ascii_values():
    """The generated enum entries use the ASCII integer values."""
    code = generate("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    assert f"Buy = {ord('1')}" in code
    assert f"Sell = {ord('2')}" in code


def test_generator_char_enum_has_to_string():
    """The generated to_string function returns the entry name as a string."""
    code = generate("""
        enum Side : char {
            Buy  = '1'
            Sell = '2'
        }
    """)
    assert 'return std::string_view("Buy")' in code
    assert 'return std::string_view("Sell")' in code


def test_generator_full_fix_side_enum():
    """A realistic FIX Side enum generates correctly."""
    code = generate("""
        enum Side : char {
            Buy             = '1'
            Sell            = '2'
            BuyMinus        = '3'
            SellPlus        = '4'
            SellShort       = '5'
            SellShortExempt = '6'
            Undisclosed     = '7'
            Cross           = '8'
            CrossShort      = '9'
        }
    """)
    assert "enum Side : char" in code
    assert f"Buy = {ord('1')}" in code
    assert f"Cross = {ord('8')}" in code
    assert f"CrossShort = {ord('9')}" in code
