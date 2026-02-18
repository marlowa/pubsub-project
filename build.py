#!/usr/bin/env python3
"""
Build script for pubsub_itc_fw_project
Supports normal builds, Valgrind-compatible builds, and Doxygen generation
"""

import argparse
import subprocess
import sys
import os
import shutil
from pathlib import Path


def run_command(cmd, cwd=None, description=None):
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
        check=False
    )

    if result.returncode != 0:
        print(f"ERROR: Command failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    return result


def check_environment_variables():
    """Verify required environment variables are set"""
    required_vars = [
        'THIRDPARTY_DIR',
        'PROTOBUF_VERSION',
        'ABSEIL_VERSION',
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


def configure_cmake(build_dir, source_dir, enable_valgrind=False):
    """Configure CMake with appropriate flags"""
    cmake_args = [
        "cmake",
        str(source_dir)
    ]

    if enable_valgrind:
        cmake_args.append("-DENABLE_VALGRIND=ON")
        print("NOTE: Building with Valgrind compatibility")
        print("  - Lock-free optimizations disabled (-mcx16 -march=x86-64-v2)")
        print("  - USING_VALGRIND macro defined")

    run_command(
        cmake_args,
        cwd=build_dir,
        description="Configuring CMake"
    )


def build_project(build_dir, jobs=None):
    """Build the project using make"""
    if jobs is None:
        # Get number of CPU cores
        try:
            import multiprocessing
            jobs = multiprocessing.cpu_count()
        except:
            jobs = 4

    run_command(
        ["make", f"-j{jobs}"],
        cwd=build_dir,
        description=f"Building project (using {jobs} cores)"
    )

    print("\n✓ Build completed successfully")


def run_tests(build_dir):
    """Run the test suite"""
    test_binary = build_dir / "libraries" / "pubsub_itc_fw" / "tests" / "pubsub_itc_fw_tests"

    if not test_binary.exists():
        print(f"ERROR: Test binary not found at {test_binary}", file=sys.stderr)
        sys.exit(1)

    run_command(
        [str(test_binary)],
        cwd=build_dir,
        description="Running test suite"
    )

    print("\n✓ All tests passed")


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

    parser.add_argument(
        '--clean',
        action='store_true',
        help='Clean build directory before building'
    )

    parser.add_argument(
        '--valgrind',
        action='store_true',
        help='Build with Valgrind compatibility (disables lock-free optimizations)'
    )

    parser.add_argument(
        '--doxygen',
        action='store_true',
        help='Generate Doxygen documentation after building'
    )

    parser.add_argument(
        '--doxygen-only',
        action='store_true',
        help='Only generate Doxygen documentation (skip build)'
    )

    parser.add_argument(
        '--no-tests',
        action='store_true',
        help='Skip running tests after building'
    )

    parser.add_argument(
        '--jobs', '-j',
        type=int,
        metavar='N',
        help='Number of parallel build jobs (default: number of CPU cores)'
    )

    parser.add_argument(
        '--build-dir',
        type=Path,
        default=Path('build'),
        help='Build directory path (default: ./build)'
    )

    args = parser.parse_args()

    # Get source directory (parent of this script)
    source_dir = Path(__file__).parent.resolve()
    build_dir = source_dir / args.build_dir

    # Verify environment variables
    check_environment_variables()

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
    configure_cmake(build_dir, source_dir, enable_valgrind=args.valgrind)

    # Build
    build_project(build_dir, jobs=args.jobs)

    # Run tests unless disabled
    if not args.no_tests:
        run_tests(build_dir)

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
