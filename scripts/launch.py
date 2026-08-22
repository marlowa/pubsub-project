#!/usr/bin/env python3
"""
launch.py — start one component and restart it if it dies.

Usage:
  launch.py --name NAME --run-dir DIR [options] -- COMMAND [ARG...]

Options:
  --name NAME                 Component name. Everything this script writes is named after
                              it: <name>.pid, <name>.launcher.pid, <name>.stop and
                              <name>.launcher.state. Use the same name devenv.py uses, or
                              the pid file will not be the one other tools read. Required.

  --run-dir DIR               Directory holding those four files. Created if absent. This is
                              the venue's run directory, the same one devenv.py is given.
                              Required.

  --minimum-runtime SECONDS   How long the command must survive before its exit counts as a
                              run that ended rather than a start that failed. A healthy
                              component runs for hours, so anything dying inside a couple of
                              seconds has failed to start. Default 2.0.

  --failure-sleep SECONDS     How long to wait after a failed start before trying again.
                              Long enough that a deterministic fault retries slowly instead
                              of spinning; short enough that a transient one is recovered
                              from quickly. Not applied after a normal exit, which is
                              restarted at once. Default 5.0.

  --max-consecutive-failures N
                              Give up after this many failed starts in a row, exiting 1.
                              Zero means never give up, which is the default: abandoning a
                              component guarantees there is none, where a slow retry keeps
                              trying to restore service. Reset by any start that survives
                              --minimum-runtime.

  -- COMMAND [ARG...]         The command to run, after a bare --. The separator is
                              conventional rather than required, but without it any leading
                              dashes in COMMAND will be read as options to this script.

Exit status:
  0   stood down as asked, by signal or by a stop file
  1   gave up after --max-consecutive-failures failed starts

Files it writes, all in --run-dir:
  <name>.pid              the COMPONENT's pid -- the plain file devenv.py, perf_run.py and
                          the resource monitor already read, so none of them need to know
                          this script exists. Removed on exit.
  <name>.launcher.pid     this script's own pid. Removed on exit.
  <name>.launcher.state   name, status, restart count, consecutive failed starts, and when
                          it was last updated. Readable with cat.
  <name>.stop             not written by this script -- read by it. See below.

Wraps exactly one process. It knows the command line it was given and nothing
else: not the topology, not which instance is primary, not who leads. That
ignorance is the design. A launcher that also assigned roles would have to know
the whole venue, and would become something every deployment had to run; this one
is optional, and a component started without it behaves identically.

See design-notes-for-ha.md sections 12 and 13.

What it does
  - Starts the command, and restarts it when it exits, for any reason.
  - Treats a death sooner than --minimum-runtime as a failed start rather than a
    normal exit, and waits --failure-sleep before trying again, so a
    deterministic fault produces a slow retry instead of a spin.
  - Writes the CHILD's pid to <run-dir>/<name>.pid, so devenv.py, perf_run.py
    and the resource monitor all keep working unchanged and never see this
    process at all. Its own pid goes to <name>.launcher.pid.
  - Counts restarts into <name>.launcher.state, so the count is readable without
    a metrics stack of any kind.

Stopping it
  Send it SIGTERM or SIGINT and it forwards that to the child, waits, and exits
  without restarting. Alternatively create <run-dir>/<name>.stop, which it
  checks before every start: that is the file to use when something other than a
  signal has to ask it to stand down.

Python 3.8 is the floor: release_check.py runs its Rocky 8 stage on 3.8, so
nothing here may need more than that.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

# A child that dies sooner than this did not run and stop; it failed to start. A healthy
# component runs for hours, so seconds is a generous line to draw.
DEFAULT_MINIMUM_RUNTIME_SECONDS = 2.0

# How long to wait after a failed start before trying again. Long enough that a component
# failing deterministically retries slowly rather than spinning, short enough that a
# transient cause is recovered from quickly.
DEFAULT_FAILURE_SLEEP_SECONDS = 5.0


def log(message: str) -> None:
    """Write a timestamped line, flushed, so it interleaves correctly with the child's output."""
    print(f"[{time.strftime('%H:%M:%S')}] launch({log.name}): {message}", flush=True)


log.name = "?"


class Launcher:
    """Starts one command and restarts it when it exits."""

    def __init__(self, name: str, run_dir: Path, command: list, minimum_runtime: float,
                 failure_sleep: float, max_consecutive_failures: int) -> None:
        self.name = name
        self.run_dir = run_dir
        self.command = command
        self.minimum_runtime = minimum_runtime
        self.failure_sleep = failure_sleep
        self.max_consecutive_failures = max_consecutive_failures

        self.child = None
        self.stopping = False
        self.restarts = 0
        self.consecutive_failures = 0

    # -- file locations ------------------------------------------------------

    @property
    def child_pid_path(self) -> Path:
        """Where the CHILD's pid goes. Deliberately the plain <name>.pid that everything else
        already reads, so no other tool has to learn that a launcher exists."""
        return self.run_dir / f"{self.name}.pid"

    @property
    def launcher_pid_path(self) -> Path:
        return self.run_dir / f"{self.name}.launcher.pid"

    @property
    def stop_path(self) -> Path:
        return self.run_dir / f"{self.name}.stop"

    @property
    def state_path(self) -> Path:
        return self.run_dir / f"{self.name}.launcher.state"

    # -- signals -------------------------------------------------------------

    def install_signal_handlers(self) -> None:
        """Forward a stop request to the child rather than dying and orphaning it.

        Without this the child outlives its launcher: it is reparented and keeps running,
        which for a matching engine means several gigabytes held by a process nothing is
        managing any more.
        """
        signal.signal(signal.SIGTERM, self.on_stop_signal)
        signal.signal(signal.SIGINT, self.on_stop_signal)

    def on_stop_signal(self, signal_number: int, _frame) -> None:
        """Remember that this was asked for, so the child's exit is not treated as a crash."""
        self.stopping = True
        log(f"received signal {signal_number}; forwarding to child and standing down")
        self.terminate_child(signal_number)

    def terminate_child(self, signal_number: int) -> None:
        if self.child is None or self.child.poll() is not None:
            return
        try:
            self.child.send_signal(signal_number)
        except OSError as error:
            log(f"could not signal child: {error}")

    # -- state ---------------------------------------------------------------

    def write_state(self, status: str) -> None:
        """Record what has happened, in a form readable with cat.

        Written to a file rather than published as a metric because monitoring must not be
        something this depends on: the venue's default configuration has no Prometheus at
        all. Anything that wants these numbers can read them; nothing has to.
        """
        text = "\n".join([
            f"name={self.name}",
            f"status={status}",
            f"restarts={self.restarts}",
            # Failed starts *before* the current attempt. A running child that was
            # preceded by failures still shows them, which is the history worth seeing;
            # the count resets only once a child has run past --minimum-runtime.
            f"consecutive_failed_starts={self.consecutive_failures}",
            f"launcher_pid={os.getpid()}",
            f"updated={time.strftime('%Y-%m-%d %H:%M:%S')}",
            "",
        ])
        self.state_path.write_text(text)

    def remove_file(self, path: Path) -> None:
        try:
            path.unlink()
        except OSError:
            pass

    # -- the loop ------------------------------------------------------------

    def stop_requested(self) -> bool:
        return self.stopping or self.stop_path.exists()

    def start_child(self) -> bool:
        """Start the command. Returns False when it could not be started at all."""
        try:
            # Not a context manager: the supervise loop owns this child's lifetime, which
            # outlives this function by design.
            self.child = subprocess.Popen(self.command)  # pylint: disable=consider-using-with
        except OSError as error:
            log(f"could not start {self.command[0]}: {error}")
            return False
        self.child_pid_path.write_text(str(self.child.pid))
        log(f"started pid {self.child.pid}")
        return True

    def wait_for_child(self) -> tuple:
        """Wait for the child, returning (exit status description, seconds it ran)."""
        started_at = time.monotonic()
        returncode = self.child.wait()
        ran_for = time.monotonic() - started_at
        if returncode < 0:
            return (f"killed by signal {-returncode}", ran_for)
        return (f"exit status {returncode}", ran_for)

    def run(self) -> int:
        """Set up, supervise until stood down, and clean up the pid files whatever happens.

        The cleanup is in a finally block because a stale <name>.pid outlives the process it
        names, and devenv.py reads that file to decide whether a component is running.
        """
        self.install_signal_handlers()
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.launcher_pid_path.write_text(str(os.getpid()))
        self.write_state("starting")

        try:
            return self.supervise()
        finally:
            self.remove_file(self.launcher_pid_path)
            self.remove_file(self.child_pid_path)

    def supervise(self) -> int:
        """Start the child, wait for it, and start it again, until asked to stop.

        Returns 0 when stood down as asked, 1 when it gave up after too many failed starts.
        """
        while True:
            if self.stop_requested():
                log("stop requested before start; standing down")
                self.write_state("stopped")
                return 0

            if not self.start_child():
                self.consecutive_failures += 1
                if self.give_up():
                    return 1
                self.write_state("failed to start")
                time.sleep(self.failure_sleep)
                continue

            self.write_state("running")
            description, ran_for = self.wait_for_child()
            self.remove_file(self.child_pid_path)

            if self.stop_requested():
                log(f"child ended ({description}) after a stop request; not restarting")
                self.write_state("stopped")
                return 0

            self.restarts += 1
            if ran_for < self.minimum_runtime:
                self.consecutive_failures += 1
                log(f"child ended ({description}) after only {ran_for:.1f}s -- treating as a "
                    f"failed start, consecutive failures {self.consecutive_failures}")
                if self.give_up():
                    return 1
                self.write_state("failed start")
                time.sleep(self.failure_sleep)
            else:
                self.consecutive_failures = 0
                log(f"child ended ({description}) after {ran_for:.1f}s -- restarting")
                self.write_state("restarting")

    def give_up(self) -> bool:
        """Whether to stop trying. Zero means never give up, which is the default.

        Giving up leaves the component down, so it is off unless asked for: a slow retry
        keeps trying to restore service, where abandoning it guarantees there is none.
        """
        if self.max_consecutive_failures <= 0:
            return False
        if self.consecutive_failures < self.max_consecutive_failures:
            return False
        log(f"giving up after {self.consecutive_failures} consecutive failed starts")
        self.write_state("gave up")
        return True


def parse_arguments(argv: list) -> argparse.Namespace:
    """Parse the launcher's own options, and the command that follows a bare --."""
    parser = argparse.ArgumentParser(
        description="Start one component and restart it if it dies.",
        epilog="The command to run follows a bare --, e.g.  launch.py --name me --run-dir run -- ./matching_engine cfg.toml")
    parser.add_argument("--name", required=True,
                        help="component name, used for the pid, stop and state file names")
    parser.add_argument("--run-dir", required=True, type=Path,
                        help="directory holding the pid, stop and state files")
    parser.add_argument("--minimum-runtime", type=float, default=DEFAULT_MINIMUM_RUNTIME_SECONDS,
                        help="a child that dies sooner than this counts as a failed start "
                             f"(default: {DEFAULT_MINIMUM_RUNTIME_SECONDS})")
    parser.add_argument("--failure-sleep", type=float, default=DEFAULT_FAILURE_SLEEP_SECONDS,
                        help="seconds to wait after a failed start before retrying "
                             f"(default: {DEFAULT_FAILURE_SLEEP_SECONDS})")
    parser.add_argument("--max-consecutive-failures", type=int, default=0,
                        help="give up after this many failed starts in a row; 0 means never give up (default: 0)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="the command to run, after a bare --")

    arguments = parser.parse_args(argv)
    command = arguments.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("no command given; put it after a bare --")
    arguments.command = command
    return arguments


def main() -> int:
    """Build a Launcher from the command line and run it until it stands down."""
    arguments = parse_arguments(sys.argv[1:])
    log.name = arguments.name
    launcher = Launcher(arguments.name, arguments.run_dir, arguments.command,
                        arguments.minimum_runtime, arguments.failure_sleep,
                        arguments.max_consecutive_failures)
    return launcher.run()


if __name__ == "__main__":
    sys.exit(main())
