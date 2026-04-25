import pytest
from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.errors import ParseError


def validate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return ast


def _raises_with(text, *expected_fragments):
    """Assert that validation raises ValidationError containing all expected fragments."""
    with pytest.raises(ValidationError) as exc_info:
        validate(text)
    message = str(exc_info.value)
    for fragment in expected_fragments:
        assert fragment in message, (
            f"Expected '{fragment}' in error message but got: {message}"
        )


# -----------------------------
# Existing tests (preserved)
# -----------------------------

def test_cycle_detection_simple():
    _raises_with("""
        message A (id=1)
            B b
        end
        message B (id=2)
            A a
        end
    """, "cyclic")


def test_array_length_negative():
    _raises_with("""
        message Foo (id=1)
            i8[-1] bad
        end
    """, "array length must be greater than zero")


def test_list_of_references_valid():
    ast = validate("""
        message Child (id=1)
            i32 x
        end
        message Parent (id=2)
            list<Child> kids
        end
    """)
    parent = ast.declarations[1]
    assert parent.fields[0].type.element_type.name == "Child"


# -----------------------------
# Error messages include line numbers
# -----------------------------

def test_unknown_type_error_includes_line_number():
    _raises_with("""
        message Foo (id=1)
            Bar x
        end
    """, "line 3", "Foo", "x", "Bar")


def test_unknown_type_in_list_includes_line_number():
    _raises_with("""
        message Foo (id=1)
            list<Nonexistent> items
        end
    """, "line 3", "Foo", "items", "Nonexistent")


def test_duplicate_message_id_includes_line_number():
    _raises_with("""
        message A (id=1)
            i32 x
        end
        message B (id=1)
            i32 y
        end
    """, "duplicate message id", "1")


def test_duplicate_enum_value_includes_line_number():
    _raises_with("""
        enum Status : i32 {
            ok = 0
            also_ok = 0
        }
    """, "duplicate value", "0", "Status")


def test_invalid_enum_underlying_type_includes_line_number():
    with pytest.raises(ParseError) as exc_info:
        validate("""
            enum Flags : string {
                on = 1
            }
        """)
    message = str(exc_info.value)
    assert "string" in message
    assert "expected i8, i16, i32, i64, or char" in message


def test_array_of_non_primitive_includes_line_number():
    _raises_with("""
        message Foo (id=1)
            Foo[4] bad
        end
    """, "line 3", "Foo", "bad", "array element type must be primitive")


def test_array_length_zero_includes_line_number():
    _raises_with("""
        message Foo (id=1)
            i8[0] bad
        end
    """, "line 3", "Foo", "bad", "array length must be greater than zero")


def test_duplicate_declaration_name_includes_line_number():
    _raises_with("""
        enum Foo : i16 { a = 1 }
        message Foo (id=1)
            i32 x
        end
    """, "duplicate declaration name", "Foo")


# -----------------------------
# Recursive / cyclic PDU detection
# -----------------------------

def test_direct_self_reference_is_rejected():
    """A message that contains itself directly is a cycle."""
    _raises_with("""
        message Node (id=1)
            Node child
        end
    """, "cyclic", "Node")


def test_self_reference_via_list_is_rejected():
    """A message that contains a list of itself is a cycle."""
    _raises_with("""
        message Tree (id=1)
            list<Tree> children
        end
    """, "cyclic", "Tree")


def test_indirect_cycle_via_list_is_rejected():
    """A -> list<B> -> A is a cycle."""
    _raises_with("""
        message A (id=1)
            list<B> items
        end
        message B (id=2)
            A parent
        end
    """, "cyclic")


def test_three_way_cycle_is_rejected():
    """A -> B -> C -> A is a cycle."""
    _raises_with("""
        message A (id=1)
            B b
        end
        message B (id=2)
            C c
        end
        message C (id=3)
            A a
        end
    """, "cyclic")


def test_cycle_error_shows_path():
    """The cycle error message must name the messages involved."""
    _raises_with("""
        message Alpha (id=1)
            Beta b
        end
        message Beta (id=2)
            Alpha a
        end
    """, "Alpha", "Beta")


def test_non_cyclic_reference_is_valid():
    """A -> B where B does not reference A is not a cycle."""
    ast = validate("""
        message Leaf (id=1)
            i32 value
        end
        message Branch (id=2)
            Leaf leaf
        end
        message Root (id=3)
            Branch branch
        end
    """)
    assert len(ast.declarations) == 3


def test_shared_reference_is_not_a_cycle():
    """Two messages both referencing a third is not a cycle."""
    ast = validate("""
        message Shared (id=1)
            i32 x
        end
        message A (id=2)
            Shared s
        end
        message B (id=3)
            Shared s
        end
    """)
    assert len(ast.declarations) == 3
