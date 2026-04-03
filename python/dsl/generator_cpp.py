from __future__ import annotations
from dataclasses import dataclass
from typing import List

from .ast import (
    DslFile,
    EnumDecl,
    MessageDecl,
    Field,
    PrimitiveType,
    StringType,
    ListType,
    ArrayType,
    ReferenceType,
)


@dataclass
class CppGenerator:
    namespace: str

    def emit(self, ast: DslFile) -> str:
        lines: List[str] = []
        w = lines.append

        # Preamble
        w("#pragma once")
        w("")
        w("#include <cstdint>")
        w("#include <string_view>")
        w("#include <cstring>")
        w("#include <array>")
        w("")
        w(f"namespace {self.namespace} {{")
        w("")
        self._emit_endian_macros(w)
        w("")
        self._emit_endian_helpers(w)
        w("")
        self._emit_list_view(w)
        w("")

        # Enums
        for decl in ast.declarations:
            if isinstance(decl, EnumDecl):
                self._emit_enum(decl, w)
                w("")

        # Forward declare messages
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                w(f"struct {decl.name};")
        if any(isinstance(d, MessageDecl) for d in ast.declarations):
            w("")

        # Messages
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_message_struct(decl, w)
                w("")

        # Functions (encode/decode/size)
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_message_functions(decl, w)
                w("")

        w(f"}} // namespace {self.namespace}")
        w("")
        return "\n".join(lines)

    # -----------------------------
    # Helpers
    # -----------------------------

    def _emit_endian_macros(self, w):
        w("#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__")
        w("#  define HOST_IS_LITTLE_ENDIAN 1")
        w("#else")
        w("#  define HOST_IS_LITTLE_ENDIAN 0")
        w("#endif")

    def _emit_endian_helpers(self, w):
        w("inline void write_uint16_le(uint8_t* buffer, std::uint16_t value) {")
        w("    buffer[0] = static_cast<std::uint8_t>(value & 0xFF);")
        w("    buffer[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);")
        w("}")
        w("")
        w("inline std::uint16_t read_uint16_le(const uint8_t* buffer) {")
        w("    return static_cast<std::uint16_t>(")
        w("        (std::uint16_t(buffer[0])      ) |")
        w("        (std::uint16_t(buffer[1]) << 8));")
        w("}")
        w("")
        w("inline void write_uint32_le(uint8_t* buffer, std::uint32_t value) {")
        w("    buffer[0] = static_cast<std::uint8_t>( value        & 0xFF);")
        w("    buffer[1] = static_cast<std::uint8_t>((value >> 8)  & 0xFF);")
        w("    buffer[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);")
        w("    buffer[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);")
        w("}")
        w("")
        w("inline std::uint32_t read_uint32_le(const uint8_t* buffer) {")
        w("    return ( std::uint32_t(buffer[0])       ) |")
        w("           ( std::uint32_t(buffer[1]) <<  8 ) |")
        w("           ( std::uint32_t(buffer[2]) << 16 ) |")
        w("           ( std::uint32_t(buffer[3]) << 24 );")
        w("}")
        w("")
        w("inline void write_uint64_le(uint8_t* buffer, std::uint64_t value) {")
        w("    for (int i = 0; i < 8; ++i) {")
        w("        buffer[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);")
        w("    }")
        w("}")
        w("")
        w("inline std::uint64_t read_uint64_le(const uint8_t* buffer) {")
        w("    std::uint64_t value = 0;")
        w("    for (int i = 0; i < 8; ++i) {")
        w("        value |= (std::uint64_t(buffer[i]) << (8 * i));")
        w("    }")
        w("    return value;")
        w("}")

    def _emit_list_view(self, w):
        w("template<typename T>")
        w("struct ListView {")
        w("    T* data = nullptr;")
        w("    std::size_t size = 0;")
        w("")
        w("    const T* begin() const { return data; }")
        w("    const T* end() const { return data + size; }")
        w("    const T& operator[](std::size_t index) const { return data[index]; }")
        w("")
        w("    bool empty() const { return size == 0; }")
        w("    std::size_t length() const { return size; }")
        w("};")

    def _emit_enum(self, enum: EnumDecl, w):
        w(f"enum {enum.name} : {self._cpp_int_type(enum.underlying_type)} {{")
        for i, entry in enumerate(enum.entries):
            comma = "," if i + 1 < len(enum.entries) else ""
            w(f"    {entry.name} = {entry.value}{comma}")
        w("};")
        w("")
        w(f"constexpr std::string_view to_string({enum.name} value) {{")
        w("    switch (value) {")
        for entry in enum.entries:
            w(f"    case {entry.name}: return std::string_view(\"{entry.name}\");")
        w("    default: return std::string_view(\"<unknown>\");")
        w("    }")
        w("}")
        w("")
        w(f"constexpr bool validate({enum.name} value) {{")
        w("    switch (value) {")
        for entry in enum.entries:
            w(f"    case {entry.name}: return true;")
        w("    default: return false;")
        w("    }")
        w("}")

    def _emit_message_struct(self, msg: MessageDecl, w):
        w(f"struct {msg.name} {{")
        for field in msg.fields:
            self._emit_field(field, w)
        w("};")

    def _emit_field(self, field: Field, w):
        if field.optional:
            w(f"    bool has_{field.name} = false;")
        cpp_type = self._cpp_type(field.type)
        w(f"    {cpp_type} {field.name};")

    def _cpp_int_type(self, name: str) -> str:
        mapping = {
            "i8": "int8_t",
            "i16": "int16_t",
            "i32": "int32_t",
            "i64": "int64_t",
        }
        return mapping[name]

    def _cpp_type(self, t) -> str:
        if isinstance(t, PrimitiveType):
            mapping = {
                "i8": "int8_t",
                "i16": "int16_t",
                "i32": "int32_t",
                "i64": "int64_t",
                "bool": "bool",
                "datetime_ns": "int64_t",
            }
            return mapping[t.name]
        if isinstance(t, StringType):
            return "std::string_view"
        if isinstance(t, ListType):
            element_cpp = self._cpp_type(t.element_type)
            return f"ListView<{element_cpp}>"
        if isinstance(t, ArrayType):
            element_cpp = self._cpp_type(t.element_type)
            return f"std::array<{element_cpp}, {t.length}>"
        if isinstance(t, ReferenceType):
            return t.name
        raise RuntimeError(f"Unknown type node: {t}")

    # -----------------------------
    # Encode / decode / size
    # -----------------------------

    def _emit_message_functions(self, msg: MessageDecl, w):
        name = msg.name
        w(f"std::size_t encoded_size(const {name}& message);")
        w(f"bool encode(const {name}& message, uint8_t* out_buffer, std::size_t out_size, std::size_t& bytes_written);")
        w(f"bool decode({name}& message, const uint8_t* data, std::size_t size, std::size_t& bytes_consumed);")
        w("")
        self._emit_encoded_size_impl(msg, w)
        w("")
        self._emit_encode_impl(msg, w)
        w("")
        self._emit_decode_impl(msg, w)

    def _emit_encoded_size_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline std::size_t encoded_size(const {name}& message) {{")
        w("    std::size_t total_bytes = 0;")
        for field in msg.fields:
            self._emit_size_for_field(field, w)
        w("    return total_bytes;")
        w("}")

    def _emit_size_for_list(self, t: ListType, expr: str, prefix: str, w,
                            indent: str = "    ", loop_var: str = "i"):
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        w(f"{indent}{prefix}total_bytes += sizeof(std::uint32_t);")

        element_type = t.element_type

        if isinstance(element_type, PrimitiveType):
            element_cpp = self._cpp_type(element_type)
            w(f"{indent}{prefix}total_bytes += sizeof({element_cpp}) * {expr}.size;")

        elif isinstance(element_type, ReferenceType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}total_bytes += encoded_size({expr}.data[{loop_var}]);")
            w(f"{indent}}}")

        elif isinstance(element_type, ListType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            self._emit_size_for_list(element_type, f"{expr}.data[{loop_var}]", "",
                                     w, inner_indent, next_var)
            w(f"{indent}}}")

        elif isinstance(element_type, StringType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}total_bytes += sizeof(std::uint32_t) + {expr}.data[{loop_var}].size();")
            w(f"{indent}}}")

        else:
            raise RuntimeError(f"Unsupported list element type in size: {element_type}")

    def _emit_size_for_field(self, field: Field, w):
        t = field.type
        prefix = ""
        if field.optional:
            w("    total_bytes += 1;")
            prefix = f"if (message.has_{field.name}) "

        if isinstance(t, PrimitiveType):
            w(f"    {prefix}total_bytes += sizeof({self._cpp_type(t)});")
        elif isinstance(t, StringType):
            w(f"    {prefix}total_bytes += sizeof(std::uint32_t) + message.{field.name}.size();")
        elif isinstance(t, ArrayType):
            element_cpp = self._cpp_type(t.element_type)
            w(f"    {prefix}total_bytes += sizeof({element_cpp}) * {t.length};")
        elif isinstance(t, ListType):
            self._emit_size_for_list(t, f"message.{field.name}", prefix, w)
        elif isinstance(t, ReferenceType):
            w(f"    {prefix}total_bytes += encoded_size(message.{field.name});")
        else:
            raise RuntimeError(f"Unknown field type in size: {t}")

    def _emit_encode_list(self, t: ListType, expr: str, field_suffix: str, w, need,
                          indent: str = "    ", loop_var: str = "i"):
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        def w_ind(line: str):
            w(indent + line)

        def need_ind(nbytes: str):
            w(f"{indent}if (bytes_remaining < {nbytes}) return false;")

        # write element count (u32 LE)
        need_ind("sizeof(std::uint32_t)")
        w_ind(f"std::uint32_t element_count_{field_suffix} = static_cast<std::uint32_t>({expr}.size);")
        w_ind(f"write_uint32_le(write_cursor, element_count_{field_suffix});")
        w_ind("write_cursor += sizeof(std::uint32_t);")
        w_ind("bytes_remaining -= sizeof(std::uint32_t);")

        element_type = t.element_type

        if isinstance(element_type, PrimitiveType):
            element_cpp = self._cpp_type(element_type)
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            if element_type.name == "i8":
                w(f"{inner_indent}if (bytes_remaining < 1) return false;")
                w(f"{inner_indent}*write_cursor = static_cast<std::uint8_t>({expr}.data[{loop_var}]);")
                w(f"{inner_indent}write_cursor += 1;")
                w(f"{inner_indent}bytes_remaining -= 1;")
            elif element_type.name == "i16":
                w(f"{inner_indent}if (bytes_remaining < 2) return false;")
                w(f"{inner_indent}write_uint16_le(write_cursor, static_cast<std::uint16_t>({expr}.data[{loop_var}]));")
                w(f"{inner_indent}write_cursor += 2;")
                w(f"{inner_indent}bytes_remaining -= 2;")
            elif element_type.name == "i32":
                w(f"{inner_indent}if (bytes_remaining < 4) return false;")
                w(f"{inner_indent}write_uint32_le(write_cursor, static_cast<std::uint32_t>({expr}.data[{loop_var}]));")
                w(f"{inner_indent}write_cursor += 4;")
                w(f"{inner_indent}bytes_remaining -= 4;")
            elif element_type.name in ("i64", "datetime_ns"):
                w(f"{inner_indent}if (bytes_remaining < 8) return false;")
                w(f"{inner_indent}write_uint64_le(write_cursor, static_cast<std::uint64_t>({expr}.data[{loop_var}]));")
                w(f"{inner_indent}write_cursor += 8;")
                w(f"{inner_indent}bytes_remaining -= 8;")
            elif element_type.name == "bool":
                w(f"{inner_indent}if (bytes_remaining < 1) return false;")
                w(f"{inner_indent}*write_cursor = {expr}.data[{loop_var}] ? 1 : 0;")
                w(f"{inner_indent}write_cursor += 1;")
                w(f"{inner_indent}bytes_remaining -= 1;")
            else:
                raise RuntimeError(f"Unsupported primitive in list encode: {element_type.name}")
            w_ind("}")

        elif isinstance(element_type, ReferenceType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}std::size_t element_bytes_written_{field_suffix} = 0;")
            w(f"{inner_indent}if (!encode({expr}.data[{loop_var}], write_cursor, bytes_remaining, element_bytes_written_{field_suffix})) return false;")
            w(f"{inner_indent}write_cursor += element_bytes_written_{field_suffix};")
            w(f"{inner_indent}bytes_remaining -= element_bytes_written_{field_suffix};")
            w_ind("}")

        elif isinstance(element_type, ListType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            self._emit_encode_list(element_type, f"{expr}.data[{loop_var}]", f"{field_suffix}_{loop_var}",
                                   w, need, inner_indent, next_var)
            w_ind("}")

        elif isinstance(element_type, StringType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}std::uint32_t string_length_{field_suffix}_{loop_var} = static_cast<std::uint32_t>({expr}.data[{loop_var}].size());")
            w(f"{inner_indent}if (bytes_remaining < sizeof(std::uint32_t) + {expr}.data[{loop_var}].size()) return false;")
            w(f"{inner_indent}write_uint32_le(write_cursor, string_length_{field_suffix}_{loop_var});")
            w(f"{inner_indent}write_cursor += sizeof(std::uint32_t);")
            w(f"{inner_indent}bytes_remaining -= sizeof(std::uint32_t);")
            w(f"{inner_indent}std::memcpy(write_cursor, {expr}.data[{loop_var}].data(), {expr}.data[{loop_var}].size());")
            w(f"{inner_indent}write_cursor += {expr}.data[{loop_var}].size();")
            w(f"{inner_indent}bytes_remaining -= {expr}.data[{loop_var}].size();")
            w_ind("}")

        else:
            raise RuntimeError(f"Unsupported list element type in encode: {element_type}")

    def _emit_decode_list(self, t: ListType, expr: str, field_suffix: str, w, need,
                          indent: str = "    ", loop_var: str = "i"):
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        def w_ind(line: str):
            w(indent + line)

        def need_ind(nbytes: str):
            w(f"{indent}if (bytes_remaining < {nbytes}) return false;")

        # read element count (u32 LE)
        need_ind("sizeof(std::uint32_t)")
        w_ind(f"std::uint32_t element_count_{field_suffix} = read_uint32_le(read_cursor);")
        w_ind("read_cursor += sizeof(std::uint32_t);")
        w_ind("bytes_remaining -= sizeof(std::uint32_t);")
        w_ind(f"{expr}.size = static_cast<std::size_t>(element_count_{field_suffix});")

        element_type = t.element_type

        if isinstance(element_type, PrimitiveType):
            element_cpp = self._cpp_type(element_type)
            w_ind(f"{expr}.data = nullptr;")
            w_ind("#if HOST_IS_LITTLE_ENDIAN")
            w_ind(f"std::size_t element_bytes_{field_suffix} = sizeof({element_cpp}) * {expr}.size;")
            w_ind(f"if (bytes_remaining < element_bytes_{field_suffix}) return false;")
            w_ind(f"{expr}.data = reinterpret_cast<{element_cpp}*>(read_cursor);")
            w_ind(f"read_cursor += element_bytes_{field_suffix};")
            w_ind(f"bytes_remaining -= element_bytes_{field_suffix};")
            w_ind("#else")
            w_ind(f"{expr}.data = new {element_cpp}[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            if element_type.name == "i8":
                w(f"{inner_indent}if (bytes_remaining < 1) return false;")
                w(f"{inner_indent}{expr}.data[{loop_var}] = static_cast<{element_cpp}>(*read_cursor);")
                w(f"{inner_indent}read_cursor += 1;")
                w(f"{inner_indent}bytes_remaining -= 1;")
            elif element_type.name == "i16":
                w(f"{inner_indent}if (bytes_remaining < 2) return false;")
                w(f"{inner_indent}{expr}.data[{loop_var}] = static_cast<{element_cpp}>(read_uint16_le(read_cursor));")
                w(f"{inner_indent}read_cursor += 2;")
                w(f"{inner_indent}bytes_remaining -= 2;")
            elif element_type.name == "i32":
                w(f"{inner_indent}if (bytes_remaining < 4) return false;")
                w(f"{inner_indent}{expr}.data[{loop_var}] = static_cast<{element_cpp}>(read_uint32_le(read_cursor));")
                w(f"{inner_indent}read_cursor += 4;")
                w(f"{inner_indent}bytes_remaining -= 4;")
            elif element_type.name in ("i64", "datetime_ns"):
                w(f"{inner_indent}if (bytes_remaining < 8) return false;")
                w(f"{inner_indent}{expr}.data[{loop_var}] = static_cast<{element_cpp}>(read_uint64_le(read_cursor));")
                w(f"{inner_indent}read_cursor += 8;")
                w(f"{inner_indent}bytes_remaining -= 8;")
            elif element_type.name == "bool":
                w(f"{inner_indent}if (bytes_remaining < 1) return false;")
                w(f"{inner_indent}{expr}.data[{loop_var}] = (*read_cursor != 0);")
                w(f"{inner_indent}read_cursor += 1;")
                w(f"{inner_indent}bytes_remaining -= 1;")
            else:
                raise RuntimeError(f"Unsupported primitive in list decode: {element_type.name}")
            w_ind("}")
            w_ind("#endif")

        elif isinstance(element_type, ReferenceType):
            element_cpp = element_type.name
            w_ind(f"{expr}.data = new {element_cpp}[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}{expr}.data[{loop_var}] = {element_cpp}{{}};")
            w(f"{inner_indent}std::size_t element_bytes_consumed_{field_suffix}_{loop_var} = 0;")
            w(f"{inner_indent}if (!decode({expr}.data[{loop_var}], read_cursor, bytes_remaining, element_bytes_consumed_{field_suffix}_{loop_var})) return false;")
            w(f"{inner_indent}read_cursor += element_bytes_consumed_{field_suffix}_{loop_var};")
            w(f"{inner_indent}bytes_remaining -= element_bytes_consumed_{field_suffix}_{loop_var};")
            w_ind("}")

        elif isinstance(element_type, ListType):
            inner_cpp = self._cpp_type(element_type)
            w_ind(f"{expr}.data = new {inner_cpp}[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}{expr}.data[{loop_var}] = {inner_cpp}{{}};")
            self._emit_decode_list(element_type, f"{expr}.data[{loop_var}]", f"{field_suffix}_{loop_var}",
                                   w, need, inner_indent, next_var)
            w_ind("}")

        elif isinstance(element_type, StringType):
            w_ind(f"{expr}.data = new std::string_view[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}if (bytes_remaining < sizeof(std::uint32_t)) return false;")
            w(f"{inner_indent}std::uint32_t string_length_{field_suffix}_{loop_var} = read_uint32_le(read_cursor);")
            w(f"{inner_indent}read_cursor += sizeof(std::uint32_t);")
            w(f"{inner_indent}bytes_remaining -= sizeof(std::uint32_t);")
            w(f"{inner_indent}if (bytes_remaining < static_cast<std::size_t>(string_length_{field_suffix}_{loop_var})) return false;")
            w(f"{inner_indent}{expr}.data[{loop_var}] = std::string_view(")
            w(f"{inner_indent}    reinterpret_cast<const char*>(read_cursor),")
            w(f"{inner_indent}    static_cast<std::size_t>(string_length_{field_suffix}_{loop_var}));")
            w(f"{inner_indent}read_cursor += string_length_{field_suffix}_{loop_var};")
            w(f"{inner_indent}bytes_remaining -= static_cast<std::size_t>(string_length_{field_suffix}_{loop_var});")
            w_ind("}")

        else:
            raise RuntimeError(f"Unsupported list element type in decode: {element_type}")

    def _emit_encode_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline bool encode(const {name}& message, uint8_t* out_buffer, std::size_t out_size, std::size_t& bytes_written) {{")
        w("    uint8_t* write_cursor = out_buffer;")
        w("    std::size_t bytes_remaining = out_size;")
        for field in msg.fields:
            self._emit_encode_field(field, w)
        w("    bytes_written = static_cast<std::size_t>(write_cursor - out_buffer);")
        w("    return true;")
        w("}")

    def _emit_encode_field(self, field: Field, w):
        t = field.type
        name = field.name

        def need(nbytes: str):
            w(f"    if (bytes_remaining < {nbytes}) return false;")

        if field.optional:
            need("1")
            w(f"    *write_cursor = message.has_{name} ? 1 : 0;")
            w("    write_cursor += 1;")
            w("    bytes_remaining -= 1;")
            w(f"    if (!message.has_{name}) {{")
            w("        goto skip_field_" + name + ";")
            w("    }")

        if isinstance(t, PrimitiveType):
            if t.name == "i8":
                need("1")
                w(f"    *write_cursor = static_cast<std::uint8_t>(message.{name});")
                w("    write_cursor += 1;")
                w("    bytes_remaining -= 1;")
            elif t.name == "i16":
                need("2")
                w(f"    write_uint16_le(write_cursor, static_cast<std::uint16_t>(message.{name}));")
                w("    write_cursor += 2;")
                w("    bytes_remaining -= 2;")
            elif t.name == "i32":
                need("4")
                w(f"    write_uint32_le(write_cursor, static_cast<std::uint32_t>(message.{name}));")
                w("    write_cursor += 4;")
                w("    bytes_remaining -= 4;")
            elif t.name in ("i64", "datetime_ns"):
                need("8")
                w(f"    write_uint64_le(write_cursor, static_cast<std::uint64_t>(message.{name}));")
                w("    write_cursor += 8;")
                w("    bytes_remaining -= 8;")
            elif t.name == "bool":
                need("1")
                w(f"    *write_cursor = message.{name} ? 1 : 0;")
                w("    write_cursor += 1;")
                w("    bytes_remaining -= 1;")
            else:
                raise RuntimeError(f"Unsupported primitive type in encode: {t.name}")

        elif isinstance(t, StringType):
            need("sizeof(std::uint32_t)")
            w(f"    std::uint32_t string_length_{name} = static_cast<std::uint32_t>(message.{name}.size());")
            w(f"    write_uint32_le(write_cursor, string_length_{name});")
            w("    write_cursor += sizeof(std::uint32_t);")
            w("    bytes_remaining -= sizeof(std::uint32_t);")
            w(f"    if (bytes_remaining < message.{name}.size()) return false;")
            w(f"    std::memcpy(write_cursor, message.{name}.data(), message.{name}.size());")
            w(f"    write_cursor += message.{name}.size();")
            w(f"    bytes_remaining -= message.{name}.size();")

        elif isinstance(t, ArrayType):
            element_cpp = self._cpp_type(t.element_type)
            w(f"    for (std::size_t i = 0; i < {t.length}; ++i) {{")
            if t.element_type.name == "i8":
                w("        if (bytes_remaining < 1) return false;")
                w(f"        *write_cursor = static_cast<std::uint8_t>(message.{name}[i]);")
                w("        write_cursor += 1;")
                w("        bytes_remaining -= 1;")
            elif t.element_type.name == "i16":
                w("        if (bytes_remaining < 2) return false;")
                w(f"        write_uint16_le(write_cursor, static_cast<std::uint16_t>(message.{name}[i]));")
                w("        write_cursor += 2;")
                w("        bytes_remaining -= 2;")
            elif t.element_type.name == "i32":
                w("        if (bytes_remaining < 4) return false;")
                w(f"        write_uint32_le(write_cursor, static_cast<std::uint32_t>(message.{name}[i]));")
                w("        write_cursor += 4;")
                w("        bytes_remaining -= 4;")
            elif t.element_type.name in ("i64", "datetime_ns"):
                w("        if (bytes_remaining < 8) return false;")
                w(f"        write_uint64_le(write_cursor, static_cast<std::uint64_t>(message.{name}[i]));")
                w("        write_cursor += 8;")
                w("        bytes_remaining -= 8;")
            elif t.element_type.name == "bool":
                w("        if (bytes_remaining < 1) return false;")
                w(f"        *write_cursor = message.{name}[i] ? 1 : 0;")
                w("        write_cursor += 1;")
                w("        bytes_remaining -= 1;")
            else:
                w(f"        static_assert(sizeof({element_cpp}) == 0, \"Unsupported array element type\");")
            w("    }")

        elif isinstance(t, ListType):
            self._emit_encode_list(t, f"message.{name}", name, w, need)

        elif isinstance(t, ReferenceType):
            w("    {")
            w("        std::size_t element_bytes_written = 0;")
            w(f"        if (!encode(message.{name}, write_cursor, bytes_remaining, element_bytes_written)) return false;")
            w("        write_cursor += element_bytes_written;")
            w("        bytes_remaining -= element_bytes_written;")
            w("    }")

        else:
            raise RuntimeError(f"Unknown field type in encode: {t}")

        if field.optional:
            w(f"skip_field_{name}: ;")

    def _emit_decode_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline bool decode({name}& message, const uint8_t* data, std::size_t size, std::size_t& bytes_consumed) {{")
        w("    uint8_t* read_cursor = const_cast<uint8_t*>(data);")
        w("    std::size_t bytes_remaining = size;")
        for field in msg.fields:
            self._emit_decode_field(field, w)
        w("    bytes_consumed = static_cast<std::size_t>(read_cursor - data);")
        w("    return true;")
        w("}")

    def _emit_decode_field(self, field: Field, w):
        t = field.type
        name = field.name

        def need(nbytes: str):
            w(f"    if (bytes_remaining < {nbytes}) return false;")

        if field.optional:
            need("1")
            w("    {")
            w("        std::uint8_t presence_flag = *read_cursor;")
            w("        read_cursor += 1;")
            w("        bytes_remaining -= 1;")
            w(f"        message.has_{name} = (presence_flag != 0);")
            w(f"        if (!message.has_{name}) goto skip_field_{name};")
            w("    }")

        if isinstance(t, PrimitiveType):
            cpp = self._cpp_type(t)
            if t.name == "i8":
                need("1")
                w(f"    message.{name} = static_cast<{cpp}>(*read_cursor);")
                w("    read_cursor += 1;")
                w("    bytes_remaining -= 1;")
            elif t.name == "i16":
                need("2")
                w(f"    message.{name} = static_cast<{cpp}>(read_uint16_le(read_cursor));")
                w("    read_cursor += 2;")
                w("    bytes_remaining -= 2;")
            elif t.name == "i32":
                need("4")
                w(f"    message.{name} = static_cast<{cpp}>(read_uint32_le(read_cursor));")
                w("    read_cursor += 4;")
                w("    bytes_remaining -= 4;")
            elif t.name in ("i64", "datetime_ns"):
                need("8")
                w(f"    message.{name} = static_cast<{cpp}>(read_uint64_le(read_cursor));")
                w("    read_cursor += 8;")
                w("    bytes_remaining -= 8;")
            elif t.name == "bool":
                need("1")
                w(f"    message.{name} = (*read_cursor != 0);")
                w("    read_cursor += 1;")
                w("    bytes_remaining -= 1;")
            else:
                raise RuntimeError(f"Unsupported primitive type in decode: {t.name}")

        elif isinstance(t, StringType):
            need("sizeof(std::uint32_t)")
            w(f"    std::uint32_t string_length_{name} = read_uint32_le(read_cursor);")
            w("    read_cursor += sizeof(std::uint32_t);")
            w("    bytes_remaining -= sizeof(std::uint32_t);")
            w(f"    if (bytes_remaining < static_cast<std::size_t>(string_length_{name})) return false;")
            w(f"    message.{name} = std::string_view(")
            w(f"        reinterpret_cast<const char*>(read_cursor),")
            w(f"        static_cast<std::size_t>(string_length_{name}));")
            w(f"    read_cursor += string_length_{name};")
            w(f"    bytes_remaining -= static_cast<std::size_t>(string_length_{name});")

        elif isinstance(t, ArrayType):
            element_cpp = self._cpp_type(t.element_type)
            w(f"    for (std::size_t i = 0; i < {t.length}; ++i) {{")
            if t.element_type.name == "i8":
                w("        if (bytes_remaining < 1) return false;")
                w(f"        message.{name}[i] = static_cast<{element_cpp}>(*read_cursor);")
                w("        read_cursor += 1;")
                w("        bytes_remaining -= 1;")
            elif t.element_type.name == "i16":
                w("        if (bytes_remaining < 2) return false;")
                w(f"        message.{name}[i] = static_cast<{element_cpp}>(read_uint16_le(read_cursor));")
                w("        read_cursor += 2;")
                w("        bytes_remaining -= 2;")
            elif t.element_type.name == "i32":
                w("        if (bytes_remaining < 4) return false;")
                w(f"        message.{name}[i] = static_cast<{element_cpp}>(read_uint32_le(read_cursor));")
                w("        read_cursor += 4;")
                w("        bytes_remaining -= 4;")
            elif t.element_type.name in ("i64", "datetime_ns"):
                w("        if (bytes_remaining < 8) return false;")
                w(f"        message.{name}[i] = static_cast<{element_cpp}>(read_uint64_le(read_cursor));")
                w("        read_cursor += 8;")
                w("        bytes_remaining -= 8;")
            elif t.element_type.name == "bool":
                w("        if (bytes_remaining < 1) return false;")
                w(f"        message.{name}[i] = (*read_cursor != 0);")
                w("        read_cursor += 1;")
                w("        bytes_remaining -= 1;")
            else:
                w(f"        static_assert(sizeof({element_cpp}) == 0, \"Unsupported array element type\");")
            w("    }")

        elif isinstance(t, ListType):
            self._emit_decode_list(t, f"message.{name}", name, w, need)

        elif isinstance(t, ReferenceType):
            w("    {")
            w("        std::size_t element_bytes_consumed = 0;")
            w(f"        if (!decode(message.{name}, read_cursor, bytes_remaining, element_bytes_consumed)) return false;")
            w("        read_cursor += element_bytes_consumed;")
            w("        bytes_remaining -= element_bytes_consumed;")
            w("    }")

        else:
            raise RuntimeError(f"Unknown field type in decode: {t}")

        if field.optional:
            w(f"skip_field_{name}: ;")

