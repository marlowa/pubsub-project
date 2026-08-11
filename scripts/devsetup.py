#!/usr/bin/env python3
"""
devsetup.py — build, package, stop, and deploy in one step for the developer sandbox.

Equivalent to running:
  python3 build.py  [build options]
  python3 release.py
  python3 devenv.py --env environments/dev.toml stop
  python3 deploy.py --artefact release/pubsub-<ver>-<hash>.tar.gz \\
                    --env environments/dev.toml \\
                    --install-dir <the directory build.py staged into>  [deploy options]

The stop step prevents "Text file busy" errors when overwriting binaries that
are still running from a previous sandbox session.

Use the individual scripts directly when you need finer control (e.g. deploying
an existing artefact, deploying to a different environment, or releasing without
immediately deploying).

Usage:
  ./devsetup.py [options]
"""

from __future__ import annotations
import argparse
import subprocess
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
# The tree above scripts/. Sibling scripts are reached through the script directory;
# everything the project owns -- environments/, db/, installed/ -- hangs off here.
_PROJECT_ROOT = _SCRIPT_DIR.parent


def _staging_dir() -> Path:
    """The directory build.py stages into on this platform, which deploy must also use.

    All four steps have to name the same directory. build.py and release.py qualify it
    by target platform -- a gcc-8.5 Rocky/RHEL8 tree stages to installed-rocky8/ so it
    cannot overwrite the host's gcc-13 one -- but deploy.py takes its destination from
    the env TOML, where "installed" is unqualified and correct, because a real target
    host deploys to that name whatever compiler built the artefact.

    Unqualified is right for deploy.py on its own and wrong here. In the Rocky container
    the repo is bind-mounted, so a deploy that honoured the env TOML wrote gcc-8.5
    binaries over the host's installed/ tree. They carry an RPATH of /opt/deps, a path
    that exists only inside the container, so every binary then failed to start with
    exit 127 and every stage run afterwards failed against a venue that could not launch.

    Imported from build.py rather than duplicated so there is one definition of the name.
    """
    import build  # noqa: PLC0415  -- deferred: only needed to resolve the default
    tag = build.platform_tag()
    return _PROJECT_ROOT / (f"installed-{tag}" if tag else "installed")


def _run(command: list[str], step: str) -> None:
    print(f"\n{'='*60}")
    print(step)
    print('='*60)
    sys.stdout.flush()
    result = subprocess.run(command)
    if result.returncode != 0:
        # Which of the four steps died, and what was run. The bare exit that stood here left a
        # failed deploy looking like a failed build: the banner above is the last thing printed
        # before the shell prompt returns, and it names the step that was starting, not ending.
        print(f"\nerror: {step} failed with exit code {result.returncode}", file=sys.stderr)
        print(f"  $ {' '.join(str(part) for part in command)}", file=sys.stderr)
        sys.exit(result.returncode)


def _find_tarball() -> Path:
    """The newest release tarball built for this platform.

    Filtered by platform, not just newest: release/ is shared with the Rocky container
    through a bind mount, so a gcc-8.5 artefact lands beside the host's and is often the
    newer of the two. Deploying it puts binaries on the host that die at startup.
    """
    import build  # noqa: PLC0415  -- deferred: matches _staging_dir() above
    release_dir = _PROJECT_ROOT / "release"
    tarballs = sorted(
        (p for p in release_dir.glob("pubsub-*.tar.gz")
         if build.artefact_belongs_to_this_platform(p.name)),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not tarballs:
        sys.exit(f"error: no tarball for this platform found in {release_dir} "
                 f"after release step")
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
    build_group.add_argument("--build-dir", metavar="DIR",
        help="build directory, forwarded to build.py. Needed when building for a "
             "different target platform in a container: the repo is bind-mounted, so "
             "without this a gcc-8.5 build writes over the host's build tree.")
    build_group.add_argument("--no-tests", action="store_true",
        help="skip all tests")
    build_group.add_argument("--no-cpp-tests", action="store_true",
        help="skip C++ tests only")
    build_group.add_argument("--no-java-tests", action="store_true",
        help="skip Java tests only")
    build_group.add_argument("--no-java", action="store_true",
        help="skip Java admin service build")
    build_group.add_argument("--no-cpp", action="store_true",
        help="skip C++ build")
    build_group.add_argument("--no-doxygen", action="store_true",
        help="skip Doxygen documentation generation")
    build_group.add_argument("--no-pylint", action="store_true",
        help="skip pylint: the Python DSL and FIX dictionary source, and the top-level scripts")
    build_group.add_argument("--no-pytest", action="store_true",
        help="skip Python DSL tests (pytest)")
    build_group.add_argument("--jobs", "-j", type=int, metavar="N",
        help="parallel C++ build jobs")

    # Deploy options (forwarded to deploy.py)
    deploy_group = parser.add_argument_group("deploy options")
    deploy_group.add_argument(
        "--env", type=Path,
        default=_PROJECT_ROOT / "environments" / "dev.toml",
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
    if args.no_cpp_tests:
        build_cmd.append("--no-cpp-tests")
    if args.no_java_tests:
        build_cmd.append("--no-java-tests")
    if args.no_java:
        build_cmd.append("--no-java")
    if args.no_cpp:
        build_cmd.append("--no-cpp")
    if args.no_doxygen:
        build_cmd.append("--no-doxygen")
    if args.no_pylint:
        build_cmd.append("--no-pylint")
    if args.no_pytest:
        build_cmd.append("--no-pytest")
    if args.jobs:
        build_cmd += ["-j", str(args.jobs)]
    if args.build_dir:
        build_cmd += ["--build-dir", args.build_dir]
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
        "--install-dir", str(_staging_dir()),
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
