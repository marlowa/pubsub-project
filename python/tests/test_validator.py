import pytest

from dsl.parser import Parser
from dsl.validator import Validator, ValidationError


def validate(text: str):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return ast


# -----------------------------
# Valid cases
# -----------------------------

def test_valid_enum_and_message():
    text = """
        enum Venue : i16 {
            lse = 1
            chix = 2
        }

        message Trade (id=1)
            i64 price
            Venue venue
        end
    """
    ast = validate(text)
    assert len(ast.declarations) == 2


def test_valid_nested_messages_and_lists():
    text = """
        enum status_code : i32 {
            ok = 0
            warning = 1
        }

        message CpuSample (id=10)
            i64 timestamp_ns
            i32 core
        end

        message Telemetry (id=11)
            list<CpuSample> samples
            optional status_code status
        end
    """
    ast = validate(text)
    assert len(ast.declarations) == 3


# -----------------------------
# Duplicate names
# -----------------------------

def test_duplicate_declaration_name():
    text = """
        enum Foo : i16 { a = 1 }
        message Foo (id=1)
            i32 x
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Duplicate enum values
# -----------------------------

def test_duplicate_enum_values():
    text = """
        enum E : i16 {
            a = 1
            b = 1
        }
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Duplicate message IDs
# -----------------------------

def test_duplicate_message_ids():
    text = """
        message A (id=1)
            i32 x
        end

        message B (id=1)
            i32 y
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Unknown type references
# -----------------------------

def test_unknown_type_reference():
    text = """
        message Foo (id=1)
            Bar x
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Invalid array element type
# -----------------------------

def test_array_of_non_primitive_disallowed():
    text = """
        message Foo (id=1)
            Foo[4] bad
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Invalid array length
# -----------------------------

def test_array_length_must_be_positive():
    text = """
        message Foo (id=1)
            i8[0] bad
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Enum underlying type must be signed integer
# -----------------------------

def test_enum_underlying_type_must_be_signed_int():
    text = """
        enum E : bool {
            x = 1
        }
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Cycle detection
# -----------------------------

def test_cycle_in_message_references():
    text = """
        message A (id=1)
            B b
        end

        message B (id=2)
            A a
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


def test_indirect_cycle():
    text = """
        message A (id=1)
            B b
        end

        message B (id=2)
            C c
        end

        message C (id=3)
            A a
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


# -----------------------------
# Optional fields
# -----------------------------

def test_optional_field_valid():
    text = """
        message Foo (id=1)
            optional string comment
            optional i64 seq
        end
    """
    ast = validate(text)
    msg = ast.declarations[0]
    assert msg.fields[0].optional is True
    assert msg.fields[1].optional is True


# -----------------------------
# List element type must exist
# -----------------------------

def test_list_of_unknown_type():
    text = """
        message Foo (id=1)
            list<Bar> xs
        end
    """
    with pytest.raises(ValidationError):
        validate(text)
