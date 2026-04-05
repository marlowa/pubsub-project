import uuid
import subprocess
import tempfile
import importlib.util
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_pybind11 import Pybind11Generator

# Path to the directory that contains the pubsub_itc_fw folder,
# i.e. the directory such that <pubsub_itc_fw/BumpAllocator.hpp> resolves.
# Adjust this to match your project layout.
_PUBSUB_ITC_FW_INCLUDE_DIR = str(
    Path(__file__).resolve().parent.parent.parent / "libraries" / "pubsub_itc_fw" / "include"
)


def compile_and_load(dsl_text: str, namespace: str = "ns"):
    """
    Parse, validate, generate C++ + pybind11 bindings, compile, and return
    the loaded extension module.

    Uses a TemporaryDirectory that is kept alive until the function returns.
    The .so is loaded before the directory is cleaned up, which is safe because
    the OS keeps the file mapped after dlopen() even after the path is deleted.
    """
    ast = Parser(dsl_text).parse()
    Validator(ast).validate()

    cpp_gen = CppGenerator(namespace=namespace)
    header_code = cpp_gen.emit(ast)

    module_name = f"dslgen_{uuid.uuid4().hex}"

    pyb_gen = Pybind11Generator(namespace=namespace, module_name=module_name)
    bindings_code = pyb_gen.emit(ast)

    with tempfile.TemporaryDirectory(prefix="dslgen_") as tmpdir:
        tmp = Path(tmpdir)

        (tmp / "generated.hpp").write_text(header_code)
        (tmp / "bindings.cpp").write_text(bindings_code)
        (tmp / "CMakeLists.txt").write_text(_cmakelists())

        subprocess.check_call(
            ["cmake", "-S", str(tmp), "-B", str(tmp / "build")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )

        build_result = subprocess.run(
            ["cmake", "--build", str(tmp / "build")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if build_result.returncode != 0:
            compiler_output = build_result.stdout.decode() + build_result.stderr.decode()
            raise RuntimeError(f"cmake build failed:\n{compiler_output}")

        so_files = list((tmp / "build").glob("dslgen*.so"))
        if not so_files:
            so_files = list(tmp.glob("dslgen*.so"))
        if not so_files:
            raise RuntimeError(
                f"No dslgen*.so found after build in {tmpdir}. "
                "Check that pybind11 is installed and CMakeLists.txt is correct."
            )

        return _load_extension(so_files[0], module_name)


def _cmakelists() -> str:
    return f"""\
cmake_minimum_required(VERSION 3.15)
project(dslgen_bindings LANGUAGES CXX)

find_package(pybind11 REQUIRED)

add_library(dslgen MODULE bindings.cpp)
target_include_directories(dslgen PRIVATE "{_PUBSUB_ITC_FW_INCLUDE_DIR}")
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
