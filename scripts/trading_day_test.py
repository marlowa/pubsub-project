#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
trading_day_test.py — runs trading days through scripts/trading_day.py on the deployed development venue, and checks
what each procedure does (docs/venue/start_and_end_of_day.md, sections 3 and 4).

Nothing of the installation may be running. The test clears the WALs, regions and records an earlier run left, as
devenv.py start --fresh-wal --fresh-book does, and removes any run manifest and the archives of the two trading days it
uses, 20991230 and 20991231, so that it can be run again. It needs the database, as every start of the venue does.

The checks, in order:

  1. start refuses while a lease promise record of another day is present, names the file and its day, and leaves it
     exactly as it was.
  2. start begins the first day with a schedule that enters pre-open and open as soon as their conditions hold, and
     the venue reaches open by the schedule alone.
  3. start refuses while the venue is running.
  4. end refuses while the venue is open, and stops nothing.
  5. With every component stopped, start refuses a schedule that differs from the run manifest's, and then, given no
     schedule, restarts the venue on the day's files, which is still open.
  6. Once an operator has closed the venue and asked for the end of day, end stops every component and archives every
     file of the day: the listing names each one with its checksum, each archived copy matches, the Quill logs named
     with the day are among them, and the installation holds no file of the day.
  7. start begins the second day, which needs nothing from the first, with a schedule that enters every phase as soon
     as its conditions hold; the venue reaches the end of day by itself, and end archives it.

Exit status: 0 when every check passes, 1 otherwise.
"""

from __future__ import annotations

import hashlib
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import devenv
import operator_requests

_SCRIPT_DIR = Path(__file__).resolve().parent
_PROJECT_ROOT = _SCRIPT_DIR.parent
_FIRST_DAY = 20991230
_SECOND_DAY = 20991231
_OPERATOR = "trading_day_test"
_failures: list[str] = []


def log(text: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def check(passed: bool, what: str, detail: str = "") -> bool:
    log(f"  {'PASS' if passed else 'FAIL'}: {what}{'' if passed or not detail else ' -- ' + detail}")
    if not passed:
        _failures.append(what)
    return passed


def run_procedure(*arguments: str) -> tuple[int, str]:
    """Runs trading_day.py with arguments, and returns its exit status and everything it printed."""
    completed = subprocess.run([sys.executable, str(_SCRIPT_DIR / "trading_day.py"), "--operator", _OPERATOR, *arguments],
                               capture_output=True, text=True, check=False, timeout=600)
    output = completed.stdout + completed.stderr
    for line in output.splitlines():
        log(f"    | {line}")
    return completed.returncode, output


def ask_for_phase(install_dir: Path, day: int, phase_name: str) -> bool:
    return operator_requests.entered(install_dir, operator_requests.enter_phases(install_dir, day, (phase_name,), _OPERATOR, timeout=60.0,
                                                                                 say=lambda _line: None))


def wait_for_phase(install_dir: Path, day: int, phase_name: str, timeout: float = 60.0) -> str:
    """Waits until the leading sequencer says the venue is in phase_name of day; returns the last state seen, in words."""
    deadline = time.monotonic() + timeout
    seen = "no answer"
    while time.monotonic() < deadline:
        state = operator_requests.trading_state(install_dir, _OPERATOR, timeout=5.0, say=lambda _line: None)
        if state.answer is not None:
            seen = f"phase {state.answer.phase.name} of trading day {state.answer.trading_day}"
            if state.answer.day_started and state.answer.trading_day == day and state.answer.phase.name == phase_name:
                return seen
        time.sleep(0.5)
    return seen


def write_schedule(path: Path, phases: dict[str, str]) -> Path:
    lines = ['time_zone = "Europe/London"', "", "[phases]"] + [f'{phase} = "{when}"' for phase, when in phases.items()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as data:
        for block in iter(lambda: data.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check_archive(env: dict, install_dir: Path, day: int) -> None:
    """Checks the archive of day: the listing, each archived copy against it, the Quill logs among them, and that the
    installation holds no file of the day."""
    import trading_day  # pylint: disable=import-outside-toplevel
    archive = devenv.archive_dir(env) / str(day) / socket.gethostname().split(".")[0]
    listing = archive / "listing.txt"
    if not check(listing.is_file(), f"the archive of trading day {day} has its listing", str(listing)):
        return
    entries = [line.split("  ", 2) for line in listing.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#")]
    mismatched = []
    for digest, _size, original in entries:
        copy = trading_day._archived_path(Path(original), install_dir, archive)  # pylint: disable=protected-access
        if not copy.is_file() or checksum(copy) != digest:
            mismatched.append(original)
    check(not mismatched and len(entries) > 0, f"each of the {len(entries)} archived copies matches its checksum in the listing",
          f"not matching: {mismatched}")
    names = [Path(original).name for _, _, original in entries]
    check(f"sequencer_primary.{day}.log" in names and f"matching_engine_primary.{day}.log" in names,
          "the Quill logs named with the day are archived")
    check(any(name.startswith("wal_") for name in names) and "run_manifest.toml" in names and "credentials.toml" in names,
          "the WAL segments, the run manifest and the credentials file are archived")
    left = trading_day.files_of_the_day(env, install_dir, logs_of_day=day)
    check(not left, f"the installation holds no file of trading day {day}", ", ".join(str(day_file.path) for day_file in left))


def main() -> int:  # pylint: disable=too-many-statements
    env = devenv.load_env(devenv.DEFAULT_ENV_FILE)
    install_dir, _, _ = devenv.resolve_paths(env)
    import trading_day  # pylint: disable=import-outside-toplevel
    work = _PROJECT_ROOT / "tmp" / "trading_day_test"
    work.mkdir(parents=True, exist_ok=True)

    running = trading_day.running_processes(env, install_dir)
    if running:
        log(f"processes of the installation are running ({', '.join(running)}); stop them first")
        return 1
    log("clearing what earlier runs left")
    devenv.clear_durable_state(env, install_dir, fresh_book=True, fresh_wal=True)
    trading_day.manifest_path(install_dir).unlink(missing_ok=True)
    for day in (_FIRST_DAY, _SECOND_DAY):
        shutil.rmtree(devenv.archive_dir(env) / str(day), ignore_errors=True)

    log("1. a lease promise record of another day stops the start")
    stale = devenv.lease_records(env, install_dir)[0]
    stale.parent.mkdir(parents=True, exist_ok=True)
    stale.write_text("trading_day=20991229 7 1 2\n", encoding="utf-8")
    before = stale.read_bytes()
    opened_by_hand = write_schedule(work / "open_when_ready.toml", {"pre_open": "when ready", "open": "when ready"})
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY), "--schedule", str(opened_by_hand))
    check(status == 1 and str(stale) in output and "trading day 20991229" in output, "start refuses, naming the file and its day")
    check(stale.is_file() and stale.read_bytes() == before, "the file is left exactly as it was")
    check(not trading_day.running_processes(env, install_dir), "nothing was started")
    stale.unlink()

    log(f"2. trading day {_FIRST_DAY} starts, and the schedule opens it")
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY), "--schedule", str(opened_by_hand))
    check(status == 0, "start succeeds")
    seen = wait_for_phase(install_dir, _FIRST_DAY, "Open")
    check(seen == f"phase Open of trading day {_FIRST_DAY}", "the venue reaches open by the schedule alone", seen)

    log("3. start refuses while the venue is running")
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY))
    check(status == 1 and "processes of the installation are running" in output, "start refuses")

    log("4. end refuses while the venue is open")
    status, output = run_procedure("end")
    check(status == 1 and "phase Open" in output, "end refuses, saying the venue is open")
    check(bool(trading_day.running_processes(env, install_dir)), "nothing was stopped")

    log("5. a restart during the day")
    subprocess.run([sys.executable, str(_SCRIPT_DIR / "devenv.py"), "stop"], capture_output=True, text=True, check=False)
    check(not trading_day.running_processes(env, install_dir), "every component is stopped")
    other = write_schedule(work / "other.toml", {"pre_open": "07:50", "open": "08:00"})
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY), "--schedule", str(other))
    check(status == 1 and "is not the one the run" in output, "start refuses a schedule that differs from the manifest's")
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY))
    check(status == 0 and "this is a restart during the day" in output, "start restarts the venue on the day's files")
    seen = wait_for_phase(install_dir, _FIRST_DAY, "Open")
    check(seen == f"phase Open of trading day {_FIRST_DAY}", "the venue is still open", seen)

    log(f"6. trading day {_FIRST_DAY} ends")
    check(ask_for_phase(install_dir, _FIRST_DAY, "Closed") and ask_for_phase(install_dir, _FIRST_DAY, "EndOfDay"),
          "an operator closes the venue and asks for the end of day")
    status, output = run_procedure("end")
    check(status == 0 and "the installation holds no file of trading day" in output, "end succeeds")
    check(not trading_day.running_processes(env, install_dir), "every process has exited")
    check_archive(env, install_dir, _FIRST_DAY)

    log(f"7. trading day {_SECOND_DAY} runs by its schedule from start to end")
    whole_day = write_schedule(work / "whole_day_when_ready.toml",
                               {"pre_open": "when ready", "open": "when ready", "closed": "when ready", "end_of_day": "when ready"})
    status, output = run_procedure("start", "--trading-day", str(_SECOND_DAY), "--schedule", str(whole_day))
    check(status == 0, "start succeeds with nothing of the first day present")
    seen = wait_for_phase(install_dir, _SECOND_DAY, "EndOfDay")
    check(seen == f"phase EndOfDay of trading day {_SECOND_DAY}", "the venue reaches the end of day by the schedule alone", seen)
    status, output = run_procedure("end")
    check(status == 0, "end succeeds")
    check_archive(env, install_dir, _SECOND_DAY)

    check(not trading_day.running_processes(env, install_dir), "nothing of the installation is left running")

    log("=" * 60)
    if _failures:
        log(f"FAILED: {len(_failures)} check(s): {'; '.join(_failures)}")
        return 1
    log("every check passed")
    return 0


def stop_whatever_is_running() -> None:
    """Stops every component, so that a check that failed part of the way through leaves nothing running."""
    import trading_day  # pylint: disable=import-outside-toplevel
    env = devenv.load_env(devenv.DEFAULT_ENV_FILE)
    install_dir, _, _ = devenv.resolve_paths(env)
    if trading_day.running_processes(env, install_dir):
        subprocess.run([sys.executable, str(_SCRIPT_DIR / "devenv.py"), "stop"], capture_output=True, text=True, check=False)
        log(f"stopped what was left running; still running: {trading_day.running_processes(env, install_dir) or 'nothing'}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        stop_whatever_is_running()
