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
    """Run pylint on the Python DSL source."""
    python_dir = source_dir / "python"
    run_command(
        [sys.executable, "-m", "pylint", "dsl"],
        cwd=python_dir,
        description="Running pylint on Python DSL source"
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
    """Generate LCOV + genhtml coverage report"""
    print("\n============================================================")
    print("Generating code coverage report")
    print("============================================================")

    raw_info = build_dir / "coverage.raw.info"
    filtered_info = build_dir / "coverage.info"
    html_dir = build_dir / "coverage_html"

    # 1. Capture coverage
    run_command([
        "lcov",
        "--capture",
        "--directory", str(build_dir),
        "--output-file", str(raw_info),
        "--rc", "geninfo_unexecuted_blocks=1",
        "--ignore-errors", "mismatch"
    ], description="Capturing coverage data")

    # 2. Filter out system, thirdparty, tests, test helpers, and generated code
    run_command([
        "lcov",
        "--remove", str(raw_info),
        "/usr/*",
        "*/thirdparty/*",
        "*/tests/*",
        "*/tests_common/*",
        "*/integration_tests/*",
        "*/build/libraries/pubsub_itc_fw/dsl/*",
        "*/build/libraries/pubsub_itc_fw/pubsub_itc_fw/*",
        "*/build/generated_dsl/*",
        "--output-file", str(filtered_info),
        "--ignore-errors", "mismatch,unused",
        "--omit-lines", "PUBSUB_LOG|^\\s+\"[^\"]*\"",
        "--erase-functions", "FMT_COMPILE_STRING"
    ], description="Filtering coverage data")

    # 3. Generate HTML
    run_command([
        "genhtml",
        str(filtered_info),
        "--output-directory", str(html_dir),
        "--legend",
        "--title", "pubsub_itc_fw Code Coverage",
        "--show-details",
        "--ignore-errors", "mismatch"
    ], description="Generating HTML coverage report")

    print("\n✓ Coverage report generated:")
    print(f"  {html_dir}/index.html")

def run_command(cmd, cwd=None, description=None, env=None, quiet=False):
    """Run a shell command and handle errors"""
    if description:
        print(f"\n{'='*60}")
        print(f"{description}")
        print(f"{'='*60}")

    print(f"Running: {' '.join(cmd) if isinstance(cmd, list) else cmd}")

    if quiet:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            shell=isinstance(cmd, str),
            check=False,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        if result.returncode != 0:
            if result.stdout:
                sys.stdout.write(result.stdout)
            if result.stderr:
                sys.stderr.write(result.stderr)
    else:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            shell=isinstance(cmd, str),
            check=False,
            env=env
        )

    if result.returncode != 0:
        if result.returncode < 0:
            import signal
            try:
                sig_name = signal.Signals(-result.returncode).name
                print(f"ERROR: Command killed by signal {sig_name} ({-result.returncode})", file=sys.stderr)
            except ValueError:
                print(f"ERROR: Command killed by signal {-result.returncode}", file=sys.stderr)
        else:
            print(f"ERROR: Command failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    return result


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
    env = os.environ.copy()
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
        description="Running integration test suite"
    )

    print("\n✓ All integration tests passed")


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


def build_java_service(source_dir: Path, install_dir: Path, skip_tests: bool, clean: bool):
    """Build the Java admin service fat JAR and copy it to install_dir/lib/."""
    check_mvn()

    java_dir = source_dir / "java" / "admin-service"
    mvn_cmd = ["mvn"] + (["clean", "package"] if clean else ["package"])
    if skip_tests:
        mvn_cmd.append("-DskipTests")

    run_command(mvn_cmd, cwd=java_dir, description="Building Java admin service")

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


def build_fix_test_client(source_dir: Path, install_dir: Path, skip_tests: bool, clean: bool):
    """Build the fix-test-client fat JAR, install it and its config into the staging tree.

    The JAR is copied to install_dir/lib/fix-test-client.jar.
    Config files (app.toml, session.cfg) are copied to
    install_dir/etc/fix_test_client/config/ so that devenv.py can start the
    service with workdir=etc/fix_test_client and session_config=config/session.cfg
    resolves correctly against the working directory.
    """
    check_mvn()

    java_dir = source_dir / "java" / "fix-test-client"
    mvn_cmd = ["mvn"] + (["clean", "package"] if clean else ["package"])
    if skip_tests:
        mvn_cmd.append("-DskipTests")

    run_command(mvn_cmd, cwd=java_dir, description="Building fix-test-client")

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
        help='Skip running all tests (suppresses pytest and C++ tests; pylint still runs)'
    )

    parser.add_argument('--no-pytest', action='store_true',
        help='Skip the Python DSL test suite only (pylint still runs; C++ tests are unaffected)'
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
        help='Generate LCOV + genhtml coverage report after running tests')

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

    # Get source directory (parent of this script)
    source_dir = Path(__file__).parent.resolve()
    build_dir = source_dir / args.build_dir

    # Staging dir: CMake installs here after the build; release.py reads from here.
    # Fixed relative to the project root, not the build directory, so that all
    # tooling (ha_test.py, auth_service_test.py, devenv.py) finds binaries in the
    # same well-known location regardless of which --build-dir was used.
    staging_dir = (source_dir / "installed").resolve()

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
        if not args.no_tests and not args.no_pytest:
            run_pytest(source_dir)

        if args.clean:
            clean_build(build_dir)

        build_dir.mkdir(parents=True, exist_ok=True)

        configure_cmake(build_dir, source_dir, enable_valgrind=args.valgrind,
                        enable_coverage=args.coverage, enable_asan=args.asan,
                        enable_tsan=args.tsan, install_dir=staging_dir,
                        enable_doxygen=not args.no_doxygen, debug=args.debug)

        build_project(build_dir, jobs=args.jobs, verbose=args.verbose)

        if not args.no_tests:
            run_tests(build_dir, use_tsan=args.tsan, tsan_suppressions=args.tsan_suppressions)
            run_integration_tests(build_dir)

        if args.coverage_report:
            generate_coverage_report(build_dir, source_dir)

        install_project(build_dir, staging_dir)

        if args.doxygen and not args.no_doxygen:
            run_doxygen(source_dir)

    # ── Java build ────────────────────────────────────────────────────────────
    if not args.no_java:
        build_java_service(source_dir, staging_dir,
                           skip_tests=args.no_tests, clean=args.clean)
        build_fix_test_client(source_dir, staging_dir,
                              skip_tests=args.no_tests, clean=args.clean)


    print("\n" + "="*60)
    print("Build process completed successfully!")
    print("="*60)

    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nBuild interrupted by user", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        print(f"\n\nUnexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
