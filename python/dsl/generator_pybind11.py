from __future__ import annotations

from dataclasses import dataclass
from typing import List, Set, Tuple

from .ast import (
    ArrayType,
    DslFile,
    Field,
    ListType,
    MessageDecl,
    PrimitiveType,
    ReferenceType,
    StringType,
)


@dataclass
class Pybind11Generator:
    """Generates a pybind11 C++ binding source file from a validated DSL AST.

    The generated bindings expose:
      - EncodedBuffer: a view over an encoded byte buffer
      - ListView<T> bindings for each list type used in the schema
      - Owning struct bindings (Xxx construction from Python kwargs for encode)
      - View struct bindings (XxxView returned from decode to Python)
      - encode_Xxx, decode_Xxx, encoded_size_Xxx functions per message

    Lifetime note for decode:
      XxxView fields may contain raw pointers into arena storage. To keep those
      pointers valid while Python holds the view, both the arena storage and the
      view are stored as static thread_local variables inside the decode lambda.
      This means callers must not call decode_Xxx again while still inspecting a
      previously returned view. This constraint is acceptable for the test harness.
    """

    namespace: str
    module_name: str = "dslgen"

    def emit(self, ast: DslFile) -> str:
        """Generate and return the complete pybind11 bindings source as a string."""
        lines: List[str] = []
        w = lines.append

        w('#include <pybind11/pybind11.h>')
        w('#include <pybind11/stl.h>')
        w('#include <vector>')
        w('#include "generated.hpp"')
        w('')
        w('namespace py = pybind11;')
        w(f'namespace ns = {self.namespace};')
        w('')

        w('struct EncodedBuffer {')
        w('    const std::uint8_t* data = nullptr;')
        w('    std::size_t size = 0;')
        w('};')
        w('')

        list_types: Set[Tuple[str, int]] = set()
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                for field in decl.fields:
                    self._collect_list_types(field.type, list_types)

        w(f'PYBIND11_MODULE({self.module_name}, m) {{')
        w('    m.doc() = "DSL-generated bindings";')
        w('')

        w('    py::class_<EncodedBuffer>(m, "EncodedBuffer", py::module_local())')
        w('        .def_readonly("data", &EncodedBuffer::data)')
        w('        .def_readonly("size", &EncodedBuffer::size)')
        w('        .def("__len__", [](const EncodedBuffer& b) { return b.size; })')
        w('        .def("__getitem__", [](const EncodedBuffer& b, std::size_t i) {')
        w('            if (i >= b.size) throw py::index_error();')
        w('            return static_cast<int>(b.data[i]);')
        w('        })')
        w('        .def("__iter__", [](const EncodedBuffer& b) {')
        w('            return py::make_iterator(b.data, b.data + b.size);')
        w('        }, py::keep_alive<0, 1>());')
        w('')

        for cpp_type, depth in sorted(list_types, key=lambda x: (x[1], x[0])):
            self._emit_listview_binding(cpp_type, depth, w)

        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_message_binding(decl, w)

        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_view_binding(decl, w)

        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_functions_binding(decl, w)

        w('}')
        w('')
        return "\n".join(lines)

    # ------------------------------------------------------------------
    # Type helpers
    # ------------------------------------------------------------------

    def _cpp_primitive(self, type_node: PrimitiveType) -> str:
        mapping = {
            "i8":          "int8_t",
            "i16":         "int16_t",
            "i32":         "int32_t",
            "i64":         "int64_t",
            "bool":        "bool",
            "datetime_ns": "int64_t",
        }
        return mapping[type_node.name]

    def _cpp_type_owning(self, type_node) -> str:
        """Return the C++ owning-side type string for a field type node."""
        if isinstance(type_node, PrimitiveType):
            return self._cpp_primitive(type_node)
        if isinstance(type_node, StringType):
            return "std::string_view"
        if isinstance(type_node, ReferenceType):
            return f"ns::{type_node.name}"
        if isinstance(type_node, ListType):
            inner = self._cpp_type_owning(type_node.element_type)
            return f"ns::ListView<{inner}>"
        if isinstance(type_node, ArrayType):
            inner = self._cpp_type_owning(type_node.element_type)
            return f"std::array<{inner}, {type_node.length}>"
        raise RuntimeError(f"Unsupported type in _cpp_type_owning: {type_node}")

    def _cpp_type_view(self, type_node) -> str:
        """Return the C++ view-side type string for a field type node."""
        if isinstance(type_node, PrimitiveType):
            return self._cpp_primitive(type_node)
        if isinstance(type_node, StringType):
            return "std::string_view"
        if isinstance(type_node, ReferenceType):
            return f"ns::{type_node.name}View"
        if isinstance(type_node, ListType):
            inner = self._cpp_type_view(type_node.element_type)
            return f"ns::ListView<{inner}>"
        if isinstance(type_node, ArrayType):
            inner = self._cpp_type_view(type_node.element_type)
            return f"std::array<{inner}, {type_node.length}>"
        raise RuntimeError(f"Unsupported type in _cpp_type_view: {type_node}")

    # ------------------------------------------------------------------
    # List type collection for ListView<T> binding generation
    # ------------------------------------------------------------------

    def _collect_list_types(self, type_node, acc: Set[Tuple[str, int]], depth: int = 1):
        """Record (base_cpp_view_type, depth) for every nesting level of a ListType.

        Ensures ListView<T> is registered before ListView<ListView<T>> in the
        pybind11 module, which is required for __getitem__ return types to be
        recognised correctly by pybind11.
        """
        if isinstance(type_node, ListType):
            elem = type_node.element_type
            if isinstance(elem, PrimitiveType):
                cpp = self._cpp_primitive(elem)
                for depth_level in range(1, depth + 1):
                    acc.add((cpp, depth_level))
            elif isinstance(elem, StringType):
                for depth_level in range(1, depth + 1):
                    acc.add(("std::string_view", depth_level))
            elif isinstance(elem, ReferenceType):
                for depth_level in range(1, depth + 1):
                    acc.add((f"ns::{elem.name}View", depth_level))
            elif isinstance(elem, ListType):
                self._collect_list_types(elem, acc, depth + 1)
            else:
                raise RuntimeError(f"Unsupported list element type in bindings: {elem}")
        elif isinstance(type_node, ArrayType):
            self._collect_list_types(type_node.element_type, acc, depth)

    # ------------------------------------------------------------------
    # ListView<T> binding emission
    # ------------------------------------------------------------------

    def _emit_listview_binding(self, cpp_type: str, depth: int, w):
        safe_name = cpp_type.replace("::", "_").replace(" ", "_").replace("<", "_").replace(">", "_")
        instance_type = ""
        py_name = ""
        for depth_level in range(1, depth + 1):
            if depth_level == 1:
                instance_type = f'ns::ListView<{cpp_type}>'
                py_name = f'ListView_{safe_name}'
            else:
                instance_type = f'ns::ListView<{instance_type}>'
                py_name = f'ListView_{py_name}'

        w(f'    py::class_<{instance_type}>(m, "{py_name}", py::module_local())')
        w(f'        .def_readwrite("data", &{instance_type}::data)')
        w(f'        .def_readwrite("size", &{instance_type}::size)')
        w(f'        .def("__len__", []({instance_type} const& v) {{ return v.size; }})')
        w(f'        .def("__getitem__", []({instance_type} const& v, std::size_t i) {{')
        w('            if (i >= v.size) throw py::index_error();')
        w('            return v.data[i];')
        w('        })')
        w(f'        .def("__iter__", []({instance_type} const& v) {{')
        w('            return py::make_iterator(v.begin(), v.end());')
        w('        }, py::keep_alive<0, 1>());')
        w('')

    # ------------------------------------------------------------------
    # Owning struct binding (for construction from Python kwargs and encode)
    # ------------------------------------------------------------------

    def _emit_message_binding(self, msg: MessageDecl, w):
        name = msg.name
        w(f'    py::class_<ns::{name}>(m, "{name}", py::module_local())')
        w('        .def(py::init([](py::kwargs kwargs) {')
        w(f'            ns::{name} obj{{}};')
        w('            for (auto& item : kwargs) {')
        w('                std::string key = py::str(item.first);')

        for field in msg.fields:
            self._emit_field_kw_assign(msg, field, w)

        w('                else {')
        w('                    throw py::type_error("Unknown field: " + key);')
        w('                }')
        w('            }')
        w('            return obj;')
        w('        }))')

        for field in msg.fields:
            w(f'        .def_readwrite("{field.name}", &ns::{name}::{field.name})')
            if field.optional:
                w(f'        .def_readwrite("has_{field.name}", &ns::{name}::has_{field.name})')

        w('        ;')
        w('')

    # ------------------------------------------------------------------
    # View struct binding (returned by decode to Python)
    # ------------------------------------------------------------------

    def _emit_view_binding(self, msg: MessageDecl, w):
        name = msg.name
        view_name = f"{name}View"
        w(f'    py::class_<ns::{view_name}>(m, "{view_name}", py::module_local())')
        for field in msg.fields:
            w(f'        .def_readwrite("{field.name}", &ns::{view_name}::{field.name})')
            if field.optional:
                w(f'        .def_readwrite("has_{field.name}", &ns::{view_name}::has_{field.name})')
        w('        ;')
        w('')

    # ------------------------------------------------------------------
    # Field keyword-argument assignment for owning struct construction
    # ------------------------------------------------------------------

    def _emit_field_kw_assign(self, msg: MessageDecl, field: Field, w):
        name = field.name
        w(f'                if (key == "{name}") {{')
        if isinstance(field.type, ListType):
            w('                    if (py::isinstance<py::list>(item.second)) {')
            self._emit_list_from_py(
                field.type,
                target_expr=f'obj.{name}',
                source_expr='item.second',
                w=w,
                indent=" " * 20,
            )
            w('                    } else {')
            w(f'                        obj.{name} = item.second.cast<decltype(obj.{name})>();')
            w('                    }')
        else:
            w(f'                    obj.{name} = item.second.cast<decltype(obj.{name})>();')
        if field.optional:
            w(f'                    obj.has_{name} = true;')
        w('                }')

    def _emit_list_from_py(self, list_type: ListType, target_expr: str,
                           source_expr: str, w, indent: str = " " * 20, prefix: str = ""):
        """Emit C++ code to populate a ListView<T> from a Python list.

        Uses heap allocation (new[]) intentionally — this runs in the pybind11
        test harness only, not in production encode or decode paths.
        """
        elem = list_type.element_type
        elem_cpp = self._cpp_type_owning(elem)

        list_name = f"lst_{prefix or '0'}"
        count_name = f"n_{prefix or '0'}"
        buf_name = f"buf_{prefix or '0'}"
        index_name = f"i_{prefix or '0'}"

        w(f'{indent}{{')
        w(f'{indent}    py::list {list_name} = {source_expr}.cast<py::list>();')
        w(f'{indent}    std::size_t {count_name} = {list_name}.size();')
        w(f'{indent}    auto* {buf_name} = new {elem_cpp}[{count_name}];')
        w(f'{indent}    for (std::size_t {index_name} = 0; {index_name} < {count_name}; ++{index_name}) {{')

        if isinstance(elem, ListType):
            self._emit_list_from_py(
                elem,
                target_expr=f'{buf_name}[{index_name}]',
                source_expr=f'{list_name}[{index_name}]',
                w=w,
                indent=indent + "        ",
                prefix=f"{prefix or '0'}_{index_name}",
            )
        else:
            w(f'{indent}        {buf_name}[{index_name}] = {list_name}[{index_name}].cast<{elem_cpp}>();')

        w(f'{indent}    }}')
        w(f'{indent}    {target_expr}.data = {buf_name};')
        w(f'{indent}    {target_expr}.size = {count_name};')
        w(f'{indent}}}')

    # ------------------------------------------------------------------
    # encode / decode / encoded_size function bindings
    # ------------------------------------------------------------------

    def _emit_functions_binding(self, msg: MessageDecl, w):
        name = msg.name

        w(f'    m.def("encoded_size_{name}", [](const ns::{name}& msg) {{')
        w(f'        return ns::encoded_size(msg);')
        w('    });')

        w(f'    m.def("encode_{name}", [](const ns::{name}& msg) {{')
        w('        std::size_t sz = ns::encoded_size(msg);')
        w('        auto* buf = new std::uint8_t[sz];')
        w('        std::size_t written = 0;')
        w('        std::size_t needed = 0;')
        w('        if (!ns::encode(msg, buf, sz, written, needed)) {')
        w('            delete[] buf;')
        w('            throw std::runtime_error("encode failed");')
        w('        }')
        w('        EncodedBuffer view;')
        w('        view.data = buf;')
        w('        view.size = written;')
        w('        return view;')
        w('    }, py::return_value_policy::move);')

        # Both the arena storage and the view are stored as static thread_local
        # so they outlive the lambda and remain valid while Python holds the view.
        # Callers must not call decode_{name} again while still using a prior result.
        arena_size = f'ns::max_decode_arena_bytes_{name}()'
        w(f'    m.def("decode_{name}", [](py::object obj) -> ns::{name}View& {{')
        w(f'        static thread_local std::vector<std::uint8_t> arena_storage;')
        w(f'        static thread_local ns::{name}View view;')
        w(f'        arena_storage.assign({arena_size}, std::uint8_t{{0}});')
        w(f'        view = ns::{name}View{{}};')
        w(f'        pubsub_itc_fw::BumpAllocator decode_arena(arena_storage.data(), arena_storage.size());')
        w('        std::size_t consumed = 0;')
        w('        if (py::isinstance<EncodedBuffer>(obj)) {')
        w('            auto buf = obj.cast<EncodedBuffer>();')
        w('            if (!ns::decode(view, buf.data, buf.size, consumed, decode_arena))')
        w('                throw std::runtime_error("decode failed");')
        w('            return view;')
        w('        }')
        w('        if (py::isinstance<py::bytes>(obj)) {')
        w('            std::string tmp = obj.cast<std::string>();')
        w('            if (!ns::decode(view, reinterpret_cast<const std::uint8_t*>(tmp.data()), tmp.size(), consumed, decode_arena))')
        w('                throw std::runtime_error("decode failed");')
        w('            return view;')
        w('        }')
        w('        throw py::type_error("decode expects EncodedBuffer or bytes");')
        w('    }, py::return_value_policy::reference);')
        w('')
