#!/usr/bin/env python3
"""
release.py — assemble a versioned deployment artefact from the build tree.

The artefact is a .tar.gz whose top-level directory mirrors the install tree
and can be handed directly to deploy.py:

  bin/            application binaries (derived from the env TOML;
                  test/bench binaries are excluded)
  lib/            libpubsub_itc_fw.so; admin-service.jar if present
  etc/            component config templates
                  (credentials.toml, *.crt, *.key excluded — generated at
                   deploy time)
  db/             Liquibase changelog, JDBC driver, create_db.py,
                  export_credentials.py
  environments/   env TOML files (dev.toml, prod.toml, …)
  devenv.py       sandbox management script
  deploy.py       deployment script (included if present)
  release.json    version, git hash, build timestamp

Artefact name:  pubsub-<version>-<git-short-hash>-<mode>.tar.gz
                pubsub-<version>-<git-short-hash>-<mode>-<sanitizer>.tar.gz
                pubsub-<version>-<mode>.tar.gz  (with --no-git-hash)

Usage:
  ./release.py [options]
"""

try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ImportError:
        import sys
        sys.exit("error: Python 3.11+ or the 'tomli' package is required to parse TOML")

import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

_SCRIPT_DIR       = Path(__file__).resolve().parent
_DEFAULT_INSTALL  = _SCRIPT_DIR / "installed"
_DEFAULT_ENV_FILE = _SCRIPT_DIR / "environments" / "dev.toml"
_DEFAULT_OUTPUT   = _SCRIPT_DIR / "release"
_CMAKE_LISTS      = _SCRIPT_DIR / "CMakeLists.txt"

# Files under etc/ that are generated at deploy/start time and must not be
# bundled into the release artefact.
_ETC_EXCLUDE_NAMES = {"credentials.toml"}
_ETC_EXCLUDE_SUFFIXES = {".crt", ".key", ".pem", ".tmp"}


# ── Helpers ───────────────────────────────────────────────────────────────────

def so_lib_dir(install_dir: Path) -> Path:
    """Return the directory where CMake installed the C++ shared library.

    GNUInstallDirs uses lib64 on RHEL/Rocky/CentOS 8 for 64-bit builds;
    everywhere else it uses lib.
    """
    for candidate in (install_dir / "lib64", install_dir / "lib"):
        if (candidate / "libpubsub_itc_fw.so").is_file():
            return candidate
    return install_dir / "lib"  # fallback; missing .so caught in stage_lib


# ── Version helpers ───────────────────────────────────────────────────────────

def read_cmake_version(cmake_lists: Path) -> str:
    """Extract the VERSION field from the top-level project() call."""
    text = cmake_lists.read_text(encoding="utf-8")
    match = re.search(r"project\s*\([^)]*\bVERSION\s+(\S+)", text, re.DOTALL)
    if not match:
        sys.exit(f"error: could not find VERSION in {cmake_lists}")
    return match.group(1)


def git_short_hash() -> str | None:
    """Return the short git commit hash, or None if unavailable."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True,
            cwd=str(_SCRIPT_DIR),
        )
        return result.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


# ── Deployment manifest from env TOML ─────────────────────────────────────────

def deployment_binaries(env: dict) -> set[str]:
    """Return the set of binary filenames needed for deployment (e.g. 'sequencer')."""
    names = set()
    for component_name in env["startup_order"]["components"]:
        comp = env["components"][component_name]
        if "binary" in comp:
            names.add(Path(comp["binary"]).name)
    return names


def deployment_jar_paths(env: dict) -> set[str]:
    """Return the set of jar paths relative to install_dir (e.g. 'lib/admin-service.jar')."""
    paths = set()
    for component_name in env["startup_order"]["components"]:
        comp = env["components"][component_name]
        if "jar" in comp:
            paths.add(comp["jar"])
    return paths


# ── Staging helpers ───────────────────────────────────────────────────────────

def stage_bin(install_dir: Path, stage: Path, binaries: set[str]) -> None:
    stage_bin_dir = stage / "bin"
    stage_bin_dir.mkdir()
    source_bin_dir = install_dir / "bin"
    included = []
    skipped = []
    for binary_name in sorted(binaries):
        source = source_bin_dir / binary_name
        if not source.is_file():
            sys.exit(f"error: deployment binary not found: {source}")
        shutil.copy2(source, stage_bin_dir / binary_name)
        included.append(binary_name)
    for path in sorted(source_bin_dir.iterdir()):
        if path.name not in binaries:
            skipped.append(path.name)
    print(f"  bin/  included: {', '.join(included)}")
    if skipped:
        print(f"        skipped:  {', '.join(skipped)}")


def stage_lib(install_dir: Path, stage: Path, jar_paths: set[str]) -> None:
    stage_lib_dir = stage / "lib"
    stage_lib_dir.mkdir()

    shared_lib = so_lib_dir(install_dir) / "libpubsub_itc_fw.so"
    if not shared_lib.is_file():
        sys.exit(f"error: shared library not found: {shared_lib}")
    shutil.copy2(shared_lib, stage_lib_dir / shared_lib.name)
    print(f"  lib/  {shared_lib.name}")

    for jar_relative in sorted(jar_paths):
        source = install_dir / jar_relative
        if source.is_file():
            shutil.copy2(source, stage_lib_dir / source.name)
            print(f"        {source.name}")
        else:
            print(f"        {source.name}  (not found — skipping)")


def stage_etc(stage: Path) -> None:
    """Copy config templates from applications/ into etc/ in the staging tree.

    Source layout:  applications/<component>/<name>.toml
    Staged layout:  etc/<component>/<name>.toml

    Only .toml files are copied; C++ sources, CMakeLists, and generated files
    (credentials.toml) are excluded.
    """
    source_apps   = _SCRIPT_DIR / "applications"
    stage_etc_dir = stage / "etc"
    stage_etc_dir.mkdir()
    copied  = 0
    skipped = 0
    for toml_path in sorted(source_apps.rglob("*.toml")):
        if toml_path.name in _ETC_EXCLUDE_NAMES:
            skipped += 1
            continue
        component_dir = toml_path.parent.name
        destination   = stage_etc_dir / component_dir / toml_path.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(toml_path, destination)
        copied += 1
    print(f"  etc/  {copied} template(s) staged, {skipped} excluded (credentials)")


def stage_java_configs(stage: Path) -> None:
    """Copy Java service config directories into etc/ in the staging tree.

    Source layout:  java/<service-name>/config/<files>
    Staged layout:  etc/<service_name>/config/<files>
                    (hyphens in the directory name are replaced with underscores)

    All files found under each config/ directory are copied verbatim; no
    exclusion or template substitution is applied.
    """
    java_dir      = _SCRIPT_DIR / "java"
    stage_etc_dir = stage / "etc"
    copied = 0
    for service_dir in sorted(java_dir.iterdir()):
        config_src = service_dir / "config"
        if not config_src.is_dir():
            continue
        component_name = service_dir.name.replace("-", "_")
        config_dst = stage_etc_dir / component_name / "config"
        config_dst.mkdir(parents=True, exist_ok=True)
        for src_file in sorted(config_src.iterdir()):
            if src_file.is_file():
                shutil.copy2(src_file, config_dst / src_file.name)
                copied += 1
    print(f"  etc/  {copied} Java config file(s) staged")


def stage_db(stage: Path) -> None:
    source_db = _SCRIPT_DIR / "db"
    stage_db_dir = stage / "db"
    shutil.copytree(
        source_db, stage_db_dir,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    count = sum(1 for _ in stage_db_dir.rglob("*") if _.is_file())
    print(f"  db/   {count} file(s)")


def stage_environments(stage: Path) -> None:
    source_env_dir = _SCRIPT_DIR / "environments"
    if not source_env_dir.is_dir():
        print("  environments/  (directory not found — skipping)")
        return
    stage_env_dir = stage / "environments"
    stage_env_dir.mkdir()
    count = 0
    for toml_file in sorted(source_env_dir.glob("*.toml")):
        shutil.copy2(toml_file, stage_env_dir / toml_file.name)
        count += 1
    print(f"  environments/  {count} file(s)")


def stage_scripts(stage: Path) -> None:
    scripts = ["devenv.py", "deploy.py"]
    for script_name in scripts:
        source = _SCRIPT_DIR / script_name
        if source.is_file():
            shutil.copy2(source, stage / script_name)
            print(f"  {script_name}")
        else:
            print(f"  {script_name}  (not found — skipping)")


def write_release_json(
    stage: Path, version: str, git_hash: str | None, install_dir: Path,
) -> None:
    payload = {
        "version":    version,
        "git_hash":   git_hash or "unknown",
        "built_at":   datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "install_dir": str(install_dir),
    }
    (stage / "release.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8",
    )
    print(f"  release.json  (version={version}, git={git_hash or 'unknown'})")


# ── Tarball creation ──────────────────────────────────────────────────────────

def create_tarball(stage: Path, output_dir: Path, artefact_name: str) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    tarball_path = output_dir / f"{artefact_name}.tar.gz"
    with tarfile.open(tarball_path, "w:gz") as tar:
        tar.add(stage, arcname=artefact_name)
    return tarball_path


# ── Entry point ───────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--install-dir", type=Path, default=_DEFAULT_INSTALL, metavar="PATH",
        help=f"cmake staging directory written by build.py (default: {_DEFAULT_INSTALL})",
    )
    parser.add_argument(
        "--env", type=Path, default=_DEFAULT_ENV_FILE, metavar="PATH",
        help=f"env TOML used to identify deployment binaries "
             f"(default: {_DEFAULT_ENV_FILE})",
    )
    parser.add_argument(
        "--version", default=None, metavar="VERSION",
        help="override version string (default: read from CMakeLists.txt)",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=_DEFAULT_OUTPUT, metavar="PATH",
        help=f"directory to write the tarball (default: {_DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--no-git-hash", action="store_true",
        help="omit the git short hash from the artefact name",
    )
    parser.add_argument(
        "--mode", choices=["release", "debug"], default="release",
        help="build mode to encode in the artefact name (default: release)",
    )
    parser.add_argument(
        "--sanitizer", choices=["none", "asan", "tsan", "valgrind"], default="none",
        help="sanitizer used in the build (default: none)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    install_dir = args.install_dir.resolve()
    if not install_dir.is_dir():
        sys.exit(f"error: install dir not found: {install_dir}")

    env_path = args.env.resolve() if args.env.is_absolute() else (_SCRIPT_DIR / args.env).resolve()
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    with open(env_path, "rb") as file_handle:
        env = tomllib.load(file_handle)

    version  = args.version or read_cmake_version(_CMAKE_LISTS)
    git_hash = None if args.no_git_hash else git_short_hash()
    if git_hash is None and not args.no_git_hash:
        print("warning: git hash unavailable — omitting from artefact name",
              file=sys.stderr)

    artefact_name = f"pubsub-{version}-{git_hash}" if git_hash else f"pubsub-{version}"
    artefact_name += f"-{args.mode}"
    if args.sanitizer != "none":
        artefact_name += f"-{args.sanitizer}"

    binaries = deployment_binaries(env)
    jar_paths = deployment_jar_paths(env)

    print(f"=== release.py ===")
    print(f"  version      : {version}")
    print(f"  git hash     : {git_hash or '(omitted)'}")
    print(f"  artefact     : {artefact_name}.tar.gz")
    print(f"  install dir  : {install_dir}")
    print(f"  output dir   : {args.output_dir.resolve()}")
    print()

    with tempfile.TemporaryDirectory(prefix="pubsub-release-") as temp_dir:
        stage = Path(temp_dir) / artefact_name

        print("=== staging ===")
        stage.mkdir()
        write_release_json(stage, version, git_hash, install_dir)
        stage_bin(install_dir, stage, binaries)
        stage_lib(install_dir, stage, jar_paths)
        stage_etc(stage)
        stage_java_configs(stage)
        stage_db(stage)
        stage_environments(stage)
        stage_scripts(stage)
        print()

        print("=== creating tarball ===")
        tarball_path = create_tarball(stage, args.output_dir.resolve(), artefact_name)
        size_mib = tarball_path.stat().st_size / (1024 * 1024)
        print(f"  {tarball_path}  ({size_mib:.1f} MiB)")
        print()

    print("=== done ===")


if __name__ == "__main__":
    main()
