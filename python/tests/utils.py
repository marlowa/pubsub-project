import os
import shutil
import uuid
import subprocess
import tempfile
import importlib.util
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_pybind11 import Pybind11Generator

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# Path to the directory that contains the pubsub_itc_fw folder,
# i.e. the directory such that <pubsub_itc_fw/BumpAllocator.hpp> resolves.
_PUBSUB_ITC_FW_INCLUDE_DIR = str(_PROJECT_ROOT / "libraries" / "pubsub_itc_fw" / "include")

# These tests compile a shared object and dlopen it, so the scratch directory must
# permit executable mappings. The system temp directory frequently does not: mounting
# /tmp noexec is a standard hardening measure and is in place on the RHEL8 target,
# where every one of these tests failed with
#
#     ImportError: .../dslgen.so: failed to map segment from shared object
#
# after building perfectly. Building under the project's own (gitignored) build tree
# fixes that, and independently satisfies the rule that a build produces nothing
# outside the project directory -- temporary files included.
# PUBSUB_BUILD_DIR is set by build.py so the scratch build follows --build-dir; a
# Rocky container run must not write into the host's build tree, because gcc-8.5
# objects and a gcc-13 CMake cache in one directory make every platform switch a
# full rebuild at best. Falls back to ./build for a bare pytest run.
_MODULE_BUILD_ROOT = Path(
    os.environ.get("PUBSUB_BUILD_DIR", str(_PROJECT_ROOT / "build"))
) / "pybind11_test_modules"


def _discard_scratch_dir(path: Path) -> None:
    """Remove a scratch build directory, and do not fail the test if it cannot be.

    The .so built in here is dlopen'ed and still mapped when the directory is removed. On a
    local filesystem that is unremarkable -- the inode outlives the name. On NFS it is not:
    unlinking a file another process still holds open makes the server rename it to .nfsXXXX
    in the same directory, so the rmdir that follows fails with ENOTEMPTY. On an RHEL8 target
    whose build tree is NFS-mounted, that failed 22 tests of 0.3.0 that had in fact all passed
    -- the OSError came out of TemporaryDirectory's cleanup, after the assertions were done.

    Nothing is leaked by leaving it: the silly-rename file disappears when this process exits
    and drops the mapping, and the empty directory is reaped by the purge below on the next
    run.
    """
    shutil.rmtree(path, ignore_errors=True)


def _purge_stale_scratch_dirs() -> None:
    """Reap the scratch directories that an earlier run could not remove.

    Safe because the suite runs serially -- build.py invokes plain "pytest -q" -- so no other
    worker owns a directory here while this module is being imported.
    """
    if not _MODULE_BUILD_ROOT.is_dir():
        return
    for stale in _MODULE_BUILD_ROOT.glob("dslgen_*"):
        shutil.rmtree(stale, ignore_errors=True)


_purge_stale_scratch_dirs()


def compile_and_load(dsl_text: str, namespace: str = "ns"):
    """
    Parse, validate, generate C++ + pybind11 bindings, compile, and return
    the loaded extension module.

    Builds in a scratch directory that is discarded once the module is loaded. The .so is
    loaded before the directory goes, which is safe because the file stays mapped after
    dlopen() whatever happens to the path -- see _discard_scratch_dir for why removing it
    is nonetheless allowed to fail.
    """
    ast = Parser(dsl_text).parse()
    Validator(ast).validate()

    cpp_gen = CppGenerator(namespace=namespace)
    header_code = cpp_gen.emit(ast)

    module_name = f"dslgen_{uuid.uuid4().hex}"

    pyb_gen = Pybind11Generator(namespace=namespace, module_name=module_name)
    bindings_code = pyb_gen.emit(ast)

    _MODULE_BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="dslgen_", dir=_MODULE_BUILD_ROOT)
    try:
        tmp = Path(tmpdir)

        (tmp / "generated.hpp").write_text(header_code)
        (tmp / "bindings.cpp").write_text(bindings_code)
        (tmp / "CMakeLists.txt").write_text(_cmakelists())

        configure_cmd = ["cmake", "-S", str(tmp), "-B", str(tmp / "build")]
        # Help cmake find pybind11 when it is pip-installed (e.g. a RHEL8 venv)
        # rather than provided as a system cmake package (as on the dev box,
        # where the pybind11 Python module is absent but /usr/lib/cmake/pybind11
        # exists). When importable, point find_package straight at its cmake dir;
        # otherwise fall back to cmake's default search.
        try:
            import pybind11
            configure_cmd.append(f"-Dpybind11_DIR={pybind11.get_cmake_dir()}")
        except Exception:
            pass

        configure_result = subprocess.run(
            configure_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if configure_result.returncode != 0:
            raise RuntimeError(f"cmake configure failed:\n{configure_result.stdout}")

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
    finally:
        _discard_scratch_dir(Path(tmpdir))


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
    try:
        spec.loader.exec_module(mod)
    except ImportError as exc:
        # dlopen's own wording for this is unhelpfully cryptic, and the cause is
        # almost always a filesystem that refuses executable mappings.
        if "failed to map segment" in str(exc):
            raise ImportError(
                f"{exc}\n\n"
                f"The extension compiled but could not be loaded from {path.parent}.\n"
                "That directory almost certainly forbids executable mappings -- a "
                "filesystem mounted noexec, or an SELinux denial.\n"
                "Check with:  mount | grep -w " + str(path.parents[3]) + "\n"
                "             getenforce && sudo ausearch -m avc -ts recent"
            ) from exc
        raise
    return mod
