#!/usr/bin/env python3
"""Convenience wrapper: build (no tests) -> release -> deploy.

Aborts on the first failure; each stage is announced so it is clear
where a failure occurred.
"""

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
    parser.add_argument("--no-pylint",     action="store_true", help="Pass --no-pylint to build.py")
    parser.add_argument("--no-cpp",        action="store_true", help="Pass --no-cpp to build.py")
    parser.add_argument("--no-doxygen",    action="store_true", help="Pass --no-doxygen to build.py")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    install_dir = script_dir / "installed"
    release_dir = script_dir / "build" / "release"

    # ── Clean ──────────────────────────────────────────────────────────────────
    if args.no_cpp:
        print(f"NOTE: --no-cpp set; skipping clean of {install_dir}")
    else:
        print(f"cleaning out {install_dir}")
        import shutil
        shutil.rmtree(install_dir, ignore_errors=True)

    # ── Build ──────────────────────────────────────────────────────────────────
    build_args = [sys.executable, str(script_dir / "build.py"), "--no-tests"]
    if args.no_pylint:
        build_args.append("--no-pylint")
    if args.no_cpp:
        build_args.append("--no-cpp")
    if args.no_doxygen:
        build_args.append("--no-doxygen")
    run(build_args, "BUILD")

    # ── Release ────────────────────────────────────────────────────────────────
    run([sys.executable, str(script_dir / "release.py")], "RELEASE")

    tarballs = sorted(release_dir.glob("pubsub-*.tar.gz"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not tarballs:
        print(f"ERROR: no release tarball found in {release_dir}", file=sys.stderr)
        return 1
    tarball = tarballs[0]
    print(f"  tarball: {tarball}")

    # ── Deploy ─────────────────────────────────────────────────────────────────
    deploy_args = [sys.executable, str(script_dir / "deploy.py"), "--artefact", str(tarball)]
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
