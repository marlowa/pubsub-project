#!/usr/bin/env python3
"""
devenv.py — manage the pubsub developer sandbox.

Subcommands:
  start              Start all components in dependency order.
  stop               Stop all running components (reverse startup order).
  status             Show running/stopped status for each component.
  restart [name]     Stop and re-start all components, or one named component.

Options:
  --env PATH         Environment TOML (default: environments/dev.toml).
  --no-ha            Skip components marked ha_only = true.  Refused when the environment
                     file still says [ha] enabled = true, because the deployed configs
                     would then expect components this flag does not start.
  --delay SECONDS    Sleep between component starts (default: 1.0).
  --debug            Override applog_level to 'debug' in C++ configs before starting.
  --supervised       Start each component under scripts/launch.py, so that a component
                     which dies is restarted. Off by default. The launcher owns
                     <name>.pid and writes the COMPONENT's pid there, so status, perf
                     and the resource monitor are unaffected; the launcher's own pid
                     goes to <name>.launcher.pid, and stop signals that.

PID files are written to [run_dir]/<name>.pid as configured in the env TOML.
Logs are written to [log_dir]/<name>.log and [log_dir]/<name>.stdout.
Both directories are created automatically on first start.

The database password is read from PUBSUB_APP_DB_PASSWORD; if not set it falls
back to the dev default used by export_credentials.py.

Java components (admin_service, fix_test_client) are launched with
  java -jar <jar> [<config>]
where <config> is appended as the first positional argument when the component
defines a 'config' key in the env TOML.  C++ components are launched with
  <binary> <log_file> <config>
as before.
"""

from __future__ import annotations
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ImportError:
        sys.exit("error: Python 3.11+ or the 'tomli' package is required to parse TOML")

_SCRIPT_DIR       = Path(__file__).resolve().parent
# The tree above scripts/. Sibling scripts are reached through the script directory;
# everything the project owns -- environments/, db/, installed/ -- hangs off here.
_PROJECT_ROOT     = _SCRIPT_DIR.parent
_DEFAULT_ENV_FILE = _PROJECT_ROOT / "environments" / "dev.toml"
_STARTUP_DELAY    = 1.0   # seconds between component starts
_SHUTDOWN_TIMEOUT = 10.0  # seconds to wait after SIGTERM before SIGKILL


# ── TOML helpers ──────────────────────────────────────────────────────────────

def load_env(path: Path) -> dict:
    """Load and return the environment TOML file as a nested dict."""
    with open(path, "rb") as file_handle:
        return tomllib.load(file_handle)


def resolve_paths(env: dict) -> tuple[Path, Path, Path]:
    """Return (install_dir, log_dir, run_dir) as absolute Paths."""
    install_dir = (_PROJECT_ROOT / env["paths"]["install_dir"]).resolve()
    log_dir     = (_PROJECT_ROOT / env["paths"]["log_dir"]).resolve()
    run_dir     = (_PROJECT_ROOT / env["paths"]["run_dir"]).resolve()
    return install_dir, log_dir, run_dir


def startup_order(env: dict, ha_enabled: bool) -> list[str]:
    """Return component names in launch order, optionally excluding ha_only entries."""
    names = env["startup_order"]["components"]
    if not ha_enabled:
        components = env["components"]
        names = [name for name in names if not components[name].get("ha_only", False)]
    return names


# ── PID file helpers ──────────────────────────────────────────────────────────

def _pid_path(run_dir: Path, name: str) -> Path:
    """Return the path of the PID file for the named component."""
    return run_dir / f"{name}.pid"


def write_pid(run_dir: Path, name: str, pid: int) -> None:
    """Write pid to the PID file for the named component."""
    _pid_path(run_dir, name).write_text(str(pid))


def launcher_pid_path(run_dir: Path, name: str) -> Path:
    """Where launch.py records its own pid, distinct from the component's.

    A supervised component has two processes: the launcher and the component it started.
    <name>.pid always names the component -- so status, perf and the resource monitor need no
    knowledge of any of this -- and the launcher goes here.
    """
    return run_dir / f"{name}.launcher.pid"


def read_launcher_pid(run_dir: Path, name: str) -> int | None:
    """The launcher's pid, or None when the component is not supervised."""
    path = launcher_pid_path(run_dir, name)
    if not path.is_file():
        return None
    try:
        return int(path.read_text().strip())
    except (ValueError, OSError):
        return None


def read_pid(run_dir: Path, name: str) -> int | None:
    """Return the PID from the PID file for name, or None if absent or unreadable."""
    path = _pid_path(run_dir, name)
    if not path.exists():
        return None
    try:
        return int(path.read_text().strip())
    except ValueError:
        return None


def remove_pid(run_dir: Path, name: str) -> None:
    """Delete the PID file for the named component if it exists."""
    _pid_path(run_dir, name).unlink(missing_ok=True)


def is_pid_alive(pid: int) -> bool:
    """Return True if a process with the given pid exists and is reachable."""
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


# ── Process management ────────────────────────────────────────────────────────

def build_command(
    name: str, comp: dict, install_dir: Path, log_dir: Path, run_dir: Path, debug: bool = False,
) -> tuple[list[str], Path]:
    """Return (command_list, working_dir) for a component.

    For C++ binaries the command is: <binary> <log_file> <config>.
    For JAR components the command is: java [-Djavax.net.debug=...] -jar <jar> [<config>],
    where the resolved config path is appended as the first positional argument only when
    the component defines a 'config' key in the env TOML.  When debug=True, the JVM
    flag -Djavax.net.debug=ssl:handshake:data is added to expose MINA/SSL details.
    For 'command' components the tool is taken from PATH and its args used verbatim.
    """
    workdir = (install_dir / comp["workdir"]).resolve()
    if "command" in comp:
        # An external tool, found on PATH rather than installed into install_dir --
        # Prometheus is the one such component. Its arguments are given verbatim in the env
        # TOML because they follow that tool's own conventions, not this project's
        # <log> <config> pair. {run_dir} and {install_dir} are substituted so a path can be
        # written without knowing where the tree was deployed.
        command = [comp["command"]] + [
            argument.format(run_dir=run_dir, install_dir=install_dir) for argument in comp.get("args", [])
        ]
    elif "jar" in comp:
        jar_path = (install_dir / comp["jar"]).resolve()
        command = ["java"]
        if debug:
            command.append("-Djavax.net.debug=ssl:handshake:data")
        command.extend(["-jar", str(jar_path)])
        if "config" in comp:
            command.append(str((install_dir / comp["config"]).resolve()))
    else:
        binary_path = (install_dir / comp["binary"]).resolve()
        log_file    = log_dir / f"{name}.log"
        config_path = (install_dir / comp["config"]).resolve()
        command = [str(binary_path), str(log_file), str(config_path)]
    return command, workdir


def start_one(  # pylint: disable=too-many-arguments,too-many-locals
    name: str, comp: dict,
    install_dir: Path, log_dir: Path, run_dir: Path,
    delay: float, debug: bool = False, supervised: bool = False,
) -> None:
    """Start a single component, writing a PID file on success.

    With supervised=True the component is started under scripts/launch.py, which restarts it
    if it dies. The launcher then owns <name>.pid and writes the COMPONENT's pid there, so
    status, perf_run.py and the resource monitor are unaffected and need to know nothing about
    it. Supervision is off by default: a component started without it behaves identically,
    which is what keeps the launcher optional.

    If the component is already running the start is skipped.  The process is
    launched with its working directory set to comp['workdir'] (created if
    absent) and with install_dir/lib prepended to LD_LIBRARY_PATH so that
    libpubsub_itc_fw.so is found at runtime.  stdout and stderr are redirected
    to log_dir/<name>.stdout.
    """
    existing_pid = read_pid(run_dir, name)
    if existing_pid is not None and is_pid_alive(existing_pid):
        print(f"  {name}: already running (PID {existing_pid}) — skipping")
        time.sleep(delay)
        return

    command, workdir = build_command(name, comp, install_dir, log_dir, run_dir, debug=debug)

    # Start every process in the machine's background CPU tier.  deploy.py
    # generates the wrapper from the resolved layout; a deployment that predates
    # it simply runs unwrapped, as before.  This is what covers the JVM
    # components, which cannot mask themselves the way the C++ ones do, and the
    # window before a C++ main() is entered.
    background_wrapper = run_dir / "background_tier"
    if background_wrapper.is_file() and os.access(background_wrapper, os.X_OK):
        command = [str(background_wrapper)] + command

    stdout_path = log_dir / f"{name}.stdout"

    if "binary" in comp:
        binary_path = (install_dir / comp["binary"]).resolve()
        if not binary_path.is_file():
            sys.exit(f"error: binary not found for {name}: {binary_path}")
    elif "jar" in comp:
        jar_path = (install_dir / comp["jar"]).resolve()
        if not jar_path.is_file():
            sys.exit(f"error: JAR not found for {name}: {jar_path}")
    elif "command" in comp:
        # Warn and skip rather than exit. An external tool is not part of this project's
        # build, so a machine that has not installed it is a normal state, not a broken
        # deployment -- and the venue must still come up. Refusing to start a trading system
        # because a monitoring tool is absent gets the priority exactly backwards.
        if shutil.which(comp["command"]) is None:
            print(f"  {name}: '{comp['command']}' not found on PATH — skipping")
            return

    workdir.mkdir(parents=True, exist_ok=True)

    # GNUInstallDirs uses lib64 on RHEL8; include both so the .so is found
    # regardless of platform without needing to probe which one CMake chose.
    lib_dirs = [str(d) for d in (install_dir / "lib64", install_dir / "lib") if d.is_dir()]
    child_env = os.environ.copy()
    existing_ldpath = child_env.get("LD_LIBRARY_PATH", "")
    ldpath = ":".join(lib_dirs)
    child_env["LD_LIBRARY_PATH"] = f"{ldpath}:{existing_ldpath}" if existing_ldpath else ldpath

    if supervised:
        # launch.py restarts the component if it dies. It is put in front of the command
        # rather than given knowledge of the venue: it wraps one process, knows only the
        # command line below, and has no idea what a primary or a leader is.
        command = [sys.executable, str(_SCRIPT_DIR / "launch.py"),
                   "--name", name, "--run-dir", str(run_dir), "--"] + list(command)

    with stdout_path.open("w") as stdout_file:
        proc = subprocess.Popen(  # pylint: disable=consider-using-with
            command,
            cwd=str(workdir),
            stdout=stdout_file,
            stderr=subprocess.STDOUT,
            env=child_env,
        )

    if supervised:
        # Deliberately NOT written here. launch.py puts the component's pid in <name>.pid --
        # writing the launcher's pid there instead would point every other tool at the wrapper,
        # and the two would race to own the same file.
        pass
    else:
        write_pid(run_dir, name, proc.pid)
    time.sleep(delay)

    # Report what actually happened, not merely what was launched. Popen succeeds as soon as
    # the fork does, so a process that exits immediately -- a port already taken, a config it
    # will not parse -- was previously announced as started, left a PID file behind, and was
    # only discovered later by `status` or by wondering why nothing worked.
    if not is_pid_alive(proc.pid):
        remove_pid(run_dir, name)
        print(f"  {name} — FAILED: exited immediately (see {stdout_path})")
        return

    print(f"  {name} — PID {proc.pid}")


def stop_supervised(name: str, run_dir: Path, launcher_pid: int, timeout: float) -> None:
    """Stop a component that is running under launch.py, by stopping its launcher.

    The launcher forwards the signal to the component and exits without restarting it, then
    removes both pid files itself. Waiting for the launcher to go is therefore waiting for the
    component to have gone too.
    """
    os.kill(launcher_pid, signal.SIGTERM)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not is_pid_alive(launcher_pid):
            break
        time.sleep(0.1)
    else:
        print(f"  {name} (launcher {launcher_pid}): still alive after {timeout:.0f}s — sending SIGKILL")
        try:
            os.kill(launcher_pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        # A SIGKILLed launcher cannot tidy up after itself, and cannot have stopped its child
        # either, so the component is dealt with directly and both files are removed here.
        component_pid = read_pid(run_dir, name)
        if component_pid is not None and is_pid_alive(component_pid):
            try:
                os.kill(component_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        remove_pid(run_dir, name)

    launcher_pid_path(run_dir, name).unlink(missing_ok=True)
    print(f"  {name} (launcher {launcher_pid}): stopped")


def stop_one(name: str, run_dir: Path, timeout: float = _SHUTDOWN_TIMEOUT) -> None:
    """Stop a single running component by sending SIGTERM, then SIGKILL if needed.

    If no PID file exists the component is assumed to be already stopped.  A
    stale PID file (process no longer alive) is removed without sending any
    signal.  After SIGTERM the function polls until the process exits or the
    timeout elapses, at which point SIGKILL is sent.
    """
    # A supervised component has a launcher in front of it, and killing the component alone
    # would simply have the launcher start it again. Signalling the launcher is what stops
    # the pair: it forwards the signal to the component, waits, and stands down.
    launcher_pid = read_launcher_pid(run_dir, name)
    if launcher_pid is not None and is_pid_alive(launcher_pid):
        stop_supervised(name, run_dir, launcher_pid, timeout)
        return

    pid = read_pid(run_dir, name)
    if pid is None:
        print(f"  {name}: no PID file — skipping")
        return
    if not is_pid_alive(pid):
        print(f"  {name}: not running (stale PID {pid})")
        remove_pid(run_dir, name)
        return

    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not is_pid_alive(pid):
            break
        time.sleep(0.1)
    else:
        print(f"  {name} (PID {pid}): still alive after {timeout:.0f}s — sending SIGKILL")
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    remove_pid(run_dir, name)
    print(f"  {name} (PID {pid}): stopped")


# ── Prometheus scrape configuration ───────────────────────────────────────────

def discover_scrape_targets(install_dir: Path) -> list[tuple[str, str, int]]:
    """Return (component, host, port) for every deployed component with metrics enabled.

    Read out of the deployed configs rather than listed anywhere, because those files are
    what the processes themselves read: a target discovered here is by construction the
    address a process will actually bind. A hand-maintained list in a scrape config would
    be a second copy of the same facts, and the failure mode is silent -- a component added
    without a matching target simply never appears in a dashboard, and nothing complains.

    The component name comes from `metrics.component`, so it matches the label the process
    puts on its own series. Components with metrics disabled, or with no [metrics] section
    at all, are skipped: scraping them would only record `up 0` for ever.
    """
    targets: list[tuple[str, str, int]] = []
    etc_dir = install_dir / "etc"
    if not etc_dir.is_dir():
        return targets

    for config_path in sorted(etc_dir.rglob("*.toml")):
        try:
            with open(config_path, "rb") as file_handle:
                data = tomllib.load(file_handle)
        except (tomllib.TOMLDecodeError, OSError):
            # credentials.toml and friends are not component configs; a file that will not
            # parse is not this function's problem to report.
            continue
        metrics = data.get("metrics")
        if not isinstance(metrics, dict) or not metrics.get("enabled"):
            continue
        host = metrics.get("listen_host")
        port = metrics.get("listen_port")
        component = metrics.get("component")
        if not host or not port or not component:
            continue
        # A configured port of 0 means "let the operating system choose", so the real port
        # is not knowable from configuration and the process must be asked. Nothing in the
        # deployed venue does this -- it exists for tests -- so it is skipped rather than
        # guessed at.
        if int(port) == 0:
            print(f"  note: {component} uses an ephemeral metrics port — not scraped")
            continue
        targets.append((component, host, int(port)))

    return targets


def write_prometheus_scrape_config(env: dict, install_dir: Path, run_dir: Path) -> Path:
    """Generate the Prometheus scrape config from the deployed venue, and return its path.

    Written into run_dir on every start, so it always describes the venue about to run.
    """
    settings = env.get("prometheus", {})
    scrape_interval = settings.get("scrape_interval", "5s")
    scrape_timeout = settings.get("scrape_timeout", "4s")
    environment_name = settings.get("environment_label", "dev")

    targets = discover_scrape_targets(install_dir)
    target_lines = "\n".join(
        f"          - {host}:{port}   # {component}" for component, host, port in targets
    )

    # Jobs for things that are not venue components and so cannot be discovered from the
    # deployed configs -- a machine-metrics exporter, typically. Listed in the env TOML
    # because they are a property of the host rather than of the venue.
    extra_job_blocks = []
    for extra in settings.get("extra_job", []):
        job_name = extra["name"]
        extra_targets = "\n".join(f"          - {target}" for target in extra["targets"])
        comment = extra.get("comment", "")
        comment_line = f"    # {comment}\n" if comment else ""
        extra_job_blocks.append(
            f"\n{comment_line}  - job_name: {job_name}\n"
            f"    static_configs:\n"
            f"      - targets:\n{extra_targets}\n"
        )
    extra_jobs = "".join(extra_job_blocks)

    content = f"""# GENERATED by devenv.py on every start -- do not edit; edits are overwritten.
#
# Targets are discovered from the deployed component configs under
# {install_dir}/etc, which is what those processes themselves read, so this
# cannot drift from the venue it describes.
#
# Every target already carries application, component and scope labels composed by
# MetricKey from its own configuration, so there is deliberately no job-per-component here:
# that would duplicate what the exposition already says and then disagree with it the first
# time something was renamed in only one of the two places. The default `instance` label is
# "host:port", which identifies a process without naming it -- group by `component`.

global:
  scrape_interval: {scrape_interval}
  scrape_timeout: {scrape_timeout}
  external_labels:
    environment: {environment_name}

scrape_configs:
  - job_name: pubsub_venue
    static_configs:
      - targets:
{target_lines}
{extra_jobs}"""

    config_path = run_dir / "prometheus.yml"
    config_path.write_text(content, encoding="utf-8")
    extra_count = sum(len(extra["targets"]) for extra in settings.get("extra_job", []))
    print(f"  scrape config: {config_path} ({len(targets)} venue + {extra_count} other target(s))")
    return config_path


# ── Credential export ─────────────────────────────────────────────────────────

def export_credentials(install_dir: Path, env: dict) -> None:
    """Re-export SCRAM credentials from the database to credentials.toml.

    Calls db/export_credentials.py using the database settings from the env
    TOML.  Exits the process if the script fails.
    """
    script      = _PROJECT_ROOT / "db" / "export_credentials.py"
    db          = env["db"]
    creds_file  = install_dir / "etc" / "authentication_service" / "credentials.toml"
    result = subprocess.run(
        [sys.executable, str(script),
         "--credentials-file", str(creds_file),
         "--db-host", db["host"],
         "--db-port", str(db["port"]),
         "--db-name", db["name"],
         "--db-user", db["user"]],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        print("error: export_credentials.py failed:", file=sys.stderr)
        print(result.stderr.strip(), file=sys.stderr)
        # psql names the host and port it could not reach but not where they came from, which
        # sends the reader looking for a hardcoded default that does not exist. Both are the
        # environment file's, and a refused connection means no server there at all.
        print(f"  connection details are the [db] section of the environment file: "
              f"{db['host']}:{db['port']}/{db['name']} as user {db['user']}",
              file=sys.stderr)
        sys.exit(1)
    print("  credentials exported")


# ── Subcommands ───────────────────────────────────────────────────────────────

def patch_debug_logging(install_dir: Path) -> None:
    """Override applog_level to 'debug' in all deployed C++ component TOML configs."""
    etc_dir = install_dir / "etc"
    for config_file in sorted(etc_dir.glob("*/*.toml")):
        content = config_file.read_text()
        patched = re.sub(r'(applog_level\s*=\s*)"[^"]*"', r'\1"debug"', content)
        if patched != content:
            config_file.write_text(patched)
            print(f"  debug logging enabled: {config_file.relative_to(install_dir)}")


# Matches an unexpanded ${placeholder} (identifier only, mirroring deploy.py), so a
# literal '$' in a value is not mistaken for one.
_UNEXPANDED_PLACEHOLDER = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*\}")


def check_configs_expanded(install_dir: Path) -> None:
    """Abort if any installed config still holds an unexpanded ${placeholder}.

    cmake --install copies the config TEMPLATES into installed/etc verbatim; deploy.py
    is what expands them for the target environment. Starting the components against
    un-expanded configs makes each one crash on parse -- which looks like a mysterious
    total outage (e.g. "no ER comes back") rather than a config problem. Fail loudly
    here, naming the offending files and the fix, instead of letting the pipeline die
    silently. This commonly happens after a rebuild: cmake --install re-lays the
    templates, so deploy.py must be re-run before start.
    """
    etc_dir = install_dir / "etc"
    offenders: list[tuple[Path, list[str]]] = []
    for toml_path in sorted(etc_dir.rglob("*.toml")):
        try:
            text = toml_path.read_text()
        except OSError:
            continue
        placeholders = sorted(set(_UNEXPANDED_PLACEHOLDER.findall(text)))
        if placeholders:
            offenders.append((toml_path.relative_to(install_dir), placeholders))

    if offenders:
        print("error: installed configs still contain unexpanded ${placeholder}s "
              "-- they have not been deployed:", file=sys.stderr)
        for rel, placeholders in offenders:
            print(f"  {rel}: {', '.join(placeholders)}", file=sys.stderr)
        sys.exit("Run deploy.py to expand the configs for this environment before starting "
                 "(cmake --install re-lays the templates unexpanded, so re-deploy after every build).")


def cmd_start(  # pylint: disable=too-many-arguments
    env: dict, ha_enabled: bool, delay: float, debug: bool = False,
    component: str | None = None, with_prometheus: bool = True, supervised: bool = False,
) -> None:
    """Implement the 'start' subcommand: export credentials then start all components.

    Components are started in the order listed in startup_order.components,
    with ha_only components skipped when ha_enabled is False.  When component
    is given, only that single component is started and the full-stack preamble
    (CPU registry reset, credential export) is skipped.

    Components marked metrics_only are skipped when with_prometheus is False.  They are the
    observability sidecars rather than part of the venue, so a host that has no Prometheus
    -- or simply does not want one -- starts the venue exactly as before.
    """
    install_dir, log_dir, run_dir = resolve_paths(env)
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    # Refuse to start against un-deployed (still-templated) configs -- see the function
    # docstring. Guards both the full-stack and single-component paths.
    check_configs_expanded(install_dir)

    if component is not None:
        if component not in env["components"]:
            sys.exit(f"error: unknown component '{component}'")
        print(f"=== starting {component} ===")
        comp = env["components"][component]
        # Starting the scraper on its own still needs a scrape config, and regenerating it
        # picks up whatever has been deployed since -- which is the usual reason for
        # restarting it by itself.
        if comp.get("metrics_only", False):
            write_prometheus_scrape_config(env, install_dir, run_dir)
        start_one(component, comp, install_dir, log_dir, run_dir, delay, debug=debug, supervised=supervised)
        return

    order = startup_order(env, ha_enabled)

    # CPU registry lives under install_dir/run/ (same location that deploy.py
    # configures in the component TOMLs).  Remove the stale file on every start
    # so the first process recreates it from scratch; without this, corrupt or
    # zero-filled entries from a previous run fill the table and every process
    # independently claims the same CPUs.
    cpu_run_dir = install_dir / "run"
    cpu_run_dir.mkdir(parents=True, exist_ok=True)
    for stale in (cpu_run_dir / "pubsub_cpu_registry", cpu_run_dir / "pubsub_cpu_registry.lock"):
        if stale.exists():
            stale.unlink()
            print(f"removed stale {stale.name}")

    print("=== exporting credentials ===")
    export_credentials(install_dir, env)
    print()

    if debug:
        print("=== enabling debug logging ===")
        patch_debug_logging(install_dir)
        print()

    if not with_prometheus:
        order = [name for name in order if not env["components"][name].get("metrics_only", False)]
    elif any(env["components"][name].get("metrics_only", False) for name in order):
        # Generated before anything starts, so the scrape config describes this venue. It
        # reads the deployed configs, not the running processes, so the ordering does not
        # matter -- and a target that is not up yet simply reads `up 0` until it is.
        print("=== generating prometheus scrape config ===")
        write_prometheus_scrape_config(env, install_dir, run_dir)
        print()

    print("=== starting components ===")
    for name in order:
        comp = env["components"][name]
        start_one(name, comp, install_dir, log_dir, run_dir, delay, debug=debug, supervised=supervised)
    print()
    print(f"all components started.  logs → {log_dir}/")


def cmd_stop(env: dict) -> None:
    """Implement the 'stop' subcommand: stop all components in reverse startup order.

    Every component with a PID file is stopped regardless of whether HA is
    enabled, so a partially-started HA environment is fully cleaned up.
    """
    _, _, run_dir = resolve_paths(env)
    order = list(reversed(env["startup_order"]["components"]))
    print("=== stopping components ===")
    for name in order:
        stop_one(name, run_dir)


def cmd_status(env: dict) -> None:
    """Implement the 'status' subcommand: print running/stopped status for every component."""
    _, _, run_dir = resolve_paths(env)
    all_components = env["startup_order"]["components"]
    print(f"  {'component':<38}  {'PID':<8}  status")
    print(f"  {'-'*38}  {'-'*8}  ------")
    for name in all_components:
        comp    = env["components"][name]
        ha_tag  = " [ha]" if comp.get("ha_only") else ""
        label   = name + ha_tag
        pid     = read_pid(run_dir, name)
        if pid is None:
            print(f"  {label:<38}  {'—':<8}  stopped")
        elif is_pid_alive(pid):
            print(f"  {label:<38}  {pid:<8}  running")
        else:
            print(f"  {label:<38}  {pid:<8}  dead (stale PID)")


def cmd_restart(  # pylint: disable=too-many-arguments
    env: dict, ha_enabled: bool, delay: float, component: str | None, debug: bool = False,
    with_prometheus: bool = True, supervised: bool = False,
) -> None:
    """Implement the 'restart' subcommand: stop and restart all or one named component.

    When component is None every component is stopped then started via cmd_stop
    and cmd_start.  When a single component name is given only that component is
    cycled; credentials are re-exported first if the component name contains
    'authentication_service' so that any database changes are picked up.
    """
    install_dir, log_dir, run_dir = resolve_paths(env)
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    # A single-component restart starts start_one directly (below), bypassing cmd_start,
    # so guard the still-templated-config case here too (the full restart re-checks via
    # cmd_start, which is a cheap no-op).
    check_configs_expanded(install_dir)

    if component is not None:
        if component not in env["components"]:
            sys.exit(f"error: unknown component '{component}'")
        print(f"=== restarting {component} ===")
        stop_one(component, run_dir)
        if "authentication_service" in component:
            print("=== re-exporting credentials ===")
            export_credentials(install_dir, env)
            print()
        comp = env["components"][component]
        if comp.get("metrics_only", False):
            write_prometheus_scrape_config(env, install_dir, run_dir)
        start_one(component, comp, install_dir, log_dir, run_dir, delay, debug=debug, supervised=supervised)
    else:
        cmd_stop(env)
        print()
        cmd_start(env, ha_enabled, delay, debug=debug, with_prometheus=with_prometheus, supervised=supervised)


# ── Entry point ───────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    """Parse and return command-line arguments."""
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--env", type=Path, default=_DEFAULT_ENV_FILE, metavar="PATH",
        help=f"environment TOML file (default: {_DEFAULT_ENV_FILE})",
    )
    parser.add_argument(
        "--db-port", type=int, default=None, metavar="PORT",
        help="PostgreSQL port, overriding the [db] section of the environment file. Used when "
             "re-exporting credentials before the auth service starts, and matching deploy.py's "
             "flag of the same name so a host with a non-default cluster names it the same way "
             "at deploy time and at start time.",
    )
    parser.add_argument(
        "--no-ha", action="store_true",
        help="skip components marked ha_only = true; refused unless [ha] enabled is already false",
    )
    parser.add_argument(
        "--debug", action="store_true",
        help="override applog_level to 'debug' in all C++ component configs before starting",
    )
    parser.add_argument(
        "--supervised", action="store_true",
        help="start each component under scripts/launch.py, which restarts it if it dies. "
             "Off by default: a component started without it behaves identically, which is "
             "what keeps the launcher optional",
    )
    parser.add_argument(
        "--no-prometheus", action="store_true",
        help="skip components marked metrics_only = true (the Prometheus scraper). "
             "The venue itself still exposes its metrics endpoints; nothing collects them",
    )
    parser.add_argument(
        "--delay", type=float, default=_STARTUP_DELAY, metavar="SECONDS",
        help=f"seconds between component starts (default: {_STARTUP_DELAY})",
    )

    subparsers = parser.add_subparsers(dest="subcommand", metavar="subcommand")
    subparsers.required = True

    start_parser = subparsers.add_parser("start", help="start all components, or one named component")
    start_parser.add_argument(
        "component", nargs="?", default=None, metavar="name",
        help="component to start (omit to start everything)",
    )
    subparsers.add_parser("stop",   help="stop all running components")
    subparsers.add_parser("status", help="show component status")

    restart_parser = subparsers.add_parser(
        "restart", help="stop and re-start all components, or one named component",
    )
    restart_parser.add_argument(
        "component", nargs="?", default=None, metavar="name",
        help="component to restart (omit to restart everything)",
    )

    return parser.parse_args()


def main() -> None:
    """Entry point: load the env TOML, resolve the HA flag, dispatch to the requested subcommand."""
    sys.stdout.reconfigure(line_buffering=True)
    args = parse_args()

    env_path = args.env.resolve() if args.env.is_absolute() else (_PROJECT_ROOT / args.env).resolve()
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    env = load_env(env_path)
    if args.db_port is not None:
        env["db"]["port"] = args.db_port

    ha_from_toml = env.get("ha", {}).get("enabled", True)

    # --no-ha skips launching ha_only components. It does NOT change what was deployed, and until
    # 2026-08-28 nothing noticed the difference: the flag suppressed the arbiters while every
    # deployed config still said high availability was on, so the sequencer waited for an arbiter
    # that would never exist, never became leader, and forwarded no orders at all -- while
    # acknowledging every member and logging nothing wrong. See docs/bug_list.md, BUG-0061.
    #
    # The flag keeps its meaning and loses the ability to disagree. To run without high
    # availability, say so where every component reads it.
    if args.no_ha and ha_from_toml:
        sys.exit(
            f"error: --no-ha would skip the high-availability components, but {env_path} has\n"
            "       [ha] enabled = true, so the deployed configs expect them. The sequencer would\n"
            "       wait for an arbiter that is never started and forward no orders.\n"
            "\n"
            "       Set [ha] enabled = false in the environment file and re-run deploy.py, which\n"
            "       makes every component agree. --no-ha is then unnecessary but harmless."
        )

    ha_enabled = ha_from_toml

    with_prometheus = not args.no_prometheus

    if args.subcommand == "start":
        cmd_start(env, ha_enabled, args.delay, debug=args.debug, component=args.component, supervised=args.supervised,
                  with_prometheus=with_prometheus)
    elif args.subcommand == "stop":
        cmd_stop(env)
    elif args.subcommand == "status":
        cmd_status(env)
    elif args.subcommand == "restart":
        cmd_restart(env, ha_enabled, args.delay, args.component, debug=args.debug, supervised=args.supervised,
                    with_prometheus=with_prometheus)


if __name__ == "__main__":
    main()
