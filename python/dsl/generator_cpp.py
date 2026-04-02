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

# pylint: disable=too-many-arguments,too-many-positional-arguments

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

    def _emit_list_view(self, w):
        w("template<typename T>")
        w("struct ListView {")
        w("    T* data = nullptr;")
        w("    std::size_t size = 0;")
        w("")
        w("    const T* begin() const { return data; }")
        w("    const T* end() const { return data + size; }")
        w("    const T& operator[](std::size_t i) const { return data[i]; }")
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
        # to_string
        w(f"constexpr std::string_view to_string({enum.name} v) {{")
        w("    switch (v) {")
        for entry in enum.entries:
            w(f"    case {entry.name}: return std::string_view(\"{entry.name}\");")
        w("    default: return std::string_view(\"<unknown>\");")
        w("    }")
        w("}")
        w("")
        # validate
        w(f"constexpr bool validate({enum.name} v) {{")
        w("    switch (v) {")
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
        # optional: bool has_x; T x;
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
                "datetime_ns": "int64_t",  # wire is i64
            }
            return mapping[t.name]
        if isinstance(t, StringType):
            return "std::string_view"
        if isinstance(t, ListType):
            elem = self._cpp_type(t.element_type)
            return f"ListView<{elem}>"
        if isinstance(t, ArrayType):
            elem = self._cpp_type(t.element_type)
            return f"std::array<{elem}, {t.length}>"
        if isinstance(t, ReferenceType):
            return t.name
        raise RuntimeError(f"Unknown type node: {t}")

    # -----------------------------
    # Encode / decode / size
    # -----------------------------

    def _emit_message_functions(self, msg: MessageDecl, w):
        name = msg.name
        w(f"std::size_t encoded_size(const {name}& msg);")
        w(f"bool encode(const {name}& msg, uint8_t* out, std::size_t out_size, std::size_t& written);")
        w(f"bool decode({name}& msg, const uint8_t* data, std::size_t size, std::size_t& consumed);")
        w("")
        self._emit_encoded_size_impl(msg, w)
        w("")
        self._emit_encode_impl(msg, w)
        w("")
        self._emit_decode_impl(msg, w)

    def _emit_encoded_size_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline std::size_t encoded_size(const {name}& msg) {{")
        w("    std::size_t total = 0;")
        for field in msg.fields:
            self._emit_size_for_field(field, w)
        w("    return total;")
        w("}")

    def _emit_size_for_list(self, t: ListType, expr: str, prefix: str, w,
                            indent: str = "    ", loop_var: str = "i"):
        """
        Emit the size contribution of a list field or nested list element.

        'prefix'   — conditional guard applied only to the outermost statement,
                     e.g. "if (msg.has_foo) ". Empty string for nested calls.
        'indent'   — current indentation level (grows by 4 spaces per nesting).
        'loop_var' — loop variable name; uses successive letters to avoid
                     shadowing when loops are nested (i, j, k, ...).
        """
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        # count: 4 bytes for the element count
        w(f"{indent}{prefix}total += sizeof(int32_t);")

        elem = t.element_type

        if isinstance(elem, PrimitiveType):
            elem_cpp = self._cpp_type(elem)
            w(f"{indent}{prefix}total += sizeof({elem_cpp}) * {expr}.size;")

        elif isinstance(elem, ReferenceType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}total += encoded_size({expr}.data[{loop_var}]);")
            w(f"{indent}}}")

        elif isinstance(elem, ListType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            self._emit_size_for_list(elem, f"{expr}.data[{loop_var}]", "",
                                     w, inner_indent, next_var)
            w(f"{indent}}}")

        elif isinstance(elem, StringType):
            w(f"{indent}{prefix}for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}total += sizeof(int32_t) + {expr}.data[{loop_var}].size();")
            w(f"{indent}}}")

        else:
            raise RuntimeError(f"Unsupported list element type in size: {elem}")

    def _emit_size_for_field(self, field: Field, w):
        t = field.type
        prefix = ""
        if field.optional:
            # presence flag: always 1 byte
            w("    total += 1;")
            prefix = f"if (msg.has_{field.name}) "

        if isinstance(t, PrimitiveType):
            w(f"    {prefix}total += sizeof({self._cpp_type(t)});")
        elif isinstance(t, StringType):
            w(f"    {prefix}total += sizeof(int32_t) + msg.{field.name}.size();")
        elif isinstance(t, ArrayType):
            elem = self._cpp_type(t.element_type)
            w(f"    {prefix}total += sizeof({elem}) * {t.length};")
        elif isinstance(t, ListType):
            self._emit_size_for_list(t, f"msg.{field.name}", prefix, w)
        elif isinstance(t, ReferenceType):
            w(f"    {prefix}total += encoded_size(msg.{field.name});")
        else:
            raise RuntimeError(f"Unknown field type in size: {t}")

    def _emit_encode_list(self, t: ListType, expr: str, name: str, w, need,
                          indent: str = "    ", loop_var: str = "i"):
        """
        Emit encode logic for a list field or nested list element.

        'expr'     — C++ expression for the ListView<T> being encoded.
        'name'     — unique suffix used for local variable names.
        'need'     — callable that emits a bounds check for N bytes.
        'indent'   — current indentation level (grows by 4 spaces per nesting).
        'loop_var' — loop variable name; uses successive letters to avoid
                     shadowing when loops are nested (i, j, k, ...).
        """
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        def w_ind(line):
            w(indent + line)

        def need_ind(nbytes):
            w(f"{indent}if (remaining < {nbytes}) return false;")

        # write element count
        need_ind("sizeof(int32_t)")
        w_ind(f"int32_t count_{name} = static_cast<int32_t>({expr}.size);")
        w_ind(f"std::memcpy(ptr, &count_{name}, sizeof(int32_t));")
        w_ind("ptr += sizeof(int32_t);")
        w_ind("remaining -= sizeof(int32_t);")

        elem = t.element_type

        if isinstance(elem, PrimitiveType):
            elem_cpp = self._cpp_type(elem)
            w_ind(f"std::size_t bytes_{name} = sizeof({elem_cpp}) * {expr}.size;")
            need_ind(f"bytes_{name}")
            w_ind(f"std::memcpy(ptr, {expr}.data, bytes_{name});")
            w_ind(f"ptr += bytes_{name};")
            w_ind(f"remaining -= bytes_{name};")

        elif isinstance(elem, ReferenceType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}std::size_t written_elem_{name} = 0;")
            w(f"{inner_indent}if (!encode({expr}.data[{loop_var}], ptr, remaining, written_elem_{name})) return false;")
            w(f"{inner_indent}ptr += written_elem_{name};")
            w(f"{inner_indent}remaining -= written_elem_{name};")
            w_ind("}")

        elif isinstance(elem, ListType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            self._emit_encode_list(elem, f"{expr}.data[{loop_var}]", f"{name}_{loop_var}",
                                   w, need, inner_indent, next_var)
            w_ind("}")

        elif isinstance(elem, StringType):
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}int32_t slen_{name}_{loop_var} = static_cast<int32_t>({expr}.data[{loop_var}].size());")
            w(f"{inner_indent}if (remaining < sizeof(int32_t) + {expr}.data[{loop_var}].size()) return false;")
            w(f"{inner_indent}std::memcpy(ptr, &slen_{name}_{loop_var}, sizeof(int32_t));")
            w(f"{inner_indent}ptr += sizeof(int32_t);")
            w(f"{inner_indent}remaining -= sizeof(int32_t);")
            w(f"{inner_indent}std::memcpy(ptr, {expr}.data[{loop_var}].data(), {expr}.data[{loop_var}].size());")
            w(f"{inner_indent}ptr += {expr}.data[{loop_var}].size();")
            w(f"{inner_indent}remaining -= {expr}.data[{loop_var}].size();")
            w_ind("}")

        else:
            raise RuntimeError(f"Unsupported list element type in encode: {elem}")

    def _emit_decode_list(self, t: ListType, expr: str, name: str, w, need,
                          indent: str = "    ", loop_var: str = "i"):
        """
        Emit decode logic for a list field or nested list element.

        'expr'     — C++ expression for the ListView<T> being populated.
        'name'     — unique suffix used for local variable names.
        'need'     — callable that emits a bounds check (unused at nested levels;
                     bounds checks are emitted directly at the correct indent).
        'indent'   — current indentation level (grows by 4 spaces per nesting).
        'loop_var' — loop variable name; uses successive letters to avoid
                     shadowing when loops are nested (i, j, k, ...).

        MEMORY MODEL
        ------------
        Primitive lists: zero-copy. data points directly into the wire buffer.
        Message lists:   heap-allocated via new[]. Each element is decoded in-place.
        Nested lists:    heap-allocated via new[]. Each inner ListView is decoded
                         recursively.
        String lists:    string_view pointing into the wire buffer; array of
                         string_views is heap-allocated.

        The caller is responsible for ensuring the wire buffer remains alive
        for the lifetime of any zero-copy views.
        """
        next_var = chr(ord(loop_var) + 1) if loop_var < "z" else loop_var + "_"
        inner_indent = indent + "    "

        def w_ind(line):
            w(indent + line)

        def need_ind(nbytes):
            w(f"{indent}if (remaining < {nbytes}) return false;")

        # read element count
        need_ind("sizeof(int32_t)")
        w_ind(f"int32_t count_{name} = 0;")
        w_ind(f"std::memcpy(&count_{name}, ptr, sizeof(int32_t));")
        w_ind("ptr += sizeof(int32_t);")
        w_ind("remaining -= sizeof(int32_t);")
        w_ind(f"if (count_{name} < 0) return false;")
        w_ind(f"{expr}.size = static_cast<std::size_t>(count_{name});")

        elem = t.element_type

        if isinstance(elem, PrimitiveType):
            # Zero-copy: point data directly into the wire buffer.
            elem_cpp = self._cpp_type(elem)
            w_ind(f"std::size_t bytes_{name} = sizeof({elem_cpp}) * {expr}.size;")
            need_ind(f"bytes_{name}")
            w_ind(f"{expr}.data = reinterpret_cast<{elem_cpp}*>(ptr);")
            w_ind(f"ptr += bytes_{name};")
            w_ind(f"remaining -= bytes_{name};")

        elif isinstance(elem, ReferenceType):
            # Heap-allocate decoded structs. Wire buffer contains encoded bytes,
            # not raw structs, so we cannot point into it directly.
            elem_cpp = elem.name
            w_ind(f"{expr}.data = new {elem_cpp}[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}{expr}.data[{loop_var}] = {elem_cpp}{{}};")
            w(f"{inner_indent}std::size_t consumed_{name}_{loop_var} = 0;")
            w(f"{inner_indent}if (!decode({expr}.data[{loop_var}], ptr, remaining, consumed_{name}_{loop_var})) return false;")
            w(f"{inner_indent}ptr += consumed_{name}_{loop_var};")
            w(f"{inner_indent}remaining -= consumed_{name}_{loop_var};")
            w_ind("}")

        elif isinstance(elem, ListType):
            # Each element is itself a ListView — heap-allocate the array of
            # ListViews and recursively decode each one.
            inner_cpp = self._cpp_type(elem)
            w_ind(f"{expr}.data = new {inner_cpp}[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}{expr}.data[{loop_var}] = {inner_cpp}{{}};")
            self._emit_decode_list(elem, f"{expr}.data[{loop_var}]", f"{name}_{loop_var}",
                                   w, need, inner_indent, next_var)
            w_ind("}")

        elif isinstance(elem, StringType):
            # Each string_view points into the wire buffer zero-copy;
            # the array of string_views itself is heap-allocated.
            w_ind(f"{expr}.data = new std::string_view[{expr}.size];")
            w_ind(f"for (std::size_t {loop_var} = 0; {loop_var} < {expr}.size; ++{loop_var}) {{")
            w(f"{inner_indent}int32_t slen_{name}_{loop_var} = 0;")
            w(f"{inner_indent}if (remaining < sizeof(int32_t)) return false;")
            w(f"{inner_indent}std::memcpy(&slen_{name}_{loop_var}, ptr, sizeof(int32_t));")
            w(f"{inner_indent}ptr += sizeof(int32_t);")
            w(f"{inner_indent}remaining -= sizeof(int32_t);")
            w(f"{inner_indent}if (slen_{name}_{loop_var} < 0 || remaining < static_cast<std::size_t>(slen_{name}_{loop_var})) return false;")
            w(f"{inner_indent}{expr}.data[{loop_var}] = std::string_view(reinterpret_cast<const char*>(ptr), static_cast<std::size_t>(slen_{name}_{loop_var}));")
            w(f"{inner_indent}ptr += slen_{name}_{loop_var};")
            w(f"{inner_indent}remaining -= static_cast<std::size_t>(slen_{name}_{loop_var});")
            w_ind("}")

        else:
            raise RuntimeError(f"Unsupported list element type in decode: {elem}")

    def _emit_encode_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline bool encode(const {name}& msg, uint8_t* out, std::size_t out_size, std::size_t& written) {{")
        w("    uint8_t* ptr = out;")
        w("    std::size_t remaining = out_size;")
        for field in msg.fields:
            self._emit_encode_field(field, w)
        w("    written = static_cast<std::size_t>(ptr - out);")
        w("    return true;")
        w("}")

    def _emit_encode_field(self, field: Field, w):
        t = field.type
        name = field.name

        def need(nbytes: str):
            w(f"    if (remaining < {nbytes}) return false;")

        # optional presence flag
        if field.optional:
            need("1")
            w(f"    *ptr = msg.has_{name} ? 1 : 0;")
            w("    ptr += 1;")
            w("    remaining -= 1;")
            w(f"    if (!msg.has_{name}) {{")
            w("        goto skip_field_" + name + ";")
            w("    }")

        if isinstance(t, PrimitiveType):
            sz = f"sizeof({self._cpp_type(t)})"
            need(sz)
            w(f"    std::memcpy(ptr, &msg.{name}, {sz});")
            w(f"    ptr += {sz};")
            w(f"    remaining -= {sz};")
        elif isinstance(t, StringType):
            need("sizeof(int32_t)")
            w(f"    int32_t len_{name} = static_cast<int32_t>(msg.{name}.size());")
            w(f"    std::memcpy(ptr, &len_{name}, sizeof(int32_t));")
            w("    ptr += sizeof(int32_t);")
            w("    remaining -= sizeof(int32_t);")
            w(f"    if (remaining < msg.{name}.size()) return false;")
            w(f"    std::memcpy(ptr, msg.{name}.data(), msg.{name}.size());")
            w(f"    ptr += msg.{name}.size();")
            w(f"    remaining -= msg.{name}.size();")
        elif isinstance(t, ArrayType):
            elem = self._cpp_type(t.element_type)
            sz = f"sizeof({elem}) * {t.length}"
            need(sz)
            w(f"    std::memcpy(ptr, msg.{name}.data(), {sz});")
            w(f"    ptr += {sz};")
            w(f"    remaining -= {sz};")
        elif isinstance(t, ListType):
            self._emit_encode_list(t, f"msg.{name}", name, w, need)
        elif isinstance(t, ReferenceType):
            w("    {")
            w("        std::size_t written_elem = 0;")
            w(f"        if (!encode(msg.{name}, ptr, remaining, written_elem)) return false;")
            w("        ptr += written_elem;")
            w("        remaining -= written_elem;")
            w("    }")
        else:
            raise RuntimeError(f"Unknown field type in encode: {t}")

        if field.optional:
            w(f"skip_field_{name}: ;")

    def _emit_decode_impl(self, msg: MessageDecl, w):
        name = msg.name
        w(f"inline bool decode({name}& msg, const uint8_t* data, std::size_t size, std::size_t& consumed) {{")
        w("    uint8_t* ptr = const_cast<uint8_t*>(data);")
        w("    std::size_t remaining = size;")
        for field in msg.fields:
            self._emit_decode_field(field, w)
        w("    consumed = static_cast<std::size_t>(ptr - data);")
        w("    return true;")
        w("}")

    def _emit_decode_field(self, field: Field, w):
        t = field.type
        name = field.name

        def need(nbytes: str):
            w(f"    if (remaining < {nbytes}) return false;")

        # optional presence flag
        if field.optional:
            need("1")
            w("    {")
            w("        uint8_t flag = *ptr;")
            w("        ptr += 1;")
            w("        remaining -= 1;")
            w(f"        msg.has_{name} = (flag != 0);")
            w(f"        if (!msg.has_{name}) goto skip_field_{name};")
            w("    }")

        if isinstance(t, PrimitiveType):
            sz = f"sizeof({self._cpp_type(t)})"
            need(sz)
            w(f"    std::memcpy(&msg.{name}, ptr, {sz});")
            w(f"    ptr += {sz};")
            w(f"    remaining -= {sz};")
        elif isinstance(t, StringType):
            need("sizeof(int32_t)")
            w(f"    int32_t len_{name} = 0;")
            w(f"    std::memcpy(&len_{name}, ptr, sizeof(int32_t));")
            w("    ptr += sizeof(int32_t);")
            w("    remaining -= sizeof(int32_t);")
            w(f"    if (len_{name} < 0 || remaining < static_cast<std::size_t>(len_{name})) return false;")
            w(f"    msg.{name} = std::string_view(reinterpret_cast<const char*>(ptr), static_cast<std::size_t>(len_{name}));")
            w(f"    ptr += len_{name};")
            w(f"    remaining -= static_cast<std::size_t>(len_{name});")
        elif isinstance(t, ArrayType):
            elem = self._cpp_type(t.element_type)
            sz = f"sizeof({elem}) * {t.length}"
            need(sz)
            w(f"    std::memcpy(msg.{name}.data(), ptr, {sz});")
            w(f"    ptr += {sz};")
            w(f"    remaining -= {sz};")
        elif isinstance(t, ListType):
            self._emit_decode_list(t, f"msg.{name}", name, w, need)
        elif isinstance(t, ReferenceType):
            w("    {")
            w("        std::size_t consumed_elem = 0;")
            w(f"        if (!decode(msg.{name}, ptr, remaining, consumed_elem)) return false;")
            w("        ptr += consumed_elem;")
            w("        remaining -= consumed_elem;")
            w("    }")
        else:
            raise RuntimeError(f"Unknown field type in decode: {t}")

        if field.optional:
            w(f"skip_field_{name}: ;")
