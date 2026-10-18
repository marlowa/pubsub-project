"""Checks the Python and Java code generators against the C++ one, byte for byte.

The C++ generator's code is the reference: it is what the venue runs. For a set of values of every message in
tests/resources/every_type.dsl, this module writes a C++ program and a Java program that build those values with
the code each generator wrote, encode them, and print the bytes; and builds the same values with the Python code.
The Java program also decodes bytes it is given and encodes them again, and prints the result.

The tests in test_generator_python.py compare what comes out. Comparing each language's encoding of the same
values with the C++ encoding catches a fault that appears in both directions -- a reversed byte order, for
instance -- which a round trip within one language would not.

The programs are built under the project's build tree, as tests/utils.py builds its modules, never in the system
temporary directory.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Dict, List, Tuple

from dsl.ast import ArrayType, BytesType, EnumDecl, ListType, MessageDecl, PrimitiveType, ReferenceType, StringType
from dsl.generator_cpp import CppGenerator
from dsl.generator_java import JavaGenerator
from dsl.generator_python import PythonGenerator, python_name
from dsl.loader import load
from dsl.validator import Validator
from tests.utils import _CPPTRACE_PREFIX, _MODULE_BUILD_ROOT, _PUBSUB_ITC_FW_INCLUDE_DIR

EVERY_TYPE_DSL = Path(__file__).resolve().parent / "resources" / "every_type.dsl"
_FRAMEWORK_SOURCES = str(Path(__file__).resolve().parent.parent.parent / "libraries" / "pubsub_itc_fw" / "src")
CPP_NAMESPACE = "every"
JAVA_CLASS = "EveryType"

I8_MIN, I8_MAX = -(2 ** 7), 2 ** 7 - 1
I16_MIN, I16_MAX = -(2 ** 15), 2 ** 15 - 1
I32_MIN, I32_MAX = -(2 ** 31), 2 ** 31 - 1
I64_MIN, I64_MAX = -(2 ** 63), 2 ** 63 - 1

INNER_ONE = {"number": 1, "label": "x"}
INNER_TWO = {"number": -2, "label": ""}

# (case name, message, values). An optional field left out, or given as None, is absent. Enum fields are given
# as their integer values. A case whose name contains "unlisted" holds enum values the enums do not define.
CASES: List[Tuple[str, str, dict]] = [
    ("integers_smallest", "Integers", {"tiny": I8_MIN, "short_number": I16_MIN, "number": I32_MIN, "wide": I64_MIN,
                                       "when": 0, "flag": False, "letter": "\0"}),
    ("integers_largest", "Integers", {"tiny": I8_MAX, "short_number": I16_MAX, "number": I32_MAX, "wide": I64_MAX,
                                      "when": 1_700_000_000_123_456_789, "flag": True, "letter": "Z"}),
    ("text_empty", "Text", {"name": "", "blob": b""}),
    ("text_full", "Text", {"name": "café ✓ tick", "blob": bytes(range(256))}),
    ("enums_lowest", "Enums", {"small": I8_MIN, "medium": I16_MIN, "large": I32_MIN, "huge": I64_MIN, "letter": ord("1")}),
    ("enums_highest", "Enums", {"small": I8_MAX, "medium": I16_MAX, "large": I32_MAX, "huge": I64_MAX, "letter": ord("2")}),
    ("enums_unlisted", "Enums", {"small": 5, "medium": 7, "large": 8, "huge": 9, "letter": ord("x")}),
    ("lists_empty", "Lists", {"numbers": [], "names": [], "blobs": [], "inners": [], "grid": [], "words": []}),
    ("lists_full", "Lists", {"numbers": [1, -1, I32_MAX], "names": ["", "a", "ü"], "blobs": [b"", b"\x00\xff"],
                             "inners": [INNER_ONE, INNER_TWO],
                             "grid": [[1, 2], [], [I16_MIN]], "words": [["a"], [], ["b", "c"]]}),
    ("arrays", "Arrays", {"small_array": [I8_MIN, 0, 1, I8_MAX], "pair": [I16_MIN, I16_MAX], "numbers": [1, 2, 3],
                          "wide": [I64_MIN, I64_MAX]}),
    ("nested_absent", "Nested", {"inner": {"number": 5, "label": "in"}, "maybe_inner": None, "after": 9}),
    ("nested_present", "Nested", {"inner": {"number": 5, "label": "in"}, "maybe_inner": {"number": -1, "label": "maybe"},
                                  "after": -9}),
    ("optionals_absent", "Optionals", {"required": 1}),
    ("optionals_present", "Optionals", {"required": 2, "tiny": -1, "wide": 2 ** 40, "flag": True, "name": "n",
                                        "blob": b"\x01", "small": I8_MAX, "numbers": [4, 5]}),
    ("optionals_present_but_empty", "Optionals", {"required": 3, "tiny": 0, "wide": 0, "flag": False, "name": "",
                                                  "blob": b"", "small": 0, "numbers": []}),
]


# (case name, pdu id, version, sequence number, payload) for checking the PDU header against the framework's own
# PduFramer. The framer refuses an empty payload or one over 256 bytes.
HEADER_CASES: List[Tuple[str, int, int, int, bytes]] = [
    ("header_small", 1, 1, 0, b"\x01"),
    ("header_negative", -2, -1, -5, bytes(10)),
    ("header_largest", I16_MAX, I8_MAX, I64_MAX, bytes(range(256))),
    ("header_smallest", I16_MIN, I8_MIN, I64_MIN, b"ab"),
]


def load_every_type():
    """The validated AST of every_type.dsl."""
    ast = load(EVERY_TYPE_DSL)
    Validator(ast).validate()
    return ast


def _declarations(ast):
    messages = {d.name: d for d in ast.declarations if isinstance(d, MessageDecl)}
    enums = {d.name: d for d in ast.declarations if isinstance(d, EnumDecl)}
    return messages, enums


def _scratch_dir(prefix: str) -> Path:
    _MODULE_BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=prefix, dir=_MODULE_BUILD_ROOT))


# ----------------------------------------------------------------------------------------------------------------
# Python
# ----------------------------------------------------------------------------------------------------------------

def load_python_module(ast, source_name: str = "every_type.dsl"):
    """Generate the Python module for ast and import it."""
    directory = _scratch_dir("pygen_")
    path = directory / "every_type_generated.py"
    path.write_text(PythonGenerator(source_name=source_name).emit(ast))
    name = f"pygen_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # a dataclass looks its module up by name while it is being defined
    spec.loader.exec_module(module)
    shutil.rmtree(directory, ignore_errors=True)
    return module


def build_python_message(module, messages, message_name: str, values: dict):
    """An instance of the generated class for message_name holding values."""
    decl = messages[message_name]
    kwargs = {}
    for fld in decl.fields:
        value = values.get(fld.name)
        if value is None:
            if fld.optional:
                continue
            raise KeyError(f"{message_name}.{fld.name} has no value in the case")
        kwargs[python_name(fld.name)] = _python_value(module, messages, fld.type, value)
    return getattr(module, message_name)(**kwargs)


def _python_value(module, messages, type_node, value):
    if isinstance(type_node, (ListType, ArrayType)):
        return [_python_value(module, messages, type_node.element_type, item) for item in value]
    if isinstance(type_node, ReferenceType) and type_node.name in messages:
        return build_python_message(module, messages, type_node.name, value)
    return value


# ----------------------------------------------------------------------------------------------------------------
# C++
# ----------------------------------------------------------------------------------------------------------------

_CPP_INTEGER_TYPES = {"i8": "int8_t", "i16": "int16_t", "i32": "int32_t", "i64": "int64_t", "datetime_ns": "int64_t"}


def _cpp_integer(value: int) -> str:
    # The smallest int64_t cannot be written as a literal: its magnitude does not fit.
    if value == I64_MIN:
        return "(-9223372036854775807LL - 1)"
    return f"{value}LL"


class _CppWriter:
    """Writes the statements that build each case's message from its values."""

    def __init__(self, messages, enums):
        self.messages = messages
        self.enums = enums
        self.lines: List[str] = []
        self.counter = 0

    def fresh(self, stem: str) -> str:
        self.counter += 1
        return f"{stem}_{self.counter}"

    def element_type(self, type_node) -> str:
        if isinstance(type_node, PrimitiveType):
            return {"bool": "bool", "char": "char"}.get(type_node.name) or _CPP_INTEGER_TYPES[type_node.name]
        if isinstance(type_node, StringType):
            return "std::string_view"
        if isinstance(type_node, BytesType):
            return "BytesView"
        if isinstance(type_node, ListType):
            return f"ListView<{self.element_type(type_node.element_type)}>"
        if isinstance(type_node, ReferenceType):
            return type_node.name
        raise RuntimeError(f"no C++ element type for {type_node}")

    def value(self, type_node, value) -> str:  # pylint: disable=too-many-return-statements
        if isinstance(type_node, PrimitiveType):
            if type_node.name == "bool":
                return "true" if value else "false"
            if type_node.name == "char":
                return f"static_cast<char>({ord(value)})"
            return f"static_cast<{_CPP_INTEGER_TYPES[type_node.name]}>({_cpp_integer(value)})"
        if isinstance(type_node, StringType):
            raw = value.encode("utf-8")
            if not raw:
                return "std::string_view{}"
            name = self.fresh("text")
            self.lines.append(f"    static const char {name}[] = {{{', '.join(str(b - 256 if b > 127 else b) for b in raw)}}};")
            return f"std::string_view({name}, {len(raw)})"
        if isinstance(type_node, BytesType):
            if not value:
                return "BytesView{}"
            name = self.fresh("blob")
            self.lines.append(f"    static const uint8_t {name}[] = {{{', '.join(str(b) for b in value)}}};")
            return f"BytesView{{{name}, {len(value)}}}"
        if isinstance(type_node, ArrayType):
            items = ", ".join(self.value(type_node.element_type, item) for item in value)
            return f"std::array<{self.element_type(type_node.element_type)}, {type_node.length}>{{{{{items}}}}}"
        if isinstance(type_node, ListType):
            element = self.element_type(type_node.element_type)
            if not value:
                return f"ListView<{element}>{{}}"
            items = [self.value(type_node.element_type, item) for item in value]
            name = self.fresh("items")
            self.lines.append(f"    static {element} {name}[] = {{{', '.join(items)}}};")
            return f"ListView<{element}>{{{name}, {len(value)}}}"
        if isinstance(type_node, ReferenceType):
            if type_node.name in self.enums:
                return f"static_cast<{type_node.name}>({_cpp_integer(value)})"
            return self.message(type_node.name, value)
        raise RuntimeError(f"no C++ value for {type_node}")

    def message(self, message_name: str, values: dict) -> str:
        name = self.fresh(message_name.lower())
        assignments = []
        for fld in self.messages[message_name].fields:
            value = values.get(fld.name)
            if value is None:
                continue
            expression = self.value(fld.type, value)
            if fld.optional:
                assignments.append(f"    {name}.has_{fld.name} = true;")
            assignments.append(f"    {name}.{fld.name} = {expression};")
        self.lines.append(f"    static {message_name} {name}{{}};")
        self.lines.extend(assignments)
        return name


def cpp_program(ast, cases, header_cases=()) -> str:  # pylint: disable=too-many-locals
    """The C++ program that prints "case hex" for each case, encoded by the C++ generator's code, and for each
    header case, the whole PDU the framework's PduFramer writes."""
    messages, enums = _declarations(ast)
    writer = _CppWriter(messages, enums)
    prints = []
    for case_name, message_name, values in cases:
        variable = writer.message(message_name, values)
        prints.append(f'    print_encoded("{case_name}", {variable});')
    for case_name, pdu_id, version, seq_no, payload in header_cases:
        name = writer.fresh("payload")
        writer.lines.append(f"    static const uint8_t {name}[] = {{{', '.join(str(b) for b in payload)}}};")
        prints.append(f'    print_framed("{case_name}", {pdu_id}, {version}, {_cpp_integer(seq_no)}, {name}, {len(payload)});')
    return "\n".join([
        '#include "every_type.hpp"',
        "#include <cstdio>",
        "#include <vector>",
        "#include <pubsub_itc_fw/PduFramer.hpp>",
        f"using namespace {CPP_NAMESPACE};",
        "// Collects whatever the framer sends, so the bytes it writes can be printed.",
        "class CollectingStream : public pubsub_itc_fw::ByteStreamInterface {",
        "  public:",
        "    std::vector<uint8_t> bytes;",
        "    std::tuple<int, std::string> send(pubsub_itc_fw::SimpleSpan<const uint8_t> data) override {",
        "        bytes.insert(bytes.end(), data.data(), data.data() + data.size());",
        "        return {static_cast<int>(data.size()), \"\"};",
        "    }",
        "    std::tuple<int, std::string> receive(pubsub_itc_fw::SimpleSpan<uint8_t>) override { return {0, \"\"}; }",
        "    void close() override {}",
        "    std::tuple<std::unique_ptr<pubsub_itc_fw::InetAddress>, std::string> get_peer_address() const override { return {nullptr, \"\"}; }",
        "};",
        "static void print_framed(const char* case_name, int16_t pdu_id, int8_t version, int64_t seq_no, const uint8_t* payload, uint32_t size) {",
        "    CollectingStream stream;",
        "    pubsub_itc_fw::PduFramer framer(stream);",
        "    const auto [sent, error] = framer.send(pdu_id, version, seq_no, payload, size);",
        "    if (!sent) { std::printf(\"%s FRAMING-FAILED\\n\", case_name); return; }",
        '    std::printf("%s ", case_name);',
        '    for (uint8_t byte : stream.bytes) std::printf("%02x", byte);',
        '    std::printf("\\n");',
        "}",
        "template <typename Message>",
        "static void print_encoded(const char* case_name, const Message& message) {",
        "    std::vector<uint8_t> buffer(encoded_size(message));",
        "    std::size_t written = 0;",
        "    std::size_t needed = 0;",
        "    if (!encode(message, buffer.data(), buffer.size(), written, needed)) {",
        '        std::printf("%s ENCODE-FAILED\\n", case_name);',
        "        return;",
        "    }",
        '    std::printf("%s ", case_name);',
        '    for (std::size_t i = 0; i < written; ++i) std::printf("%02x", buffer[i]);',
        '    std::printf("\\n");',
        "}",
        "int main() {",
        *writer.lines,
        *prints,
        "    return 0;",
        "}",
        "",
    ])


def run_cpp(ast, cases, header_cases=()) -> Dict[str, bytes]:
    """Build and run the C++ program; return each case's bytes, and each header case's whole PDU."""
    directory = _scratch_dir("cppref_")
    try:
        (directory / "every_type.hpp").write_text(CppGenerator(namespace=CPP_NAMESPACE).emit(ast))
        (directory / "main.cpp").write_text(cpp_program(ast, cases, header_cases))
        (directory / "CMakeLists.txt").write_text("\n".join([
            "cmake_minimum_required(VERSION 3.16)",
            "project(cross_language_reference CXX)",
            "set(CMAKE_CXX_STANDARD 17)",
            "set(CMAKE_CXX_STANDARD_REQUIRED ON)",
            "find_package(cpptrace REQUIRED CONFIG)",
            f'add_executable(reference main.cpp "{_FRAMEWORK_SOURCES}/PduFramer.cpp")',
            f'target_include_directories(reference PRIVATE "{_PUBSUB_ITC_FW_INCLUDE_DIR}")',
            "target_link_libraries(reference PRIVATE cpptrace::cpptrace)",
            "",
        ]))
        configure = ["cmake", "-S", str(directory), "-B", str(directory / "build"), "-DCMAKE_BUILD_TYPE=Release"]
        if _CPPTRACE_PREFIX:
            configure.append(f"-DCMAKE_PREFIX_PATH={_CPPTRACE_PREFIX}")
        subprocess.run(configure, check=True, capture_output=True, text=True)
        built = subprocess.run(["cmake", "--build", str(directory / "build")], check=False, capture_output=True, text=True)
        if built.returncode != 0:
            raise RuntimeError(f"the C++ reference program did not build:\n{built.stdout}\n{built.stderr}")
        result = subprocess.run([str(directory / "build" / "reference")], check=True, capture_output=True, text=True)
        return _parse_hex_lines(result.stdout)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def _parse_hex_lines(text: str) -> Dict[str, bytes]:
    out: Dict[str, bytes] = {}
    for line in text.splitlines():
        case_name, _, hex_text = line.partition(" ")
        if hex_text.endswith("FAILED") or hex_text.startswith("EXCEPTION"):
            raise RuntimeError(f"case {case_name}: {hex_text}")
        out[case_name] = bytes.fromhex(hex_text)
    return out


# ----------------------------------------------------------------------------------------------------------------
# Java
# ----------------------------------------------------------------------------------------------------------------

_JAVA_PRIMITIVES = {"i8": "byte", "char": "byte", "i16": "short", "i32": "int", "i64": "long", "datetime_ns": "long",
                    "bool": "boolean"}


class _JavaWriter:
    """Writes the Java expressions that build each case's message from its values."""

    def __init__(self, messages, enums):
        self.messages = messages
        self.enums = enums
        self.lines: List[str] = []
        self.counter = 0

    def java_type(self, type_node) -> str:
        if isinstance(type_node, PrimitiveType):
            return _JAVA_PRIMITIVES[type_node.name]
        if isinstance(type_node, StringType):
            return "String"
        if isinstance(type_node, BytesType):
            return "byte[]"
        if isinstance(type_node, (ListType, ArrayType)):
            return self.java_type(type_node.element_type) + "[]"
        if isinstance(type_node, ReferenceType):
            return f"{JAVA_CLASS}.{type_node.name}"
        raise RuntimeError(f"no Java type for {type_node}")

    def value(self, type_node, value) -> str:  # pylint: disable=too-many-return-statements
        if isinstance(type_node, PrimitiveType):
            name = type_node.name
            if name == "bool":
                return "true" if value else "false"
            if name == "char":
                return f"(byte) {ord(value)}"
            if name in ("i8", "i16"):
                return f"({_JAVA_PRIMITIVES[name]}) {value}"
            if name == "i32":
                return str(value)
            return f"{value}L"
        if isinstance(type_node, StringType):
            return '"' + "".join(c if c.isascii() and c.isalnum() or c in " _" else f"\\u{ord(c):04x}" for c in value) + '"'
        if isinstance(type_node, BytesType):
            return "new byte[]{" + ", ".join(f"(byte) {b}" for b in value) + "}"
        if isinstance(type_node, (ListType, ArrayType)):
            element = self.java_type(type_node.element_type)
            base, suffix = element, ""
            while base.endswith("[]"):
                base, suffix = base[:-2], suffix + "[]"
            items = ", ".join(self.value(type_node.element_type, item) for item in value)
            return f"new {base}[]{suffix}{{{items}}}"
        if isinstance(type_node, ReferenceType):
            if type_node.name in self.enums:
                underlying = self.enums[type_node.name].underlying_type
                literal = f"{value}L" if underlying == "i64" else str(value)
                return f"{JAVA_CLASS}.{type_node.name}.fromValue({literal})"
            return self.message(type_node.name, value)
        raise RuntimeError(f"no Java value for {type_node}")

    def message(self, message_name: str, values: dict) -> str:
        self.counter += 1
        name = f"{message_name.lower()}_{self.counter}"
        assignments = []
        for fld in self.messages[message_name].fields:
            value = values.get(fld.name)
            if value is None:
                continue
            expression = self.value(fld.type, value)
            if fld.optional:
                assignments.append(f"        {name}.has_{fld.name} = true;")
            assignments.append(f"        {name}.{fld.name} = {expression};")
        self.lines.append(f"        {JAVA_CLASS}.{message_name} {name} = new {JAVA_CLASS}.{message_name}();")
        self.lines.extend(assignments)
        return name


def java_program(ast, cases) -> str:
    """The Java program: "encode" prints "case hex" for each case; "reencode" reads "case message hex" lines,
    decodes each and encodes it again, and prints "case hex", or "case EXCEPTION ..." if that fails."""
    messages, enums = _declarations(ast)
    writer = _JavaWriter(messages, enums)
    prints = []
    for case_name, message_name, values in cases:
        variable = writer.message(message_name, values)
        prints.append(f"        {{ ByteBuffer b = ByteBuffer.allocate({JAVA_CLASS}.{message_name}.encodedSize({variable}));"
                      f" {JAVA_CLASS}.{message_name}.encode({variable}, b); print(\"{case_name}\", b); }}")
    dispatch = []
    for message_name in messages:
        dispatch.append(f'            case "{message_name}": {{ {JAVA_CLASS}.{message_name} m = {JAVA_CLASS}.{message_name}.decode(in);'
                        f" ByteBuffer b = ByteBuffer.allocate({JAVA_CLASS}.{message_name}.encodedSize(m));"
                        f" {JAVA_CLASS}.{message_name}.encode(m, b); return b; }}")
    return "\n".join([
        "import java.io.BufferedReader;",
        "import java.io.InputStreamReader;",
        "import java.nio.ByteBuffer;",
        "",
        "public final class CrossLanguage {",
        "    static void print(String caseName, ByteBuffer b) {",
        "        StringBuilder sb = new StringBuilder(caseName).append(' ');",
        "        for (int i = 0; i < b.position(); i++) sb.append(String.format(\"%02x\", b.get(i)));",
        "        System.out.println(sb);",
        "    }",
        "    static ByteBuffer reencode(String messageName, ByteBuffer in) {",
        "        switch (messageName) {",
        *dispatch,
        "            default: throw new IllegalArgumentException(messageName);",
        "        }",
        "    }",
        "    public static void main(String[] args) throws Exception {",
        '        if (args[0].equals("encode")) {',
        *writer.lines,
        *prints,
        "            return;",
        "        }",
        "        BufferedReader reader = new BufferedReader(new InputStreamReader(System.in));",
        "        String line;",
        "        while ((line = reader.readLine()) != null) {",
        '            String[] parts = line.split(" ");',
        "            byte[] bytes = new byte[parts[2].length() / 2];",
        "            for (int i = 0; i < bytes.length; i++) bytes[i] = (byte) Integer.parseInt(parts[2].substring(2 * i, 2 * i + 2), 16);",
        "            try {",
        "                print(parts[0], reencode(parts[1], ByteBuffer.wrap(bytes)));",
        "            } catch (Exception e) {",
        '                System.out.println(parts[0] + " EXCEPTION " + e.getClass().getSimpleName());',
        "            }",
        "        }",
        "    }",
        "}",
        "",
    ])


def java_available() -> bool:
    """Whether a Java compiler and runtime are on the path."""
    return shutil.which("javac") is not None and shutil.which("java") is not None


def run_java(ast, cases, reencode_input: Dict[str, Tuple[str, bytes]]) -> Tuple[Dict[str, bytes], Dict[str, str]]:
    """Build the Java program; return each case's bytes as Java encodes its values, and for each entry of
    reencode_input (case -> (message, bytes)), the bytes Java writes after decoding them, as hex, or the exception."""
    directory = _scratch_dir("javaref_")
    try:
        (directory / f"{JAVA_CLASS}.java").write_text(JavaGenerator(class_name=JAVA_CLASS).emit(ast))
        (directory / "CrossLanguage.java").write_text(java_program(ast, cases))
        compiled = subprocess.run(["javac", "-d", str(directory), str(directory / f"{JAVA_CLASS}.java"),
                                   str(directory / "CrossLanguage.java")], check=False, capture_output=True, text=True)
        if compiled.returncode != 0:
            raise RuntimeError(f"the Java programs did not compile:\n{compiled.stdout}\n{compiled.stderr}")
        encoded = subprocess.run(["java", "-cp", str(directory), "CrossLanguage", "encode"],
                                 check=True, capture_output=True, text=True)
        stdin = "".join(f"{case} {message} {raw.hex()}\n" for case, (message, raw) in reencode_input.items())
        reencoded = subprocess.run(["java", "-cp", str(directory), "CrossLanguage", "reencode"], input=stdin,
                                   check=True, capture_output=True, text=True)
        again: Dict[str, str] = {}
        for line in reencoded.stdout.splitlines():
            case_name, _, rest = line.partition(" ")
            again[case_name] = rest
        return _parse_hex_lines(encoded.stdout), again
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def environment_ready() -> bool:
    """Whether the C++ reference program can be built: cmake, and cpptrace found where the build puts it."""
    return shutil.which("cmake") is not None and bool(_CPPTRACE_PREFIX or os.environ.get("CMAKE_PREFIX_PATH"))
