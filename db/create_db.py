#!/usr/bin/env python3
from __future__ import annotations
"""
create_db.py — Create the pubsub PostgreSQL database from scratch.

Creates the dedicated application role, the database, then runs
'liquibase update' to install the Liquibase tracking tables.  After
this script completes the database contains only the two Liquibase
metadata tables (DATABASECHANGELOG and DATABASECHANGELOGLOCK).
Application schema is managed separately via Liquibase changesets.

Usage:
    python3 db/create_db.py [options]

Environment:
    PUBSUB_APP_DB_PASSWORD   Password for the pubsub_app database role.
                             Falls back to 'pubsub_dev' if not set.

PostgreSQL superuser access:
    The script needs to connect as the PostgreSQL superuser (default:
    postgres) to create the role and database.  Three approaches:

      a) Run the script from a shell already logged in as the postgres
         OS user.  Nothing extra is needed: psql connects over the Unix
         socket using peer authentication, no password and no sudo.
         This is the least troublesome option where sudo is restricted.

      b) Pass --sudo-postgres to prefix psql commands with
         'sudo -u postgres'; psql then connects via the Unix socket
         using peer authentication (no password required).

      c) Allow password auth for the superuser in pg_hba.conf (md5 or
         scram-sha-256 for host 127.0.0.1/32), then supply the password
         either interactively (psql will prompt) or non-interactively
         via the PGPASSWORD environment variable.

    The Unix socket is used for (a) and (b) -- that is, whenever this
    process will reach PostgreSQL as the superuser's own OS account.
    Otherwise the connection is forced over TCP, because peer
    authentication is not available there and a password can be.

Recreating from scratch:
    --drop-existing drops the database and the application role before
    recreating them and re-running the changesets, which is the way to
    get back to a known state when the schema has drifted.  Sessions
    still attached to the database are disconnected first; without that
    the drop fails, and something is nearly always attached.

PostgreSQL JDBC driver:
    Liquibase needs the PostgreSQL JDBC driver (postgresql-*.jar) installed
    in its lib directory so that it is loaded automatically on startup.
    This is a one-time environment setup step:

        sudo cp postgresql-42.x.x.jar /opt/liquibase/lib/

    On Debian/Ubuntu you can get the JAR from the system package:

        sudo apt install libpostgresql-jdbc-java
        sudo cp /usr/share/java/postgresql.jar /opt/liquibase/lib/

    The script will fail with a clear message if the driver is absent.
"""

import argparse
import getpass
import os
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_DB_NAME      = "pubsub"
DEFAULT_APP_USER     = "pubsub_app"
DEFAULT_APP_PASS_ENV = "PUBSUB_APP_DB_PASSWORD"
DEFAULT_DEV_PASSWORD = "pubsub_dev"
# libpq's own environment variables supply the default, so a host whose cluster is not on
# 5432 needs PGHOST/PGPORT exported and nothing else. An explicit --pg-host/--pg-port still
# wins, and that is what deploy.py passes from the [db] section of the env TOML.
DEFAULT_PG_HOST      = os.environ.get("PGHOST", "localhost")
DEFAULT_PG_PORT      = int(os.environ.get("PGPORT", "5432"))
DEFAULT_PG_SUPERUSER = "postgres"


def _run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    print(f"  $ {' '.join(str(c) for c in command)}", flush=True)
    return subprocess.run(command, check=True, **kwargs)


def _running_as(user: str) -> bool:
    """True when this process's own OS account is `user`."""
    try:
        return getpass.getuser() == user
    except Exception:
        # getuser() consults the environment before falling back to the password
        # database, and can raise where neither answers. Not knowing is not the
        # same as being the superuser, so say no and let TCP be used.
        return False


def _superuser_psql_args(psql_prefix: list[str], host: str, port: int,
                          superuser: str) -> list[str]:
    # Use the Unix socket (omit --host) whenever psql will reach PostgreSQL as
    # the superuser's own OS account, because peer authentication then applies
    # and no password is involved.  That is true two ways: with 'sudo -u
    # postgres' in front, and when this process is already running as postgres,
    # which is what a shell logged in as that user gives.
    #
    # Testing for sudo alone missed the second case, so someone logged in as
    # postgres — precisely to sidestep sudo — was pushed onto TCP and asked for
    # a password.  Otherwise force TCP, where password auth (md5/scram) works
    # and peer auth does not.
    local_cluster = host in ("localhost", "127.0.0.1", "::1", "")
    use_socket = local_cluster and (bool(psql_prefix) or _running_as(superuser))
    host_args = [] if use_socket else ["--host", host]
    return psql_prefix + [
        "psql",
        *host_args, "--port", str(port),
        "--username", superuser,
        "--dbname", "postgres",
    ]


def _psql_as_superuser(psql_prefix: list[str], host: str, port: int,
                        superuser: str, sql: str) -> None:
    _run(_superuser_psql_args(psql_prefix, host, port, superuser) + ["--command", sql])


def _database_exists(psql_prefix: list[str], host: str, port: int,
                      superuser: str, db_name: str) -> bool:
    result = subprocess.run(
        _superuser_psql_args(psql_prefix, host, port, superuser) + [
            "--tuples-only",
            "--command", f"SELECT 1 FROM pg_database WHERE datname = '{db_name}';",
        ],
        capture_output=True, text=True, check=True,
    )
    return "1" in result.stdout


def _check_jdbc_driver() -> Path:
    """
    Locate the PostgreSQL JDBC driver for Liquibase.

    First checks Liquibase's own lib directory.  If not found there, looks in
    db/drivers/ (committed to the repo) and copies the JAR into the Liquibase
    lib directory automatically — so no manual setup is required on a fresh
    machine or Docker container.
    """
    liquibase_real = Path(os.path.realpath("/usr/bin/liquibase"))
    liquibase_lib  = liquibase_real.parent / "lib"

    jars = sorted(liquibase_lib.glob("postgresql*.jar"))
    if jars:
        return jars[0]

    # Fall back to the driver bundled in the repository.
    script_dir  = Path(__file__).resolve().parent
    bundled_jars = sorted((script_dir / "drivers").glob("postgresql*.jar"))
    if bundled_jars:
        src = bundled_jars[0]
        dst = liquibase_lib / src.name
        print(f"  copying bundled JDBC driver to {dst}")
        liquibase_lib.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        return dst

    print(
        "\nerror: PostgreSQL JDBC driver not found in the Liquibase lib directory.\n"
        f"\n"
        f"Expected location: {liquibase_lib}/postgresql-*.jar\n"
        f"\n"
        f"Install it once as part of environment setup:\n"
        f"    sudo cp postgresql-42.x.x.jar {liquibase_lib}/\n"
        f"\n"
        f"On Debian/Ubuntu you can get the JAR from the system package:\n"
        f"    sudo apt install libpostgresql-jdbc-java\n"
        f"    sudo cp /usr/share/java/postgresql.jar {liquibase_lib}/\n",
        file=sys.stderr,
    )
    sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--db-name", default=DEFAULT_DB_NAME,
        help=f"Database name to create (default: {DEFAULT_DB_NAME})",
    )
    parser.add_argument(
        "--app-user", default=DEFAULT_APP_USER,
        help=f"Dedicated application role (default: {DEFAULT_APP_USER})",
    )
    parser.add_argument(
        "--pg-host", default=DEFAULT_PG_HOST,
        help=f"PostgreSQL host (default: {DEFAULT_PG_HOST})",
    )
    parser.add_argument(
        "--pg-port", type=int, default=DEFAULT_PG_PORT,
        help=f"PostgreSQL port (default: {DEFAULT_PG_PORT})",
    )
    parser.add_argument(
        "--pg-superuser", default=DEFAULT_PG_SUPERUSER,
        help=f"PostgreSQL superuser for setup (default: {DEFAULT_PG_SUPERUSER})",
    )
    parser.add_argument(
        "--sudo-postgres", action="store_true",
        help="Prefix psql commands with 'sudo -u postgres'",
    )
    parser.add_argument(
        "--drop-existing", action="store_true",
        help="Drop the database and role before recreating (destructive)",
    )
    parser.add_argument(
        "--contexts", default="",
        help=(
            "Liquibase context filter (e.g. 'production' to skip test fixtures). "
            "Default: empty — all changesets including context='test' are applied."
        ),
    )
    args = parser.parse_args()

    app_password = os.environ.get(DEFAULT_APP_PASS_ENV, DEFAULT_DEV_PASSWORD)
    psql_prefix  = ["sudo", "-u", "postgres"] if args.sudo_postgres else []
    script_dir   = Path(__file__).resolve().parent

    print(f"=== pubsub database setup ===")
    print(f"  database   : {args.db_name}")
    print(f"  host       : {args.pg_host}:{args.pg_port}")
    print(f"  superuser  : {args.pg_superuser}")
    print(f"  app user   : {args.app_user}")
    print(f"  password   : from {DEFAULT_APP_PASS_ENV}"
          + ("" if DEFAULT_APP_PASS_ENV in os.environ
             else f" (not set — using dev default)"))
    print()

    if args.drop_existing:
        print("--- Dropping existing database and role ---")
        # PostgreSQL refuses to drop a database that anything is still connected
        # to, and after a venue has been running something nearly always is --
        # an idle psql, a pooled application connection, a monitoring query. The
        # drop then fails and the recreate never happens, which is the opposite
        # of what --drop-existing was asked for. Disconnect them first.
        _psql_as_superuser(psql_prefix, args.pg_host, args.pg_port,
                           args.pg_superuser,
                           "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                           f"WHERE datname = '{args.db_name}' AND pid <> pg_backend_pid();")
        _psql_as_superuser(psql_prefix, args.pg_host, args.pg_port,
                           args.pg_superuser,
                           f"DROP DATABASE IF EXISTS {args.db_name};")
        _psql_as_superuser(psql_prefix, args.pg_host, args.pg_port,
                           args.pg_superuser,
                           f"DROP ROLE IF EXISTS {args.app_user};")
        print()

    # Escape single quotes in the password for the SQL literal.
    escaped_password = app_password.replace("'", "''")

    print("--- Creating application role ---")
    _psql_as_superuser(
        psql_prefix, args.pg_host, args.pg_port, args.pg_superuser,
        f"DO $$ BEGIN "
        f"  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '{args.app_user}') THEN "
        f"    CREATE ROLE {args.app_user} LOGIN PASSWORD '{escaped_password}'; "
        f"  ELSE "
        f"    ALTER ROLE {args.app_user} LOGIN PASSWORD '{escaped_password}'; "
        f"  END IF; "
        f"END $$;",
    )
    print()

    print("--- Creating database ---")
    if _database_exists(psql_prefix, args.pg_host, args.pg_port,
                        args.pg_superuser, args.db_name):
        print(f"  database '{args.db_name}' already exists — skipping")
    else:
        _psql_as_superuser(
            psql_prefix, args.pg_host, args.pg_port, args.pg_superuser,
            f"CREATE DATABASE {args.db_name} OWNER {args.app_user};",
        )
    print()

    print("--- Locating PostgreSQL JDBC driver ---")
    jdbc_driver = _check_jdbc_driver()
    print(f"  found: {jdbc_driver}")
    print()

    print("--- Running liquibase update (installs tracking tables) ---")
    # Liquibase 5.x resolves --changeLogFile relative to --search-path, not
    # as a filesystem absolute path.  Point the search path at db/ so that
    # changelog paths are relative to the same root used by liquibase.properties.
    liquibase_cmd = [
        "liquibase",
        f"--search-path={script_dir}",
        f"--url=jdbc:postgresql://{args.pg_host}:{args.pg_port}/{args.db_name}",
        f"--username={args.app_user}",
        f"--password={app_password}",
        "--changeLogFile=changelog/db.changelog-root.xml",
    ]
    if args.contexts:
        liquibase_cmd.append(f"--contexts={args.contexts}")
    liquibase_cmd.append("update")
    _run(liquibase_cmd)
    print()

    print("=== Done ===")
    print(f"  Database '{args.db_name}' is ready.")
    print(f"  Liquibase tracking tables (DATABASECHANGELOG, DATABASECHANGELOGLOCK) installed.")
    print(f"  No application schema yet — add changesets to db/changelog/ to evolve the schema.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        # Name the command and repeat its stderr. Most steps here run with the child's output
        # inherited, so psql has already said what went wrong -- but the database-exists probe
        # captures instead, and reported nothing but an exit code when it failed on RHEL8.
        command = " ".join(str(part) for part in exc.cmd) if isinstance(exc.cmd, list) else str(exc.cmd)
        print(f"\nerror: command failed with exit code {exc.returncode}", file=sys.stderr)
        print(f"  $ {command}", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr.strip(), file=sys.stderr)
        sys.exit(exc.returncode)
