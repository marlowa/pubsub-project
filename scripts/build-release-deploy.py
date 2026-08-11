#!/usr/bin/env python3
"""Convenience wrapper: build (no tests) -> release -> deploy.

Aborts on the first failure; each stage is announced so it is clear
where a failure occurred.
"""

from __future__ import annotations
import argparse
import subprocess
import sys
from pathlib import Path


def step(label: str) -> None:
    print(f"\n=== {label} ===")


def run(cmd: list[str], description: str) -> None:
    step(description)
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        print(f"\nERROR: {description} failed with exit code {result.returncode}",
              file=sys.stderr)
        sys.exit(result.returncode)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build (no tests), release, and deploy the pubsub project."
    )
    parser.add_argument("--skip-db",       action="store_true", help="Pass --skip-db to deploy.py")
    parser.add_argument("--drop-db",       action="store_true", help="Pass --drop-db to deploy.py (drop and recreate DB)")
    parser.add_argument("--sudo-postgres", action="store_true", help="Pass --sudo-postgres to deploy.py")
    parser.add_argument("--debug",         action="store_true", help="Pass --debug to build.py (Debug build type)")
    parser.add_argument("--asan",          action="store_true", help="Pass --asan to build.py")
    parser.add_argument("--tsan",          action="store_true", help="Pass --tsan to build.py")
    parser.add_argument("--valgrind",      action="store_true", help="Pass --valgrind to build.py")
    parser.add_argument("--no-pylint",     action="store_true", help="Pass --no-pylint to build.py")
    parser.add_argument("--no-cpp",        action="store_true", help="Pass --no-cpp to build.py")
    parser.add_argument("--no-doxygen",    action="store_true", help="Pass --no-doxygen to build.py")
    args = parser.parse_args()

    if args.asan and args.tsan:
        sys.exit("error: --asan and --tsan are mutually exclusive")
    if args.valgrind and (args.asan or args.tsan):
        sys.exit("error: --valgrind cannot be combined with --asan or --tsan")

    import build  # noqa: PLC0415  -- deferred: only needed to resolve platform names

    script_dir = Path(__file__).resolve().parent
    # The tree above scripts/. Sibling scripts are reached through script_dir;
    # everything the project owns -- release/, installed/ -- hangs off project_root.
    project_root = script_dir.parent
    # Platform-qualified, matching build.py and release.py. Unqualified is wrong here
    # twice over: this script deletes the directory before building, so run in the Rocky
    # container -- which bind-mounts the repository -- it would remove the host's tree,
    # then deploy gcc-8.5 binaries in its place.
    install_dir = project_root / ("installed" + build.platform_suffix())
    release_dir = project_root / "release"

    # ── Clean ──────────────────────────────────────────────────────────────────
    if args.no_cpp:
        print(f"NOTE: --no-cpp set; skipping clean of {install_dir}")
    else:
        print(f"cleaning out {install_dir}")
        import shutil
        shutil.rmtree(install_dir, ignore_errors=True)

    # ── Build ──────────────────────────────────────────────────────────────────
    build_args = [sys.executable, str(script_dir / "build.py"), "--no-tests"]
    if args.debug:
        build_args.append("--debug")
    if args.asan:
        build_args.append("--asan")
    if args.tsan:
        build_args.append("--tsan")
    if args.valgrind:
        build_args.append("--valgrind")
    if args.no_pylint:
        build_args.append("--no-pylint")
    if args.no_cpp:
        build_args.append("--no-cpp")
    if args.no_doxygen:
        build_args.append("--no-doxygen")
    run(build_args, "BUILD")

    # ── Release ────────────────────────────────────────────────────────────────
    mode = "debug" if args.debug else "release"
    sanitizer = "asan" if args.asan else ("tsan" if args.tsan else ("valgrind" if args.valgrind else "none"))
    release_args = [sys.executable, str(script_dir / "release.py"), "--mode", mode]
    if sanitizer != "none":
        release_args += ["--sanitizer", sanitizer]
    run(release_args, "RELEASE")

    # Filtered by platform, not just newest: release/ is shared with the Rocky container
    # through a bind mount, so a gcc-8.5 artefact lands beside the host's and is often
    # the newer of the two.
    tarballs = sorted((p for p in release_dir.glob("pubsub-*.tar.gz")
                       if build.artefact_belongs_to_this_platform(p.name)),
                      key=lambda p: p.stat().st_mtime, reverse=True)
    if not tarballs:
        print(f"ERROR: no release tarball for this platform found in {release_dir}", file=sys.stderr)
        return 1
    tarball = tarballs[0]
    print(f"  tarball: {tarball}")

    # ── Deploy ─────────────────────────────────────────────────────────────────
    deploy_args = [sys.executable, str(script_dir / "deploy.py"), "--artefact", str(tarball),
                   "--install-dir", str(install_dir)]
    if args.skip_db:
        deploy_args.append("--skip-db")
    if args.drop_db:
        deploy_args.append("--drop-db")
    if args.sudo_postgres:
        deploy_args.append("--sudo-postgres")
    run(deploy_args, "DEPLOY")

    step("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
