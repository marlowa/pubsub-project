#!/usr/bin/env python3
"""
deploy.py — deploy a pubsub release artefact.

Steps (in order):
  1. Unpack the release .tar.gz into the install directory (if --artefact given).
  2. Expand ${placeholder} in etc/**/*.toml and in application.properties inside
     the admin-service JAR, using values from the env TOML.
     Placeholder names are the full flattened TOML path, e.g.
       [arbiter_primary] peer_host  →  ${arbiter_primary_peer_host}.
  3. Resolve the CPU core layout for this machine and write run/cpu_layout.toml.
     The env TOML declares which processes run on each host ([machines.*]) and
     the order in which they surrender a dedicated core (hot_path_rank); this
     step reads the host's real topology and turns that intent into core ids.
     Because deploy runs on the target machine, one declaration serves hosts of
     different sizes and survives a hardware refresh.
  4. Generate self-signed TLS certificates for each [tls.*] section in the env
     TOML (unless --skip-certs).
  5. Create or update the PostgreSQL database via db/create_db.py
     (unless --skip-db).
  6. Provision FIX client SCRAM credentials from [[fix_credentials]] entries in
     the env TOML.  Derives fresh SCRAM material from the configured plaintext
     password and writes it directly into the database, making this step the
     single authoritative source for those credentials.
  7. Export SCRAM credentials from the database to credentials.toml via
     db/export_credentials.py.

Usage:
  ./deploy.py [options]
"""

from __future__ import annotations
try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ImportError:
        import sys
        sys.exit("error: Python 3.11+ or the 'tomli' package is required to parse TOML")

import argparse
import hashlib
import hmac
import os
import re
import secrets
import shutil
import string
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

import cpu_layout

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
    Lists of scalars are rendered as a TOML array literal, so a value such as a
    histogram's bucket bounds can be declared once here and expanded into several
    component files that must agree on it. Lists of tables are skipped: a component
    file needs its own [[section]] headers, which no single substitution can produce.
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
        elif isinstance(value, list):
            if all(isinstance(item, (bool, int, float, str)) for item in value):
                result[full_key] = _render_toml_array(value)
    return result


def _render_toml_array(items: list) -> str:
    """Render a list of TOML scalars back into TOML array syntax.

    The result is substituted into a component .toml verbatim, so it has to be valid
    TOML on its own -- booleans lowercase, strings quoted, numbers bare.
    """
    rendered = []
    for item in items:
        if isinstance(item, bool):
            rendered.append("true" if item else "false")
        elif isinstance(item, str):
            rendered.append(f'"{item}"')
        else:
            rendered.append(str(item))
    return "[" + ", ".join(rendered) + "]"


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

def map_config_files_to_components(env: dict, install_dir: Path) -> dict[Path, str]:
    """Map each component's config file to the component key that owns it.

    The mapping is already declared, one to one, by components.<key>.config; this
    just inverts it.  It exists so a component can be told its own name during
    template expansion, which is what lets it find its entry in the machine-wide
    CPU layout file.  The name has to be the instance ("sequencer_secondary"),
    not the binary, because a primary and its secondary run the same binary and
    are ranked and placed separately.
    """
    owners: dict[Path, str] = {}
    for name, component in env.get("components", {}).items():
        config = component.get("config")
        if config is None:
            continue
        owners[(install_dir / config).resolve()] = name
    return owners


def expand_templates(install_dir: Path, namespace: dict[str, str], config_owners: dict[Path, str] | None = None) -> None:
    """Expand ${placeholder} in all .toml files under install_dir/etc/.

    Most placeholders come from the shared namespace and mean the same thing in
    every file.  ${component_name} is the exception: it resolves per file, to the
    component that declared that file as its config.
    """
    etc_dir = install_dir / "etc"
    if not etc_dir.is_dir():
        print(f"  warning: {etc_dir} not found — no templates to expand")
        return

    owners = config_owners or {}
    expanded = 0
    for toml_path in sorted(etc_dir.rglob("*.toml")):
        text = toml_path.read_text(encoding="utf-8")
        # A config file with no owner is one no component in *this* environment
        # declares as its config -- the release artefact ships the templates for
        # every environment, so dev installs prod's matching_engine.toml and
        # never launches it.  Such a file gets an empty component name rather
        # than an error: it has no component identity here, and saying so
        # honestly means that if it were ever launched, CpuLayout would fail at
        # startup with "no CPU layout component name configured" rather than
        # silently matching the wrong entry.
        file_namespace = {**namespace, "component_name": owners.get(toml_path.resolve(), "")}
        # safe_substitute leaves unrecognised $ sequences (e.g. bcrypt hashes
        # containing $2a$12$…) intact instead of raising ValueError.  We then
        # scan the result for any remaining ${identifier} patterns to catch
        # typos or genuinely missing namespace entries.
        result = string.Template(text).safe_substitute(file_namespace)
        unresolved = _PLACEHOLDER_RE.findall(result)
        if unresolved:
            sys.exit(f"error: undefined placeholder(s) {unresolved} in {toml_path.relative_to(install_dir)}")
        if result != text:
            toml_path.write_text(result, encoding="utf-8")
            expanded += 1
    print(f"  {expanded} template(s) expanded in {etc_dir.relative_to(install_dir.parent)}/")


# ── JAR property patching ─────────────────────────────────────────────────────

def patch_jar_properties(env: dict, install_dir: Path, namespace: dict[str, str]) -> None:
    """Expand ${placeholder} in application.properties inside the admin-service JAR."""
    comp = env.get("components", {}).get("admin_service")
    if comp is None or "jar" not in comp:
        print("  no admin_service jar — skipping")
        return

    jar_path = (install_dir / comp["jar"]).resolve()
    if not jar_path.is_file():
        print(f"  warning: {jar_path.name} not found — skipping JAR properties patch")
        return

    prop_entry = "application.properties"
    with zipfile.ZipFile(jar_path, "r") as zf:
        if prop_entry not in zf.namelist():
            sys.exit(f"error: {prop_entry} not found in {jar_path.name}")
        original_text = zf.read(prop_entry).decode("utf-8")

    result = string.Template(original_text).safe_substitute(namespace)
    unresolved = _PLACEHOLDER_RE.findall(result)
    if unresolved:
        sys.exit(f"error: undefined placeholder(s) {unresolved} in {prop_entry}")

    if result == original_text:
        print(f"  {prop_entry}: no placeholders to expand")
        return

    tmp_path = jar_path.with_suffix(".jar.tmp")
    try:
        with zipfile.ZipFile(jar_path, "r") as zin, \
             zipfile.ZipFile(tmp_path, "w") as zout:
            for item in zin.infolist():
                data = result.encode("utf-8") if item.filename == prop_entry else zin.read(item)
                zout.writestr(item, data)
        shutil.move(str(tmp_path), str(jar_path))
    except Exception:
        if tmp_path.exists():
            tmp_path.unlink()
        raise
    print(f"  patched: {prop_entry} in {jar_path.name}")


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

    _generate_fix_client_truststore(env, install_dir, force)


def _generate_fix_client_truststore(env: dict, install_dir: Path, force: bool) -> None:
    """Import a FIX gateway's TLS cert into a JKS truststore for fix-test-client.

    Takes the first gateway instance that has a [tls.*] section. The instances share a
    certificate today, so the client trusts whichever it connects to; if they are ever
    given distinct certs, every one of them needs importing into the same truststore.
    """
    gateway_name = next((name for name in env.get("tls", {}) if name.startswith("fix_order_gateway")), None)
    if gateway_name is None:
        return
    gateway_tls = env["tls"][gateway_name]

    gateway_comp = env.get("components", {}).get(gateway_name)
    client_comp  = env.get("components", {}).get("fix_test_client")
    if gateway_comp is None or client_comp is None:
        return

    cert_path = (install_dir / gateway_comp["workdir"]).resolve() / gateway_tls.get("cert", "")
    if not cert_path.exists():
        return

    jks_dir  = (install_dir / client_comp["workdir"]).resolve() / "config"
    jks_path = jks_dir / "fix_gateway_trust.jks"

    if not force and jks_path.exists():
        print(f"  fix_gateway_trust.jks: already exists — skipping (use --force-certs to regenerate)")
        return

    jks_dir.mkdir(parents=True, exist_ok=True)
    if jks_path.exists():
        jks_path.unlink()

    subprocess.run(
        [
            "keytool", "-importcert", "-trustcacerts", "-noprompt",
            "-file",      str(cert_path),
            "-keystore",  str(jks_path),
            "-storepass", "pubsub_dev",
            "-alias",     "fix_gateway",
        ],
        check=True,
        capture_output=True,
    )
    print(f"  generated: {jks_path.relative_to(install_dir)}")


# ── FIX credential provisioning ───────────────────────────────────────────────

def _derive_scram(password: str, iterations: int = 4096) -> dict:
    """Derive SCRAM-SHA-256 material from a plaintext password.

    Returns a dict with stored_key, server_key, salt (all hex strings) and
    iterations.  A fresh random salt is generated on each call so the output
    changes every deploy; that is intentional -- the auth service is the source
    of truth and always receives authoritative values from this step.
    """
    salt = secrets.token_bytes(16)
    salted = hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), salt, iterations, dklen=32)
    client_key = hmac.new(salted, b'Client Key', hashlib.sha256).digest()
    stored_key = hashlib.sha256(client_key).hexdigest()
    server_key = hmac.new(salted, b'Server Key', hashlib.sha256).digest().hex()
    return {
        'stored_key': stored_key,
        'server_key': server_key,
        'salt':       salt.hex(),
        'iterations': iterations,
    }


def provision_fix_credentials(env: dict) -> None:
    """Provision FIX client SCRAM credentials from [[fix_credentials]] in the env TOML.

    For each entry, derives fresh SCRAM material from the plaintext password and
    writes it to the database.  The subsequent export_credentials.py step will
    then export correct values regardless of what was in the database before.

    This step is the single authoritative source for FIX client credentials: the
    plaintext password lives in the env TOML and the database always reflects it
    after a deploy.
    """
    entries = env.get("fix_credentials", [])
    if not entries:
        print("  no [[fix_credentials]] entries -- skipping")
        return

    db       = env["db"]
    host     = db["host"]
    port     = db["port"]
    name     = db["name"]
    user     = db["user"]
    prefix   = db.get("table_prefix", "pubsub_")
    app_pass = os.environ.get("PUBSUB_APP_DB_PASSWORD", "pubsub_dev")

    for entry in entries:
        comp_id  = entry.get("comp_id", "")
        password = entry.get("password", "")
        if not comp_id or not password:
            sys.exit("error: [[fix_credentials]] entry is missing comp_id or password")

        scram = _derive_scram(password)
        sql = (
            f"UPDATE {prefix}comp_id "
            f"SET stored_key='{scram['stored_key']}', "
            f"    server_key='{scram['server_key']}', "
            f"    salt='{scram['salt']}', "
            f"    iterations={scram['iterations']} "
            f"WHERE comp_id='{comp_id}';"
        )
        result = subprocess.run(
            ["psql", "--host", host, "--port", str(port),
             "--username", user, "--dbname", name,
             "--command", sql],
            capture_output=True, text=True,
            env={**os.environ, "PGPASSWORD": app_pass},
        )
        if result.returncode != 0:
            sys.exit(
                f"error: failed to provision credential for '{comp_id}':\n"
                f"  {result.stderr.strip()}"
            )
        if "UPDATE 0" in result.stdout:
            sys.exit(
                f"error: no row found for comp_id='{comp_id}' -- "
                f"create the comp_id via the admin service before deploying"
            )
        print(f"  provisioned: {comp_id}")


# ── CPU core layout ───────────────────────────────────────────────────────────

# Reactor thread plus the one ApplicationThread every component registers.
# Used only when the binary cannot be asked; see query_hot_path_thread_count().
_ASSUMED_HOT_PATH_THREAD_COUNT = 2


def query_hot_path_thread_count(install_dir: Path, component: dict) -> tuple[int, bool]:
    """Ask a component's binary how many hot-path threads it will register.

    The application knows how many threads it registers with the Reactor and the
    environment TOML must not need to know: a count in configuration is a second
    source of truth that drifts the moment someone adds a thread, and drifts
    silently.  deploy.py runs on the target host, so the binary is present and
    can be asked.

    Falls back to the framework invariant -- reactor thread plus the one
    ApplicationThread every component registers -- when the binary is absent or
    does not support the query, so a deploy from a partial tree still produces a
    layout.  Returns (count, queried) so the caller can report the fallback: an
    assumed count is correct today and would be wrong the moment a component
    registers a second ApplicationThread, which is exactly the drift the query
    exists to prevent.
    """
    binary = component.get("binary")
    if binary is None:
        return _ASSUMED_HOT_PATH_THREAD_COUNT, False

    binary_path = install_dir / binary
    if not binary_path.is_file():
        return _ASSUMED_HOT_PATH_THREAD_COUNT, False

    # The installed binaries do not carry an RPATH, so they cannot find
    # libpubsub_itc_fw.so on their own.  devenv.py prepends the install lib
    # directories when it launches a component and this must do the same, or
    # every query fails with a loader error and the layout is silently computed
    # from assumed counts.  GNUInstallDirs uses lib64 on RHEL8; include both
    # rather than probing which one CMake chose.
    library_dirs = [str(d) for d in (install_dir / "lib64", install_dir / "lib") if d.is_dir()]
    child_env = os.environ.copy()
    existing = child_env.get("LD_LIBRARY_PATH", "")
    child_env["LD_LIBRARY_PATH"] = ":".join(library_dirs + ([existing] if existing else []))

    try:
        result = subprocess.run(
            [str(binary_path), "--hot-path-thread-count"],
            capture_output=True, text=True, timeout=10, check=False, env=child_env,
        )
    except (OSError, subprocess.SubprocessError):
        return _ASSUMED_HOT_PATH_THREAD_COUNT, False

    if result.returncode != 0:
        return _ASSUMED_HOT_PATH_THREAD_COUNT, False
    try:
        return int(result.stdout.strip()), True
    except ValueError:
        return _ASSUMED_HOT_PATH_THREAD_COUNT, False


def resolve_cpu_layout(env: dict, install_dir: Path, run_dir: Path) -> Path | None:
    """Resolve declared hot_path_rank values into core ids for this machine.

    Writes one machine-wide layout file into run/ and reports the result.  The
    report is not decoration: whether a demoted rank group is correct depends on
    the machine, and nothing in the layout can tell a functional-test VM from a
    production host.  A demotion visible here is diagnosable; one visible only as
    unexplained latency is not.
    """
    machines = env.get("machines")
    if not machines:
        print("  no [machines.*] in the env TOML -- no layout computed")
        return None

    try:
        machine_key, machine = cpu_layout.select_machine(machines)
    except cpu_layout.LayoutError as exc:
        sys.exit(f"error: {exc}")

    components = env.get("components", {})
    on_machine = machine.get("components", [])

    unknown = [name for name in on_machine if name not in components]
    if unknown:
        sys.exit(
            f"error: [machines.\"{machine_key}\"] lists component(s) with no "
            f"[components.*] entry: {', '.join(unknown)}"
        )

    ranks = {
        name: components[name]["hot_path_rank"]
        for name in on_machine
        if "hot_path_rank" in components[name]
    }
    thread_counts: dict[str, int] = {}
    assumed: list[str] = []
    for name in ranks:
        count, queried = query_hot_path_thread_count(install_dir, components[name])
        thread_counts[name] = count
        if not queried:
            assumed.append(name)
    if assumed:
        print(
            f"  note: {len(assumed)} binaries could not be asked for their hot-path thread count;\n"
            f"        assuming {_ASSUMED_HOT_PATH_THREAD_COUNT} each (reactor thread + one "
            f"ApplicationThread):\n"
            f"        {', '.join(sorted(assumed))}"
        )
        print()

    # reserve_cpu0 is a framework-wide setting in [shared], not a per-machine one:
    # it says cpu0 belongs to the OS, which is true of every host or none.
    reserve_cpu0 = bool(env.get("shared", {}).get("reactor_cpu_pinning_reserve_cpu0", True))

    try:
        layout = cpu_layout.resolve_layout(
            machine=machine_key,
            components_on_machine=on_machine,
            ranks=ranks,
            thread_counts=thread_counts,
            topology=cpu_layout.read_topology(),
            minimum_background_cores=machine.get("minimum_background_cores", 0),
            reserve_cpu0=reserve_cpu0,
            # Only the C++ components have a Quill backend to place; the JVMs
            # are declared with "jar" rather than "binary" and have none.
            quill_backend_components=[name for name in on_machine if "binary" in components[name]],
        )
    except (cpu_layout.LayoutError, RuntimeError) as exc:
        sys.exit(f"error: {exc}")

    print(cpu_layout.format_layout(layout))

    layout_path = run_dir / "cpu_layout.toml"
    layout_path.write_text(cpu_layout.render_layout_file(layout), encoding="utf-8")

    # The wrapper closes the two holes a C++ self-mask cannot: the window before
    # main() is entered, and the JVM components, which have no main() of ours.
    wrapper_path = run_dir / "background_tier"
    wrapper_path.write_text(cpu_layout.render_background_wrapper(layout), encoding="utf-8")
    wrapper_path.chmod(0o755)

    print()
    print(f"  wrote {layout_path}")
    print(f"  wrote {wrapper_path}")
    return layout_path


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
        sys.exit(
            f"error: create_db.py exited with code {result.returncode}\n"
            "  hint: if the database already exists and is up to date, re-run with --skip-db"
        )


def check_admin_service_db_url(env: dict) -> None:
    """Fail when [admin_service] db_url disagrees with the [db] section.

    Two keys name one database: the Python tooling connects with [db], the Java admin service
    with a JDBC URL that repeats the same host, port and name. A comment in each environment
    file was the only thing holding them together, and a host whose cluster is not on 5432 has
    to change both -- so the failure mode is a deploy that succeeds while the admin service
    alone cannot connect, reported as a refused connection nobody traces back to this file.
    """
    url = env.get("admin_service", {}).get("db_url", "")
    if not url:
        return
    match = re.match(r"jdbc:postgresql://([^:/]+):(\d+)/([^?]+)", url)
    if match is None:
        sys.exit(f"error: [admin_service] db_url is not jdbc:postgresql://host:port/name: {url}")

    db = env["db"]
    expected = (str(db["host"]), str(db["port"]), str(db["name"]))
    if match.groups() != expected:
        sys.exit(
            f"error: [admin_service] db_url disagrees with the [db] section\n"
            f"  db_url : {match.group(1)}:{match.group(2)}/{match.group(3)}\n"
            f"  [db]   : {expected[0]}:{expected[1]}/{expected[2]}\n"
            f"  both name the same database; change them together"
        )


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
    check_admin_service_db_url(env)

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
    # CPU registry files live under install_dir/run/.  This is the only place
    # that decides where, so the env TOMLs carry no value for it: two
    # installations on one machine must not share a registry.  The absolute paths
    # are injected here so operators can see the resolved paths in installed/etc/.
    cpu_run_dir = install_dir / "run"
    cpu_run_dir.mkdir(parents=True, exist_ok=True)
    cpu_registry_path = cpu_run_dir / "pubsub_cpu_registry"
    cpu_registry_lock_path = cpu_run_dir / "pubsub_cpu_registry.lock"
    namespace["shared_reactor_cpu_registry_shm_path"] = str(cpu_registry_path)
    namespace["shared_reactor_cpu_registry_lock_file"] = str(cpu_registry_lock_path)
    # Written below by resolve_cpu_layout(); the path is injected here because
    # the component TOMLs are expanded before the layout is computed, and it is
    # this file that decides where run/ lives.
    namespace["shared_reactor_cpu_layout_file"] = str(cpu_run_dir / "cpu_layout.toml")

    # Nothing else clears these -- they are ordinary files, not tmpfs entries that
    # a reboot would remove -- so a fresh install starts from an empty registry
    # rather than inheriting core claims recorded by a previous install.
    for stale in (cpu_registry_path, cpu_registry_lock_path):
        if stale.exists():
            stale.unlink()
            print(f"removed stale {stale.name}")

    # Resolve WAL directory paths relative to install_dir when not absolute.
    # The sequencer binary requires an absolute path; dev.toml stores them as
    # relative paths so they stay portable across machines and containers.
    for key in ("sequencer_primary_wal_directory", "sequencer_secondary_wal_directory"):
        if key in namespace:
            wal_path = Path(namespace[key])
            if not wal_path.is_absolute():
                wal_path = install_dir / wal_path
            wal_path.mkdir(parents=True, exist_ok=True)
            namespace[key] = str(wal_path)

    expand_templates(install_dir, namespace, map_config_files_to_components(env, install_dir))
    patch_jar_properties(env, install_dir, namespace)
    print()

    # Step 3: CPU core layout.  Runs on the target machine and reads its real
    # topology, so the same declaration resolves differently on a 32-core
    # workstation, a 20-core host or a VM.
    print("=== resolving CPU core layout ===")
    resolve_cpu_layout(env, install_dir, cpu_run_dir)
    print()

    # Step 4: TLS certificates
    if not args.skip_certs:
        print("=== generating TLS certificates ===")
        generate_tls_certs(env, install_dir, force=args.force_certs)
        print()

    # Steps 5-7: database and credentials
    if not args.skip_db:
        if not args.skip_create_db:
            print("=== creating database ===")
            run_create_db(env, args.drop_db, args.sudo_postgres, args.liquibase_contexts)
            print()

        print("=== provisioning FIX credentials ===")
        provision_fix_credentials(env)
        print()

        print("=== exporting credentials ===")
        run_export_credentials(env, install_dir)
        print()

    print("=== done ===")


if __name__ == "__main__":
    main()
