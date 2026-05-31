import pytest

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_java import JavaGenerator


def generate(text: str, class_name="TestSchema", package_name=""):
    ast = Parser(text).parse()
    Validator(ast).validate()
    gen = JavaGenerator(class_name=class_name, package_name=package_name)
    return gen.emit(ast)


def test_class_wrapping():
    code = generate("enum Venue : i16 { lse = 1 }", class_name="MySchema")
    assert "public final class MySchema {" in code
    assert "} // class MySchema" in code


def test_no_package_by_default():
    code = generate("enum Venue : i16 { lse = 1 }")
    assert "package " not in code


def test_package_declaration():
    code = generate("enum Venue : i16 { lse = 1 }", package_name="com.example.app")
    assert "package com.example.app;" in code


def test_imports_present():
    code = generate("enum Venue : i16 { lse = 1 }")
    assert "import java.nio.ByteBuffer;" in code
    assert "import java.nio.ByteOrder;" in code
    assert "import java.nio.charset.StandardCharsets;" in code


def test_enum_entries():
    text = """
        enum Venue : i16 {
            lse = 1
            chix = 2
        }
    """
    code = generate(text)
    assert "public enum Venue {" in code
    assert "lse(1)," in code
    assert "chix(2);" in code


def test_enum_value_field_and_constructor():
    text = "enum Venue : i16 { lse = 1 }"
    code = generate(text)
    assert "public final int value;" in code
    assert "Venue(int v) { this.value = v; }" in code


def test_enum_from_value():
    text = "enum Venue : i16 { lse = 1 }"
    code = generate(text)
    assert "public static Venue fromValue(int v) {" in code
    assert "for (Venue e : values()) { if (e.value == v) return e; }" in code
    assert "return null;" in code


def test_enum_wire_size_i8():
    text = "enum Flag : i8 { on = 1 off = 0 }"
    code = generate(text)
    assert "public static int wireSize() {" in code
    assert "return 1;" in code


def test_enum_wire_size_i16():
    text = "enum Venue : i16 { lse = 1 }"
    code = generate(text)
    assert "return 2;" in code


def test_enum_wire_size_i32():
    text = "enum Code : i32 { alpha = 100 }"
    code = generate(text)
    assert "return 4;" in code


def test_enum_i64_uses_long_value_type():
    text = "enum BigEnum : i64 { large = 1 }"
    code = generate(text)
    assert "public final long value;" in code
    assert "BigEnum(long v) { this.value = v; }" in code
    assert "public static BigEnum fromValue(long v) {" in code


def test_message_class_declaration():
    text = """
        message Trade (id=1)
            i64 price
        end
    """
    code = generate(text)
    assert "public static final class Trade {" in code


def test_primitive_field_types():
    text = """
        message All (id=1)
            i8  a
            i16 b
            i32 c
            i64 d
            bool e
            datetime_ns f
        end
    """
    code = generate(text)
    assert "public byte a = (byte) 0;" in code
    assert "public short b = (short) 0;" in code
    assert "public int c = 0;" in code
    assert "public long d = 0L;" in code
    assert "public boolean e = false;" in code
    assert "public long f = 0L;" in code


def test_char_maps_to_byte_not_java_char():
    # DSL 'char' is a 1-byte value; Java char is 2-byte UTF-16 — must NOT map to Java char
    text = """
        message Foo (id=1)
            char tag
        end
    """
    code = generate(text)
    assert "public byte tag = (byte) 0;" in code
    assert "public char tag" not in code


def test_string_field():
    text = """
        message Foo (id=1)
            string name
        end
    """
    code = generate(text)
    assert 'public String name = "";' in code


def test_bytes_field():
    text = """
        message Foo (id=1)
            bytes payload
        end
    """
    code = generate(text)
    assert "public byte[] payload = new byte[0];" in code


def test_optional_field_has_prefix():
    text = """
        message Foo (id=1)
            optional i32 qty
        end
    """
    code = generate(text)
    assert "public boolean has_qty = false;" in code
    assert "public int qty = 0;" in code


def test_list_field_primitive():
    text = """
        message Foo (id=1)
            list<i32> values
        end
    """
    code = generate(text)
    assert "public int[] values = new int[0];" in code


def test_list_field_string():
    text = """
        message Foo (id=1)
            list<string> tags
        end
    """
    code = generate(text)
    assert "public String[] tags = new String[0];" in code


def test_array_field():
    text = """
        message Foo (id=1)
            i8[16] digest
        end
    """
    code = generate(text)
    assert "public byte[] digest = new byte[16];" in code


def test_encode_signature():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "public static int encode(Foo msg, ByteBuffer buf) {" in code


def test_encoded_size_signature():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "public static int encodedSize(Foo msg) {" in code


def test_decode_signature():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "public static Foo decode(ByteBuffer buf) {" in code


def test_decode_fields_signature():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "static Foo _decodeFields(ByteBuffer buf) {" in code


def test_byte_order_little_endian_in_encode():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "buf.order(ByteOrder.LITTLE_ENDIAN);" in code


def test_encode_capacity_check():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "if (buf.remaining() < needed) return -1;" in code


def test_buffer_underflow_handling():
    text = """
        message Foo (id=1)
            i32 x
        end
    """
    code = generate(text)
    assert "} catch (java.nio.BufferUnderflowException e) {" in code
    assert "buf.position(startPos);" in code
    assert "return null;" in code


def test_string_encoding_uses_utf8_bytes():
    text = """
        message Foo (id=1)
            string name
        end
    """
    code = generate(text)
    assert "getBytes(StandardCharsets.UTF_8)" in code
    assert "buf.putInt(_strBytes.length);" in code
    assert "buf.put(_strBytes);" in code


def test_string_decoding_uses_utf8():
    text = """
        message Foo (id=1)
            string name
        end
    """
    code = generate(text)
    assert "buf.getInt();" in code
    assert "new String(_strBytes, StandardCharsets.UTF_8)" in code


def test_list_encode_writes_count_prefix():
    text = """
        message Foo (id=1)
            list<i32> xs
        end
    """
    code = generate(text)
    assert "buf.putInt(msg.xs.length);" in code


def test_list_decode_reads_count():
    text = """
        message Foo (id=1)
            list<i32> xs
        end
    """
    code = generate(text)
    assert "int count_i = buf.getInt();" in code


def test_optional_field_encode():
    text = """
        message Foo (id=1)
            optional i32 qty
        end
    """
    code = generate(text)
    assert "buf.put(msg.has_qty ? (byte) 1 : (byte) 0);" in code
    assert "if (msg.has_qty) {" in code


def test_optional_field_decode():
    text = """
        message Foo (id=1)
            optional i32 qty
        end
    """
    code = generate(text)
    assert "out.has_qty = (buf.get() != 0);" in code
    assert "if (out.has_qty) {" in code


def test_enum_field_encode():
    text = """
        enum Venue : i16 { lse = 1 chix = 2 }
        message Trade (id=1)
            Venue venue
        end
    """
    code = generate(text)
    assert "buf.putShort((short) msg.venue.value);" in code


def test_enum_field_decode():
    text = """
        enum Venue : i16 { lse = 1 chix = 2 }
        message Trade (id=1)
            Venue venue
        end
    """
    code = generate(text)
    assert "out.venue = Venue.fromValue(buf.getShort());" in code


def test_nested_message_encode():
    text = """
        message A (id=1)
            i32 x
        end
        message B (id=2)
            A child
        end
    """
    code = generate(text)
    assert "A.encode(msg.child, buf);" in code


def test_nested_message_decode():
    text = """
        message A (id=1)
            i32 x
        end
        message B (id=2)
            A child
        end
    """
    code = generate(text)
    assert "out.child = A._decodeFields(buf);" in code


def test_nested_list_of_messages():
    text = """
        message Item (id=1)
            i32 value
        end
        message Container (id=2)
            list<Item> items
        end
    """
    code = generate(text)
    assert "Item._decodeFields(buf);" in code
    assert "Item.encode(" in code


def test_nested_list_of_lists():
    text = """
        message Foo (id=1)
            list<list<i32>> matrix
        end
    """
    code = generate(text)
    # Outer count and inner count both present
    assert "count_i" in code
    assert "count_j" in code
    # Array type: int[][]
    assert "int[][] matrix" in code


def test_bool_field_encode():
    text = """
        message Foo (id=1)
            bool flag
        end
    """
    code = generate(text)
    assert "buf.put(msg.flag ? (byte) 1 : (byte) 0);" in code


def test_bool_field_decode():
    text = """
        message Foo (id=1)
            bool flag
        end
    """
    code = generate(text)
    assert "out.flag = (buf.get() != 0);" in code


def test_datetime_ns_maps_to_long():
    text = """
        message Foo (id=1)
            datetime_ns ts
        end
    """
    code = generate(text)
    assert "public long ts = 0L;" in code
    assert "buf.putLong(msg.ts);" in code
    assert "out.ts = buf.getLong();" in code


def test_bytes_field_encode():
    text = """
        message Foo (id=1)
            bytes data
        end
    """
    code = generate(text)
    assert "buf.putInt(msg.data.length);" in code
    assert "buf.put(msg.data);" in code


def test_bytes_field_decode():
    text = """
        message Foo (id=1)
            bytes data
        end
    """
    code = generate(text)
    assert "int _bytesLen = buf.getInt();" in code
    assert "out.data = new byte[_bytesLen];" in code
    assert "buf.get(out.data);" in code


def test_reference_field_type():
    text = """
        message A (id=1)
            i32 x
        end
        message B (id=2)
            A child
        end
    """
    code = generate(text)
    assert "public A child = null;" in code


def test_enum_field_default_is_null():
    text = """
        enum Venue : i16 { lse = 1 }
        message Trade (id=1)
            Venue venue
        end
    """
    code = generate(text)
    assert "public Venue venue = null;" in code


def test_generated_header_comment():
    code = generate("enum Venue : i16 { lse = 1 }")
    assert "This file was automatically generated by the DSL code generator." in code
    assert "Do NOT edit this file manually." in code
