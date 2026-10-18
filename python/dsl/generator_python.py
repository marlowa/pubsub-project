"""Python code generator for the pubsub_itc_fw DSL.

Writes one plain Python module for a DSL file, using only the standard library, so that a Python program -- the
high availability test harness first of all -- can encode and decode the same messages as the C++ and Java code,
byte for byte. The generated code runs on Python 3.8 and later, because the Rocky Linux 8 image has 3.8.

The wire format is the one in docs/dsl_design.dox, section "Wire format", and the generated code does what the
C++ generator's code does wherever the two could differ:

- an enumerated field decodes whatever value it holds; a value the enumeration does not list is kept as a plain
  int, and first_invalid_field() is what rejects it;
- a string is decoded with the "surrogateescape" error handler, so bytes that are not UTF-8 are kept, as the C++
  decoder keeps them, and are written back unchanged when the message is encoded again;
- a message that ends where an optional field would begin decodes with that field absent, which is how a field
  is added to a message whose earlier encodings are still read;
- a bool is written as 1 or 0, and decodes as true for any byte that is not 0; an optional field's flag the same.
"""

from __future__ import annotations

import keyword
from dataclasses import dataclass, field as dataclass_field
from typing import Dict, List, Set

from .ast import (
    ArrayType,
    BytesType,
    DslFile,
    EnumDecl,
    Field,
    FramingDecl,
    ListType,
    MessageDecl,
    PrimitiveType,
    ReferenceType,
    StringType,
)

# The struct format of each integer type, little-endian, as the DSL's wire format has it.
_INTEGER_FORMATS = {
    "i8": "_I8",
    "i16": "_I16",
    "i32": "_I32",
    "i64": "_I64",
    "datetime_ns": "_I64",
}



def python_name(name: str) -> str:
    """A DSL name as a Python identifier: one that is a Python keyword, such as None or from, gains a trailing _."""
    return name + "_" if keyword.iskeyword(name) else name


# What every generated module starts with: the two exceptions and the helpers the generated functions call. Each
# module carries its own copy, so a module can be used on its own, with nothing else installed beside it.
_PRELUDE = '''
import dataclasses
import enum
import struct
from typing import ClassVar, Dict, List, Optional, Tuple, Type, Union


class DecodeError(ValueError):
    """A buffer that does not hold a whole message of the kind asked for."""


class EncodeError(ValueError):
    """A value that cannot be written in the field it is in."""


_I8 = struct.Struct("<b")
_I16 = struct.Struct("<h")
_I32 = struct.Struct("<i")
_I64 = struct.Struct("<q")
_U8 = struct.Struct("<B")
_U32 = struct.Struct("<I")


def _pack(form: struct.Struct, value: int, out: bytearray, where: str) -> None:
    try:
        out += form.pack(value)
    except struct.error as error:
        raise EncodeError(f"{where}: {value!r} cannot be written: {error}") from None


def _unpack(form: struct.Struct, buffer: bytes, position: int, end: int, where: str) -> Tuple[int, int]:
    if end - position < form.size:
        raise DecodeError(f"{where}: the buffer ends {end - position} byte(s) after it starts, and {form.size} are needed")
    return form.unpack_from(buffer, position)[0], position + form.size


def _pack_bool(value: bool, out: bytearray) -> None:
    out.append(1 if value else 0)


def _unpack_bool(buffer: bytes, position: int, end: int, where: str) -> Tuple[bool, int]:
    value, position = _unpack(_U8, buffer, position, end, where)
    return value != 0, position


def _pack_char(value: str, out: bytearray, where: str) -> None:
    if not isinstance(value, str) or len(value) != 1 or ord(value) > 255:
        raise EncodeError(f"{where}: {value!r} is not a single character with a code below 256")
    out.append(ord(value))


def _unpack_char(buffer: bytes, position: int, end: int, where: str) -> Tuple[str, int]:
    value, position = _unpack(_U8, buffer, position, end, where)
    return chr(value), position


def _pack_bytes(value: bytes, out: bytearray, where: str) -> None:
    _pack(_U32, len(value), out, where)
    out += value


def _unpack_bytes(buffer: bytes, position: int, end: int, where: str) -> Tuple[bytes, int]:
    length, position = _unpack(_U32, buffer, position, end, where)
    if end - position < length:
        raise DecodeError(f"{where}: the buffer ends {end - position} byte(s) into a value of {length} byte(s)")
    return bytes(buffer[position:position + length]), position + length


def _pack_string(value: str, out: bytearray, where: str) -> None:
    _pack_bytes(value.encode("utf-8", "surrogateescape"), out, where)


def _unpack_string(buffer: bytes, position: int, end: int, where: str) -> Tuple[str, int]:
    raw, position = _unpack_bytes(buffer, position, end, where)
    return raw.decode("utf-8", "surrogateescape"), position


def _pack_count(values: list, out: bytearray, where: str) -> None:
    _pack(_U32, len(values), out, where)


def _check_array(values: list, length: int, where: str) -> None:
    if len(values) != length:
        raise EncodeError(f"{where}: a fixed array of {length} element(s) holds {len(values)}")


def _enum_value(enum_type: type, value: int) -> Union[enum.IntEnum, int]:
    try:
        return enum_type(value)
    except ValueError:
        return value


def _enum_is_valid(enum_type: type, value: int) -> bool:
    try:
        enum_type(value)
        return True
    except ValueError:
        return False
'''


@dataclass
class PythonGenerator:
    """Generates a plain Python module from a validated DSL AST.

    For each DSL enum, an IntEnum class. For each DSL message, a dataclass with:
      - MESSAGE_ID, and MESSAGE_VERSION when the DSL gives one, as class constants
      - its fields in the order the DSL gives them, each with a default (0, False, "\\0", "", b"", an empty list,
        a fixed array of zeros, a default nested message, or None for an optional field)
      - encode() -> bytes, encode_into(bytearray), encoded_size() -> int
      - decode(buffer, position=0, end=None) -> (message, position after it), a class method
      - first_invalid_field() -> the name of the first field whose value the definition does not allow, or None
    And MESSAGES_BY_ID, mapping each message id to its class, and the framing block's constants when there is one.
    """

    source_name: str = ""

    _enum_decls: Dict[str, EnumDecl] = dataclass_field(default_factory=dict, init=False, repr=False)
    _message_names: Set[str] = dataclass_field(default_factory=set, init=False, repr=False)

    def emit(self, ast: DslFile) -> str:
        """Generate and return the complete Python module as a string."""
        self._enum_decls = {d.name: d for d in ast.declarations if isinstance(d, EnumDecl)}
        self._message_names = {d.name for d in ast.declarations if isinstance(d, MessageDecl)}
        lines: List[str] = []
        w = lines.append
        w("# -----------------------------------------------------------------------------")
        w("#  This file was generated by the DSL code generator (python/dsl/generator_python.py).")
        w("#  Do NOT edit it by hand.")
        if self.source_name:
            w(f"#  Source: {self.source_name}")
        w("# -----------------------------------------------------------------------------")
        w(f'"""Messages of {self.source_name or "a DSL file"}, encoded and decoded as the C++ and Java code does."""')
        w("")
        w("from __future__ import annotations")
        lines.extend(_PRELUDE.split("\n"))
        for decl in ast.declarations:
            if isinstance(decl, FramingDecl):
                self._emit_framing(decl, w)
        for decl in ast.declarations:
            if isinstance(decl, EnumDecl):
                self._emit_enum(decl, w)
        messages = [d for d in ast.declarations if isinstance(d, MessageDecl)]
        for msg in messages:
            self._emit_message(msg, w)
        w("")
        w("# Each message class, by the message id the PDU header carries.")
        w("MESSAGES_BY_ID: Dict[int, type] = {")
        for msg in messages:
            if "id" in msg.metadata:
                w(f"    {msg.metadata['id']}: {msg.name},")
        w("}")
        w("")
        return "\n".join(lines)

    # ------------------------------------------------------------------
    # Framing constants and enums
    # ------------------------------------------------------------------

    def _emit_framing(self, framing: FramingDecl, w) -> None:
        w("")
        w("# The framing block's constants.")
        for key, value in framing.constants.items():
            formatted = f"0x{value:08X}" if value > 0xFFFF else str(value)
            w(f"{key.upper()} = {formatted}")

    def _emit_enum(self, enum: EnumDecl, w) -> None:
        w("")
        w("")
        w(f"class {enum.name}(enum.IntEnum):")
        if not enum.entries:
            w("    pass")
        for entry in enum.entries:
            w(f"    {python_name(entry.name)} = {entry.value}")
        w("")
        w("")
        w(f"_{enum.name}_FORM = {self._integer_form(enum.underlying_type)}")

    @staticmethod
    def _integer_form(type_name: str) -> str:
        if type_name == "char":
            return "_U8"
        return _INTEGER_FORMATS[type_name]

    # ------------------------------------------------------------------
    # Messages
    # ------------------------------------------------------------------

    def _emit_message(self, msg: MessageDecl, w) -> None:  # pylint: disable=too-many-statements
        w("")
        w("")
        w("@dataclasses.dataclass")
        w(f"class {msg.name}:")
        w(f'    """The DSL message {msg.name}."""')
        w("")
        if "id" in msg.metadata:
            w(f"    MESSAGE_ID: ClassVar[int] = {msg.metadata['id']}")
        if "version" in msg.metadata:
            w(f"    MESSAGE_VERSION: ClassVar[int] = {msg.metadata['version']}")
        for fld in msg.fields:
            w(f"    {python_name(fld.name)}: {self._annotation(fld)} = {self._default(fld)}")
        w("")
        w("    def encode(self) -> bytes:")
        w('        """The message\'s bytes, as the C++ encoder writes them."""')
        w("        out = bytearray()")
        w("        self.encode_into(out)")
        w("        return bytes(out)")
        w("")
        w("    def encoded_size(self) -> int:")
        w('        """How many bytes encode() writes."""')
        w("        return len(self.encode())")
        w("")
        w("    def encode_into(self, out: bytearray) -> None:")
        w('        """Append the message\'s bytes to out."""')
        if not msg.fields:
            w("        return")
        for fld in msg.fields:
            self._emit_encode_field(msg, fld, w)
        w("")
        w("    @classmethod")
        w(f"    def decode(cls, buffer: bytes, position: int = 0, end: Optional[int] = None) -> Tuple[{msg.name}, int]:")
        w('        """Decode a message starting at position, ending no later than end; return it and the position after it.')
        w("")
        w("        Raises DecodeError, naming the message and the field, if the buffer ends before the message does.")
        w('        """')
        w("        if end is None:")
        w("            end = len(buffer)")
        w("        message = cls()")
        for fld in msg.fields:
            self._emit_decode_field(msg, fld, w)
        w("        return message, position")
        w("")
        w("    def first_invalid_field(self) -> Optional[str]:")
        w('        """The first field whose value the definition does not allow, as the C++ first_invalid_field names it.')
        w("")
        w("        A value an enumerated field's enumeration does not define, or an empty string in a string field that")
        w("        is not optional. Nested messages and the elements of lists are checked in the same way.")
        w('        """')
        checks: List[str] = []
        for fld in msg.fields:
            self._emit_validity_check(fld, checks.append)
        for line in checks:
            w(line)
        w("        return None")

    # ------------------------------------------------------------------
    # Annotations and defaults
    # ------------------------------------------------------------------

    def _python_type(self, type_node) -> str:
        if isinstance(type_node, PrimitiveType):
            return {"bool": "bool", "char": "str"}.get(type_node.name, "int")
        if isinstance(type_node, StringType):
            return "str"
        if isinstance(type_node, BytesType):
            return "bytes"
        if isinstance(type_node, (ListType, ArrayType)):
            return f"List[{self._python_type(type_node.element_type)}]"
        if isinstance(type_node, ReferenceType):
            if type_node.name in self._enum_decls:
                return f"Union[{type_node.name}, int]"
            return type_node.name
        raise RuntimeError(f"Unknown type in Python type mapping: {type_node}")

    def _annotation(self, fld: Field) -> str:
        python_type = self._python_type(fld.type)
        return f"Optional[{python_type}]" if fld.optional else python_type

    def _default(self, fld: Field) -> str:  # pylint: disable=too-many-return-statements
        if fld.optional:
            return "None"
        type_node = fld.type
        if isinstance(type_node, PrimitiveType):
            return {"bool": "False", "char": '"\\0"'}.get(type_node.name, "0")
        if isinstance(type_node, StringType):
            return '""'
        if isinstance(type_node, BytesType):
            return 'b""'
        if isinstance(type_node, ListType):
            return "dataclasses.field(default_factory=list)"
        if isinstance(type_node, ArrayType):
            element = {"bool": "False", "char": '"\\0"'}.get(type_node.element_type.name, "0")
            return f"dataclasses.field(default_factory=lambda: [{element}] * {type_node.length})"
        if isinstance(type_node, ReferenceType):
            if type_node.name in self._enum_decls:
                enum = self._enum_decls[type_node.name]
                return f"{type_node.name}.{python_name(enum.entries[0].name)}" if enum.entries else "0"
            return f"dataclasses.field(default_factory=lambda: {type_node.name}())"
        raise RuntimeError(f"No Python default for type: {type_node}")

    # ------------------------------------------------------------------
    # Encode
    # ------------------------------------------------------------------

    def _emit_encode_field(self, msg: MessageDecl, fld: Field, w) -> None:
        where = f'"{msg.name}.{fld.name}"'
        expr = f"self.{python_name(fld.name)}"
        if fld.optional:
            w(f"        out.append(0 if {expr} is None else 1)")
            w(f"        if {expr} is not None:")
            self._emit_encode_value(fld.type, expr, where, w, indent="            ", depth=0)
        else:
            self._emit_encode_value(fld.type, expr, where, w, indent="        ", depth=0)

    def _emit_encode_value(self, type_node, expr: str, where: str, w, *, indent: str, depth: int) -> None:  # pylint: disable=too-many-arguments,too-many-branches
        if isinstance(type_node, PrimitiveType):
            name = type_node.name
            if name == "bool":
                w(f"{indent}_pack_bool({expr}, out)")
            elif name == "char":
                w(f"{indent}_pack_char({expr}, out, {where})")
            else:
                w(f"{indent}_pack({_INTEGER_FORMATS[name]}, {expr}, out, {where})")
        elif isinstance(type_node, StringType):
            w(f"{indent}_pack_string({expr}, out, {where})")
        elif isinstance(type_node, BytesType):
            w(f"{indent}_pack_bytes({expr}, out, {where})")
        elif isinstance(type_node, ArrayType):
            element = f"element_{depth}"
            w(f"{indent}_check_array({expr}, {type_node.length}, {where})")
            w(f"{indent}for {element} in {expr}:")
            self._emit_encode_value(type_node.element_type, element, where, w, indent=indent + "    ", depth=depth + 1)
        elif isinstance(type_node, ListType):
            element = f"element_{depth}"
            w(f"{indent}_pack_count({expr}, out, {where})")
            w(f"{indent}for {element} in {expr}:")
            self._emit_encode_value(type_node.element_type, element, where, w, indent=indent + "    ", depth=depth + 1)
        elif isinstance(type_node, ReferenceType):
            if type_node.name in self._enum_decls:
                w(f"{indent}_pack(_{type_node.name}_FORM, int({expr}), out, {where})")
            else:
                w(f"{indent}{expr}.encode_into(out)")
        else:
            raise RuntimeError(f"Unknown type in Python encode: {type_node}")

    # ------------------------------------------------------------------
    # Decode
    # ------------------------------------------------------------------

    def _emit_decode_field(self, msg: MessageDecl, fld: Field, w) -> None:
        where = f'"{msg.name}.{fld.name}"'
        target = f"message.{python_name(fld.name)}"
        if fld.optional:
            # A message that ends where an optional field would begin was encoded before the field existed.
            w("        if position < end:")
            w(f"            present, position = _unpack_bool(buffer, position, end, {where})")
            w("            if present:")
            self._emit_decode_value(fld.type, target, where, w, indent="                ", depth=0)
        else:
            self._emit_decode_value(fld.type, target, where, w, indent="        ", depth=0)

    def _emit_decode_value(self, type_node, target: str, where: str, w, *, indent: str, depth: int) -> None:  # pylint: disable=too-many-arguments,too-many-branches
        if isinstance(type_node, PrimitiveType):
            name = type_node.name
            if name == "bool":
                w(f"{indent}{target}, position = _unpack_bool(buffer, position, end, {where})")
            elif name == "char":
                w(f"{indent}{target}, position = _unpack_char(buffer, position, end, {where})")
            else:
                w(f"{indent}{target}, position = _unpack({_INTEGER_FORMATS[name]}, buffer, position, end, {where})")
        elif isinstance(type_node, StringType):
            w(f"{indent}{target}, position = _unpack_string(buffer, position, end, {where})")
        elif isinstance(type_node, BytesType):
            w(f"{indent}{target}, position = _unpack_bytes(buffer, position, end, {where})")
        elif isinstance(type_node, (ArrayType, ListType)):
            items = f"items_{depth}"
            count = f"count_{depth}"
            item = f"item_{depth}"
            w(f"{indent}{items} = []")
            if isinstance(type_node, ArrayType):
                w(f"{indent}{count} = {type_node.length}")
            else:
                w(f"{indent}{count}, position = _unpack(_U32, buffer, position, end, {where})")
            w(f"{indent}for _ in range({count}):")
            self._emit_decode_value(type_node.element_type, item, where, w, indent=indent + "    ", depth=depth + 1)
            w(f"{indent}    {items}.append({item})")
            w(f"{indent}{target} = {items}")
        elif isinstance(type_node, ReferenceType):
            if type_node.name in self._enum_decls:
                raw = f"raw_{depth}"
                w(f"{indent}{raw}, position = _unpack(_{type_node.name}_FORM, buffer, position, end, {where})")
                w(f"{indent}{target} = _enum_value({type_node.name}, {raw})")
            else:
                w(f"{indent}{target}, position = {type_node.name}.decode(buffer, position, end)")
        else:
            raise RuntimeError(f"Unknown type in Python decode: {type_node}")

    # ------------------------------------------------------------------
    # first_invalid_field, as the C++ generator writes it
    # ------------------------------------------------------------------

    def _emit_validity_check(self, fld: Field, w) -> None:
        name = fld.name
        expr = f"self.{python_name(name)}"
        type_node = fld.type
        if isinstance(type_node, StringType):
            if not fld.optional:
                w(f"        if {expr} == \"\":")
                w(f"            return \"{name}\"")
        elif isinstance(type_node, ReferenceType):
            if type_node.name in self._enum_decls:
                guard = f"{expr} is not None and " if fld.optional else ""
                w(f"        if {guard}not _enum_is_valid({type_node.name}, {expr}):")
                w(f"            return \"{name}\"")
            else:
                guard = f"{expr} is not None and " if fld.optional else ""
                w(f"        if {guard}{expr}.first_invalid_field() is not None:")
                w(f"            return {expr}.first_invalid_field()")
        elif isinstance(type_node, ListType):
            element = type_node.element_type
            if isinstance(element, ReferenceType):
                w(f"        for element in {expr} or []:")
                if element.name in self._enum_decls:
                    w(f"            if not _enum_is_valid({element.name}, element):")
                    w(f"                return \"{name}\"")
                else:
                    w("            if element.first_invalid_field() is not None:")
                    w("                return element.first_invalid_field()")
