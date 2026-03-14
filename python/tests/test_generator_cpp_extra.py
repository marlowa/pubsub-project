from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator

def generate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return CppGenerator(namespace="ns").emit(ast)

def test_generator_optional_field_emits_has_flag():
    code = generate("""
        message Foo (id=1)
            optional i32 x
        end
    """)
    assert "bool has_x" in code
    assert "msg.has_x" in code


def test_generator_array_field_emits_std_array():
    code = generate("""
        message Foo (id=1)
            i8[4] hash
        end
    """)
    assert "std::array<int8_t, 4>" in code


def test_generator_reference_field_calls_encode_decode():
    code = generate("""
        message A (id=1)
            i32 x
        end
        message B (id=2)
            A child
        end
    """)
    assert "encode(msg.child" in code
    assert "decode(msg.child" in code
