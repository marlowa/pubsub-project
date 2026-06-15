#!/usr/bin/env python3
"""
devsetup.py — build, package, stop, and deploy in one step for the developer sandbox.

Equivalent to running:
  python3 build.py  [build options]
  python3 release.py
  python3 devenv.py --env environments/dev.toml stop
  python3 deploy.py --artefact release/pubsub-<ver>-<hash>.tar.gz \\
                    --env environments/dev.toml  [deploy options]

The stop step prevents "Text file busy" errors when overwriting binaries that
are still running from a previous sandbox session.

Use the individual scripts directly when you need finer control (e.g. deploying
an existing artefact, deploying to a different environment, or releasing without
immediately deploying).

Usage:
  ./devsetup.py [options]
"""

import argparse
import subprocess
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent


def _run(command: list[str], step: str) -> None:
    print(f"\n{'='*60}")
    print(step)
    print('='*60)
    sys.stdout.flush()
    result = subprocess.run(command)
    if result.returncode != 0:
        sys.exit(result.returncode)


def _find_tarball() -> Path:
    release_dir = _SCRIPT_DIR / "release"
    tarballs = sorted(
        release_dir.glob("pubsub-*.tar.gz"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not tarballs:
        sys.exit(f"error: no tarball found in {release_dir} after release step")
    return tarballs[0]


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # Build options (forwarded to build.py)
    build_group = parser.add_argument_group("build options")
    build_group.add_argument("--clean", action="store_true",
        help="clean before building")
    build_group.add_argument("--no-tests", action="store_true",
        help="skip all tests")
    build_group.add_argument("--no-java", action="store_true",
        help="skip Java admin service build")
    build_group.add_argument("--no-cpp", action="store_true",
        help="skip C++ build")
    build_group.add_argument("--jobs", "-j", type=int, metavar="N",
        help="parallel C++ build jobs")

    # Deploy options (forwarded to deploy.py)
    deploy_group = parser.add_argument_group("deploy options")
    deploy_group.add_argument(
        "--env", type=Path,
        default=_SCRIPT_DIR / "environments" / "dev.toml",
        metavar="PATH",
        help="environment TOML for deploy (default: environments/dev.toml)",
    )
    deploy_group.add_argument("--skip-db", action="store_true",
        help="skip database creation and credential export")
    deploy_group.add_argument("--skip-create-db", action="store_true",
        help="skip database creation but still export credentials")
    deploy_group.add_argument("--skip-certs", action="store_true",
        help="skip TLS certificate generation")
    deploy_group.add_argument("--force-certs", action="store_true",
        help="regenerate TLS certificates even if they already exist")
    deploy_group.add_argument("--drop-db", action="store_true",
        help="drop and recreate the database before applying changesets (destructive)")
    deploy_group.add_argument("--sudo-postgres", action="store_true",
        help="prefix psql commands with 'sudo -u postgres'")

    args = parser.parse_args()

    # Step 1: build
    build_cmd = [sys.executable, str(_SCRIPT_DIR / "build.py")]
    if args.clean:
        build_cmd.append("--clean")
    if args.no_tests:
        build_cmd.append("--no-tests")
    if args.no_java:
        build_cmd.append("--no-java")
    if args.no_cpp:
        build_cmd.append("--no-cpp")
    if args.jobs:
        build_cmd += ["-j", str(args.jobs)]
    _run(build_cmd, "Step 1/4: build")

    # Step 2: release
    _run([sys.executable, str(_SCRIPT_DIR / "release.py")], "Step 2/4: release")

    tarball = _find_tarball()
    print(f"\n  artefact: {tarball}")

    # Step 3: stop any running sandbox (prevents ETXTBSY when overwriting binaries)
    _run(
        [sys.executable, str(_SCRIPT_DIR / "devenv.py"),
         "--env", str(args.env), "stop"],
        "Step 3/4: stop running sandbox",
    )

    # Step 4: deploy
    deploy_cmd = [
        sys.executable, str(_SCRIPT_DIR / "deploy.py"),
        "--artefact", str(tarball),
        "--env", str(args.env),
    ]
    if args.skip_db:
        deploy_cmd.append("--skip-db")
    if args.skip_create_db:
        deploy_cmd.append("--skip-create-db")
    if args.skip_certs:
        deploy_cmd.append("--skip-certs")
    if args.force_certs:
        deploy_cmd.append("--force-certs")
    if args.drop_db:
        deploy_cmd.append("--drop-db")
    if args.sudo_postgres:
        deploy_cmd.append("--sudo-postgres")
    _run(deploy_cmd, "Step 4/4: deploy")

    print()
    print("="*60)
    print("devsetup complete.")
    print("Start the sandbox with:  python3 devenv.py start")
    print("="*60)


if __name__ == "__main__":
    main()
