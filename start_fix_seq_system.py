#!/usr/bin/env python3
from __future__ import annotations
"""
start_fix_seq_system.py

Starts the sequencer-based FIX order flow system for testing with fix8.

Startup order:
  1. auth_service (primary)  -- gateway connects outbound to it at startup.
                               Must be up before the gateway.
  2. witness                 -- arbiter tie-breaker; arbiters connect outbound
                               to it (port 7100). Must be up before arbiters.
  3. arbiter (primary)       -- component listener port 7200, peer listener 7203.
                               Must be up before sequencers try to connect.
  4. arbiter (secondary)     -- component listener port 7201, peer listener 7204.
  5. order_gateway           -- must be listening on port 7010 before sequencers
                               start, because sequencers connect outbound to the
                               gateway's ER inbound listener at startup.
  6. sequencer (primary)     -- instance_id=1, listens on port 7001
  7. sequencer (secondary)   -- instance_id=2, listens on port 7002
  8. matching_engine         -- connects outbound to sequencer ER listener (7021)
  9. admin_service           -- Java admin web UI (port 8080)
 10. fix_test_client         -- Java FIX test client web UI (port 8081)

Usage with valgrind:
  ./start_fix_seq_system.py installed --valgrind --valgrind_command "vg"
"""

import argparse
import os
import signal
import subprocess
import sys
import time
import shlex
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Start the sequencer-based FIX order flow system.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "prefix",
        metavar="install_prefix",
        help="Path to the cmake install prefix (e.g. installed).",
    )
    parser.add_argument(
        "--startup-delay",
        type=float,
        default=1.0,
        metavar="SECONDS",
        help="Seconds to wait between starting each process (default: 1.0).",
    )
    parser.add_argument(
        "--valgrind",
        action="store_true",
        help="Run all processes under a valgrind-style wrapper.",
    )
    parser.add_argument(
        "--valgrind_command",
        default="vg",
        metavar="CMD",
        help="The command to use for valgrind (default: vg).",
    )
    return parser.parse_args()


def resolve_prefix(raw: str) -> Path:
    prefix = Path(raw).resolve()
    if not prefix.is_dir():
        sys.exit(f"ERROR: install prefix '{raw}' does not exist or is not a directory")
    return prefix


def check_executables(bin_dir: Path, names: list[str]) -> None:
    for name in names:
        exe = bin_dir / name
        if not exe.is_file():
            sys.exit(f"ERROR: {exe} not found")
        if not os.access(exe, os.X_OK):
            sys.exit(f"ERROR: {exe} is not executable")


def check_jars(jars: list[Path]) -> None:
    if not shutil.which("java"):
        sys.exit("ERROR: 'java' not found on PATH — install a JRE to run the Java components")
    for jar in jars:
        if not jar.is_file():
            sys.exit(f"ERROR: JAR not found: {jar}")


def check_wrapper(wrapper_cmd: str) -> None:
    main_exe = shlex.split(wrapper_cmd)[0]
    if not shutil.which(main_exe):
        sys.exit(f"ERROR: Wrapper command '{main_exe}' not found in PATH.")


def launch(name: str, cmd: list[str], log_dir: Path,
           cwd: Path | None = None) -> subprocess.Popen:
    stdout_path = log_dir / f"{name}.stdout"
    actual_cwd = cwd if cwd is not None else log_dir
    actual_cwd.mkdir(parents=True, exist_ok=True)
    print(f"Starting {name}...")
    print(f"  Command: {' '.join(cmd)}")

    with stdout_path.open("w") as stdout_file:
        proc = subprocess.Popen(
            cmd,
            cwd=str(actual_cwd),
            stdout=stdout_file,
            stderr=subprocess.STDOUT,
        )
    print(f"  {name} PID {proc.pid}")
    return proc


def tail_log(path: Path, lines: int = 20) -> None:
    """Print the last `lines` lines of a file if it exists and is non-empty."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return
    if not text.strip():
        return
    all_lines = text.splitlines()
    shown = all_lines[-lines:]
    print(f"  --- last {len(shown)} lines of {path.name} ---")
    for line in shown:
        print(f"  {line}")


def show_failure_logs(name: str, log_dir: Path) -> None:
    """Show the tail of whichever log files exist for a failed process."""
    # C++ processes write structured logs to <name>.log via Quill.
    # Java and all processes also have stdout/stderr captured in <name>.stdout.
    for suffix in (".log", ".stdout"):
        tail_log(log_dir / f"{name}{suffix}")


def shutdown(procs: list[tuple[str, subprocess.Popen]]) -> None:
    print("\nShutting down all processes...")
    for name, proc in procs:
        if proc.poll() is None:
            print(f"  Sending SIGTERM to {name} (PID {proc.pid})")
            proc.send_signal(signal.SIGTERM)
    for name, proc in procs:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print(f"  WARNING: {name} (PID {proc.pid}) did not exit within 5s")
    print("Done.")


def main() -> None:
    args = parse_args()
    prefix = resolve_prefix(args.prefix)

    bin_dir = prefix / "bin"
    etc_dir = prefix / "etc"
    log_dir = prefix / "log"

    cpp_executables = [
        "authentication_service",
        "witness",
        "arbiter",
        "sequencer",
        "matching_engine",
        "order_gateway",
    ]

    java_jars = [
        prefix / "lib" / "admin-service.jar",
        prefix / "lib" / "fix-test-client.jar",
    ]

    check_executables(bin_dir, cpp_executables)
    check_jars(java_jars)
    if args.valgrind:
        check_wrapper(args.valgrind_command)

    log_dir.mkdir(parents=True, exist_ok=True)

    # GNUInstallDirs uses lib64 on RHEL8; include both so the .so is found
    # regardless of platform without needing to probe which one CMake chose.
    lib_dirs = [str(d) for d in (prefix / "lib64", prefix / "lib") if d.is_dir()]
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    ldpath = ":".join(lib_dirs)
    os.environ["LD_LIBRARY_PATH"] = f"{ldpath}:{existing}" if existing else ldpath

    processes: list[tuple[str, subprocess.Popen]] = []

    # C++ components in startup order.
    # Tuple: (name, bin_name, log_name, config_path, workdir)
    # workdir=None means use log_dir (default for C++ apps that write their own logs).
    cpp_steps = [
        ("auth_service_primary",  "authentication_service", "auth_service_primary.log",
         etc_dir / "authentication_service" / "authentication_service.toml",
         etc_dir / "authentication_service"),
        ("witness",               "witness",                "witness.log",
         etc_dir / "witness"               / "witness.toml",                  None),
        ("arbiter_primary",       "arbiter",                "arbiter_primary.log",
         etc_dir / "arbiter"               / "arbiter.toml",                  None),
        ("arbiter_secondary",     "arbiter",                "arbiter_secondary.log",
         etc_dir / "arbiter"               / "arbiter_secondary.toml",        None),
        ("order_gateway",         "order_gateway",          "order_gateway.log",
         etc_dir / "order_gateway"         / "order_gateway.toml",            None),
        ("sequencer_primary",     "sequencer",              "sequencer_primary.log",
         etc_dir / "sequencer"             / "sequencer.toml",                None),
        ("sequencer_secondary",   "sequencer",              "sequencer_secondary.log",
         etc_dir / "sequencer"             / "sequencer_secondary.toml",      None),
        ("matching_engine",       "matching_engine",        "matching_engine.log",
         etc_dir / "matching_engine"       / "matching_engine.toml",          None),
    ]

    # Java components in startup order.
    # Tuple: (name, jar_path, config_path_or_None, log_name, workdir)
    java_steps = [
        ("admin_service",
         prefix / "lib" / "admin-service.jar",
         None,
         "admin_service.log",
         etc_dir / "admin_service"),
        ("fix_test_client",
         prefix / "lib" / "fix-test-client.jar",
         etc_dir / "fix_test_client" / "config" / "app.toml",
         "fix_test_client.log",
         etc_dir / "fix_test_client"),
    ]

    try:
        for name, bin_name, log_name, config, workdir in cpp_steps:
            if not config.is_file():
                sys.exit(f"ERROR: config file not found: {config}")

            app_args = [str(bin_dir / bin_name), str(log_dir / log_name), str(config)]

            if args.valgrind:
                cmd = shlex.split(args.valgrind_command) + app_args
            else:
                cmd = app_args

            proc = launch(name, cmd, log_dir, cwd=workdir)
            processes.append((name, proc))

            current_delay = args.startup_delay * 5 if args.valgrind else args.startup_delay
            time.sleep(current_delay)

        for name, jar, config, log_name, workdir in java_steps:
            cmd = ["java", "-jar", str(jar)]
            if config is not None:
                cmd.append(str(config))

            proc = launch(name, cmd, log_dir, cwd=workdir)
            processes.append((name, proc))

            current_delay = args.startup_delay * 5 if args.valgrind else args.startup_delay
            time.sleep(current_delay)

        print(f"\nAll processes started. Logs in {log_dir}/")
        print("Press Ctrl-C to shut everything down.\n")

        while True:
            time.sleep(1)
            for name, proc in processes:
                if proc.poll() is not None:
                    print(f"\nERROR: {name} (PID {proc.pid}) exited unexpectedly "
                          f"with code {proc.returncode}")
                    show_failure_logs(name, log_dir)
                    shutdown(processes)
                    sys.exit(1)

    except KeyboardInterrupt:
        shutdown(processes)


if __name__ == "__main__":
    main()
