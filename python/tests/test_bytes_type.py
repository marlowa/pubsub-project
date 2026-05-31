"""Tests for the bytes type in the DSL."""

import pytest

from dsl.lexer import Lexer
from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_pybind11 import Pybind11Generator
from dsl.ast import BytesType, MessageDecl
from tests.utils import compile_and_load


# =============================================================================
# Lexer
# =============================================================================

def test_lexer_recognises_bytes_keyword():
    tokens = []
    lex = Lexer("bytes")
    tok = lex.next_token()
    assert tok.kind == "KEYWORD"
    assert tok.value == "bytes"


def test_lexer_bytes_not_treated_as_ident():
    lex = Lexer("bytes")
    tok = lex.next_token()
    assert tok.kind != "IDENT"


# =============================================================================
# Parser
# =============================================================================

def test_parser_produces_bytes_type():
    ast = Parser("""
        message Foo (id=1)
            bytes payload
        end
    """).parse()
    msg = ast.declarations[0]
    assert isinstance(msg, MessageDecl)
    assert isinstance(msg.fields[0].type, BytesType)


def test_parser_bytes_field_name_preserved():
    ast = Parser("""
        message Foo (id=1)
            bytes client_nonce
        end
    """).parse()
    assert ast.declarations[0].fields[0].name == "client_nonce"


# =============================================================================
# Validator
# =============================================================================

def test_validator_accepts_bytes_field():
    ast = Parser("""
        message Foo (id=1)
            bytes payload
        end
    """).parse()
    Validator(ast).validate()


def test_validator_accepts_optional_bytes_field():
    ast = Parser("""
        message Foo (id=1)
            optional bytes payload
        end
    """).parse()
    Validator(ast).validate()


def test_validator_accepts_list_of_bytes():
    ast = Parser("""
        message Foo (id=1)
            list<bytes> chunks
        end
    """).parse()
    Validator(ast).validate()


# =============================================================================
# C++ code generator
# =============================================================================

def generate_cpp(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return CppGenerator(namespace="ns").emit(ast)


def test_generator_emits_bytes_view_struct():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    assert "struct BytesView" in code
    assert "const uint8_t* data" in code
    assert "std::size_t size" in code


def test_generator_bytes_field_in_owning_struct():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    assert "BytesView payload" in code


def test_generator_bytes_field_size_uses_dot_size():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    assert "message.payload.size" in code
    assert "message.payload.data" in code


def test_generator_bytes_field_decode_uses_bytes_view_initialiser():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    assert "BytesView{" in code
    assert "out.payload" in code


def test_generator_bytes_field_skip_uses_bytes_len():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    assert "bytes_len" in code


def test_generator_bytes_view_in_ifdef_guard():
    code = generate_cpp("message Foo (id=1)\n    bytes payload\nend")
    guard_start = code.index("PUBSUB_ITC_FW_APP_DSL_SHARED_HELPERS_DEFINED")
    bytes_view_pos = code.index("struct BytesView")
    assert bytes_view_pos > guard_start


# =============================================================================
# Pybind11 roundtrip — single bytes field
# =============================================================================

def test_roundtrip_bytes_field():
    mod = compile_and_load("""
        message Foo (id=1)
            bytes payload
        end
    """)
    foo = mod.Foo(payload=b"\x01\x02\x03\x04\x05")
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.payload == b"\x01\x02\x03\x04\x05"


def test_roundtrip_bytes_field_empty():
    mod = compile_and_load("""
        message Foo (id=1)
            bytes payload
        end
    """)
    foo = mod.Foo(payload=b"")
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.payload == b""


def test_roundtrip_bytes_field_binary_content():
    mod = compile_and_load("""
        message Foo (id=1)
            bytes payload
        end
    """)
    data = bytes(range(256))
    foo = mod.Foo(payload=data)
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.payload == data


# =============================================================================
# Pybind11 roundtrip — bytes alongside other field types
# =============================================================================

def test_roundtrip_mixed_fields_with_bytes():
    mod = compile_and_load("""
        message AuthenticationProof (id=1)
            i64 request_id
            bytes client_proof
        end
    """)
    proof = mod.AuthenticationProof(
        request_id=42,
        client_proof=b"\xde\xad\xbe\xef",
    )
    buf = mod.encode_AuthenticationProof(proof)
    view = mod.decode_AuthenticationProof(buf)
    assert view.request_id == 42
    assert view.client_proof == b"\xde\xad\xbe\xef"


def test_roundtrip_multiple_bytes_fields():
    mod = compile_and_load("""
        message ScramChallenge (id=1)
            bytes server_nonce
            bytes salt
            i32 iterations
        end
    """)
    msg = mod.ScramChallenge(
        server_nonce=b"nonce_server_suffix",
        salt=b"\x12\x34\x56\x78",
        iterations=4096,
    )
    buf = mod.encode_ScramChallenge(msg)
    view = mod.decode_ScramChallenge(buf)
    assert view.server_nonce == b"nonce_server_suffix"
    assert view.salt == b"\x12\x34\x56\x78"
    assert view.iterations == 4096


# =============================================================================
# Pybind11 roundtrip — list<bytes>
# =============================================================================

def test_roundtrip_list_of_bytes():
    mod = compile_and_load("""
        message Foo (id=1)
            list<bytes> chunks
        end
    """)
    foo = mod.Foo(chunks=[b"hello", b"world", b"\x00\x01\x02"])
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.chunks.size == 3
    assert foo2.chunks[0] == b"hello"
    assert foo2.chunks[1] == b"world"
    assert foo2.chunks[2] == b"\x00\x01\x02"


def test_roundtrip_list_of_bytes_empty():
    mod = compile_and_load("""
        message Foo (id=1)
            list<bytes> chunks
        end
    """)
    foo = mod.Foo(chunks=[])
    buf = mod.encode_Foo(foo)
    foo2 = mod.decode_Foo(buf)
    assert foo2.chunks.size == 0


# =============================================================================
# encoded_size correctness
# =============================================================================

def test_encoded_size_bytes_field():
    mod = compile_and_load("""
        message Foo (id=1)
            bytes payload
        end
    """)
    foo = mod.Foo(payload=b"hello")
    size = mod.encoded_size_Foo(foo)
    # 4 bytes length prefix + 5 bytes data
    assert size == 9


def test_encoded_size_bytes_field_empty():
    mod = compile_and_load("""
        message Foo (id=1)
            bytes payload
        end
    """)
    foo = mod.Foo(payload=b"")
    size = mod.encoded_size_Foo(foo)
    assert size == 4
