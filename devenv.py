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
  --no-ha            Skip components marked ha_only = true.
  --delay SECONDS    Sleep between component starts (default: 1.0).

PID files are written to [run_dir]/<name>.pid as configured in the env TOML.
Logs are written to [log_dir]/<name>.log and [log_dir]/<name>.stdout.
Both directories are created automatically on first start.

The database password is read from PUBSUB_APP_DB_PASSWORD; if not set it falls
back to the dev default used by export_credentials.py.
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
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

_SCRIPT_DIR       = Path(__file__).resolve().parent
_DEFAULT_ENV_FILE = _SCRIPT_DIR / "environments" / "dev.toml"
_STARTUP_DELAY    = 1.0   # seconds between component starts
_SHUTDOWN_TIMEOUT = 10.0  # seconds to wait after SIGTERM before SIGKILL


# ── TOML helpers ──────────────────────────────────────────────────────────────

def load_env(path: Path) -> dict:
    with open(path, "rb") as file_handle:
        return tomllib.load(file_handle)


def resolve_paths(env: dict) -> tuple[Path, Path, Path]:
    """Return (install_dir, log_dir, run_dir) as absolute Paths."""
    install_dir = (_SCRIPT_DIR / env["paths"]["install_dir"]).resolve()
    log_dir     = (_SCRIPT_DIR / env["paths"]["log_dir"]).resolve()
    run_dir     = Path(env["paths"]["run_dir"]).expanduser().resolve()
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
    return run_dir / f"{name}.pid"


def write_pid(run_dir: Path, name: str, pid: int) -> None:
    _pid_path(run_dir, name).write_text(str(pid))


def read_pid(run_dir: Path, name: str) -> int | None:
    path = _pid_path(run_dir, name)
    if not path.exists():
        return None
    try:
        return int(path.read_text().strip())
    except ValueError:
        return None


def remove_pid(run_dir: Path, name: str) -> None:
    _pid_path(run_dir, name).unlink(missing_ok=True)


def is_pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


# ── Process management ────────────────────────────────────────────────────────

def build_command(
    name: str, comp: dict, install_dir: Path, log_dir: Path,
) -> tuple[list[str], Path]:
    """Return (command_list, working_dir) for a component."""
    workdir = (install_dir / comp["workdir"]).resolve()
    if "jar" in comp:
        jar_path = (install_dir / comp["jar"]).resolve()
        command = ["java", "-jar", str(jar_path)]
    else:
        binary_path = (install_dir / comp["binary"]).resolve()
        log_file    = log_dir / f"{name}.log"
        config_path = (install_dir / comp["config"]).resolve()
        command = [str(binary_path), str(log_file), str(config_path)]
    return command, workdir


def start_one(
    name: str, comp: dict,
    install_dir: Path, log_dir: Path, run_dir: Path,
    delay: float,
) -> None:
    existing_pid = read_pid(run_dir, name)
    if existing_pid is not None and is_pid_alive(existing_pid):
        print(f"  {name}: already running (PID {existing_pid}) — skipping")
        time.sleep(delay)
        return

    command, workdir = build_command(name, comp, install_dir, log_dir)
    stdout_path = log_dir / f"{name}.stdout"

    if not workdir.is_dir():
        sys.exit(f"error: workdir not found for {name}: {workdir}")
    if "binary" in comp:
        binary_path = (install_dir / comp["binary"]).resolve()
        if not binary_path.is_file():
            sys.exit(f"error: binary not found for {name}: {binary_path}")
    elif "jar" in comp:
        jar_path = (install_dir / comp["jar"]).resolve()
        if not jar_path.is_file():
            sys.exit(f"error: JAR not found for {name}: {jar_path}")

    with stdout_path.open("w") as stdout_file:
        proc = subprocess.Popen(
            command,
            cwd=str(workdir),
            stdout=stdout_file,
            stderr=subprocess.STDOUT,
        )
    write_pid(run_dir, name, proc.pid)
    print(f"  {name} — PID {proc.pid}")
    time.sleep(delay)


def stop_one(name: str, run_dir: Path, timeout: float = _SHUTDOWN_TIMEOUT) -> None:
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
    print(f"  {name}: stopped")


# ── Credential export ─────────────────────────────────────────────────────────

def export_credentials(install_dir: Path, env: dict) -> None:
    script      = _SCRIPT_DIR / "db" / "export_credentials.py"
    db          = env["db"]
    creds_file  = install_dir / "etc" / "authentication_service" / "credentials.toml"
    result = subprocess.run(
        [sys.executable, str(script),
         "--credentials-file", str(creds_file),
         "--db-host", db["host"],
         "--db-port", str(db["port"]),
         "--db-name", db["name"],
         "--db-user", db["user"]],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print("error: export_credentials.py failed:", file=sys.stderr)
        print(result.stderr.strip(), file=sys.stderr)
        sys.exit(1)
    print("  credentials exported")


# ── Subcommands ───────────────────────────────────────────────────────────────

def cmd_start(env: dict, ha_enabled: bool, delay: float) -> None:
    install_dir, log_dir, run_dir = resolve_paths(env)
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    order = startup_order(env, ha_enabled)

    print("=== exporting credentials ===")
    export_credentials(install_dir, env)
    print()

    print("=== starting components ===")
    for name in order:
        comp = env["components"][name]
        start_one(name, comp, install_dir, log_dir, run_dir, delay)
    print()
    print(f"all components started.  logs → {log_dir}/")


def cmd_stop(env: dict) -> None:
    _, _, run_dir = resolve_paths(env)
    # Stop everything that has a PID file, in reverse startup order so
    # dependents shut down before the services they depend on.
    order = list(reversed(env["startup_order"]["components"]))
    print("=== stopping components ===")
    for name in order:
        stop_one(name, run_dir)


def cmd_status(env: dict) -> None:
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


def cmd_restart(
    env: dict, ha_enabled: bool, delay: float, component: str | None,
) -> None:
    install_dir, log_dir, run_dir = resolve_paths(env)
    run_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    if component is not None:
        if component not in env["components"]:
            sys.exit(f"error: unknown component '{component}'")
        print(f"=== restarting {component} ===")
        stop_one(component, run_dir)
        # Re-export credentials whenever an auth service is (re)started so
        # that any DB changes are picked up before the service reads its file.
        if "authentication_service" in component:
            print("=== re-exporting credentials ===")
            export_credentials(install_dir, env)
            print()
        comp = env["components"][component]
        start_one(component, comp, install_dir, log_dir, run_dir, delay)
    else:
        cmd_stop(env)
        print()
        cmd_start(env, ha_enabled, delay)


# ── Entry point ───────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--env", type=Path, default=_DEFAULT_ENV_FILE, metavar="PATH",
        help=f"environment TOML file (default: {_DEFAULT_ENV_FILE})",
    )
    parser.add_argument(
        "--no-ha", action="store_true",
        help="skip components marked ha_only = true",
    )
    parser.add_argument(
        "--delay", type=float, default=_STARTUP_DELAY, metavar="SECONDS",
        help=f"seconds between component starts (default: {_STARTUP_DELAY})",
    )

    subparsers = parser.add_subparsers(dest="subcommand", metavar="subcommand")
    subparsers.required = True

    subparsers.add_parser("start",  help="start all components")
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
    args = parse_args()

    env_path = args.env.resolve() if args.env.is_absolute() else (_SCRIPT_DIR / args.env).resolve()
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    env = load_env(env_path)

    ha_from_toml = env.get("ha", {}).get("enabled", True)
    ha_enabled   = ha_from_toml and not args.no_ha

    if args.subcommand == "start":
        cmd_start(env, ha_enabled, args.delay)
    elif args.subcommand == "stop":
        cmd_stop(env)
    elif args.subcommand == "status":
        cmd_status(env)
    elif args.subcommand == "restart":
        cmd_restart(env, ha_enabled, args.delay, args.component)


if __name__ == "__main__":
    main()
