import pytest

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator


def generate(text: str, namespace="test_ns"):
    ast = Parser(text).parse()
    Validator(ast).validate()
    gen = CppGenerator(namespace=namespace)
    return gen.emit(ast)


def test_namespace_wrapping():
    code = generate("enum Venue : i16 { lse = 1 }")
    assert "namespace test_ns {" in code
    assert "} // namespace test_ns" in code


def test_enum_generation():
    text = """
        enum Venue : i16 {
            lse = 1
            chix = 2
        }
    """
    code = generate(text)

    # plain enum, not enum class
    assert "enum Venue : int16_t" in code

    # snake_case values
    assert "lse = 1" in code
    assert "chix = 2" in code

    # to_string helper
    assert "constexpr std::string_view to_string(Venue" in code

    # validate helper
    assert "constexpr bool validate(Venue" in code


def test_message_struct_generation():
    text = """
        message Trade (id=1)
            i64 price
            string source
            optional i32 qty
        end
    """
    code = generate(text)

    # struct name
    assert "struct Trade" in code

    # primitive field
    assert "int64_t price;" in code

    # string field
    assert "std::string_view source;" in code

    # optional field pattern
    assert "bool has_qty" in code
    assert "int32_t qty;" in code


def test_list_field_generation():
    text = """
        message Foo (id=1)
            list<i64> values
        end
    """
    code = generate(text)

    # ListView<int64_t>
    assert "ListView<int64_t> values;" in code


def test_reference_field_generation():
    text = """
        message A (id=1)
            i32 x
        end

        message B (id=2)
            A child
        end
    """
    code = generate(text)

    # forward declaration
    assert "struct A;" in code
    assert "struct B;" in code

    # reference type
    assert "A child;" in code


def test_encode_decode_size_signatures():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)

    assert "std::size_t encoded_size(const Foo& msg);" in code
    assert "bool encode(const Foo& msg" in code
    assert "bool decode(Foo& msg" in code


def test_string_encoding_logic_present():
    text = """
        message Foo (id=1)
            string name
        end
    """
    code = generate(text)

    # length prefix
    assert "int32_t len_name" in code

    # memcpy of string bytes
    assert "std::memcpy(ptr, msg.name.data()" in code


def test_list_encoding_logic_present():
    text = """
        message Foo (id=1)
            list<i32> xs
        end
    """
    code = generate(text)

    # count prefix
    assert "int32_t count_xs" in code

    # memcpy for primitive list
    assert "std::memcpy(ptr, msg.xs.data" in code


def test_nested_message_encode_decode():
    text = """
        message A (id=1)
            i32 x
        end

        message B (id=2)
            A child
        end
    """
    code = generate(text)

    # nested encode call
    assert "encode(msg.child" in code

    # nested decode call
    assert "decode(msg.child" in code
