"""The generated first_invalid_field: which fields it checks, and which it leaves alone."""

from dsl.generator_cpp import CppGenerator
from dsl.parser import Parser
from dsl.validator import Validator


def generate(text: str) -> str:
    ast = Parser(text).parse()
    Validator(ast).validate()
    return CppGenerator(namespace="test_ns").emit(ast)


def function_body(code: str, view_name: str) -> str:
    """The body of first_invalid_field for one view type, up to its closing brace."""
    start = code.index(f"first_invalid_field(const {view_name}& message) {{")
    return code[start:code.index("\n}\n", start)]


MESSAGES = """
    enum Side : i8 {
        buy = 49
        sell = 50
    }

    message Party (id=1)
        optional string party_id
        optional Side role
    end

    message Order (id=2)
        string cl_ord_id
        optional string text
        Side side
        optional Side other_side
        i64 quantity
        list<Party> parties
    end
"""


def test_a_required_string_must_not_be_empty():
    body = function_body(generate(MESSAGES), "OrderView")
    assert 'if (message.cl_ord_id.empty()) return std::string_view("cl_ord_id");' in body


def test_an_optional_string_is_not_required_to_have_a_value():
    body = function_body(generate(MESSAGES), "OrderView")
    assert "message.text" not in body


def test_a_required_enumerated_field_must_hold_a_defined_value():
    body = function_body(generate(MESSAGES), "OrderView")
    assert 'if (!validate(message.side)) return std::string_view("side");' in body


def test_an_optional_enumerated_field_is_checked_only_when_present():
    body = function_body(generate(MESSAGES), "OrderView")
    assert 'if (message.has_other_side && !validate(message.other_side)) return std::string_view("other_side");' in body


def test_a_number_is_not_checked():
    body = function_body(generate(MESSAGES), "OrderView")
    assert "quantity" not in body


def test_the_elements_of_a_list_of_messages_are_checked():
    code = generate(MESSAGES)
    body = function_body(code, "OrderView")
    assert "for (const auto& element : message.parties)" in body
    assert "first_invalid_field(element)" in body
    party = function_body(code, "PartyView")
    assert 'if (message.has_role && !validate(message.role)) return std::string_view("role");' in party


def test_a_message_with_nothing_to_check_marks_its_parameter_unused():
    code = generate("""
        message Empty (id=3)
            i32 count
        end
    """)
    assert "first_invalid_field([[maybe_unused]] const EmptyView& message)" in code
