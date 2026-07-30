#!/usr/bin/env python3
"""
release_check.py -- everything that must pass before a release is tagged.

This is deliberately NOT a normal build. It is slower, stricter and wider than
devsetup.sh, and it is expected to be run once, by hand, immediately before
tagging.

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

    thirdparty = args.thirdparty
    if thirdparty is None:
        return False, ("--thirdparty is required for the Rocky stage: the container needs a\n"
                       "third-party tree built for Rocky 8. A tree built on the development\n"
                       "host compiles but fails to link.")
    thirdparty = Path(thirdparty).resolve()
    if not thirdparty.is_dir():
        return False, f"third-party tree not found: {thirdparty}"

    command = [
        "docker", "run", "--rm", "--entrypoint", "bash",
        "-v", f"{PROJECT_ROOT}:{CONTAINER_WORKSPACE}",
        "-v", f"{thirdparty}:{CONTAINER_WORKSPACE}/thirdparty",
        ROCKY_IMAGE,
        "-lc",
        # The image ships no Java or Maven, so those are skipped here and covered
        # by the local stage instead. Everything else runs.
        f"cd {CONTAINER_WORKSPACE} && ./build.sh --clean --no-java",
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
    """A performance smoke run -- proves the pipeline carries load, not a benchmark.

    Note this measures nothing trustworthy unless the hot-path cores are quiet;
    see cpu_audit.py --strict.
    """
    code, out = run([sys.executable, "perf_run.py", "--burst", "1", "--clients", "1"],
                    timeout=args.test_timeout)
    return code == 0, tail(out, 30)


STAGES = [
    ("git-clean", stage_git_clean, "working tree has no uncommitted tracked changes"),
    ("version", stage_version_consistency, "CMakeLists, README x2 and CHANGELOG agree"),
    ("standards", stage_standards, "check_standards.py"),
    ("build-local", stage_build_local, "full clean build, nothing skipped"),
    ("rocky", stage_rocky, "build and test under RHEL8's gcc 8.5 in the Rocky container"),
    ("deploy", stage_deploy, "deployment scripts present and runnable"),
    ("ha", stage_ha, "every ha_test scenario"),
    ("perf", stage_perf, "performance smoke run"),
]

_DEFAULT_SKIPPED = {"ha", "perf"}


def select_stages(args) -> list:
    """Resolve --only / --skip / --all into the ordered list of stages to run."""
    selected = []
    for name, function, desc in STAGES:
        if args.only and name not in args.only:
            continue
        if name in args.skip:
            continue
        if not args.only and not args.all and name in _DEFAULT_SKIPPED:
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
    if "rocky" not in {r.name for r in results}:
        print("NOTE: the rocky stage did not run, so the RHEL8 toolchain is unverified.")
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
                        help="Rocky 8-built third-party tree, mounted into the container. "
                             "Required by the rocky stage.")
    parser.add_argument("--only", metavar="STAGE", action="append",
                        help="Run only this stage; repeatable.")
    parser.add_argument("--skip", metavar="STAGE", action="append", default=[],
                        help="Skip this stage; repeatable.")
    parser.add_argument("--all", action="store_true",
                        help="Also run the stages skipped by default "
                             f"({', '.join(sorted(_DEFAULT_SKIPPED))}), "
                             "which need a deployed environment.")
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
            default = " (skipped unless --all)" if name in _DEFAULT_SKIPPED else ""
            print(f"  {name:<14} {desc}{default}")
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
