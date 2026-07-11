"""Tests for --pdu-id-enum mode: PduId enum validation and generation."""

import pytest
from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_cpp import CppGenerator


def validate(text, pdu_id_enum=False):
    ast = Parser(text).parse()
    Validator(ast).validate(pdu_id_enum=pdu_id_enum)
    return ast


def generate(text, pdu_id_enum=False):
    ast = Parser(text).parse()
    Validator(ast).validate(pdu_id_enum=pdu_id_enum)
    gen = CppGenerator(namespace="test_ns")
    return gen.emit(ast, pdu_id_enum=pdu_id_enum)


def _raises_with(text, *expected_fragments, pdu_id_enum=True):
    with pytest.raises(ValidationError) as exc_info:
        validate(text, pdu_id_enum=pdu_id_enum)
    message = str(exc_info.value)
    for fragment in expected_fragments:
        assert fragment in message, (
            f"Expected '{fragment}' in error message but got: {message}"
        )


VALID_PDU_ID_DSL = """
enum PduId : i16 {
    NewOrder    = 10
    CancelOrder = 11
}

message NewOrder (id=PduId.NewOrder)
    i64 price
    i32 quantity
end

message CancelOrder (id=PduId.CancelOrder)
    i64 order_id
end
"""


# =============================================================================
# Validator: pdu-id-enum mode acceptance
# =============================================================================

def test_valid_pdu_id_file_passes():
    """A well-formed file with a PduId enum and all ids referencing it passes."""
    ast = validate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert len(ast.declarations) == 3  # PduId enum + 2 messages


def test_pdu_id_mode_off_bare_integers_still_work():
    """Without --pdu-id-enum, bare integers in metadata are still valid."""
    validate("""
        message Foo (id=10)
            i32 x
        end
    """, pdu_id_enum=False)


def test_pdu_id_mode_off_does_not_require_pdu_id_enum():
    """Without --pdu-id-enum, no PduId enum is required."""
    validate("""
        message Foo (id=10)
            i32 x
        end
    """, pdu_id_enum=False)


# =============================================================================
# Validator: pdu-id-enum mode rejection
# =============================================================================

def test_pdu_id_mode_requires_pdu_id_enum():
    """A file with no PduId enum is rejected in pdu-id-enum mode."""
    _raises_with("""
        message Foo (id=10)
            i32 x
        end
    """, "PduId", "requires")


def test_pdu_id_mode_rejects_bare_integer_id():
    """A message with a bare integer id is rejected in pdu-id-enum mode."""
    _raises_with("""
        enum PduId : i16 {
            Foo = 10
        }
        message Foo (id=10)
            i32 x
        end
    """, "Foo", "PduId", "bare integers are not allowed")


def test_pdu_id_mode_rejects_enum_ref_to_wrong_enum():
    """A message id referencing an enum other than PduId is rejected."""
    _raises_with("""
        enum PduId : i16 {
            Foo = 10
        }
        enum OtherId : i16 {
            Foo = 10
        }
        message Foo (id=OtherId.Foo)
            i32 x
        end
    """, "Foo", "PduId", "bare integers are not allowed")


def test_pdu_id_mode_rejects_mixed_ids():
    """If one message uses a bare integer and another a PduId ref, the file is rejected."""
    _raises_with("""
        enum PduId : i16 {
            Foo = 10
        }
        message Foo (id=PduId.Foo)
            i32 x
        end
        message Bar (id=20)
            i32 y
        end
    """, "Bar", "PduId", "bare integers are not allowed")


# =============================================================================
# Generator: PduId enum class output
# =============================================================================

def test_generator_emits_pdu_id_class_not_c_enum():
    """In pdu-id-enum mode, PduId is emitted as a class, not a C-style enum."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "class PduId {" in code
    assert "enum PduIdTag" in code
    # Must NOT emit the plain C-style enum or to_string free function
    assert "enum PduId :" not in code
    assert "to_string(PduId" not in code


def test_generator_pdu_id_class_has_tag_enum():
    """The PduId class contains a PduIdTag C-style enum with correct values."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "NewOrder = 10," in code
    assert "CancelOrder = 11," in code


def test_generator_pdu_id_class_has_constructor():
    """The PduId class has an explicit constructor taking PduIdTag."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "explicit PduId(PduIdTag t)" in code


def test_generator_pdu_id_class_has_as_tag():
    """The PduId class has as_tag() returning PduIdTag."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "as_tag()" in code
    assert "PduIdTag" in code


def test_generator_pdu_id_class_has_as_string():
    """The PduId class has as_string() returning std::string."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "as_string()" in code
    assert '"NewOrder"' in code
    assert '"CancelOrder"' in code


def test_generator_pdu_id_class_has_is_equal():
    """The PduId class has is_equal()."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "is_equal(" in code


def test_generator_pdu_id_class_has_operator_eq():
    """A free operator== is emitted for PduId."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "operator==(const PduId&" in code


def test_generator_pdu_id_class_has_operator_ne():
    """A free operator!= is emitted for PduId."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "operator!=(const PduId&" in code


def test_generator_normal_messages_still_generated():
    """Message encode/decode code is still generated alongside the PduId class."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=True)
    assert "struct NewOrder {" in code
    assert "struct CancelOrder {" in code
    assert "encoded_size" in code


def test_generator_without_pdu_id_flag_emits_plain_enum():
    """Without --pdu-id-enum, a PduId enum is emitted as a plain C-style enum class."""
    code = generate(VALID_PDU_ID_DSL, pdu_id_enum=False)
    assert "enum class PduId :" in code
    assert "class PduId {" not in code
