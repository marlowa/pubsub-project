#!/usr/bin/env python3
"""
Build script for pubsub_itc_fw_project
Supports normal builds, Valgrind-compatible builds, and Doxygen generation
"""

import argparse
import subprocess
import sys
import os
import platform
import re
import shutil
from pathlib import Path

def _is_rhel8():
    try:
        text = Path('/etc/os-release').read_text()
        lines = {l.split('=')[0]: l.split('=', 1)[1] for l in text.splitlines() if '=' in l}
        return lines.get('ID', '').strip('"') in ('rhel', 'rocky', 'centos') \
            and lines.get('VERSION_ID', '').strip('"').startswith('8')
    except OSError:
        return False


def run_check_standards(source_dir):
    """Run check_standards.py and abort the build if any violations are found."""
    script = source_dir / "check_standards.py"
    result = subprocess.run(
        [sys.executable, str(script)],
        cwd=source_dir,
        check=False,
        text=True,
    )
    if result.returncode != 0:
        print("\nERROR: coding-standard violations found. Fix them before building.", file=sys.stderr)
        sys.exit(result.returncode)
    print("\n✓ Coding standards check passed")


def run_pylint(source_dir):
    """Run pylint on the Python DSL and FIX dictionary generator source."""
    python_dir = source_dir / "python"
    run_command(
        [sys.executable, "-m", "pylint", "dsl", "fix_dictionary"],
        cwd=python_dir,
        description="Running pylint on Python DSL and FIX dictionary source"
    )
    print("\n✓ pylint passed")


def run_pytest(source_dir):
    """Run the Python DSL test suite."""
    python_dir = source_dir / "python"
    run_command(
        [sys.executable, "-m", "pytest", "-q"],
        cwd=python_dir,
        description="Running Python DSL test suite"
    )
    print("\n✓ Python tests passed")


def generate_coverage_report(build_dir, source_dir):
    """Generate an HTML coverage report: gcovr captures, genhtml renders.

    RHEL8/Rocky 8 ship lcov 1.14, whose capture step (geninfo) rejects the
    lcov 2.x options the original pipeline used and produced an empty report.
    gcovr does the capture instead (pip-installable, no root, reads the same
    --coverage instrumentation) and emits an lcov tracefile; genhtml renders it.
    genhtml's rendering works fine on lcov 1.14 and gives the sortable, per-file
    "rich" HTML that gcovr's own HTML lacks -- the 1.14 limitation was only in
    capture, not rendering.

    gcovr embeds a per-line source checksum (third DA field) and a VER line that
    lcov 1.14's genhtml recomputes differently and rejects, so those are stripped
    before rendering.
    """
    print("\n============================================================")
    print("Generating code coverage report (gcovr capture + genhtml)")
    print("============================================================")

    raw_info = build_dir / "coverage.raw.info"
    clean_info = build_dir / "coverage.info"
    html_dir = build_dir / "coverage_html"
    html_dir.mkdir(parents=True, exist_ok=True)

    # Coverage focus is the libraries. Drop third-party code, the applications
    # (examples, tested ad-hoc), tests, test helpers, and generated code under the
    # build directory. gcovr matches --exclude against the path relative to --root
    # (gcovr 8.6+), so a leading ".*/" misses a top-level directory like
    # applications/; "(.*/)?NAME/.*" matches whether the directory is at the root,
    # nested, or given as an absolute path (older gcovr matched absolute).
    excludes = [
        r"(.*/)?" + re.escape(build_dir.name) + r"/.*",
        r"(.*/)?thirdparty/.*",
        r"(.*/)?applications/.*",
        r"(.*/)?tests/.*",
        r"(.*/)?tests_common/.*",
        r"(.*/)?integration_tests/.*",
        # Pure Quill wrapper macros/templates: every logging call instantiates
        # hundreds of template functions here that gcovr counts as uncovered
        # "functions", crushing the function-coverage figure. It carries no
        # testable logic of its own, so exclude it from the denominator.
        r"(.*/)?LoggingMacros\.hpp",
    ]

    # Prefer the gcovr executable on PATH (e.g. a pipx/user install) so the report
    # does not depend on gcovr being importable by whichever Python runs build.py.
    # Fall back to `python -m gcovr` only when no executable is found.
    gcovr_executable = shutil.which("gcovr")
    gcovr_command = [gcovr_executable] if gcovr_executable is not None else [sys.executable, "-m", "gcovr"]

    cmd = gcovr_command + [
        "--root", str(source_dir),
        str(build_dir),
        # Match the old lcov --omit-lines: drop log macros and bare string lines.
        "--exclude-lines-by-pattern", r'PUBSUB_LOG|^\s+"[^"]*"',
        "--exclude-throw-branches",
        "--exclude-unreachable-branches",
        "--print-summary",
        "--lcov", str(raw_info),
    ]
    for pattern in excludes:
        cmd += ["--exclude", pattern]

    run_command(cmd, description="Capturing coverage with gcovr")

    # Strip gcovr's per-line source checksums and VER line so lcov 1.14's genhtml
    # does not abort on a checksum it recomputes differently.
    da_line = re.compile(r'^(DA:\d+,\d+),.*$')
    cleaned = []
    for line in raw_info.read_text(encoding="utf-8").splitlines(keepends=True):
        if line.startswith("VER:"):
            continue
        cleaned.append(da_line.sub(r'\1', line))
    clean_info.write_text("".join(cleaned), encoding="utf-8")

    run_command(
        [
            "genhtml", str(clean_info),
            "--output-directory", str(html_dir),
            "--legend",
            # Strip the repo root so genhtml groups by libraries/... instead of
            # rendering divergent trees (e.g. scram_crypto) from the filesystem root.
            "--prefix", str(source_dir),
            "--title", "pubsub_itc_fw Code Coverage",
        ],
        description="Rendering HTML with genhtml",
    )

    print("\n✓ Coverage report generated:")
    print(f"  {html_dir}/index.html")

def run_command(cmd, cwd=None, description=None, env=None, quiet=False):
    """Run a shell command, streaming output in real time while capturing it.

    On failure, prints the captured output (quiet mode) or the last 30 lines
    (non-quiet mode, where the full output already streamed to the terminal)
    inside the error banner so the cause is clearly visible.
    """
    if description:
        print(f"\n{'='*60}")
        print(f"{description}")
        print(f"{'='*60}")

    print(f"Running: {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    sys.stdout.flush()

    process = subprocess.Popen(
        cmd,
        cwd=cwd,
        shell=isinstance(cmd, str),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    lines = []
    for line in process.stdout:
        if not quiet:
            sys.stdout.write(line)
            sys.stdout.flush()
        lines.append(line)
    process.wait()

    if process.returncode != 0:
        sys.stdout.flush()
        step = description or (cmd[0] if isinstance(cmd, list) else str(cmd))
        print(f"\n{'='*60}", file=sys.stderr)
        if process.returncode < 0:
            import signal
            try:
                sig_name = signal.Signals(-process.returncode).name
                print(f"ERROR: '{step}' killed by signal {sig_name} ({-process.returncode})", file=sys.stderr)
            except ValueError:
                print(f"ERROR: '{step}' killed by signal {-process.returncode}", file=sys.stderr)
        else:
            print(f"ERROR: '{step}' failed with exit code {process.returncode}", file=sys.stderr)
        if quiet:
            if lines:
                print("--- output ---", file=sys.stderr)
                sys.stderr.write(''.join(lines))
        else:
            tail = lines[-30:]
            if tail:
                print("--- last 30 lines ---", file=sys.stderr)
                sys.stderr.write(''.join(tail))
        print(f"{'='*60}", file=sys.stderr)
        sys.stderr.flush()
        sys.exit(process.returncode)

    return process


def check_environment_variables():
    """Verify required environment variables are set"""
    required_vars = [
        'THIRDPARTY_DIR',
        'FMT_VERSION',
        'QUILL_VERSION',
        'ARGPARSE_VERSION',
        'GOOGLETEST_VERSION',
        'ROBINMAP_VERSION',
    ]

    missing = [var for var in required_vars if var not in os.environ]

    if missing:
        print("ERROR: Missing required environment variables:", file=sys.stderr)
        for var in missing:
            print(f"  - {var}", file=sys.stderr)
        sys.exit(1)


def run_doxygen(source_dir):
    """Run Doxygen to generate documentation"""
    doxyfile = source_dir / "Doxyfile"

    if not doxyfile.exists():
        print(f"ERROR: Doxyfile not found at {doxyfile}", file=sys.stderr)
        print("Please create a Doxyfile in your project root", file=sys.stderr)
        sys.exit(1)

    run_command(
        ["doxygen", str(doxyfile)],
        cwd=source_dir,
        description="Generating Doxygen documentation",
        quiet=_is_rhel8()
    )

    print("\n✓ Doxygen documentation generated successfully")


def configure_cmake(build_dir, source_dir, enable_valgrind=False, enable_coverage=False,
                    enable_asan=False, enable_tsan=False, install_dir=None,
                    enable_doxygen=True, debug=False):
    cmake_args = [
        "cmake",
        str(source_dir)
    ]

    cmake_args.append(f"-DCMAKE_BUILD_TYPE={'Debug' if debug else 'Release'}")
    if not enable_doxygen:
        cmake_args.append("-DENABLE_DOXYGEN=OFF")
    if install_dir is not None:
        cmake_args.append(f"-DCMAKE_INSTALL_PREFIX={install_dir}")
    if enable_valgrind:
        cmake_args.append("-DENABLE_VALGRIND=ON")
        print("NOTE: Building with Valgrind compatibility")
        print("  - Lock-free optimizations disabled (-mcx16 -march=x86-64-v2)")
        print("  - USING_VALGRIND macro defined")

    if enable_coverage:
        cmake_args.append("-DENABLE_COVERAGE=ON")

    if enable_asan:
        cmake_args.append("-DENABLE_ASAN=ON")
        print("NOTE: Building with AddressSanitizer")
        print("  - Link with -fsanitize=address")

    if enable_tsan:
        cmake_args.append("-DENABLE_TSAN=ON")
        print("NOTE: Building with ThreadSanitizer")
        print("  - Lock-free optimizations disabled (-mcx16 -march=x86-64-v2)")
        print("  - Link with -fsanitize=thread")

    run_command(cmake_args, cwd=build_dir, description="Configuring CMake")


def build_project(build_dir, jobs=None, verbose=False):
    """Build the project using cmake --build (works with make and ninja)"""
    if jobs is None:
        try:
            import multiprocessing
            jobs = multiprocessing.cpu_count()
        except:
            jobs = 4

    cmd = ["cmake", "--build", str(build_dir), "--parallel", str(jobs)]
    if verbose:
        cmd.append("--verbose")

    run_command(
        cmd,
        description=f"Building project (using {jobs} cores)"
    )

    print("\n✓ Build completed successfully")

def test_environment(build_dir):
    """
    Environment for running a test binary against the library just built.

    The test binaries carry a RUNPATH pointing at the build tree, but an
    LD_LIBRARY_PATH set in the surrounding environment takes precedence over
    RUNPATH, and the developer profile points it at installed/lib.  Since the
    install step runs after the tests, that silently tests the *previous* build's
    library: a change to a library .cpp could be validated against the old .so and
    pass.  Prepending the build tree makes the freshly built library win.
    """
    env = os.environ.copy()
    build_library_dir = str((build_dir / "libraries" / "pubsub_itc_fw").resolve())
    existing = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{build_library_dir}:{existing}" if existing else build_library_dir
    return env


def install_directory_name(args):
    """
    Name of the staging directory for this build's instrumentation flavour.

    The plain build stages into "installed"; anything instrumented stages into
    "installed-<flavour>". A coverage or sanitizer library is a different artefact
    -- built at -O0, inlining disabled, gcov counters or a sanitizer runtime linked
    in -- and it must not overwrite the plain one.

    @param[in] args Parsed command-line arguments.
    @return Directory name, relative to the project root.
    """
    flavours = []
    if args.coverage:
        flavours.append("coverage")
    if args.asan:
        flavours.append("asan")
    if args.tsan:
        flavours.append("tsan")
    if args.valgrind:
        flavours.append("valgrind")
    if not flavours:
        return "installed"
    return "installed-" + "-".join(flavours)


def install_directory_notice(staging_dir):
    """Explain a non-default staging directory, and how to use it deliberately."""
    print(f"NOTE: instrumented build -- staging to {staging_dir.name}/ instead of installed/")
    print("  installed/ is left untouched so devenv.py, perf_run.py, ha_test.py and the")
    print("  standalone test scripts keep loading uninstrumented libraries.  This matters")
    print("  because LD_LIBRARY_PATH points at installed/lib and takes precedence over the")
    print("  binaries' RUNPATH, so an instrumented library installed there would silently")
    print("  become what every other tool loads -- a perf_run.py after a coverage build")
    print("  would report figures with no indication that they are meaningless.")
    print(f"  To run the system instrumented, name this prefix: ./start_fix_seq_system.py {staging_dir.name}")


def run_tests(build_dir, use_tsan=False, tsan_suppressions=None):
    """Run the test suite"""
    test_binary = build_dir / "libraries" / "pubsub_itc_fw" / "tests" / "pubsub_itc_fw_tests"

    if not test_binary.exists():
        print(f"ERROR: Test binary not found at {test_binary}", file=sys.stderr)
        sys.exit(1)

    if use_tsan:
        # TSan reserves specific virtual address ranges for its shadow memory.
        # ASLR can place kernel mappings in those ranges, causing TSan to abort
        # with "unexpected memory mapping" before any tests run.
        # setarch -R disables ASLR for this process only, giving TSan the
        # address space it needs. This has no effect on the rest of the system.
        cmd = ["setarch", platform.machine(), "-R", str(test_binary)]
    else:
        cmd = [str(test_binary)]

    # TSan suppressions are passed via environment variable, not command line.
    env = test_environment(build_dir)
    if use_tsan and tsan_suppressions is not None:
        suppressions_path = Path(tsan_suppressions).resolve()
        if not suppressions_path.exists():
            print(f"WARNING: TSan suppressions file not found: {suppressions_path}", file=sys.stderr)
        else:
            env["TSAN_OPTIONS"] = f"suppressions={suppressions_path}"
            print(f"NOTE: Using TSan suppressions from {suppressions_path}")

    run_command(
        cmd,
        cwd=build_dir,
        description="Running test suite",
        env=env
    )

    print("\n✓ All tests passed")


def run_integration_tests(build_dir):
    """Run the integration test suite. Only called after unit tests pass."""
    test_binary = (build_dir / "libraries" / "pubsub_itc_fw" /
                   "integration_tests" / "pubsub_itc_fw_integration_tests")

    if not test_binary.exists():
        print(f"NOTE: Integration test binary not found at {test_binary} — skipping")
        return

    run_command(
        [str(test_binary)],
        cwd=build_dir,
        description="Running integration test suite",
        env=test_environment(build_dir)
    )

    print("\n✓ All integration tests passed")


def run_scram_crypto_tests(build_dir):
    """Run the scram_crypto known-answer tests. Only called after unit tests pass."""
    test_binary = build_dir / "libraries" / "scram_crypto" / "scram_crypto_tests"

    if not test_binary.exists():
        print(f"NOTE: scram_crypto test binary not found at {test_binary} — skipping")
        return

    run_command(
        [str(test_binary)],
        cwd=build_dir,
        description="Running scram_crypto test suite",
        env=test_environment(build_dir)
    )

    print("\n✓ All scram_crypto tests passed")


def run_fix_codec_tests(build_dir):
    """Run the fix_codec library unit tests. Only called after unit tests pass."""
    test_binary = build_dir / "libraries" / "fix_codec" / "fix_codec_tests"

    if not test_binary.exists():
        print(f"NOTE: fix_codec test binary not found at {test_binary} — skipping")
        return

    run_command(
        [str(test_binary)],
        cwd=build_dir,
        description="Running fix_codec test suite",
        env=test_environment(build_dir)
    )

    print("\n✓ All fix_codec tests passed")


def run_fix_codec_performance_tests(build_dir):
    """Run the fix_codec performance regression guard.

    Kept apart from the unit tests because it asserts on timing rather than on
    behaviour. Its main assertions are ratios -- how validation cost responds to
    field count, and how each stage compares with framing the same message -- so it
    means the same thing on a slow machine as on a fast one. The binary is absent in
    sanitizer and coverage builds, where timings measure the instrumentation.
    """
    test_binary = build_dir / "libraries" / "fix_codec" / "fix_codec_performance_tests"

    if not test_binary.exists():
        print(f"NOTE: fix_codec performance test binary not found at {test_binary} — skipping")
        return

    run_command(
        [str(test_binary)],
        cwd=build_dir,
        description="Running fix_codec performance regression tests",
        env=test_environment(build_dir)
    )

    print("\n✓ All fix_codec performance tests passed")


def install_project(build_dir, install_dir):
    """Install the project to the specified directory."""
    run_command(
        ["cmake", "--install", str(build_dir)],
        description=f"Installing to {install_dir}",
        quiet=_is_rhel8()
    )
    print(f"\n✓ Installation complete: {install_dir}")


def clean_build(build_dir):
    """Remove build directory"""
    if build_dir.exists():
        print(f"Removing build directory: {build_dir}")
        shutil.rmtree(build_dir)
        print("✓ Build directory cleaned")
    else:
        print(f"Build directory does not exist: {build_dir}")


def check_mvn():
    if shutil.which("mvn") is None:
        print("ERROR: 'mvn' not found on PATH — install Maven to build the Java admin service",
              file=sys.stderr)
        sys.exit(1)


def maven_goals(clean: bool, generate_coverage: bool):
    """Return the Maven goal list. JaCoCo binds its report goal to the verify phase,
    so a coverage build runs `verify` rather than `package`; otherwise `package`.
    """
    goal = "verify" if generate_coverage else "package"
    return ["clean", goal] if clean else [goal]


def build_java_service(source_dir: Path, install_dir: Path, skip_tests: bool, clean: bool, coverage: bool = False):
    """Build the Java admin service fat JAR and copy it to install_dir/lib/."""
    check_mvn()

    java_dir = source_dir / "java" / "admin-service"
    # Coverage needs the tests to run, so it is only produced when tests are enabled.
    generate_coverage = coverage and not skip_tests
    mvn_cmd = ["mvn"] + maven_goals(clean, generate_coverage)
    if skip_tests:
        mvn_cmd.append("-DskipTests")

    run_command(mvn_cmd, cwd=java_dir, description="Building Java admin service")

    if generate_coverage:
        print(f"✓ Java coverage report (admin-service): "
              f"{java_dir / 'target' / 'site' / 'jacoco' / 'index.html'}")

    target_dir = java_dir / "target"
    candidates = [
        jar for jar in target_dir.glob("admin-service-*.jar")
        if not jar.name.startswith("original-")
    ]
    if len(candidates) != 1:
        print(f"ERROR: expected exactly one admin-service JAR in {target_dir}, "
              f"found: {[c.name for c in candidates]}", file=sys.stderr)
        sys.exit(1)

    lib_dir = install_dir / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    jar_dst = lib_dir / "admin-service.jar"
    shutil.copy2(candidates[0], jar_dst)
    print(f"\n✓ Java admin service installed: {jar_dst}")


def build_fix_test_client(source_dir: Path, install_dir: Path, skip_tests: bool, clean: bool, coverage: bool = False):
    """Build the fix-test-client fat JAR, install it and its config into the staging tree.

    The JAR is copied to install_dir/lib/fix-test-client.jar.
    Config files (app.toml, session.cfg) are copied to
    install_dir/etc/fix_test_client/config/ so that devenv.py can start the
    service with workdir=etc/fix_test_client and session_config=config/session.cfg
    resolves correctly against the working directory.
    """
    check_mvn()

    java_dir = source_dir / "java" / "fix-test-client"
    # Coverage needs the tests to run, so it is only produced when tests are enabled.
    generate_coverage = coverage and not skip_tests
    mvn_cmd = ["mvn"] + maven_goals(clean, generate_coverage)
    if skip_tests:
        mvn_cmd.append("-DskipTests")

    run_command(mvn_cmd, cwd=java_dir, description="Building fix-test-client")

    if generate_coverage:
        print(f"✓ Java coverage report (fix-test-client): "
              f"{java_dir / 'target' / 'site' / 'jacoco' / 'index.html'}")

    target_dir = java_dir / "target"
    candidates = [
        jar for jar in target_dir.glob("fix-test-client-*.jar")
        if not jar.name.startswith("original-")
    ]
    if len(candidates) != 1:
        print(f"ERROR: expected exactly one fix-test-client JAR in {target_dir}, "
              f"found: {[c.name for c in candidates]}", file=sys.stderr)
        sys.exit(1)

    lib_dir = install_dir / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    jar_dst = lib_dir / "fix-test-client.jar"
    shutil.copy2(candidates[0], jar_dst)

    # Mirror java/fix-test-client/config/ → install_dir/etc/fix_test_client/config/
    # so that devenv.py can pass the absolute path to app.toml as argv[1], while
    # session_config = "config/session.cfg" in app.toml resolves against the workdir.
    config_src = java_dir / "config"
    config_dst = install_dir / "etc" / "fix_test_client" / "config"
    config_dst.mkdir(parents=True, exist_ok=True)
    for src_file in config_src.iterdir():
        if src_file.is_file():
            shutil.copy2(src_file, config_dst / src_file.name)

    print(f"\n✓ fix-test-client installed: {jar_dst}")


def main():
    parser = argparse.ArgumentParser(
        description="Build script for pubsub_itc_fw_project",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                                    # Build C++ and Java, run all tests
  %(prog)s --clean                            # Clean and rebuild both
  %(prog)s --no-java                          # C++ only
  %(prog)s --no-cpp                           # Java admin service only
  %(prog)s --no-tests                         # Build without running any tests
  %(prog)s --no-cpp-tests                     # Skip C++ tests only
  %(prog)s --no-java-tests                    # Skip Java tests only
  %(prog)s --no-pylint                        # Skip pylint on the Python DSL
  %(prog)s --valgrind                         # C++ build with Valgrind compatibility
  %(prog)s --doxygen                          # Build and generate Doxygen docs
  %(prog)s --doxygen-only                     # Only generate documentation
  %(prog)s --doxygen --no-doxygen             # Skip Doxygen (overrides --doxygen)
        """
    )

    parser.add_argument('--clean', action='store_true',
        help='Clean build directory before building'
    )

    parser.add_argument('--valgrind', action='store_true',
        help='Build with Valgrind compatibility (disables lock-free optimizations)'
    )

    parser.add_argument('--doxygen', action='store_true',
        help='Generate Doxygen documentation after building'
    )

    parser.add_argument('--doxygen-only', action='store_true',
        help='Only generate Doxygen documentation (skip build)'
    )

    parser.add_argument('--no-doxygen', action='store_true',
        help='Skip Doxygen generation even if --doxygen or --doxygen-only is set'
    )

    parser.add_argument('--no-tests', action='store_true',
        help='Skip all tests: C++ unit+integration, Python DSL pytest, and Java Maven tests (pylint still runs)'
    )

    parser.add_argument('--no-cpp-tests', action='store_true',
        help='Skip C++ unit and integration tests only; Python and Java tests are unaffected'
    )

    parser.add_argument('--no-performance-tests', action='store_true',
        help='Skip the fix_codec performance regression tests only; other C++ tests are unaffected'
    )

    parser.add_argument('--no-java-tests', action='store_true',
        help='Skip Java Maven tests only (-DskipTests); C++ and Python tests are unaffected'
    )

    parser.add_argument('--no-pytest', action='store_true',
        help='Skip the Python DSL test suite only (pylint still runs; C++ and Java tests are unaffected)'
    )

    parser.add_argument('--no-pylint', action='store_true',
        help='Skip pylint on the Python DSL source'
    )

    parser.add_argument('--jobs', '-j', type=int, metavar='N',
        help='Number of parallel build jobs (default: number of CPU cores)'
    )

    parser.add_argument('--verbose', '-v', action='store_true',
        help='Show compiler and linker command lines during build'
    )

    parser.add_argument('--build-dir', type=Path, default=Path('build'),
        help='Build directory path (default: ./build)'
    )

    parser.add_argument('--coverage', action='store_true',
        help='Build with GCC/Clang code coverage instrumentation')

    parser.add_argument('--coverage-report', action='store_true',
        help='Generate coverage reports after running tests: LCOV + genhtml for C++, '
             'and JaCoCo HTML for the Java modules (admin-service, fix-test-client)')

    parser.add_argument('--java-coverage', action='store_true',
        help='Generate the JaCoCo HTML coverage report for the Java modules only '
             '(admin-service, fix-test-client); does not run the C++ coverage report')

    parser.add_argument('--asan', action='store_true',
        help='Build with AddressSanitizer (cannot be combined with --tsan or --valgrind)'
    )

    parser.add_argument('--tsan', action='store_true',
        help='Build with ThreadSanitizer (cannot be combined with --asan or --valgrind)'
    )

    parser.add_argument('--tsan-suppressions', type=str, metavar='FILE',
        help='Path to TSan suppressions file (only used with --tsan)'
    )

    parser.add_argument('--debug', action='store_true',
        help='Build with CMAKE_BUILD_TYPE=Debug (default: Release)'
    )

    parser.add_argument('--no-cpp', action='store_true',
        help='Skip the C++ build (cmake/make/tests/install); build Java only'
    )

    parser.add_argument('--no-java', action='store_true',
        help='Skip the Java admin service build; build C++ only'
    )

    args = parser.parse_args()

    if args.no_cpp and args.no_java:
        print("ERROR: --no-cpp and --no-java together leave nothing to build", file=sys.stderr)
        sys.exit(1)

    skip_cpp_tests  = args.no_tests or args.no_cpp_tests
    skip_java_tests = args.no_tests or args.no_java_tests
    skip_pytest     = args.no_tests or args.no_pytest

    # Get source directory (parent of this script)
    source_dir = Path(__file__).parent.resolve()
    build_dir = source_dir / args.build_dir

    # Staging dir: CMake installs here after the build; release.py reads from here.
    # Fixed relative to the project root, not the build directory, so that all
    # tooling (ha_test.py, auth_service_test.py, devenv.py) finds binaries in the
    # same well-known location regardless of which --build-dir was used.
    #
    # Instrumented builds stage elsewhere -- see install_directory_name(). Note that
    # a separate --build-dir alone does not protect installed/, because the install
    # prefix is independent of it.
    staging_dir = (source_dir / install_directory_name(args)).resolve()
    if staging_dir.name != "installed":
        install_directory_notice(staging_dir)

    # Sanitizer mutual exclusion checks
    if args.asan and args.tsan:
        print("ERROR: --asan and --tsan are mutually exclusive", file=sys.stderr)
        sys.exit(1)

    if args.valgrind and (args.asan or args.tsan):
        print("ERROR: --valgrind cannot be combined with --asan or --tsan", file=sys.stderr)
        sys.exit(1)

    # Handle doxygen-only mode
    if args.doxygen_only:
        if not args.no_doxygen:
            run_doxygen(source_dir)
        else:
            print("NOTE: --no-doxygen is set; skipping Doxygen")
        return 0

    # ── C++ build ─────────────────────────────────────────────────────────────
    if not args.no_cpp:
        # Verify C++ build environment variables
        check_environment_variables()

        # Coding-standard check runs first -- violations are fatal.
        run_check_standards(source_dir)

        # Python DSL checks run before the (much slower) C++ build begins.
        if not args.no_pylint:
            run_pylint(source_dir)
        else:
            print("NOTE: --no-pylint is set; skipping pylint")
        if not skip_pytest:
            run_pytest(source_dir)

        if args.clean:
            clean_build(build_dir)

        build_dir.mkdir(parents=True, exist_ok=True)

        configure_cmake(build_dir, source_dir, enable_valgrind=args.valgrind,
                        enable_coverage=args.coverage, enable_asan=args.asan,
                        enable_tsan=args.tsan, install_dir=staging_dir,
                        enable_doxygen=not args.no_doxygen, debug=args.debug)

        build_project(build_dir, jobs=args.jobs, verbose=args.verbose)

        if not skip_cpp_tests:
            run_tests(build_dir, use_tsan=args.tsan, tsan_suppressions=args.tsan_suppressions)
            run_fix_codec_tests(build_dir)
            run_scram_crypto_tests(build_dir)
            run_integration_tests(build_dir)
            if not args.no_performance_tests:
                run_fix_codec_performance_tests(build_dir)

        if args.coverage_report:
            generate_coverage_report(build_dir, source_dir)

        install_project(build_dir, staging_dir)

        if args.doxygen and not args.no_doxygen:
            run_doxygen(source_dir)

    # ── Java build ────────────────────────────────────────────────────────────
    if not args.no_java:
        java_coverage = args.coverage_report or args.java_coverage
        build_java_service(source_dir, staging_dir,
                           skip_tests=skip_java_tests, clean=args.clean,
                           coverage=java_coverage)
        build_fix_test_client(source_dir, staging_dir,
                              skip_tests=skip_java_tests, clean=args.clean,
                              coverage=java_coverage)


    print("\n" + "="*60)
    print("Build process completed successfully!")
    print("="*60)

    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nBuild interrupted by user", file=sys.stderr)
        print("\n" + "="*60, file=sys.stderr)
        print("BUILD FAILED", file=sys.stderr)
        print("="*60, file=sys.stderr)
        sys.exit(130)
    except SystemExit as e:
        if e.code and e.code != 0:
            print("\n" + "="*60, file=sys.stderr)
            print("BUILD FAILED", file=sys.stderr)
            print("="*60, file=sys.stderr)
        raise
    except Exception as e:
        print(f"\n\nUnexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        print("\n" + "="*60, file=sys.stderr)
        print("BUILD FAILED", file=sys.stderr)
        print("="*60, file=sys.stderr)
        sys.exit(1)
