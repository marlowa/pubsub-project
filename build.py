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


def platform_tag():
    """Short name for the target platform, or None when this is the ordinary dev host.

    Used to keep build output for a different toolchain apart. A gcc-8.5 Rocky/RHEL8
    build is a different artefact from a gcc-13 host build in exactly the way a
    coverage or sanitizer build is: same sources, incompatible objects. Sharing a
    directory means every platform switch is a full rebuild at best, and a confusing
    mixture of stale objects and a stale CMake cache at worst.
    """
    try:
        text = Path('/etc/os-release').read_text()
        lines = {l.split('=')[0]: l.split('=', 1)[1] for l in text.splitlines() if '=' in l}
        identifier = lines.get('ID', '').strip('"')
        version = lines.get('VERSION_ID', '').strip('"')
    except OSError:
        return None
    if identifier in ('rhel', 'rocky', 'centos') and version.startswith('8'):
        return 'rhel8' if identifier == 'rhel' else 'rocky8'
    return None


# Every value platform_tag() can return. Needed to recognise another platform's
# artefact by name, which the ordinary dev host cannot do from its own tag alone
# because its tag is None.
known_platform_tags = ('rhel8', 'rocky8')


def platform_suffix():
    """The suffix that distinguishes this platform's build products from another's.

    Empty on the ordinary dev host, so its names are unchanged and only a
    cross-compiled build is qualified -- the same rule the staging directory follows,
    where the host keeps installed/ and Rocky gets installed-rocky8/.
    """
    tag = platform_tag()
    return f"-{tag}" if tag else ""


def artefact_belongs_to_this_platform(artefact_name):
    """Whether a release tarball was built for the platform running this code.

    A release artefact is not portable between them: a gcc-8.5 tree links against an
    older glibc and carries an RPATH naming the build machine's third-party tree.
    Deploying the wrong one produces binaries that die at startup with exit 127.

    Selecting "the newest tarball" cannot tell them apart, because the release
    directory is shared -- the Rocky container writes into a bind mount of the same
    repository, so its artefact lands beside the host's and is usually newer.

    @param[in] artefact_name File name, with or without the .tar.gz suffix.
    @return True if this artefact is for the current platform.
    """
    stem = artefact_name[:-len('.tar.gz')] if artefact_name.endswith('.tar.gz') else artefact_name
    tag = platform_tag()
    if tag:
        return stem.endswith(f"-{tag}")
    return not any(stem.endswith(f"-{other}") for other in known_platform_tags)


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


# pylint's exit status is a bitmask of the categories it found, not a pass/fail: 1 fatal,
# 2 error, 4 warning, 8 refactor, 16 convention, 32 usage error.
PYLINT_NON_ERROR_EXIT_BITS = 4 | 8 | 16


def run_pylint(source_dir):
    """Run pylint on the Python DSL, the FIX dictionary generator, and the top-level scripts.

    Two invocations, at two standards, deliberately.

    The DSL and dictionary generator are held at 10.00/10, style included: they are library
    code, they are read as much as run, and they are small enough for that to be free.

    The top-level scripts are checked for ERRORS ONLY. They were checked for nothing at all
    until 2026-08-09, because the invocation above names two package directories and every
    script in the repository root falls outside them -- which is how an undefined variable
    lived in perf_run.py's FIX path long enough for a release check to find it. These are the
    scripts that deploy the venue and run the tests, so a defect that stops one of them
    outright matters far more than its line lengths, and errors-only is the bar they can pass
    today. Raising it means fixing the style first, which is worth doing and is not urgent.

    Globbed rather than listed. A hard-coded list is a list that goes stale, and a new script
    would silently inherit the very gap this closes.

    import-error is excluded, and that exclusion is load-bearing. It reports what is INSTALLED
    rather than what the code says: pubsub_metrics.py imports requests, matplotlib and tkinter,
    none of which exist in the Rocky container, so enabling it makes a C++ toolchain check fail
    over optional visualisation packages. The checks worth having here -- undefined-variable
    among them -- are properties of the source and hold in any environment.

    The errors-only run tolerates a non-error exit status, because asking for errors does not
    guarantee that only errors are reported. A stale entry in an inline disable comment is
    reported as useless-option-value (R0022) whatever the message filter says, since it is the
    filter itself that is being complained about -- and that stopped the RHEL8 build of 0.3.0
    on a newer pylint than the one the code was written against, over two disables naming
    bad-whitespace, a check removed from pylint years ago. An error still fails the gate; a
    check retired by a future pylint no longer does.
    """
    python_dir = source_dir / "python"
    run_command(
        [sys.executable, "-m", "pylint", "dsl", "fix_dictionary"],
        cwd=python_dir,
        description="Running pylint on Python DSL and FIX dictionary source"
    )

    top_level_scripts = sorted(path.name for path in source_dir.glob("*.py"))
    if top_level_scripts:
        run_command(
            [sys.executable, "-m", "pylint", "--disable=all", "--enable=E",
             "--disable=import-error", *top_level_scripts],
            cwd=source_dir,
            description=f"Running pylint (errors only) on {len(top_level_scripts)} top-level scripts",
            tolerated_exit_bits=PYLINT_NON_ERROR_EXIT_BITS
        )

    print("\n✓ pylint passed")


def check_scripts_support_help(source_dir):
    """Every top-level script must answer --help without failing.

    Cheap, and it catches two things nothing else does. A script whose argparse setup throws
    only discovers it when someone runs it, which for release.py or deploy.py means at the
    worst possible moment. And --help is where a module docstring surfaces: perf_run.py
    carried its whole usage block in a triple-quoted string that was not the docstring at
    all, because a `from __future__` import preceded it, so `description=__doc__` passed
    None and --help showed nothing. Eleven scripts had the same fault.

    Running each one is safe only because --help exits before any of them does work. A script
    that acts at import time would be exercised by this check, which is itself worth knowing.
    """
    scripts = sorted(path.name for path in source_dir.glob("*.py"))
    print(f"\n=== Checking --help on {len(scripts)} top-level scripts ===")
    broken, skipped = [], []
    for name in scripts:
        try:
            result = subprocess.run([sys.executable, name, "--help"], cwd=source_dir,
                                    capture_output=True, timeout=60, check=False)
        except subprocess.TimeoutExpired:
            broken.append((name, "timed out -- does it do work before parsing arguments?"))
            continue
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).decode(errors="replace").strip()
            last = detail.splitlines()[-1] if detail else f"exited {result.returncode}"
            # A script that cannot import an OPTIONAL third-party package is reported and
            # not failed. The Rocky container has no matplotlib or psutil, so demanding it
            # would make a C++ toolchain check depend on which visualisation libraries a
            # machine happens to carry -- the same fault as leaving import-error enabled
            # above. That these scripts cannot describe themselves without their plotting
            # dependencies is a real defect, recorded in docs/bug_list.md; the fix is to
            # import lazily, as pubsub_metrics.py already does so it can run headless.
            if "ModuleNotFoundError" in detail or "Missing dependency" in detail:
                skipped.append((name, last))
            else:
                broken.append((name, last))

    for name, why in skipped:
        print(f"  SKIP {name}: {why}")

    if broken:
        print("\nERROR: these scripts do not support --help:", file=sys.stderr)
        for name, why in broken:
            print(f"  {name}: {why}", file=sys.stderr)
        sys.exit(1)
    print(f"\n\u2713 {len(scripts) - len(skipped)} of {len(scripts)} scripts answer --help"
          f"{f', {len(skipped)} skipped for missing optional packages' if skipped else ''}")


def run_pytest(source_dir, build_dir=None):
    """Run the Python DSL test suite.

    PUBSUB_BUILD_DIR tells the pybind11 test harness where to compile its extension
    modules. They must not go under the system temp directory -- /tmp is mounted
    noexec on the RHEL8 target, so the module builds and then fails to dlopen -- and
    they must follow --build-dir so a container run does not write into the host's
    build tree.
    """
    python_dir = source_dir / "python"
    environment = dict(os.environ)
    if build_dir is not None:
        environment["PUBSUB_BUILD_DIR"] = str(Path(build_dir).resolve())
    run_command(
        [sys.executable, "-m", "pytest", "-q"],
        cwd=python_dir,
        env=environment,
        description="Running Python DSL test suite"
    )
    print("\n✓ Python tests passed")


def rewrite_tracefile_for_genhtml(raw_info, clean_info):
    """
    Rewrite gcovr's lcov tracefile into something genhtml renders honestly.

    Three corrections, each of which was producing a misleading report:

    1. gcovr embeds a per-line source checksum (the third DA field) and a VER
       line. lcov 1.14's genhtml recomputes the checksum differently and aborts.

    2. Every PUBSUB_LOG call site emits an FMT_COMPILE_STRING lambda that gcov
       records as a function and never marks executed -- measured 2026-07-26 as 144
       records, every one uncovered, because they are uncoverable by construction.
       They wreck the per-file function figures: OutboundConnectionManager.cpp read
       40.5% while all 17 of its real member functions were being called, and
       TimerHandler.cpp read 25.0% with 3 of 3 called. The log *lines* are already
       dropped by --exclude-lines-by-pattern; these are the same artefact at the
       same call sites, so removing them is consistency rather than generosity.

    3. gcovr's LF/LH and FNF/FNH summary counters disagree with the per-line and
       per-function records for template-heavy headers -- ThreadWithJoinTimeout.hpp
       claimed LF:1145 for a 196-line file whose records hold 64 lines. genhtml
       recomputes from the records, so its HTML was right, but anything reading the
       tracefile directly is misled. Recompute the counters from what survives.

    @param[in]  raw_info    Tracefile as gcovr wrote it.
    @param[out] clean_info  Tracefile to hand to genhtml.
    """
    da_line = re.compile(r'^DA:(\d+),(\d+)')
    function_line = re.compile(r'^(FN|FNDA):[^,]+,(.+)$')
    function_data_line = re.compile(r'^FNDA:(\d+),(.+)$')
    function_name_line = re.compile(r'^FN:\d+,(.+)$')

    output = []
    record = []

    def flush_record():
        """Emit one source-file record with its counters recomputed."""
        if not record:
            return
        line_hits = {}
        function_hits = {}
        for entry in record:
            match = da_line.match(entry)
            if match is not None:
                number = int(match.group(1))
                line_hits[number] = line_hits.get(number, 0) + int(match.group(2))
                continue
            match = function_name_line.match(entry)
            if match is not None:
                function_hits.setdefault(match.group(1), 0)
                continue
            match = function_data_line.match(entry)
            if match is not None:
                name = match.group(2)
                function_hits[name] = function_hits.get(name, 0) + int(match.group(1))
        for entry in record:
            if entry.startswith(("LF:", "LH:", "FNF:", "FNH:")):
                continue
            output.append(entry)
        output.append(f"FNF:{len(function_hits)}\n")
        output.append(f"FNH:{sum(1 for count in function_hits.values() if count > 0)}\n")
        output.append(f"LF:{len(line_hits)}\n")
        output.append(f"LH:{sum(1 for count in line_hits.values() if count > 0)}\n")
        record.clear()

    for line in raw_info.read_text(encoding="utf-8").splitlines(keepends=True):
        if line.startswith("VER:"):
            continue
        match = function_line.match(line.rstrip("\n"))
        if match is not None and "FMT_COMPILE_STRING" in match.group(2):
            continue
        if line.startswith("end_of_record"):
            flush_record()
            output.append(line)
            continue
        # Drop gcovr's DA checksum, keeping line number and hit count.
        record.append(re.sub(r'^(DA:\d+,\d+),.*$', r'\1', line.rstrip("\n")) + "\n"
                      if line.startswith("DA:") else line)
    flush_record()

    clean_info.write_text("".join(output), encoding="utf-8")

    lines_found = sum(int(entry[3:]) for entry in output if entry.startswith("LF:"))
    lines_hit = sum(int(entry[3:]) for entry in output if entry.startswith("LH:"))
    functions_found = sum(int(entry[4:]) for entry in output if entry.startswith("FNF:"))
    functions_hit = sum(int(entry[4:]) for entry in output if entry.startswith("FNH:"))
    return lines_found, lines_hit, functions_found, functions_hit


def remove_orphaned_target_directories(build_dir, source_dir):
    """Delete build-tree directories whose source directory no longer exists.

    CMake mirrors the source layout into the build tree, but never tidies up after a
    source directory is renamed or removed: the old target directory, its CMakeFiles/ and
    every .gcno inside it stay behind indefinitely. applications/binary_gateway/ was
    renamed to applications/binary_order_gateway/ and its build tree sat there afterwards.

    That is fatal to coverage specifically. gcovr searches the whole build tree for
    coverage data before the report-level --exclude filters are applied, so it finds the
    orphan's .gcno, finds no .gcda beside it, cannot resolve the compilation directory the
    notes record, and searches upward -- ending at / , where it cannot write .gcov files
    and aborts the entire run. The visible symptom is hundreds of "Could not open output
    file" lines naming standard library headers, which says nothing whatever about the
    cause.

    Deleting them is safe: with no source directory there is no target, so nothing in the
    current build can refer to these files. They are not rebuilt, only left behind.

    @param[in] build_dir  Build tree to tidy.
    @param[in] source_dir Source tree it mirrors.
    @return Number of directories removed.
    """
    removed = 0
    for cmake_files in sorted(build_dir.rglob("CMakeFiles")):
        if not cmake_files.is_dir():
            continue
        mirrored = cmake_files.parent
        if mirrored == build_dir:
            continue  # the build root mirrors the source root, which always exists
        relative = mirrored.relative_to(build_dir)
        if (source_dir / relative).is_dir():
            continue
        print(f"  removing orphaned build directory {relative} "
              f"(no {relative} in the source tree -- renamed or deleted)")
        shutil.rmtree(mirrored, ignore_errors=True)
        removed += 1
    return removed


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

    # Before gcovr searches the tree, not after: an orphan left by a renamed source
    # directory aborts the whole capture, and the report-level excludes come too late to
    # prevent it. See remove_orphaned_target_directories.
    remove_orphaned_target_directories(build_dir, source_dir)
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
        # Benchmark and bench-harness mains. These are main() functions of
        # standalone performance executables, never run by the test suites, so they
        # sit permanently at 0% and only depress the figure -- same reasoning as
        # tests/ and applications/ above.
        r"(.*/)?performance/.*",
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
        "--lcov", str(raw_info),
    ]
    for pattern in excludes:
        cmd += ["--exclude", pattern]

    run_command(cmd, description="Capturing coverage with gcovr")

    lines_found, lines_hit, functions_found, functions_hit = rewrite_tracefile_for_genhtml(raw_info, clean_info)
    # gcovr's own --print-summary counts each template instantiation as separate
    # lines, so it reports a different (larger) denominator than the rendered
    # report. Print what the HTML shows, so there is one number to quote.
    print(f"  lines     {lines_hit}/{lines_found} = {100.0 * lines_hit / lines_found:.1f}%")
    print(f"  functions {functions_hit}/{functions_found} = {100.0 * functions_hit / functions_found:.1f}%")

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

def run_command(cmd, cwd=None, description=None, env=None, quiet=False, stdin_text=None,
                tolerated_exit_bits=0):
    """Run a shell command, streaming output in real time while capturing it.

    On failure, prints the captured output (quiet mode) or the last 30 lines
    (non-quiet mode, where the full output already streamed to the terminal)
    inside the error banner so the cause is clearly visible.

    @param[in] stdin_text Text piped to the command's stdin, for tools configured that
                          way (doxygen reads its configuration from stdin as "doxygen -").
    @param[in] tolerated_exit_bits Bits that do not count as failure, for a tool whose exit
                          code is a bitmask of finding categories rather than a pass/fail.
                          A non-zero status made up only of these bits is a pass.
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
        stdin=subprocess.PIPE if stdin_text is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    if stdin_text is not None:
        process.stdin.write(stdin_text)
        process.stdin.close()

    lines = []
    for line in process.stdout:
        if not quiet:
            sys.stdout.write(line)
            sys.stdout.flush()
        lines.append(line)
    process.wait()

    tolerated = (process.returncode > 0 and tolerated_exit_bits != 0
                 and (process.returncode & ~tolerated_exit_bits) == 0)

    if process.returncode != 0 and not tolerated:
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
        'PROMETHEUS_VERSION',
    ]

    missing = [var for var in required_vars if var not in os.environ]

    if missing:
        print("ERROR: Missing required environment variables:", file=sys.stderr)
        for var in missing:
            print(f"  - {var}", file=sys.stderr)
        sys.exit(1)


def run_doxygen(source_dir, build_dir=None):
    """Run Doxygen to generate documentation.

    Doxyfile hardcodes OUTPUT_DIRECTORY = build/docs, which ignores --build-dir and so
    writes a Rocky container's docs over the host's. Rather than edit the Doxyfile --
    which would break a plain `doxygen Doxyfile` run by hand -- the config is piped in
    with an override appended. In a Doxygen config the last assignment of a key wins.
    """
    doxyfile = source_dir / "Doxyfile"

    if not doxyfile.exists():
        print(f"ERROR: Doxyfile not found at {doxyfile}", file=sys.stderr)
        print("Please create a Doxyfile in your project root", file=sys.stderr)
        sys.exit(1)

    configuration = doxyfile.read_text()
    if build_dir is not None:
        configuration += f"\nOUTPUT_DIRECTORY = {Path(build_dir).resolve() / 'docs'}\n"

    run_command(
        ["doxygen", "-"],
        cwd=source_dir,
        description="Generating Doxygen documentation",
        quiet=_is_rhel8(),
        stdin_text=configuration,
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
    # Always explicit. Passing only the OFF case made this a one-way switch: CMake
    # cache entries persist, so a single --no-doxygen run left ENABLE_DOXYGEN=OFF in
    # the cache and silently disabled the install-time documentation build from then
    # on, however many times it was rebuilt without the flag.
    cmake_args.append(f"-DENABLE_DOXYGEN={'ON' if enable_doxygen else 'OFF'}")
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


def flavour_suffix(args):
    """
    Directory suffix naming this build's instrumentation flavour, or "" for a plain build.

    The single place the flavour is decided, because the build directory and the staging
    directory must agree about it. They were derived separately once, and only the staging
    one was automatic: a coverage build whose caller forgot --build-dir put -O0 --coverage
    -fno-inline objects straight into build/, on top of the plain build's, while dutifully
    staging them to installed-coverage/.

    @param[in] args Parsed command-line arguments.
    @return Suffix beginning with "-", or an empty string.
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
    tag = platform_tag()
    if tag:
        flavours.append(tag)
    return "-" + "-".join(flavours) if flavours else ""


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
    return "installed" + flavour_suffix(args)


def build_directory_name(args):
    """
    Name of the build directory for this build's instrumentation flavour.

    This is about time and hygiene, NOT correctness. Sharing one directory between
    flavours is already known to be correct here: every object rule carries flags.make as
    a prerequisite, so changing flavour changes that file and every object is rebuilt.
    That was measured on 2026-07-26 and is written up in docs/building.md -- do not
    justify this function by claiming a mixture of instrumented and plain objects, which
    is the plausible-sounding thing that does not actually happen.

    What it buys is that forgetting the flag no longer costs anything. Before, a coverage
    build without --build-dir correctly rebuilt every object in build/ as instrumented,
    and the next plain build correctly rebuilt them all back -- two full rebuilds, for a
    mistake with no visible symptom. Per-flavour directories keep each flavour's objects
    warm, which is exactly the use docs/building.md already recommends --build-dir for;
    this only makes it the default rather than something to remember.

    Only used when the caller did not name a directory. An explicit --build-dir is obeyed
    verbatim, because callers that pass one are separating builds on an axis this function
    knows nothing about -- release_check.py uses build-rocky for its container run.

    @param[in] args Parsed command-line arguments.
    @return Directory name, relative to the project root.
    """
    return "build" + flavour_suffix(args)


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
  %(prog)s --no-pylint                        # Skip pylint on all project Python
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
        help='Skip pylint: the Python DSL and FIX dictionary source, and the top-level scripts'
    )

    parser.add_argument('--jobs', '-j', type=int, metavar='N',
        help='Number of parallel build jobs (default: number of CPU cores)'
    )

    parser.add_argument('--verbose', '-v', action='store_true',
        help='Show compiler and linker command lines during build'
    )

    parser.add_argument('--build-dir', type=Path, default=None,
        help='Build directory path. Defaults to ./build for a plain build and to '
             './build-<flavour> for an instrumented one (build-coverage, build-asan, '
             'build-tsan, build-valgrind, plus a platform tag off the ordinary dev host) '
             'because those objects cannot share a directory with the plain ones. '
             'An explicit value is used verbatim'
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
    # An explicit --build-dir wins; otherwise the flavour names it, so that forgetting the
    # flag cannot drop instrumented objects into the plain build's directory.
    build_dir = source_dir / (args.build_dir if args.build_dir is not None
                              else build_directory_name(args))
    if args.build_dir is None and build_dir.name != "build":
        print(f"NOTE: instrumented build -- building in {build_dir.name}/ instead of build/")
        print("  Sharing build/ would still be CORRECT (CMake rebuilds every object when the")
        print("  flavour changes -- see docs/building.md), but it costs a full rebuild in each")
        print("  direction every time you switch. Each flavour now keeps its own objects warm.")
        print("  Pass --build-dir explicitly to override this.")

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
            run_doxygen(source_dir, build_dir)
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
            check_scripts_support_help(source_dir)
        else:
            print("NOTE: --no-pylint is set; skipping pylint")
        if not skip_pytest:
            run_pytest(source_dir, build_dir)

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
            run_doxygen(source_dir, build_dir)

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
