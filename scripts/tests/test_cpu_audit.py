"""Tests for cpu_audit.py, and particularly for its claim to know where a thread ran.

The placement tests drive real processes rather than a fake /proc.  What is being checked is
that the right two numbers are read out of /proc/<pid>/task/<tid>/stat and combined by the
right rule, and both halves of that are assumptions a mock would simply restate: a fake
reader would hand back whatever field offsets the test author believed in, which is the very
thing that is worth doubting.

They burn a little CPU on the highest processor this test is allowed to use, so that a run on
a machine where a venue is up does not land on a hot-path core.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIR))

import cpu_audit  # noqa: E402  -- needs the path set above


# Each child waits to be told to start.  Starting an interpreter costs a few milliseconds of
# CPU, and a child left to begin on its own spends them before it can be pinned and before
# sampling begins -- which would credit a thread that then sleeps for five seconds with having
# run, and on whichever processor the interpreter happened to start on.
WAIT = "import sys, time\nsys.stdin.readline()\n"
BURN = WAIT + "end = time.monotonic() + {0}\nwhile time.monotonic() < end: pass\n"
SLEEP = WAIT + "time.sleep({0})\n"

SETTLE = 0.15   # seconds between releasing a child and the first reading of it


def quiet_processor() -> int:
    """The highest processor this test may use, which is the least likely to be hot path."""
    return max(os.sched_getaffinity(0))


def child(source: str, processor: int) -> subprocess.Popen:
    """Start a child, pin it, and only then release it, so where it runs is known."""
    process = subprocess.Popen([sys.executable, "-c", source], stdin=subprocess.PIPE)
    os.sched_setaffinity(process.pid, {processor})
    process.stdin.write(b"go\n")
    process.stdin.flush()
    time.sleep(SETTLE)
    return process


def placement_of(process: subprocess.Popen, seconds: float = 0.6) -> cpu_audit.Placement:
    """Sample one process's main thread for a window and return what was seen."""
    placement = cpu_audit.Placement(label="child", component=None, kind="userspace")
    targets = {(process.pid, process.pid): placement}
    cpu_audit.sample_placement(targets, seconds=seconds, interval=0.05)
    return placement


# -- where a thread really ran ----------------------------------------------

def test_a_busy_thread_is_seen_on_the_processor_it_is_pinned_to():
    processor = quiet_processor()
    process = child(BURN.format(2.0), processor)
    try:
        placement = placement_of(process)
    finally:
        process.kill()
        process.wait()

    assert placement.ran, "a thread that burned CPU for the whole window was recorded as idle"
    assert set(placement.cores) == {processor}
    assert placement.cores[processor] > 1, "only one interval was attributed across the window"


def test_a_sleeping_thread_is_not_credited_with_the_core_it_last_ran_on():
    """The processor field survives the thread going to sleep; consumed time is what dates it."""
    processor = quiet_processor()
    process = child(SLEEP.format(5.0), processor)
    try:
        placement = placement_of(process)
    finally:
        process.kill()
        process.wait()

    assert not placement.ran
    assert not placement.cores, "a sleeping thread was reported as running on a core"


def test_a_thread_that_has_gone_is_not_an_error():
    """Threads come and go constantly while /proc is being walked; that is not a failure."""
    placement = cpu_audit.Placement(label="gone", component=None, kind="userspace")
    cpu_audit.sample_placement({(999999999, 999999999): placement}, seconds=0.0, interval=0.01)
    assert not placement.ran


def test_a_zero_length_window_still_compares_two_readings():
    """--sample-seconds 0 is an instant, not a no-op: it must still yield one interval."""
    readings = iter([(10, 3), (12, 3)])
    seen = []

    def reader(pid, tid):
        seen.append((pid, tid))
        return next(readings, (12, 3))

    placement = cpu_audit.Placement(label="fake", component=None, kind="userspace")
    cpu_audit.sample_placement({(1, 1): placement}, seconds=0.0, interval=0.01, read=reader)
    assert len(seen) == 2
    assert placement.ticks == 2
    assert placement.cores[3] == 1


# -- the boot line ----------------------------------------------------------

@pytest.mark.parametrize("cmdline, expected", [
    ("BOOT_IMAGE=/vmlinuz ro quiet splash", []),
    ("quiet isolcpus=2-15 nohz_full=2-15", list(range(2, 16))),
    ("isolcpus=domain,managed_irq,2-15 quiet", list(range(2, 16))),
    ("isolcpus=1,4-6", [1, 4, 5, 6]),
    ("isolcpus=nohz,domain", []),
])
def test_isolated_cores_are_read_from_the_boot_line(cmdline, expected):
    assert cpu_audit.parse_isolated_cores(cmdline) == expected


# -- classification ---------------------------------------------------------

@pytest.mark.parametrize("kernel, name, expected", [
    (True, "irq/225-iwlwifi", "irq"),
    (True, "ksoftirqd/4", "kernel"),
    (True, "kworker/u128:3", "kernel"),
    (False, "firefox-bin", "userspace"),
    (False, "irq/not-really", "userspace"),
])
def test_threads_are_classified_by_what_can_be_done_about_them(kernel, name, expected):
    assert cpu_audit.classify(kernel, name) == expected
