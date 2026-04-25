"""Tests for --topics mode: Topics enum validation and generation."""

import pytest
from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_cpp import CppGenerator


def validate(text, topics=False):
    ast = Parser(text).parse()
    Validator(ast).validate(topics=topics)
    return ast


def generate(text, topics=False):
    ast = Parser(text).parse()
    Validator(ast).validate(topics=topics)
    gen = CppGenerator(namespace="test_ns")
    return gen.emit(ast, topics=topics)


def _raises_with(text, *expected_fragments, topics=True):
    with pytest.raises(ValidationError) as exc_info:
        validate(text, topics=topics)
    message = str(exc_info.value)
    for fragment in expected_fragments:
        assert fragment in message, (
            f"Expected '{fragment}' in error message but got: {message}"
        )


VALID_TOPICS_DSL = """
enum Topics : i16 {
    NewOrder    = 10
    CancelOrder = 11
}

message NewOrder (id=Topics.NewOrder)
    i64 price
    i32 quantity
end

message CancelOrder (id=Topics.CancelOrder)
    i64 order_id
end
"""


# =============================================================================
# Validator: topics mode acceptance
# =============================================================================

def test_valid_topics_file_passes():
    """A well-formed topics file with Topics enum and all ids referencing it passes."""
    ast = validate(VALID_TOPICS_DSL, topics=True)
    assert len(ast.declarations) == 3  # Topics enum + 2 messages


def test_topics_mode_off_bare_integers_still_work():
    """Without --topics, bare integers in metadata are still valid."""
    validate("""
        message Foo (id=10)
            i32 x
        end
    """, topics=False)


def test_topics_mode_off_does_not_require_topics_enum():
    """Without --topics, no Topics enum is required."""
    validate("""
        message Foo (id=10)
            i32 x
        end
    """, topics=False)


# =============================================================================
# Validator: topics mode rejection
# =============================================================================

def test_topics_mode_requires_topics_enum():
    """A file with no Topics enum is rejected in topics mode."""
    _raises_with("""
        message Foo (id=10)
            i32 x
        end
    """, "Topics", "requires")


def test_topics_mode_rejects_bare_integer_id():
    """A message with a bare integer id is rejected in topics mode."""
    _raises_with("""
        enum Topics : i16 {
            Foo = 10
        }
        message Foo (id=10)
            i32 x
        end
    """, "Foo", "Topics", "bare integers are not allowed")


def test_topics_mode_rejects_enum_ref_to_wrong_enum():
    """A message id referencing an enum other than Topics is rejected."""
    _raises_with("""
        enum Topics : i16 {
            Foo = 10
        }
        enum OtherId : i16 {
            Foo = 10
        }
        message Foo (id=OtherId.Foo)
            i32 x
        end
    """, "Foo", "Topics", "bare integers are not allowed")


def test_topics_mode_rejects_mixed_ids():
    """If one message uses a bare integer and another uses Topics ref, file is rejected."""
    _raises_with("""
        enum Topics : i16 {
            Foo = 10
        }
        message Foo (id=Topics.Foo)
            i32 x
        end
        message Bar (id=20)
            i32 y
        end
    """, "Bar", "Topics", "bare integers are not allowed")


# =============================================================================
# Generator: Topics enum class output
# =============================================================================

def test_generator_emits_topics_class_not_c_enum():
    """In topics mode, Topics is emitted as a class, not a C-style enum."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "class Topics {" in code
    assert "enum TopicsTag" in code
    # Must NOT emit the plain C-style enum or to_string free function
    assert "enum Topics :" not in code
    assert "to_string(Topics" not in code


def test_generator_topics_class_has_tag_enum():
    """The Topics class contains a TopicsTag C-style enum with correct values."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "NewOrder = 10," in code
    assert "CancelOrder = 11," in code


def test_generator_topics_class_has_constructor():
    """The Topics class has an explicit constructor taking TopicsTag."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "explicit Topics(TopicsTag t)" in code


def test_generator_topics_class_has_as_tag():
    """The Topics class has as_tag() returning TopicsTag."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "as_tag()" in code
    assert "TopicsTag" in code


def test_generator_topics_class_has_as_string():
    """The Topics class has as_string() returning std::string."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "as_string()" in code
    assert '"NewOrder"' in code
    assert '"CancelOrder"' in code


def test_generator_topics_class_has_is_equal():
    """The Topics class has is_equal()."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "is_equal(" in code


def test_generator_topics_class_has_operator_eq():
    """A free operator== is emitted for Topics."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "operator==(const Topics&" in code


def test_generator_topics_class_has_operator_ne():
    """A free operator!= is emitted for Topics."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "operator!=(const Topics&" in code


def test_generator_normal_messages_still_generated():
    """Message encode/decode code is still generated alongside the Topics class."""
    code = generate(VALID_TOPICS_DSL, topics=True)
    assert "struct NewOrder {" in code
    assert "struct CancelOrder {" in code
    assert "encoded_size" in code


def test_generator_without_topics_flag_emits_plain_enum():
    """Without --topics, a Topics enum is emitted as a plain C-style enum."""
    code = generate(VALID_TOPICS_DSL, topics=False)
    assert "enum Topics :" in code
    assert "class Topics {" not in code
