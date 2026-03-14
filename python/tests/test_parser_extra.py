import pytest
from dsl.parser import Parser
from dsl.errors import ParseError

def test_parser_unexpected_eof_in_message_fields():
    text = "message Foo (id=1)\n    i32 x\n"
    with pytest.raises(ParseError):
        Parser(text).parse()


def test_parser_unexpected_token_in_declaration():
    with pytest.raises(ParseError):
        Parser("nonsense").parse()


def test_parser_array_parsing():
    ast = Parser("""
        message A (id=1)
            i8[16] hash
        end
    """).parse()

    msg = ast.declarations[0]
    field = msg.fields[0]
    assert field.type.length == 16
    assert field.type.element_type.name == "i8"
