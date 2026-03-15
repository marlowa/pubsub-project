from __future__ import annotations
from dataclasses import dataclass
from typing import List, Set, Tuple

from .ast import (
    DslFile,
    MessageDecl,
    Field,
    PrimitiveType,
    StringType,
    ListType,
    ArrayType,
    ReferenceType,
)

@dataclass
class Pybind11Generator:
    namespace: str  # same as CppGenerator namespace
    module_name: str = "dslgen"

    def emit(self, ast: DslFile) -> str:
        lines: List[str] = []
        w = lines.append

        w('#include <pybind11/pybind11.h>')
        w('#include <pybind11/stl.h>')
        w('#include "generated.hpp"')
        w('')
        w('namespace py = pybind11;')
        w(f'namespace ns = {self.namespace};')
        w('')

        # EncodedBuffer view type
        w('struct EncodedBuffer {')
        w('    const std::uint8_t* data = nullptr;')
        w('    std::size_t size = 0;')
        w('};')
        w('')

        # Collect all list types we need bindings for
        list_types: Set[Tuple[str, int]] = set()
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                for field in decl.fields:
                    self._collect_list_types(field.type, list_types)

        w(f'PYBIND11_MODULE({self.module_name}, m) {{')
        w('    m.doc() = "DSL-generated bindings";')
        w('')

        # Bind EncodedBuffer
        w('    py::class_<EncodedBuffer>(m, "EncodedBuffer")')
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

        # Bind ListView<T> instantiations
        for cpp_type, depth in sorted(list_types):
            self._emit_listview_binding(cpp_type, depth, w)

        # Bind all messages
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_message_binding(decl, w)

        # Bind encode/decode/encoded_size for each message
        for decl in ast.declarations:
            if isinstance(decl, MessageDecl):
                self._emit_functions_binding(decl, w)

        w('}')
        w('')
        return "\n".join(lines)

    # ---------- helpers ----------

    def _cpp_type(self, t) -> str:
        # Mirror CppGenerator’s notion of types, but as strings
        if isinstance(t, PrimitiveType):
            return self._cpp_primitive(t)
        if isinstance(t, ReferenceType):
            return f"{self.namespace}::{t.name}"
        if isinstance(t, ListType):
            # Only needed if you ever want elem_cpp for nested lists;
            # for the current kw-assign we only call this on element_type.
            inner = self._cpp_type(t.element_type)
            return f"ns::ListView<{inner}>"
        if isinstance(t, ArrayType):
            inner = self._cpp_type(t.element_type)
            return f"std::array<{inner}, {t.size}>"
        raise RuntimeError(f"Unsupported type in _cpp_type: {t}")

    def _collect_list_types(self, t, acc: Set[Tuple[str, int]], depth: int = 1):
        if isinstance(t, ListType):
            elem = t.element_type
            if isinstance(elem, PrimitiveType):
                cpp = self._cpp_primitive(elem)
                acc.add((cpp, depth))
            elif isinstance(elem, ReferenceType):
                acc.add((f"{self.namespace}::{elem.name}", depth))
            elif isinstance(elem, ListType):
                # nested list: recurse, increasing depth
                self._collect_list_types(elem, acc, depth + 1)
            else:
                raise RuntimeError(f"Unsupported list element type in bindings: {elem}")
        elif isinstance(t, ArrayType):
            self._collect_list_types(t.element_type, acc, depth)
        # other types: nothing to do

    def _cpp_primitive(self, t: PrimitiveType) -> str:
        mapping = {
            "i8": "std::int8_t",
            "i16": "std::int16_t",
            "i32": "std::int32_t",
            "i64": "std::int64_t",
            "bool": "bool",
            "datetime_ns": "std::int64_t",
        }
        return mapping[t.name]

    def _emit_listview_binding(self, cpp_type: str, depth: int, w):
        # depth 1: ListView<T>
        # depth 2: ListView<ListView<T>>
        # etc.
        name = cpp_type.replace("::", "_").replace(" ", "_")
        for d in range(1, depth + 1):
            if d == 1:
                inst = f'ns::ListView<{cpp_type}>'
                pyname = f'ListView_{name}'
            else:
                inst = f'ns::ListView<{inst}>'
                pyname = f'ListView_{pyname}'

        w(f'    py::class_<{inst}>(m, "{pyname}")')
        w(f'        .def_readwrite("data", &{inst}::data)')
        w(f'        .def_readwrite("size", &{inst}::size)')
        w(f'        .def("__len__", []({inst} const& v) {{ return v.size; }})')
        w(f'        .def("__getitem__", []({inst} const& v, std::size_t i) {{')
        w('            if (i >= v.size) throw py::index_error();')
        w('            return v.data[i];')
        w('        })')
        w(f'        .def("__iter__", []({inst} const& v) {{')
        w('            return py::make_iterator(v.begin(), v.end());')
        w('        }, py::keep_alive<0, 1>());')
        w('')

    def _emit_message_binding(self, msg: MessageDecl, w):
        name = msg.name
        w(f'    py::class_<ns::{name}>(m, "{name}")')
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

    def _emit_field_kw_assign(self, msg: MessageDecl, field: Field, w):
        name = field.name
        w(f'                if (key == "{name}") {{')
        # Special case: list<T>
        if isinstance(field.type, ListType):
            elem_cpp = self._cpp_type(field.type.element_type)

            w('                    if (py::isinstance<py::list>(item.second)) {')
            w('                        py::list lst = item.second.cast<py::list>();')
            w('                        std::size_t n = lst.size();')
            w(f'                        auto* buf = new {elem_cpp}[n];')
            w('                        for (std::size_t i = 0; i < n; ++i)')
            w(f'                            buf[i] = lst[i].cast<{elem_cpp}>();')
            w(f'                        obj.{name}.data = buf;')
            w(f'                        obj.{name}.size = n;')
            w('                    } else {')
            w(f'                        obj.{name} = item.second.cast<decltype(obj.{name})>();')
            w('                    }')
        else:
            # Normal field
            w(f'                    obj.{name} = item.second.cast<decltype(obj.{name})>();')
        if field.optional:
            w(f'                    obj.has_{name} = true;')
        w('                }')

    def _emit_functions_binding(self, msg: MessageDecl, w):
        name = msg.name

        # encoded_size
        w(f'    m.def("encoded_size_{name}", [](const ns::{name}& msg) {{')
        w(f'        return ns::encoded_size(msg);')
        w('    });')

        # encode -> EncodedBuffer
        w(f'    m.def("encode_{name}", [](const ns::{name}& msg) {{')
        w('        std::size_t sz = ns::encoded_size(msg);')
        w('        auto* buf = new std::uint8_t[sz];')
        w('        std::size_t written = 0;')
        w('        if (!ns::encode(msg, buf, sz, written)) {')
        w('            delete[] buf;')
        w('            throw std::runtime_error("encode failed");')
        w('        }')
        w('        EncodedBuffer view;')
        w('        view.data = buf;')
        w('        view.size = written;')
        w('        return view;')
        w('    }, py::return_value_policy::move);')

        # decode from EncodedBuffer or bytes
        w(f'    m.def("decode_{name}", [](py::object obj) {{')
        w(f'        ns::{name} msg{{}};')
        w('        std::size_t consumed = 0;')
        w('        if (py::isinstance<EncodedBuffer>(obj)) {')
        w('            auto buf = obj.cast<EncodedBuffer>();')
        w('            if (!ns::decode(msg, buf.data, buf.size, consumed))')
        w('                throw std::runtime_error("decode failed");')
        w('            return msg;')
        w('        }')
        w('        if (py::isinstance<py::bytes>(obj)) {')
        w('            std::string tmp = obj.cast<std::string>();')
        w('            if (!ns::decode(msg, reinterpret_cast<const std::uint8_t*>(tmp.data()), tmp.size(), consumed))')
        w('                throw std::runtime_error("decode failed");')
        w('            return msg;')
        w('        }')
        w('        throw py::type_error("decode expects EncodedBuffer or bytes");')
        w('    });')
        w('')
