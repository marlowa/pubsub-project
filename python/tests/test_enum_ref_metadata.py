"""Tests for EnumRef metadata values (EnumName.EntryName syntax in message metadata)."""

import pytest
from dsl.parser import Parser
from dsl.validator import Validator
from dsl.ast import EnumRef, MessageDecl
from dsl.errors import ParseError, ValidationError


def parse(text):
    return Parser(text).parse()


def validate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return ast


def _raises_with(text, *expected_fragments):
    with pytest.raises(ValidationError) as exc_info:
        validate(text)
    message = str(exc_info.value)
    for fragment in expected_fragments:
        assert fragment in message, (
            f"Expected '{fragment}' in error message but got: {message}"
        )


# =============================================================================
# Parser: EnumRef production
# =============================================================================

def test_parser_bare_integer_still_works():
    """Existing behaviour: bare integer in metadata is unchanged."""
    ast = parse("""
        message Foo (id=42)
        end
    """)
    assert ast.declarations[0].metadata["id"] == 42


def test_parser_enum_ref_in_id():
    """EnumName.EntryName in id metadata produces an EnumRef node."""
    ast = parse("""
        enum MsgId : i16 {
            Foo = 10
        }
        message Foo (id=MsgId.Foo)
        end
    """)
    msg = ast.declarations[1]
    assert isinstance(msg, MessageDecl)
    ref = msg.metadata["id"]
    assert isinstance(ref, EnumRef)
    assert ref.enum_name == "MsgId"
    assert ref.entry_name == "Foo"


def test_parser_enum_ref_in_version():
    """EnumRef works for non-id metadata keys too."""
    ast = parse("""
        enum Ver : i8 {
            V1 = 1
        }
        message Bar (id=1, version=Ver.V1)
        end
    """)
    ref = ast.declarations[1].metadata["version"]
    assert isinstance(ref, EnumRef)
    assert ref.enum_name == "Ver"
    assert ref.entry_name == "V1"


def test_parser_enum_ref_records_line_number():
    """The EnumRef carries a non-zero line number."""
    ast = parse("""
        enum MsgId : i16 {
            Foo = 10
        }
        message Foo (id=MsgId.Foo)
        end
    """)
    ref = ast.declarations[1].metadata["id"]
    assert ref.line > 0


def test_parser_rejects_bare_ident_without_dot():
    """A bare identifier (not followed by dot) in metadata is a parse error."""
    with pytest.raises(ParseError, match="Expected '\\.'"):
        parse("""
            message Foo (id=SomeName)
            end
        """)


def test_parser_rejects_missing_entry_after_dot():
    """EnumName. with no entry name is a parse error."""
    with pytest.raises(ParseError):
        parse("""
            message Foo (id=MsgId.)
            end
        """)


# =============================================================================
# Validator: EnumRef resolution
# =============================================================================

def test_validator_resolves_enum_ref_to_integer():
    """After validation, EnumRef metadata values are resolved to plain integers."""
    ast = validate("""
        enum MsgId : i16 {
            Foo = 10
        }
        message Foo (id=MsgId.Foo)
        end
    """)
    msg = ast.declarations[1]
    assert msg.metadata["id"] == 10


def test_validator_resolves_multiple_enum_refs():
    """Multiple EnumRef values in the same file are all resolved."""
    ast = validate("""
        enum MsgId : i16 {
            Foo = 10
            Bar = 11
        }
        message Foo (id=MsgId.Foo)
        end
        message Bar (id=MsgId.Bar)
        end
    """)
    assert ast.declarations[1].metadata["id"] == 10
    assert ast.declarations[2].metadata["id"] == 11


def test_validator_detects_duplicate_ids_after_resolution():
    """Duplicate ID check still works when one ID comes from an enum ref
    and another is a bare integer that resolves to the same value."""
    _raises_with("""
        enum MsgId : i16 {
            Foo = 10
        }
        message Foo (id=MsgId.Foo)
        end
        message Bar (id=10)
        end
    """, "duplicate message id")


def test_validator_rejects_unknown_enum_name():
    """An EnumRef referring to a non-existent enum raises ValidationError."""
    _raises_with("""
        message Foo (id=NoSuchEnum.Entry)
        end
    """, "NoSuchEnum")


def test_validator_rejects_unknown_entry_name():
    """An EnumRef referring to a non-existent entry raises ValidationError."""
    _raises_with("""
        enum MsgId : i16 {
            Foo = 10
        }
        message Bar (id=MsgId.NoSuchEntry)
        end
    """, "MsgId", "NoSuchEntry")


def test_validator_error_includes_message_name_and_key():
    """The error message identifies the message and metadata key involved."""
    _raises_with("""
        message Foo (id=NoSuchEnum.Entry)
        end
    """, "Foo", "id", "NoSuchEnum")


def test_mixed_integer_and_enum_ref_in_same_file():
    """A file with both bare integers and EnumRef metadata values is valid."""
    ast = validate("""
        enum MsgId : i16 {
            Bar = 20
        }
        message Foo (id=10)
        end
        message Bar (id=MsgId.Bar)
        end
    """)
    assert ast.declarations[1].metadata["id"] == 10
    assert ast.declarations[2].metadata["id"] == 20
