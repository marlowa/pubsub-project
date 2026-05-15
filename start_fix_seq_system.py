#!/usr/bin/env python3
"""
start_fix_seq_system.py

Starts the sequencer-based FIX order flow system for testing with fix8.

Startup order:
  1. arbiter                -- must be up before sequencers try to connect
  2. sample_fix_gateway_seq -- must be listening on port 7010 before sequencers
                               start, because sequencers connect outbound to the
                               gateway's ER inbound listener at startup.
  3. sequencer (primary)    -- instance_id=1, listens on port 7001
  4. matching_engine        -- connects outbound to sequencer ER listener (7021)

Usage with valgrind:
  ./start_fix_seq_system.py build/installed --valgrind --valgrind_command "vg"
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
        help="Path to the cmake install prefix (e.g. build/installed).",
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


def check_wrapper(wrapper_cmd: str) -> None:
    """Ensure the wrapper (e.g., 'vg' or 'valgrind') exists in PATH."""
    main_exe = shlex.split(wrapper_cmd)[0]
    if not shutil.which(main_exe):
        sys.exit(f"ERROR: Wrapper command '{main_exe}' not found in PATH.")


def launch(name: str, cmd: list[str], log_dir: Path) -> subprocess.Popen:
    stdout_path = log_dir / f"{name}.stdout"
    print(f"Starting {name}...")
    print(f"  Command: {' '.join(cmd)}")

    with stdout_path.open("w") as stdout_file:
        proc = subprocess.Popen(
            cmd,
            cwd=str(log_dir),
            stdout=stdout_file,
            stderr=subprocess.STDOUT,
        )
    print(f"  {name} PID {proc.pid}")
    return proc


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

    executables = [
        "arbiter",
        "sequencer",
        "matching_engine",
        "sample_fix_gateway_seq",
    ]

    check_executables(bin_dir, executables)
    if args.valgrind:
        check_wrapper(args.valgrind_command)

    log_dir.mkdir(parents=True, exist_ok=True)

    # Extend LD_LIBRARY_PATH so the installed shared library is found.
    lib_dir = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    processes: list[tuple[str, subprocess.Popen]] = []

    # Definition of the system components
    steps_data = [
        ("arbiter", "arbiter", "arbiter.log",
         etc_dir / "arbiter" / "arbiter.toml"),
        ("sample_fix_gateway_seq", "sample_fix_gateway_seq", "sample_fix_gateway_seq.log",
         etc_dir / "sample_fix_gateway_seq" / "sample_fix_gateway_seq.toml"),
        ("sequencer_primary", "sequencer", "sequencer_primary.log",
         etc_dir / "sequencer" / "sequencer.toml"),
        ("matching_engine", "matching_engine", "matching_engine.log",
         etc_dir / "matching_engine" / "matching_engine.toml"),
    ]

    try:
        for name, bin_name, log_name, config in steps_data:
            if not config.is_file():
                sys.exit(f"ERROR: config file not found: {config}")

            # Assemble core application arguments
            app_args = [str(bin_dir / bin_name), str(log_dir / log_name), str(config)]

            # Use shlex to handle complex wrapper commands safely
            if args.valgrind:
                cmd = shlex.split(args.valgrind_command) + app_args
            else:
                cmd = app_args

            proc = launch(name, cmd, log_dir)
            processes.append((name, proc))

            # Increase delay under instrumentation to allow for slow startup
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
                    shutdown(processes)
                    sys.exit(1)

    except KeyboardInterrupt:
        shutdown(processes)


if __name__ == "__main__":
    main()
