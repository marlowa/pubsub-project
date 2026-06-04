#!/usr/bin/env python3
"""
deploy.py — deploy a pubsub release artefact.

Steps (in order):
  1. Unpack the release .tar.gz into the install directory (if --artefact given).
  2. Expand ${placeholder} in etc/**/*.toml using values from the env TOML.
     Placeholder names are the full flattened TOML path, e.g.
       [arbiter_primary] peer_host  →  ${arbiter_primary_peer_host}.
  3. Generate self-signed TLS certificates for each [tls.*] section in the env
     TOML (unless --skip-certs).
  4. Create or update the PostgreSQL database via db/create_db.py
     (unless --skip-db).
  5. Export SCRAM credentials from the database to credentials.toml via
     db/export_credentials.py.

Usage:
  ./deploy.py [options]
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
import re
import string
import subprocess
import sys
import tarfile
from pathlib import Path

# Matches intentional ${placeholder} patterns — used to catch unresolved
# placeholders after safe_substitute (which silently skips unknowns).
_PLACEHOLDER_RE = re.compile(r'\$\{([_a-zA-Z][_a-zA-Z0-9]*)\}')

_SCRIPT_DIR       = Path(__file__).resolve().parent
_DEFAULT_ENV_FILE = _SCRIPT_DIR / "environments" / "dev.toml"


# ── TOML helpers ──────────────────────────────────────────────────────────────

def load_env(path: Path) -> dict:
    with open(path, "rb") as file_handle:
        return tomllib.load(file_handle)


def flatten_toml(data: dict, prefix: str = "") -> dict[str, str]:
    """Recursively flatten a parsed TOML dict into a {flattened_key: str_value} map.

    Nested sections are joined with underscores:
      [arbiter_primary] peer_host  →  arbiter_primary_peer_host
    Booleans are rendered as TOML literals ('true'/'false').
    Lists and other non-scalar types are silently skipped.
    """
    result: dict[str, str] = {}
    for key, value in data.items():
        full_key = f"{prefix}_{key}" if prefix else key
        if isinstance(value, bool):
            result[full_key] = "true" if value else "false"
        elif isinstance(value, (str, int, float)):
            result[full_key] = str(value)
        elif isinstance(value, dict):
            result.update(flatten_toml(value, full_key))
    return result


# ── Artefact handling ─────────────────────────────────────────────────────────

def unpack_artefact(artefact_path: Path, install_dir: Path) -> None:
    """Unpack a release .tar.gz into install_dir, stripping the artefact's top-level directory."""
    install_dir.mkdir(parents=True, exist_ok=True)
    print(f"  {artefact_path.name}  →  {install_dir}")
    with tarfile.open(artefact_path, "r:gz") as tar:
        for member in tar.getmembers():
            # Strip the top-level directory (e.g. "pubsub-1.0-abc1234/").
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            member.name = parts[1]
            try:
                tar.extract(member, path=install_dir)
            except OSError as exc:
                if exc.errno == 26:  # ETXTBSY — binary is running
                    target = install_dir / member.name
                    sys.exit(
                        f"\nerror: cannot overwrite running binary: {target}\n"
                        f"Stop the sandbox first:\n"
                        f"  python3 devenv.py stop\n"
                        f"Then re-run deploy."
                    )
                raise
    file_count = sum(1 for _ in install_dir.rglob("*") if _.is_file())
    print(f"  {file_count} file(s) installed")


# ── Template expansion ────────────────────────────────────────────────────────

def expand_templates(install_dir: Path, namespace: dict[str, str]) -> None:
    """Expand ${placeholder} in all .toml files under install_dir/etc/."""
    etc_dir = install_dir / "etc"
    if not etc_dir.is_dir():
        print(f"  warning: {etc_dir} not found — no templates to expand")
        return

    expanded = 0
    for toml_path in sorted(etc_dir.rglob("*.toml")):
        text = toml_path.read_text(encoding="utf-8")
        # safe_substitute leaves unrecognised $ sequences (e.g. bcrypt hashes
        # containing $2a$12$…) intact instead of raising ValueError.  We then
        # scan the result for any remaining ${identifier} patterns to catch
        # typos or genuinely missing namespace entries.
        result = string.Template(text).safe_substitute(namespace)
        unresolved = _PLACEHOLDER_RE.findall(result)
        if unresolved:
            sys.exit(f"error: undefined placeholder(s) {unresolved} in {toml_path.relative_to(install_dir)}")
        if result != text:
            toml_path.write_text(result, encoding="utf-8")
            expanded += 1
    print(f"  {expanded} template(s) expanded in {etc_dir.relative_to(install_dir.parent)}/")


# ── TLS certificate generation ────────────────────────────────────────────────

def _generate_self_signed_cert(cert_path: Path, key_path: Path) -> None:
    subprocess.run(
        [
            "openssl", "req", "-x509",
            "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key_path),
            "-out",    str(cert_path),
            "-days",   "3650",
            "-subj",   "/CN=localhost",
        ],
        check=True,
        capture_output=True,
    )


def generate_tls_certs(env: dict, install_dir: Path, force: bool) -> None:
    """Generate a self-signed cert/key pair for each unique [tls.*] endpoint."""
    tls_sections = env.get("tls", {})
    if not tls_sections:
        print("  no [tls.*] sections in env TOML — skipping")
        return

    seen: set[tuple[Path, Path]] = set()
    for tls_name, tls_conf in tls_sections.items():
        cert_name = tls_conf.get("cert", "")
        key_name  = tls_conf.get("key", "")
        if not cert_name or not key_name:
            continue

        comp = env.get("components", {}).get(tls_name)
        if comp is None:
            print(f"  warning: no component '{tls_name}' for [tls.{tls_name}] — skipping")
            continue

        workdir   = (install_dir / comp["workdir"]).resolve()
        cert_path = workdir / cert_name
        key_path  = workdir / key_name

        pair = (cert_path, key_path)
        if pair in seen:
            continue
        seen.add(pair)

        if not force and cert_path.exists() and key_path.exists():
            print(f"  {cert_name}: already exists — skipping (use --force-certs to regenerate)")
            continue

        workdir.mkdir(parents=True, exist_ok=True)
        _generate_self_signed_cert(cert_path, key_path)
        print(f"  generated: {cert_path.relative_to(install_dir)}")


# ── Database setup ────────────────────────────────────────────────────────────

def run_create_db(
    env: dict,
    drop_existing: bool,
    sudo_postgres: bool,
    liquibase_contexts: str,
) -> None:
    db      = env["db"]
    script  = _SCRIPT_DIR / "db" / "create_db.py"
    command = [
        sys.executable, str(script),
        "--db-name",  db["name"],
        "--app-user", db["user"],
        "--pg-host",  db["host"],
        "--pg-port",  str(db["port"]),
    ]
    if drop_existing:
        command.append("--drop-existing")
    if sudo_postgres:
        command.append("--sudo-postgres")
    if liquibase_contexts:
        command.extend(["--contexts", liquibase_contexts])

    result = subprocess.run(command)
    if result.returncode != 0:
        sys.exit(f"error: create_db.py exited with code {result.returncode}")


def run_export_credentials(env: dict, install_dir: Path) -> None:
    db         = env["db"]
    script     = _SCRIPT_DIR / "db" / "export_credentials.py"
    creds_file = install_dir / "etc" / "authentication_service" / "credentials.toml"

    result = subprocess.run([
        sys.executable, str(script),
        "--credentials-file", str(creds_file),
        "--db-host",          db["host"],
        "--db-port",          str(db["port"]),
        "--db-name",          db["name"],
        "--db-user",          db["user"],
    ])
    if result.returncode != 0:
        sys.exit(f"error: export_credentials.py exited with code {result.returncode}")


# ── Entry point ───────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--artefact", type=Path, default=None, metavar="PATH",
        help="release .tar.gz to unpack into the install directory before deploying",
    )
    parser.add_argument(
        "--env", type=Path, default=_DEFAULT_ENV_FILE, metavar="PATH",
        help=f"environment TOML (default: {_DEFAULT_ENV_FILE})",
    )
    parser.add_argument(
        "--install-dir", type=Path, default=None, metavar="PATH",
        help="install directory for binaries and config "
             "(default: paths.install_dir from the env TOML)",
    )
    parser.add_argument(
        "--skip-certs", action="store_true",
        help="skip TLS certificate generation",
    )
    parser.add_argument(
        "--force-certs", action="store_true",
        help="regenerate TLS certificates even if they already exist",
    )
    parser.add_argument(
        "--skip-db", action="store_true",
        help="skip database creation and credential export",
    )
    parser.add_argument(
        "--skip-create-db", action="store_true",
        help="skip database creation but still export credentials (use when the database already exists)",
    )
    parser.add_argument(
        "--drop-db", action="store_true",
        help="drop and recreate the database before applying Liquibase changesets (destructive)",
    )
    parser.add_argument(
        "--sudo-postgres", action="store_true",
        help="prefix psql commands with 'sudo -u postgres' (passed to create_db.py)",
    )
    parser.add_argument(
        "--liquibase-contexts", default="", metavar="CONTEXTS",
        help="Liquibase context filter passed to create_db.py, e.g. 'production'",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    env_path = (
        args.env.resolve()
        if args.env.is_absolute()
        else (_SCRIPT_DIR / args.env).resolve()
    )
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    env = load_env(env_path)

    if args.install_dir is not None:
        install_dir = args.install_dir.resolve()
    else:
        install_dir = (_SCRIPT_DIR / env["paths"]["install_dir"]).resolve()

    print("=== deploy.py ===")
    print(f"  env         : {env_path}")
    print(f"  install dir : {install_dir}")
    if args.artefact:
        print(f"  artefact    : {args.artefact.resolve()}")
    print()

    # Step 1: unpack artefact
    if args.artefact:
        artefact_path = args.artefact.resolve()
        if not artefact_path.is_file():
            sys.exit(f"error: artefact not found: {artefact_path}")
        print("=== unpacking artefact ===")
        unpack_artefact(artefact_path, install_dir)
        print()

    # Step 2: template expansion
    print("=== expanding templates ===")
    namespace = flatten_toml(env)
    # Override paths_install_dir with the resolved absolute path so that any
    # namespace values that reference ${paths_install_dir} expand correctly.
    namespace["paths_install_dir"] = str(install_dir)
    # CPU registry files always live under install_dir/run/ regardless of what
    # the env TOML says.  Inject the absolute paths here so the templates get
    # concrete values and operators can see the resolved paths in installed/etc/.
    cpu_run_dir = install_dir / "run"
    cpu_run_dir.mkdir(parents=True, exist_ok=True)
    namespace["shared_reactor_cpu_registry_shm_path"] = str(cpu_run_dir / "pubsub_cpu_registry")
    namespace["shared_reactor_cpu_registry_lock_file"] = str(cpu_run_dir / "pubsub_cpu_registry.lock")
    expand_templates(install_dir, namespace)
    print()

    # Step 3: TLS certificates
    if not args.skip_certs:
        print("=== generating TLS certificates ===")
        generate_tls_certs(env, install_dir, force=args.force_certs)
        print()

    # Steps 4 & 5: database
    if not args.skip_db:
        if not args.skip_create_db:
            print("=== creating database ===")
            run_create_db(env, args.drop_db, args.sudo_postgres, args.liquibase_contexts)
            print()

        print("=== exporting credentials ===")
        run_export_credentials(env, install_dir)
        print()

    print("=== done ===")


if __name__ == "__main__":
    main()
