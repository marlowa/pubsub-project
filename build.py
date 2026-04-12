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
        "--output-file", str(filtered_info),
        "--ignore-errors", "mismatch"
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

def run_command(cmd, cwd=None, description=None, env=None):
    """Run a shell command and handle errors"""
    if description:
        print(f"\n{'='*60}")
        print(f"{description}")
        print(f"{'='*60}")

    print(f"Running: {' '.join(cmd) if isinstance(cmd, list) else cmd}")

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
        'GOOGLETEST_VERSION'
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
        description="Generating Doxygen documentation"
    )

    print("\n✓ Doxygen documentation generated successfully")


def configure_cmake(build_dir, source_dir, enable_valgrind=False, enable_coverage=False,
                    enable_asan=False, enable_tsan=False):
    cmake_args = [
        "cmake",
        str(source_dir)
    ]

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
    """Build the project using make"""
    if jobs is None:
        # Get number of CPU cores
        try:
            import multiprocessing
            jobs = multiprocessing.cpu_count()
        except:
            jobs = 4

    make_cmd = ["make", f"-j{jobs}"]
    if verbose:
        make_cmd.append("VERBOSE=1")

    run_command(
        make_cmd,
        cwd=build_dir,
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


def clean_build(build_dir):
    """Remove build directory"""
    if build_dir.exists():
        print(f"Removing build directory: {build_dir}")
        shutil.rmtree(build_dir)
        print("✓ Build directory cleaned")
    else:
        print(f"Build directory does not exist: {build_dir}")


def main():
    parser = argparse.ArgumentParser(
        description="Build script for pubsub_itc_fw_project",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                          # Normal build and test
  %(prog)s --clean                  # Clean and rebuild
  %(prog)s --valgrind               # Build with Valgrind compatibility
  %(prog)s --doxygen                # Build and generate docs
  %(prog)s --doxygen-only           # Only generate documentation
  %(prog)s --no-tests               # Build without running tests
  %(prog)s --clean --valgrind       # Clean build for Valgrind
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

    parser.add_argument('--no-tests', action='store_true',
        help='Skip running tests after building'
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

    args = parser.parse_args()

    # Get source directory (parent of this script)
    source_dir = Path(__file__).parent.resolve()
    build_dir = source_dir / args.build_dir

    # Verify environment variables
    check_environment_variables()

    # Sanitizer mutual exclusion checks
    if args.asan and args.tsan:
        print("ERROR: --asan and --tsan are mutually exclusive", file=sys.stderr)
        sys.exit(1)

    if args.valgrind and (args.asan or args.tsan):
        print("ERROR: --valgrind cannot be combined with --asan or --tsan", file=sys.stderr)
        sys.exit(1)

    # Handle doxygen-only mode
    if args.doxygen_only:
        run_doxygen(source_dir)
        return 0

    # Clean if requested
    if args.clean:
        clean_build(build_dir)

    # Create build directory
    build_dir.mkdir(parents=True, exist_ok=True)

    # Configure
    configure_cmake(build_dir, source_dir, enable_valgrind=args.valgrind,
                    enable_coverage=args.coverage, enable_asan=args.asan, enable_tsan=args.tsan)

    # Build
    build_project(build_dir, jobs=args.jobs, verbose=args.verbose)

    # Run tests unless disabled
    if not args.no_tests:
        run_tests(build_dir, use_tsan=args.tsan, tsan_suppressions=args.tsan_suppressions)
        run_integration_tests(build_dir)

    if args.coverage_report:
        generate_coverage_report(build_dir, source_dir)

    # Generate Doxygen if requested
    if args.doxygen:
        run_doxygen(source_dir)


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
