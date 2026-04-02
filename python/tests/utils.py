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
    """
    Parse, validate, generate C++ + pybind11 bindings, compile, and return
    the loaded extension module.

    Uses a TemporaryDirectory that is kept alive until the function returns.
    The .so is loaded before the directory is cleaned up, which is safe because
    the OS keeps the file mapped after dlopen() even after the path is deleted.
    """
    # Parse + validate DSL
    ast = Parser(dsl_text).parse()
    Validator(ast).validate()

    # Generate C++ header
    cpp_gen = CppGenerator(namespace=namespace)
    header_code = cpp_gen.emit(ast)

    # Generate a unique module name so multiple tests can run in the same process
    module_name = f"dslgen_{uuid.uuid4().hex}"

    # Generate pybind11 bindings
    pyb_gen = Pybind11Generator(namespace=namespace, module_name=module_name)
    bindings_code = pyb_gen.emit(ast)

    with tempfile.TemporaryDirectory(prefix="dslgen_") as tmpdir:
        tmp = Path(tmpdir)

        (tmp / "generated.hpp").write_text(header_code)
        (tmp / "bindings.cpp").write_text(bindings_code)
        (tmp / "CMakeLists.txt").write_text(_cmakelists())

        # Configure + build
        subprocess.check_call(
            ["cmake", "-S", str(tmp), "-B", str(tmp / "build")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        subprocess.check_call(
            ["cmake", "--build", str(tmp / "build")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )

        # Find the built .so and load it before the temp dir is cleaned up.
        # dlopen() keeps the file mapped in memory after the path is removed.
        so_files = list((tmp / "build").glob("dslgen*.so"))
        if not so_files:
            # Some CMake generators put it directly in tmpdir
            so_files = list(tmp.glob("dslgen*.so"))
        if not so_files:
            raise RuntimeError(
                f"No dslgen*.so found after build in {tmpdir}. "
                "Check that pybind11 is installed and CMakeLists.txt is correct."
            )

        return _load_extension(so_files[0], module_name)


def _cmakelists() -> str:
    return """\
cmake_minimum_required(VERSION 3.15)
project(dslgen_bindings LANGUAGES CXX)

find_package(pybind11 REQUIRED)

add_library(dslgen MODULE bindings.cpp)
target_link_libraries(dslgen PRIVATE pybind11::module)
set_target_properties(dslgen PROPERTIES
    CXX_STANDARD 17
    PREFIX ""
)
"""


def _load_extension(path: Path, module_name: str):
    spec = importlib.util.spec_from_file_location(module_name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
