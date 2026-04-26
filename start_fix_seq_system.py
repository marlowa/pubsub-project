#!/usr/bin/env python3
"""
start_fix_seq_system.py

Starts the sequencer-based FIX order flow system for testing with fix8.

Startup order:
  1. arbiter               -- must be up before sequencers try to connect
  2. sequencer (primary)   -- instance_id=1, listens on port 7001
  3. sequencer (secondary) -- instance_id=2, listens on port 7002
  4. matching_engine       -- connects outbound to gateway ER listener
  5. sample_fix_gateway_seq -- connects to both sequencers; listens for
                               FIX clients on port 9879 and ME ERs on 7010

Quill log files land in <prefix>/log/ because each process is started
with that directory as its working directory. stdout/stderr of each
process is captured to <name>.stdout in the same directory.
"""

import argparse
import os
import signal
import subprocess
import sys
import time
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
        help="Path to the cmake install prefix "
             "(e.g. build/installed or /opt/pubsub).",
    )
    parser.add_argument(
        "--startup-delay",
        type=float,
        default=1.0,
        metavar="SECONDS",
        help="Seconds to wait between starting each process (default: 1.0).",
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


def launch(name: str, exe: Path, log_file: Path, config: Path, log_dir: Path) -> subprocess.Popen:
    stdout_path = log_dir / f"{name}.stdout"
    print(f"Starting {name}...")
    with stdout_path.open("w") as stdout_file:
        proc = subprocess.Popen(
            [str(exe), str(log_file), str(config)],
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

    log_dir.mkdir(parents=True, exist_ok=True)

    # Extend LD_LIBRARY_PATH so the installed shared library is found.
    lib_dir = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    processes: list[tuple[str, subprocess.Popen]] = []

    try:
        steps = [
            ("arbiter",
             bin_dir / "arbiter",
             log_dir / "arbiter.log",
             etc_dir / "arbiter" / "arbiter.toml"),
            ("sequencer_primary",
             bin_dir / "sequencer",
             log_dir / "sequencer_primary.log",
             etc_dir / "sequencer" / "sequencer.toml"),
            ("sequencer_secondary",
             bin_dir / "sequencer",
             log_dir / "sequencer_secondary.log",
             etc_dir / "sequencer" / "sequencer_secondary.toml"),
            ("matching_engine",
             bin_dir / "matching_engine",
             log_dir / "matching_engine.log",
             etc_dir / "matching_engine" / "matching_engine.toml"),
            ("sample_fix_gateway_seq",
             bin_dir / "sample_fix_gateway_seq",
             log_dir / "sample_fix_gateway_seq.log",
             etc_dir / "sample_fix_gateway_seq" / "sample_fix_gateway_seq.toml"),
        ]

        for name, exe, log_file, config in steps:
            if not config.is_file():
                sys.exit(f"ERROR: config file not found: {config}")
            proc = launch(name, exe, log_file, config, log_dir)
            processes.append((name, proc))
            time.sleep(args.startup_delay)

        print(f"\nAll processes started. Logs in {log_dir}/")
        print("Press Ctrl-C to shut everything down.\n")

        # Monitor: exit if any process dies unexpectedly.
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
