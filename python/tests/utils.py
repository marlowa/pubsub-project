import uuid
import subprocess
import tempfile
import importlib.util
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_pybind11 import Pybind11Generator

def compile_and_load(dsl_text: str, namespace: str = "ns"):
    # Parse + validate DSL
    ast = Parser(dsl_text).parse()
    Validator(ast).validate()

    # Generate C++ header
    cpp_gen = CppGenerator(namespace=namespace)
    header_code = cpp_gen.emit(ast)

    # Generate a unique module name
    module_name = f"dslgen_{uuid.uuid4().hex}"

    # Generate pybind11 bindings
    pyb_gen = Pybind11Generator(namespace=namespace, module_name=module_name)
    bindings_code = pyb_gen.emit(ast)

    with tempfile.TemporaryDirectory(prefix="dslgen_") as tmpdir:
        tmp = Path(tmpdir)

        # Write generated files
        (tmp / "generated.hpp").write_text(header_code)
        (tmp / "bindings.cpp").write_text(bindings_code)
        (tmp / "CMakeLists.txt").write_text(_cmakelists())

        # Configure + build
        subprocess.check_call(["cmake", "-S", str(tmp), "-B", str(tmp)])
        subprocess.check_call(["cmake", "--build", str(tmp)])

        # Find built extension module
        for so in tmp.glob("dslgen*.so"):
            return _load_extension(so, module_name)

        raise RuntimeError("dslgen module not built")


def _cmakelists() -> str:
    return """\
cmake_minimum_required(VERSION 3.15)
project(dslgen_bindings LANGUAGES CXX)

find_package(pybind11 REQUIRED)

add_library(dslgen MODULE bindings.cpp)
target_link_libraries(dslgen PRIVATE pybind11::module)
set_target_properties(dslgen PROPERTIES
    CXX_STANDARD 20
    PREFIX ""
)
"""


def _load_extension(path: Path, module_name: str):
    spec = importlib.util.spec_from_file_location(module_name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
