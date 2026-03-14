import pytest
from dsl.parser import Parser
from dsl.errors import ParseError
from dsl.ast import (
    DslFile,
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


def parse(text):
    return Parser(text).parse()


def test_parse_simple_enum():
    ast = parse("enum Venue : i16 { lse = 1 chix = 2 }")
    assert isinstance(ast, DslFile)
    assert len(ast.declarations) == 1

    enum = ast.declarations[0]
    assert isinstance(enum, EnumDecl)
    assert enum.name == "Venue"
    assert enum.underlying_type == "i16"
    assert [e.name for e in enum.entries] == ["lse", "chix"]
    assert [e.value for e in enum.entries] == [1, 2]


def test_parse_simple_message():
    text = """
        message Trade (id=1, version=1)
            i64 price
            i32 quantity
        end
    """
    ast = parse(text)
    msg = ast.declarations[0]
    assert isinstance(msg, MessageDecl)
    assert msg.name == "Trade"
    assert msg.metadata == {"id": 1, "version": 1}
    assert len(msg.fields) == 2
    assert msg.fields[0].name == "price"
    assert isinstance(msg.fields[0].type, PrimitiveType)


def test_parse_string_and_list():
    text = """
        message Foo (id=10)
            string name
            list<i32> values
        end
    """
    ast = parse(text)
    msg = ast.declarations[0]
    f_name = msg.fields[0]
    f_vals = msg.fields[1]

    assert isinstance(f_name.type, StringType)
    assert isinstance(f_vals.type, ListType)
    assert isinstance(f_vals.type.element_type, PrimitiveType)
    assert f_vals.type.element_type.name == "i32"


def test_parse_array_and_reference():
    text = """
        enum status : i32 { ok = 0 }
        message Bar (id=5)
            i8[16] hash
            status code
        end
    """
    ast = parse(text)
    msg = ast.declarations[1]

    f_hash = msg.fields[0]
    f_code = msg.fields[1]

    assert isinstance(f_hash.type, ArrayType)
    assert f_hash.type.length == 16
    assert isinstance(f_code.type, ReferenceType)
    assert f_code.type.name == "status"


def test_parse_optional_fields():
    text = """
        message Foo (id=1)
            optional string comment
            optional i64 seq
        end
    """
    ast = parse(text)
    msg = ast.declarations[0]

    assert msg.fields[0].optional is True
    assert isinstance(msg.fields[0].type, StringType)

    assert msg.fields[1].optional is True
    assert isinstance(msg.fields[1].type, PrimitiveType)


def test_parse_nested_messages():
    text = """
        message Inner (id=1)
            i32 x
        end

        message Outer (id=2)
            Inner child
        end
    """
    ast = parse(text)
    outer = ast.declarations[1]
    f = outer.fields[0]
    assert isinstance(f.type, ReferenceType)
    assert f.type.name == "Inner"


def test_parse_error_on_bad_syntax():
    with pytest.raises(ParseError):
        parse("message Foo id=1 end")  # missing parentheses
