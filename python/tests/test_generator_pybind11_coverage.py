"""Tests to improve coverage of generator_pybind11.py.

These tests exercise the pybind11 generator's type helpers and optional field
bindings that were not covered by the existing roundtrip tests.
"""

import pytest
from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_pybind11 import Pybind11Generator
from tests.utils import compile_and_load


def generate_bindings(text):
    """Generate both the C++ header and pybind11 bindings for inspection."""
    ast = Parser(text).parse()
    Validator(ast).validate()
    cpp_code = CppGenerator(namespace="ns").emit(ast)
    pyb_code = Pybind11Generator(namespace="ns", module_name="test_mod").emit(ast)
    return cpp_code, pyb_code


def build(text):
    return compile_and_load(text)


# =============================================================================
# String fields — covers _cpp_type_owning StringType (line 125)
# and _cpp_type_view StringType (line 140-141)
# =============================================================================

def test_bindings_contain_string_field():
    """A message with a string field generates correct pybind11 bindings."""
    _, pyb = generate_bindings("""
        message Foo (id=1)
            string name
        end
    """)
    assert "name" in pyb


def test_roundtrip_string_field():
    """A string field round-trips correctly through encode/decode."""
    mod = build("""
        message Foo (id=1)
            string name
        end
    """)
    foo = mod.Foo(name="hello")
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.name == "hello"


def test_roundtrip_multiple_string_fields():
    """Multiple string fields round-trip correctly."""
    mod = build("""
        message Order (id=1)
            string sender_comp_id
            string target_comp_id
            string cl_ord_id
        end
    """)
    order = mod.Order(
        sender_comp_id="GATEWAY1",
        target_comp_id="EXCHANGE",
        cl_ord_id="ORD-001"
    )
    buf = mod.encode_Order(order)
    order2 = mod.decode_Order(buf)
    assert order2.sender_comp_id == "GATEWAY1"
    assert order2.target_comp_id == "EXCHANGE"
    assert order2.cl_ord_id == "ORD-001"


# =============================================================================
# Array fields — covers _cpp_type_owning ArrayType (lines 131-133)
# and _cpp_type_view ArrayType (lines 147-149)
# =============================================================================

def test_bindings_contain_array_field():
    """A message with an array field generates correct pybind11 bindings."""
    _, pyb = generate_bindings("""
        message Foo (id=1)
            i8[16] hash
        end
    """)
    assert "hash" in pyb


def test_roundtrip_array_field():
    """An array field round-trips correctly through encode/decode."""
    mod = build("""
        message Foo (id=1)
            i8[4] data
        end
    """)
    foo = mod.Foo(data=[1, 2, 3, 4])
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert list(foo2.data) == [1, 2, 3, 4]


# =============================================================================
# Optional fields — covers has_X bindings (lines 236, 252, 278)
# =============================================================================

def test_bindings_contain_optional_has_field():
    """An optional field generates has_X binding in pybind11 output."""
    _, pyb = generate_bindings("""
        message Foo (id=1)
            optional i32 price
        end
    """)
    assert "has_price" in pyb


def test_roundtrip_optional_field_present():
    """An optional field that is present round-trips correctly."""
    mod = build("""
        message Foo (id=1)
            optional i32 price
        end
    """)
    foo = mod.Foo(has_price=True, price=12345)
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.has_price is True
    assert foo2.price == 12345


def test_roundtrip_optional_field_absent():
    """An optional field that is absent round-trips correctly."""
    mod = build("""
        message Foo (id=1)
            optional i32 price
        end
    """)
    foo = mod.Foo(has_price=False, price=0)
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.has_price is False


def test_roundtrip_optional_string_field():
    """An optional string field round-trips correctly."""
    mod = build("""
        message Foo (id=1)
            optional string comment
        end
    """)
    foo = mod.Foo(has_comment=True, comment="test")
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.has_comment is True
    assert foo2.comment == "test"


# =============================================================================
# List of strings — covers _collect_list_types for StringType (lines 170-171)
# =============================================================================

def test_bindings_contain_list_of_string():
    """A message with a list<string> field generates correct pybind11 bindings."""
    _, pyb = generate_bindings("""
        message Foo (id=1)
            list<string> tags
        end
    """)
    assert "tags" in pyb


def test_roundtrip_list_of_string():
    """A list<string> field round-trips correctly."""
    mod = build("""
        message Foo (id=1)
            list<string> tags
        end
    """)
    foo = mod.Foo(tags=["alpha", "beta", "gamma"])
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert list(foo2.tags) == ["alpha", "beta", "gamma"]


def test_roundtrip_empty_list_of_string():
    """An empty list<string> round-trips correctly."""
    mod = build("""
        message Foo (id=1)
            list<string> tags
        end
    """)
    foo = mod.Foo(tags=[])
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.tags.size == 0
