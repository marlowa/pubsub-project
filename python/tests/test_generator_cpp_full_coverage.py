from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
import pytest


def generate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return CppGenerator(namespace="ns").emit(ast)


def test_list_of_reference_types():
    code = generate("""
        message Child (id=1)
            i32 x
        end

        message Parent (id=2)
            list<Child> kids
        end
    """)
    assert code  # should not crash


def test_nested_lists_of_primitives():
    code = generate("""
        message Foo (id=1)
            list<list<i32>> matrix
        end
    """)
    assert code


def test_optional_list_of_reference_types():
    code = generate("""
        message Child (id=1)
            i32 x
        end

        message Parent (id=2)
            optional list<Child> maybe_kids
        end
    """)
    assert code


def test_optional_reference_type():
    code = generate("""
        message A (id=1)
            i32 x
        end

        message B (id=2)
            optional A child
        end
    """)
    assert code


def test_array_of_reference_type_disallowed():
    # Validator should reject this before generator runs
    with pytest.raises(Exception):
        generate("""
            message A (id=1)
                i32 x
            end

            message B (id=2)
                A[4] bad
            end
        """)


def test_unknown_type_fallback_in_generator():
    # Construct a fake AST node to force generator fallback
    from dsl.ast import MessageDecl, Field, Type, DslFile

    class FakeType(Type):
        pass

    fake = MessageDecl(
        name="X",
        metadata={"id": 1},
        fields=[Field(name="f", type=FakeType())],
    )

    ast = DslFile([fake])

    with pytest.raises(RuntimeError):
        CppGenerator(namespace="ns").emit(ast)


def test_list_of_reference_decode_branch():
    code = generate("""
        message Node (id=1)
            i32 value
        end

        message Tree (id=2)
            list<Node> nodes
        end
    """)
    assert code


def test_nested_reference_lists():
    code = generate("""
        message Leaf (id=1)
            i32 x
        end

        message Branch (id=2)
            list<Leaf> leaves
        end

        message Tree (id=3)
            list<Branch> branches
        end
    """)
    assert code
