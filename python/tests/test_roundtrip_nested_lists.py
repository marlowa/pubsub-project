import pytest

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from tests.utils import compile_and_load  # new pybind11-based version


def build(text: str):
    """Parse, validate, generate C++ + bindings, build dslgen, and return the module."""
    ast = Parser(text).parse()
    Validator(ast).validate()
    # CppGenerator is still responsible only for generated.hpp
    code = CppGenerator("ns").emit(ast)
    # compile_and_load now takes DSL text (or AST) and returns the pybind11 module
    # We ignore `code` here; compile_and_load will re-run the generator internally
    # if you keep that design. If you prefer, you can refactor compile_and_load
    # to accept `ast` instead.
    return compile_and_load(text)


def test_roundtrip_list_of_i32():
    mod = build("""
        message Foo (id=1)
            list<i32> xs
        end
    """)

    # Keyword-arg construction; xs is a Python list
    foo = mod.Foo(xs=[1, 2, 3])

    buf = mod.encode_Foo(foo)          # EncodedBuffer view
    foo2 = mod.decode_Foo(buf)         # accepts EncodedBuffer

    assert list(foo2.xs) == [1, 2, 3]


def test_roundtrip_list_of_list_of_i32():
    mod = build("""
        message Foo (id=1)
            list<list<i32>> matrix
        end
    """)

    foo = mod.Foo(
        matrix=[
            [1, 2, 3],
            [10, 20],
        ]
    )

    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)

    assert list(foo2.matrix[0]) == [1, 2, 3]
    assert list(foo2.matrix[1]) == [10, 20]


def test_roundtrip_list_of_list_of_list_of_i32():
    mod = build("""
        message Foo (id=1)
            list<list<list<i32>>> cube
        end
    """)

    foo = mod.Foo(
        cube=[
            [
                [7, 8],
            ],
        ]
    )

    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)

    assert list(foo2.cube[0][0]) == [7, 8]


def test_roundtrip_list_of_list_of_message():
    mod = build("""
        message Child (id=1)
            i32 x
        end

        message Parent (id=2)
            list<list<Child>> groups
        end
    """)

    parent = mod.Parent(
        groups=[
            [mod.Child(x=5), mod.Child(x=7)],
            [mod.Child(x=42)],
        ]
    )

    buf = mod.encode_Parent(parent)
    parent2 = mod.decode_Parent(buf)

    assert [child.x for child in parent2.groups[0]] == [5, 7]
    assert [child.x for child in parent2.groups[1]] == [42]


def test_roundtrip_empty_nested_lists():
    mod = build("""
        message Foo (id=1)
            list<list<i32>> matrix
        end
    """)

    foo = mod.Foo(matrix=[])

    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)

    assert foo2.matrix.size == 0
