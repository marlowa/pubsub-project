"""Tests for launch.py, the per-process launcher.

The end-to-end tests run a real child through a real launcher subprocess, because the
properties that matter -- which pid lands in which file, whether a signal reaches the child,
whether a stop request suppresses a restart -- are all properties of actual processes and are
exactly what a mock would assume rather than check.

Timings are kept small deliberately: --minimum-runtime and --failure-sleep are parameters, so a
crash loop that would take half a minute in production takes under a second here.
"""

from __future__ import annotations

import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIR))

import launch  # noqa: E402  -- needs the path set above


# -- a child that behaves as asked ------------------------------------------

DIES_AT_ONCE = "import sys; sys.exit(7)"
RUNS_UNTIL_KILLED = "import time\nwhile True: time.sleep(0.05)"
DIES_AFTER = "import sys, time; time.sleep(float(sys.argv[1])); sys.exit(0)"


def child(source: str, *args: str) -> list:
    return [sys.executable, "-c", source] + list(args)


def start_launcher(tmp_path: Path, command: list, name: str = "demo", **options) -> subprocess.Popen:
    argv = [sys.executable, str(SCRIPTS_DIR / "launch.py"), "--name", name, "--run-dir", str(tmp_path)]
    for key, value in options.items():
        argv += ["--" + key.replace("_", "-"), str(value)]
    argv += ["--"] + command
    # Not a context manager: the test owns this process and stops it explicitly below.
    return subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)  # pylint: disable=consider-using-with


def wait_for(predicate, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


def read_state(tmp_path: Path, name: str = "demo") -> dict:
    """The state file as a dict, empty when it has not been written yet.

    Tolerant of absence on purpose: wait_for() polls through this, and a launcher that has
    not got as far as writing the file is a "not yet", not an error.
    """
    path = tmp_path / (name + ".launcher.state")
    if not path.exists():
        return {}
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)


# -- pid identity, which is what keeps every other tool working --------------

def test_the_plain_pid_file_holds_the_child_not_the_launcher(tmp_path):
    """Every other tool reads <name>.pid, so it must name the component, not the wrapper."""
    # devenv.py, perf_run.py and the resource monitor all read <name>.pid. If the launcher
    # put its own pid there, perf would profile a sleeping wrapper and the RSS graph would
    # read a few hundred kilobytes.
    launcher = start_launcher(tmp_path, child(RUNS_UNTIL_KILLED))
    try:
        assert wait_for(lambda: (tmp_path / "demo.pid").exists())
        child_pid = int((tmp_path / "demo.pid").read_text())
        assert child_pid != launcher.pid
        # And the pid named there is genuinely the child of the launcher.
        parent = Path(f"/proc/{child_pid}/stat").read_text().rsplit(")", 1)[1].split()[1]
        assert int(parent) == launcher.pid
    finally:
        launcher.send_signal(signal.SIGTERM)
        launcher.wait(timeout=10)


def test_the_launcher_records_its_own_pid_separately(tmp_path):
    """Its own pid is still worth having, just not under the name everything else reads."""
    launcher = start_launcher(tmp_path, child(RUNS_UNTIL_KILLED))
    try:
        assert wait_for(lambda: (tmp_path / "demo.launcher.pid").exists())
        assert int((tmp_path / "demo.launcher.pid").read_text()) == launcher.pid
    finally:
        launcher.send_signal(signal.SIGTERM)
        launcher.wait(timeout=10)


def test_both_pid_files_are_removed_when_it_stops(tmp_path):
    """A stale pid file outlives the process it names and misleads devenv.py about it."""
    launcher = start_launcher(tmp_path, child(RUNS_UNTIL_KILLED))
    assert wait_for(lambda: (tmp_path / "demo.pid").exists())
    launcher.send_signal(signal.SIGTERM)
    launcher.wait(timeout=10)
    assert not (tmp_path / "demo.pid").exists()
    assert not (tmp_path / "demo.launcher.pid").exists()


# -- restarting -------------------------------------------------------------

def test_a_child_that_exits_cleanly_is_still_restarted(tmp_path):
    """Exiting 0 is not a reason to leave the venue without a component; only a stop request is."""
    # A component exiting 0 is not a reason to leave the venue without it. Only a stop
    # request is.
    launcher = start_launcher(tmp_path, child(DIES_AFTER, "0.3"), minimum_runtime=0.1, failure_sleep=0.1)
    try:
        assert wait_for(lambda: int(read_state(tmp_path).get("restarts", 0)) >= 2, timeout=15)
    finally:
        launcher.send_signal(signal.SIGTERM)
        launcher.wait(timeout=10)


def test_a_fast_death_counts_as_a_failed_start(tmp_path):
    """The crash-loop guard: dying at once is a failed start, not a run that ended."""
    launcher = start_launcher(tmp_path, child(DIES_AT_ONCE), minimum_runtime=1.0, failure_sleep=0.1)
    try:
        assert wait_for(lambda: int(read_state(tmp_path).get("consecutive_failed_starts", 0)) >= 3, timeout=15)
    finally:
        launcher.send_signal(signal.SIGTERM)
        launcher.wait(timeout=10)


def test_it_gives_up_when_told_to_after_enough_failed_starts(tmp_path):
    """Only when asked, since giving up guarantees the component stays down."""
    launcher = start_launcher(tmp_path, child(DIES_AT_ONCE), minimum_runtime=1.0,
                              failure_sleep=0.05, max_consecutive_failures=3)
    assert launcher.wait(timeout=20) == 1
    assert read_state(tmp_path)["status"] == "gave up"


# -- stopping ---------------------------------------------------------------

def test_a_stop_file_present_at_the_start_prevents_it_running_at_all(tmp_path):
    """Checked before every start, so a stop set in advance is honoured."""
    (tmp_path / "demo.stop").write_text("")
    launcher = start_launcher(tmp_path, child(RUNS_UNTIL_KILLED))
    assert launcher.wait(timeout=10) == 0
    assert read_state(tmp_path)["status"] == "stopped"


def test_a_stop_file_appearing_later_prevents_the_next_restart(tmp_path):
    """A stop file is how something without a signal to send asks the launcher to stand down."""
    launcher = start_launcher(tmp_path, child(DIES_AFTER, "0.4"), minimum_runtime=0.1, failure_sleep=0.1)
    assert wait_for((tmp_path / "demo.pid").exists)
    (tmp_path / "demo.stop").write_text("")
    assert launcher.wait(timeout=15) == 0
    assert read_state(tmp_path)["status"] == "stopped"


def test_sigterm_reaches_the_child_rather_than_orphaning_it(tmp_path):
    """A stop request must reach the child, not just the wrapper."""
    # Without signal forwarding the launcher dies and the child is reparented to init --
    # for a matching engine, gigabytes held by a process nothing manages any more.
    launcher = start_launcher(tmp_path, child(RUNS_UNTIL_KILLED))
    assert wait_for((tmp_path / "demo.pid").exists)
    child_pid = int((tmp_path / "demo.pid").read_text())
    launcher.send_signal(signal.SIGTERM)
    launcher.wait(timeout=10)
    assert wait_for(lambda: not Path(f"/proc/{child_pid}").exists(), timeout=10), \
        "the child outlived its launcher"


# -- argument handling ------------------------------------------------------

def test_the_command_may_be_given_with_or_without_the_bare_separator():
    """The separator is conventional, not required; forgetting it should still work."""
    with_separator = launch.parse_arguments(["--name", "x", "--run-dir", "/tmp", "--", "prog", "-a"])
    without = launch.parse_arguments(["--name", "x", "--run-dir", "/tmp", "prog", "-a"])
    assert with_separator.command == ["prog", "-a"]
    assert without.command == ["prog", "-a"]


def test_a_missing_command_is_refused():
    """A launcher with nothing to launch is a mistake worth failing on, not a no-op."""
    with pytest.raises(SystemExit):
        launch.parse_arguments(["--name", "x", "--run-dir", "/tmp"])


def test_never_giving_up_is_the_default(tmp_path):
    """Giving up leaves the component down, so it is off unless explicitly asked for."""
    arguments = launch.parse_arguments(["--name", "x", "--run-dir", str(tmp_path), "--", "prog"])
    assert arguments.max_consecutive_failures == 0
    launcher = launch.Launcher("x", tmp_path, ["prog"], 2.0, 5.0, 0)
    launcher.consecutive_failures = 1000
    assert launcher.give_up() is False
