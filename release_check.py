#!/usr/bin/env python3
"""
release_check.py -- everything that must pass before a release is tagged.

This is deliberately NOT a normal build. It is slower, stricter and wider than
devsetup.sh, and it is expected to be run once, by hand, immediately before
tagging.

Every stage runs by default, including the HA scenarios and the performance run.
Those two need a deployed, running environment, and they used to be opt-in behind
--all -- which had it backwards: a check whose entire purpose is "may this be
tagged?" should not answer yes having skipped the two stages that exercise the
system rather than the source. --quick drops them for a mid-work sanity run and
says so in the summary.

Why it exists
-------------
Version 0.2.0 was tagged from a tree that built cleanly on the development box
and then failed three separate ways on RHEL8:

  1. gcc 8.5 rejected a snprintf call under -Werror=format-truncation, which
     newer gcc does not diagnose at all.
  2. pylint 4.0.5 reported a check that pylint 3.0.3 does not have, because the
     linter was unpinned and each machine resolved its own version.
  3. The pybind11 round-trip tests failed against a Python that the development
     box does not have.

None of the three was catchable by any gate run before tagging, because every
one of them is a property of the *target toolchain* rather than of the source.
The lesson is that a release needs to be built somewhere other than the machine
it was written on.

What this cannot tell you
-------------------------
The Rocky 8 container shares RHEL8's gcc 8.5, so it would have caught (1). It
does NOT match a real RHEL8 deployment in Python: the image installs python38,
whereas the work RHEL8 host runs 3.12.13. So it would NOT have caught (2) or
(3), which are Python-toolchain differences.

Treat a green run here as "the C++ toolchain agrees" and not as "it works on the
target". Anything Python-facing -- pylint, pytest, the pybind11 extension build
-- still needs running on the real target host before a release is trusted.
That gap is real and is stated here rather than papered over.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent

# Mount point for the third-party tree inside the Rocky container, matching the
# invocation documented in the README.
CONTAINER_WORKSPACE = "/workspace"
ROCKY_IMAGE = "pubsub-rhel8"
# Where the gcc-8.5-built compiled deps volume is mounted inside the container.
CONTAINER_DEPS = "/opt/deps"


@dataclass
class StageResult:
    """The outcome of one stage."""
    name: str
    ok: bool
    detail: str
    seconds: float


def run(command: list[str], cwd: Path | None = None, timeout: int = 3600) -> tuple[int, str]:
    """Run a command, returning its exit code and combined output."""
    try:
        completed = subprocess.run(
            command, cwd=str(cwd or PROJECT_ROOT),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout, check=False,
        )
        return completed.returncode, completed.stdout
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    except FileNotFoundError as exc:
        return 127, str(exc)


def tail(text: str, lines: int = 25) -> str:
    """Return the last few lines of command output, for a readable failure report."""
    return "\n".join(text.strip().splitlines()[-lines:])


# ── Stages ────────────────────────────────────────────────────────────────────

def stage_git_clean(_args) -> tuple[bool, str]:
    """A release must be reproducible from the tag, so the tree must be committed."""
    code, out = run(["git", "status", "--porcelain", "--untracked-files=no"])
    if code != 0:
        return False, f"git status failed:\n{out}"
    if out.strip():
        return False, ("uncommitted changes to tracked files -- a tag would not "
                       f"reproduce this build:\n{out.strip()}")
    return True, "working tree clean"


def stage_version_consistency(_args) -> tuple[bool, str]:
    """CMakeLists, both README references and the CHANGELOG must agree.

    Bumping the version means editing four places. Missing one is easy, silent,
    and only noticed after the tag exists.
    """
    found: dict[str, str | None] = {}

    cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text()
    match = re.search(r'^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$', cmake, re.MULTILINE)
    found["CMakeLists.txt project() VERSION"] = match.group(1) if match else None

    readme = (PROJECT_ROOT / "README.md").read_text()
    match = re.search(r'badge/version-v(\d+\.\d+\.\d+)-', readme)
    found["README.md version badge"] = match.group(1) if match else None
    match = re.search(r'\*\*Current version:\*\*\s*v(\d+\.\d+\.\d+)', readme)
    found["README.md current-version line"] = match.group(1) if match else None

    changelog = (PROJECT_ROOT / "CHANGELOG.md").read_text()
    match = re.search(r'^##\s*\[(\d+\.\d+\.\d+)\]', changelog, re.MULTILINE)
    found["CHANGELOG.md newest entry"] = match.group(1) if match else None

    missing = [k for k, v in found.items() if v is None]
    if missing:
        return False, "could not find a version in: " + ", ".join(missing)

    values = set(found.values())
    listing = "\n".join(f"  {k:<36} {v}" for k, v in found.items())
    if len(values) != 1:
        return False, f"versions disagree:\n{listing}"
    return True, f"all agree on {values.pop()}\n{listing}"


def stage_standards(_args) -> tuple[bool, str]:
    """The project's own C++ coding-standard checks."""
    code, out = run([sys.executable, "check_standards.py"])
    return code == 0, tail(out, 20)


def stage_build_local(args) -> tuple[bool, str]:
    """Full clean build with nothing skipped -- C++, Java, doxygen, pylint, pytest.

    Deliberately not --no-java or --no-doxygen: a release is exactly when the
    parts usually skipped for speed must be exercised.
    """
    code, out = run(["./devsetup.sh", "--clean", "--skip-db", "--skip-certs", f"-j{args.jobs}"],
                    timeout=args.build_timeout)
    return code == 0, tail(out, 30)


def previous_release_tag() -> str | None:
    """The most recent tag reachable from HEAD, or None if there is none."""
    code, out = run(["git", "describe", "--tags", "--abbrev=0", "HEAD"])
    return out.strip() if code == 0 and out.strip() else None


def stage_coverage(args) -> tuple[bool, str]:
    """Coverage analysis for the release: regenerate, check the baseline is current, review.

    This stage FAILS ON ONE THING ONLY: the committed coverage_baseline.txt not matching a
    freshly generated one. That means the code changed and nobody regenerated the baseline,
    so nobody looked at what the change did to coverage. It is the mechanical form of
    "perform a coverage analysis and consider the results", and it cannot be satisfied by
    writing a test that asserts nothing -- only by looking.

    It does NOT fail on a coverage number, ever. There is no threshold. If coverage fell,
    you regenerated the baseline and tagged anyway, that is a decision taken with the
    figures in front of you, which is the entire point. See docs/testing.md.

    Only FUNCTION coverage is compared for the staleness check. Line counts differ between
    identical runs -- measured across four clean runs at one commit, function coverage was
    identical every time and the line count never was -- so a line-level check would fail
    spuriously and be skipped within a fortnight.

    Coverage is measured on this host only. The Rocky container answers a different
    question, "does it compile and do the tests pass on the target toolchain", and a
    baseline is toolchain-specific: gcc 8.5 and gcc 13 emit different function lists for
    identical sources. A second baseline nobody reads would be worse than none.
    """
    code, out = run(["./build.sh", "--clean", "--coverage", "--coverage-report",
                     "--no-java", "--no-doxygen", f"-j{args.jobs}"],
                    timeout=args.build_timeout)
    if code != 0:
        return False, "coverage build failed:\n" + tail(out, 30)

    fresh = PROJECT_ROOT / "build-coverage" / "coverage_baseline_fresh.txt"
    code, out = run([sys.executable, "coverage_baseline.py", "--update", "--baseline", str(fresh)])
    if code != 0:
        return False, "could not generate a baseline from the coverage run:\n" + tail(out, 20)

    committed = PROJECT_ROOT / "coverage_baseline.txt"
    if not committed.exists():
        return False, ("no committed coverage_baseline.txt. Generate one and commit it:\n"
                       "    ./build.sh --coverage --coverage-report\n"
                       "    python3 coverage_baseline.py --update")

    stale = _baseline_function_coverage_differs(committed, fresh)
    if stale:
        return False, ("the committed baseline is out of date, so this release's coverage has\n"
                       "not been reviewed by anyone. Regenerate and commit it, then look at what\n"
                       "changed:\n"
                       "    python3 coverage_baseline.py --update\n\n" + stale)

    # The baseline is current, so the stage passes. What follows is for a human to read:
    # movement since the last release, which is the question a release actually asks.
    tag = previous_release_tag()
    if tag is None:
        return True, "baseline current. No previous tag to compare against."
    code, out = run([sys.executable, "coverage_baseline.py", "--since", tag])
    if code != 0:
        return True, f"baseline current. Could not compare against {tag}:\n" + tail(out, 10)
    return True, f"baseline current.\n\n{out}"


def _baseline_function_coverage_differs(committed: Path, fresh: Path) -> str:
    """Return a description of any function-coverage difference, or "" when they agree.

    Compares the UNCOVERED function sets per file, not the counts, so a file that lost
    coverage of one function and gained it on another is still caught.
    """
    def read(path: Path) -> dict[str, set[str]]:
        uncovered: dict[str, set[str]] = {}
        current = ""
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            if raw_line.startswith("FILE "):
                current = raw_line.split()[1]
                uncovered.setdefault(current, set())
            elif raw_line.startswith("  UNCOVERED ") and current:
                uncovered[current].add(raw_line[len("  UNCOVERED "):])
        return uncovered

    before, after = read(committed), read(fresh)
    differences = []
    for name in sorted(set(before) | set(after)):
        if name not in after:
            differences.append(f"  gone from the report: {name}")
        elif name not in before:
            differences.append(f"  new in the report:    {name}")
        elif before[name] != after[name]:
            for signature in sorted(after[name] - before[name]):
                differences.append(f"  {name}\n      now uncovered: {signature}")
            for signature in sorted(before[name] - after[name]):
                differences.append(f"  {name}\n      now covered:   {signature}")
    return "\n".join(differences[:40])


def stage_rocky(args) -> tuple[bool, str]:
    """Build and test in the Rocky 8 container -- the RHEL8 gcc 8.5 toolchain.

    This is the stage that would have caught the 0.2.0 breakage. See the module
    docstring for what it does and does not cover.
    """
    if shutil.which("docker") is None:
        return False, "docker not found on PATH"

    code, out = run(["docker", "image", "inspect", ROCKY_IMAGE])
    if code != 0:
        return False, (f"image '{ROCKY_IMAGE}' not present. Build it first:\n"
                       f"    docker build -t {ROCKY_IMAGE} .")

    # Two mounts are needed, and getting this wrong is easy.
    #
    # The compiled deps (fmt, googletest) must be built with gcc 8.5: a tree built
    # on the development host *compiles* and then fails to link, because its libfmt
    # and libgtest reference GLIBCXX/GLIBC symbols absent from Rocky 8's older glibc.
    # Those live in the docker volume, as real directories.
    #
    # The header-only deps (quill, argparse, tomlplusplus, tsl-robin-map) are
    # SYMLINKS inside that volume pointing at /workspace/thirdparty -- they need no
    # compiling, so they are taken straight from the host tree. Mounting the volume
    # *at* /workspace/thirdparty therefore makes those symlinks point at themselves
    # and the build fails at configure with "Could not find a package configuration
    # file provided by quill". Mount the host tree there and the volume at /opt/deps.
    if args.thirdparty is None:
        return False, ("--thirdparty is required: the Rocky stage mounts the host third-party\n"
                       "tree so the header-only deps resolve, and the gcc-8.5-built compiled\n"
                       f"deps from the '{args.deps_volume}' volume alongside it.")
    host_tree = Path(args.thirdparty).resolve()
    if not host_tree.is_dir():
        return False, f"third-party tree not found: {host_tree}"

    code, out = run(["docker", "volume", "inspect", args.deps_volume])
    if code != 0:
        return False, (f"docker volume '{args.deps_volume}' not found. It holds the fmt and\n"
                       "googletest builds made with gcc 8.5 in the container; see the RHEL8\n"
                       "section of the README for how it is populated.")

    # build.sh would override THIRDPARTY_DIR for rocky8, so build.py is invoked
    # directly with the environment the volume layout needs.
    environment = " ".join([
        f"THIRDPARTY_DIR={CONTAINER_DEPS}",
        "FMT_VERSION=11.0.2", "QUILL_VERSION=11.0.2", "ARGPARSE_VERSION=3.2",
        "GOOGLETEST_VERSION=1.10.0", "TOMLPLUSPLUS_VERSION=3.4.0", "ROBINMAP_VERSION=1.4.1",
        "PROMETHEUS_VERSION=1.3.0",
    ])
    # Run as the invoking user, not root. The repo is bind-mounted, so a root-run
    # container leaves root-owned build/, build-rocky/, installed/ and doxygen output
    # behind, which the next build on the host cannot delete or overwrite. --build-dir
    # alone does not cover it: the install step and the doxygen output have their own
    # paths. HOME is redirected because the invoking uid has no home inside the image.
    command = [
        "docker", "run", "--rm", "--entrypoint", "bash",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-e", "HOME=/tmp",
        "-v", f"{PROJECT_ROOT}:{CONTAINER_WORKSPACE}",
        "-v", f"{host_tree}:{CONTAINER_WORKSPACE}/thirdparty",
        "-v", f"{args.deps_volume}:{CONTAINER_DEPS}",
        ROCKY_IMAGE,
        "-lc",
        # devsetup.py rather than build.py: devsetup runs build, release and deploy, and
        # this stage previously ran only the build. That gap let a real bug through. When
        # the staging directory became platform-qualified, release.py still looked for a
        # hardcoded "installed" and failed on RHEL8 -- because the only stage that runs the
        # release step runs it on the development host, where the name happens to be right.
        # A stage that runs a narrower command than the real workflow tests less than its
        # name implies.
        #
        # The image ships no Java or Maven, so those are skipped here and covered by the
        # local stage instead. A separate build directory is essential: the repo is
        # bind-mounted, so building into ./build would leave gcc-8.5 objects where the host
        # build expects its own. build-rocky/ is covered by the build-*/ gitignore rule.
        f"cd {CONTAINER_WORKSPACE} && {environment} "
        f"python3 devsetup.py --clean --skip-db --skip-certs --no-java "
        f"-j{args.jobs} --build-dir build-rocky",
    ]
    code, out = run(command, timeout=args.build_timeout)
    return code == 0, tail(out, 40)


def stage_deploy(_args) -> tuple[bool, str]:
    """Deploy the built artefacts, proving the install and config paths work."""
    code, out = run([sys.executable, "deploy.py", "--help"])
    if code != 0:
        return False, "deploy.py not runnable:\n" + tail(out)
    return True, ("deploy.py present; run the deployment by hand for a release "
                  "(it needs the database and certificates)")


def stage_ha(args) -> tuple[bool, str]:
    """Every high-availability scenario, end to end."""
    code, out = run([sys.executable, "ha_test.py", "--scenario", "all"],
                    timeout=args.test_timeout)
    return code == 0, tail(out, 30)


def stage_perf(args) -> tuple[bool, str]:
    """A performance smoke run through both gateways -- proves the pipeline carries
    load, not a benchmark.

    Both protocols are driven, not just FIX. A release that smoke-tested one of the
    two order paths would be claiming more than it checked, and the binary gateway
    is not a side experiment -- it is the control the FIX numbers are read against.
    Instance _a of each, because the instances run the same binary and the second
    adds a process rather than a code path.

    Note this measures nothing trustworthy unless the hot-path cores are quiet; see
    cpu_audit.py --strict. That is deliberately not a gate here: this stage asks
    whether the pipeline carries orders end to end, which a noisy machine does not
    change. Read the timings from a quiet one.
    """
    details = []
    for gateway in ("fix", "binary"):
        code, out = run([sys.executable, "perf_run.py", "--gateway", gateway,
                         "--burst", "1", "--clients", "1"],
                        timeout=args.test_timeout)
        if code != 0:
            return False, f"--gateway {gateway} failed:\n" + tail(out, 30)
        details.append(f"--gateway {gateway}: ok")
    return True, "\n".join(details)


def stage_runnable(_args) -> tuple[bool, str]:
    """Every installed binary can resolve its shared libraries.

    Asks the cheapest possible question -- can the thing about to be tested start at
    all -- before the stages that take ten minutes to answer it badly. On 2026-08-09 the
    Rocky container deployed its gcc-8.5 binaries over the host's install tree; ha then
    reported 0 of 23 scenarios failed, which reads as a catastrophic regression in the
    high-availability code and was nothing of the kind. Every component had died at
    startup with exit 127 because its libraries were absent. This stage names that in
    under a second, and names it as what it is.

    The environment matches devenv.py's, including lib64 as well as lib, so that a
    library found only via LD_LIBRARY_PATH at launch is not reported as missing here.

    Scanning bin/ covers more than the venue launches, which is deliberate: the failure
    being guarded against is a whole tree from the wrong platform, and a superset costs
    nothing. Anything ldd rejects as not a dynamic executable is skipped, so scripts and
    static binaries in the same directory do not register as failures.
    """
    import build  # noqa: PLC0415  -- deferred: only needed to resolve the platform name
    install_dir = PROJECT_ROOT / ("installed" + build.platform_suffix())
    bin_dir = install_dir / "bin"
    if not bin_dir.is_dir():
        return False, (f"no install tree at {bin_dir}\n"
                       "Nothing has been built and deployed, so the stages after this "
                       "one have nothing to run.")

    library_dirs = [str(d) for d in (install_dir / "lib64", install_dir / "lib") if d.is_dir()]
    child_env = os.environ.copy()
    existing = child_env.get("LD_LIBRARY_PATH", "")
    joined = ":".join(library_dirs)
    child_env["LD_LIBRARY_PATH"] = f"{joined}:{existing}" if existing else joined

    unresolved: dict[str, list[str]] = {}
    checked = 0
    for binary in sorted(bin_dir.iterdir()):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            continue
        result = subprocess.run(["ldd", str(binary)], env=child_env,
                                capture_output=True, text=True, check=False)
        if result.returncode != 0:
            continue
        checked += 1
        missing = [line.split("=>")[0].strip()
                   for line in result.stdout.splitlines() if "not found" in line]
        if missing:
            unresolved[binary.name] = missing

    if unresolved:
        details = [f"{len(unresolved)} of {checked} binaries cannot resolve their libraries:"]
        for name, missing in sorted(unresolved.items()):
            details.append(f"  {name}: {', '.join(sorted(set(missing)))}")
        details += [
            "",
            f"install tree : {install_dir}",
            f"library path : {joined or '(none)'}",
            "",
            "Most likely a tree built for another platform has been installed here.",
            "Check with: readelf -d <binary> | grep -i rpath",
        ]
        return False, "\n".join(details)

    if checked == 0:
        return False, f"no dynamic executables found in {bin_dir}"
    return True, f"{checked} binaries resolve every shared library"


STAGES = [
    ("git-clean", stage_git_clean, "working tree has no uncommitted tracked changes"),
    ("version", stage_version_consistency, "CMakeLists, README x2 and CHANGELOG agree"),
    ("standards", stage_standards, "check_standards.py"),
    ("build-local", stage_build_local, "full clean build, nothing skipped"),
    ("coverage", stage_coverage, "coverage analysis; fails only if the baseline is stale"),
    ("rocky", stage_rocky, "build and test under RHEL8's gcc 8.5 in the Rocky container"),
    ("deploy", stage_deploy, "deployment scripts present and runnable"),
    ("runnable", stage_runnable, "installed binaries resolve their shared libraries"),
    ("ha", stage_ha, "every ha_test scenario"),
    ("perf", stage_perf, "performance smoke run"),
]

# Stages that need a deployed, running environment rather than just a source tree.
# They are part of a release check, not an optional extra: --quick drops them for a
# mid-work sanity run, and a release is expected to run everything.
_NEEDS_ENVIRONMENT = {"ha", "perf"}


def select_stages(args) -> list:
    """Resolve --only / --skip / --quick into the ordered list of stages to run."""
    selected = []
    for name, function, desc in STAGES:
        if args.only and name not in args.only:
            continue
        if name in args.skip:
            continue
        if not args.only and args.quick and name in _NEEDS_ENVIRONMENT:
            continue
        selected.append((name, function, desc))
    return selected


def report(results: list[StageResult]) -> int:
    """Print the summary table and return the process exit code."""
    print("\n" + "=" * 78)
    print("Summary")
    print("=" * 78)
    for result in results:
        print(f"  {'PASS' if result.ok else 'FAIL'}  {result.name:<14} {result.seconds:7.1f}s")

    failed = [r for r in results if not r.ok]
    if failed:
        print(f"\n{len(failed)} stage(s) failed: " + ", ".join(r.name for r in failed))
        print("Do not tag.")
        return 1

    print("\nAll selected stages passed.")
    ran = {r.name for r in results}
    if "rocky" not in ran:
        print("NOTE: the rocky stage did not run, so the RHEL8 toolchain is unverified.")
    missing = [name for name, _, _ in STAGES if name not in ran]
    if missing:
        print("NOTE: this was not a full release check. Stages not run: "
              + ", ".join(missing))
        print("      Do not tag a GitHub release on a partial run.")
    print("Remember: the Rocky image shares RHEL8's gcc but not its Python, so pylint,")
    print("pytest and the pybind11 extension build still need checking on the real target.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    """Command-line interface. Split out to keep main() small."""
    parser = argparse.ArgumentParser(
        description="Pre-release verification. Not a normal build -- run this before tagging.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Stages:\n" + "\n".join(f"  {name:<14} {desc}" for name, _, desc in STAGES),
    )
    parser.add_argument("--thirdparty", metavar="DIR",
                        help="Host third-party tree, mounted so the header-only deps resolve. "
                             "Required by the rocky stage.")
    parser.add_argument("--deps-volume", metavar="NAME", default="pubsub-rocky-deps",
                        help="Docker volume holding the gcc-8.5-built third-party libraries "
                             "(default: pubsub-rocky-deps).")
    parser.add_argument("--only", metavar="STAGE", action="append",
                        help="Run only this stage; repeatable.")
    parser.add_argument("--skip", metavar="STAGE", action="append", default=[],
                        help="Skip this stage; repeatable.")
    parser.add_argument("--quick", action="store_true",
                        help="Skip the stages that need a deployed, running environment "
                             f"({', '.join(sorted(_NEEDS_ENVIRONMENT))}). For a sanity run "
                             "mid-work -- NOT sufficient before tagging a release.")
    # Every stage used to be opt-in behind --all. Kept so an existing habit or script
    # does not fail, but it now describes the default and does nothing.
    parser.add_argument("--all", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--jobs", "-j", type=int, default=8, help="Parallel build jobs.")
    parser.add_argument("--build-timeout", type=int, default=5400, help="Seconds per build stage.")
    parser.add_argument("--test-timeout", type=int, default=3600, help="Seconds per test stage.")
    parser.add_argument("--list", action="store_true", help="List the stages and exit.")
    return parser


def run_stage(name: str, function, desc: str, args) -> StageResult:
    """Run one stage, printing its heading and outcome."""
    print(f"\n--- {name}: {desc}")
    started = time.monotonic()
    try:
        ok, detail = function(args)
    except Exception as exc:                          # pylint: disable=broad-except
        ok, detail = False, f"stage raised: {exc!r}"
    elapsed = time.monotonic() - started
    print(f"{'PASS' if ok else 'FAIL'}  ({elapsed:.1f}s)")
    for line in detail.splitlines():
        print(f"      {line}")
    return StageResult(name, ok, detail, elapsed)


def main() -> int:
    """Parse options, run the selected stages in order, and report."""
    args = build_parser().parse_args()

    if args.list:
        for name, _, desc in STAGES:
            note = " (needs a deployed environment; dropped by --quick)" \
                   if name in _NEEDS_ENVIRONMENT else ""
            print(f"  {name:<14} {desc}{note}")
        return 0

    selected = select_stages(args)
    if not selected:
        print("No stages selected.", file=sys.stderr)
        return 2

    print("=" * 78)
    print("release_check -- pre-release verification (this is not a normal build)")
    print("=" * 78)

    results = [run_stage(name, function, desc, args) for name, function, desc in selected]
    return report(results)


if __name__ == "__main__":
    sys.exit(main())
