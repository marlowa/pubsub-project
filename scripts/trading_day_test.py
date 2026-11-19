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
  2. start begins the first day, each component under its launcher (scripts/launch.py), with a schedule that enters
     pre-open and open as soon as their conditions hold, and the venue reaches open by the schedule alone.
  3. start refuses while the venue is running.
  4. end refuses while the venue is open, and stops nothing.
  5. With every component stopped, start refuses a schedule that differs from the run manifest's, and then, given no
     schedule, restarts the venue on the day's files, which is still open.
  6. Once an operator has closed the venue and asked for the end of day, the day ends by itself, with no end: the
     leading sequencer writes the end-of-day end, every component of the day stops and its launcher with it, the
     last-state files say the end, and each launcher has archived its component's files with a listing whose checksums
     match the archived copies, the Quill logs and the WAL segments among them. The installation holds no file of the
     day but the run manifest and the credentials, which belong to no single component and are left for the next start
     (docs/venue/start_and_end_of_day.md, section 4.2, decisions 5 to 7).
  7. With only the run manifest and the credentials of the first day left, start archives both with a listing of their
     own and begins the second day, with no launchers and a schedule that enters every phase as soon as its conditions
     hold. The venue reaches the end of day by itself and every component stops; with no launcher to archive their
     files, end archives them on the operator's word, without asking the leading sequencer, and the listing records the
     operator's word, as for a component that missed the end of day (decision 6).

Exit status: 0 when every check passes, 1 otherwise.
"""

from __future__ import annotations

import argparse
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
# A day before the first, whose lease promise record check 1 puts in place for start to refuse.
_EARLIER_DAY = 20991229
_OPERATOR = "trading_day_test"
# The components that take no part in the trading day, and so do not stop at its end: a member's test client and the
# metrics scraper. Every other component stops by itself on the end-of-day end (docs/venue/start_and_end_of_day.md,
# section 4.4).
_NOT_IN_THE_DAY = ("fix_test_client", "prometheus")
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


def venue_processes(env: dict, install_dir: Path) -> list[str]:
    """The processes of the installation still running, leaving out the components that take no part in the trading day."""
    import trading_day  # pylint: disable=import-outside-toplevel
    return [running for running in trading_day.running_processes(env, install_dir) if not running.startswith(_NOT_IN_THE_DAY)]


def wait_for_the_venue_to_stop(env: dict, install_dir: Path, timeout: float) -> list[str]:
    """Waits until every component of the trading day, and its launcher, has exited; returns those still running."""
    deadline = time.monotonic() + timeout
    left = venue_processes(env, install_dir)
    while left and time.monotonic() < deadline:
        time.sleep(1.0)
        left = venue_processes(env, install_dir)
    return left


def says_the_end(env: dict, name: str, day: int) -> bool:
    """Whether the component's last-state file says the end-of-day end of day."""
    _, _, run_dir = devenv.resolve_paths(env)
    path = run_dir / f"{name}.last_state"
    return path.is_file() and path.read_text(encoding="utf-8").strip() == f"trading_day={day} EndOfDayEnd"


def check_launcher_archives(env: dict, install_dir: Path, day: int) -> None:
    """Checks what the launchers archived when their components stopped for the end of day: a listing for each component
    of the day, each archived copy against its listing, the Quill logs and the WAL segments among them, and that the
    installation holds no file of the day but the run manifest and the credentials, which belong to no single component
    and are left for the next start to archive."""
    import trading_day  # pylint: disable=import-outside-toplevel
    archive = devenv.archive_dir(env) / str(day) / socket.gethostname().split(".")[0]
    ha_enabled = bool(env.get("ha", {}).get("enabled", False))
    expected = sorted(name for name, component in env["components"].items()
                      if name not in _NOT_IN_THE_DAY and (ha_enabled or not component.get("ha_only", False)))
    listings = {path.name[len("listing."):-len(".txt")]: path for path in archive.glob("listing.*.txt")}
    missing = [name for name in expected if name not in listings]
    check(not missing, f"each of the {len(expected)} components of the day has a listing written by its launcher", f"missing: {missing}")
    names: list[str] = []
    mismatched = []
    for listing in listings.values():
        for line in listing.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("#"):
                continue
            digest, _size, original = line.split("  ", 2)
            names.append(Path(original).name)
            copy = trading_day._archived_path(Path(original), install_dir, archive)  # pylint: disable=protected-access
            if not copy.is_file() or checksum(copy) != digest:
                mismatched.append(original)
    check(not mismatched and len(names) > 0, f"each of the {len(names)} archived copies matches its checksum in its listing",
          f"not matching: {mismatched}")
    check(f"sequencer_primary.{day}.log" in names and f"matching_engine_primary.{day}.log" in names and any(name.startswith("wal_") for name in names),
          "the Quill logs named with the day and the WAL segments are archived")
    left = [day_file.path for day_file in trading_day.files_of_the_day(env, install_dir, logs_of_day=day)]
    # The captured output of the components that take no part in the day is not archived by any launcher: they do not
    # stop at its end, and run only in the development environment.
    others = [path for path in left
              if path.name not in ("run_manifest.toml", "credentials.toml") and not path.name.startswith(_NOT_IN_THE_DAY)]
    check(not others, f"the installation holds no file of trading day {day} but the run manifest, the credentials, and the output of the "
          f"components that take no part in the day", ", ".join(map(str, others)))
    check(trading_day.manifest_path(install_dir).is_file() and devenv.credentials_path(install_dir).is_file(),
          "the run manifest and the credentials are left for the next start")


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
    for day in (_EARLIER_DAY, _FIRST_DAY, _SECOND_DAY):
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

    log(f"2. trading day {_FIRST_DAY} starts, each component under its launcher, and the schedule opens it")
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY), "--schedule", str(opened_by_hand), "--supervised")
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
    status, output = run_procedure("start", "--trading-day", str(_FIRST_DAY), "--supervised")
    check(status == 0 and "this is a restart during the day" in output, "start restarts the venue on the day's files")
    seen = wait_for_phase(install_dir, _FIRST_DAY, "Open")
    check(seen == f"phase Open of trading day {_FIRST_DAY}", "the venue is still open", seen)

    log(f"6. trading day {_FIRST_DAY} ends by itself, with no end")
    check(ask_for_phase(install_dir, _FIRST_DAY, "Closed") and ask_for_phase(install_dir, _FIRST_DAY, "EndOfDayBegin"),
          "an operator closes the venue and asks for the end of day")
    left = wait_for_the_venue_to_stop(env, install_dir, 120.0)
    check(not left, "every component of the day stops by itself, and its launcher with it", ", ".join(left))
    check(says_the_end(env, "sequencer_primary", _FIRST_DAY) and says_the_end(env, "admin_service", _FIRST_DAY),
          "the last-state files of the leading sequencer and the administration service say the end-of-day end")
    check_launcher_archives(env, install_dir, _FIRST_DAY)
    subprocess.run([sys.executable, str(_SCRIPT_DIR / "devenv.py"), "stop"], capture_output=True, text=True, check=False)
    check(not trading_day.running_processes(env, install_dir), "the components that take no part in the day are stopped")

    log(f"7. trading day {_SECOND_DAY} runs by its schedule from start to end, with no launchers, and end archives it on the operator's word")
    whole_day = write_schedule(work / "whole_day_when_ready.toml",
                               {"pre_open": "when ready", "open": "when ready", "closed": "when ready", "end_of_day": "when ready"})
    manifest_left = trading_day.manifest_path(install_dir)
    credentials_left = devenv.credentials_path(install_dir)
    status, output = run_procedure("start", "--trading-day", str(_SECOND_DAY), "--schedule", str(whole_day))
    check(status == 0, "start succeeds with only the run manifest and the credentials of the first day present")
    first_archive = trading_day.day_archive(env, _FIRST_DAY)
    first_listing = first_archive / "listing.next_start.txt"
    listed = first_listing.read_text(encoding="utf-8") if first_listing.is_file() else ""
    check(str(manifest_left) in listed and str(credentials_left) in listed,
          "start archives the run manifest and credentials of the first day, with a listing of their own", listed)
    check(f"trading_day = {_FIRST_DAY}" in (first_archive / manifest_left.relative_to(install_dir)).read_text(encoding="utf-8")
          if (first_archive / manifest_left.relative_to(install_dir)).is_file() else False,
          "the archived run manifest is the first day's")
    left = wait_for_the_venue_to_stop(env, install_dir, 180.0)
    check(not left and says_the_end(env, "sequencer_primary", _SECOND_DAY),
          "the venue reaches the end of day by the schedule alone, and every component of the day stops", ", ".join(left))
    subprocess.run([sys.executable, str(_SCRIPT_DIR / "devenv.py"), "stop"], capture_output=True, text=True, check=False)
    # With no launcher, nothing archived the components' files: the manual way does it, as for a component that missed the
    # end of day (docs/venue/start_and_end_of_day.md, section 4.2, decision 6).
    status, output = run_procedure("end", "--operator-says-the-day-ended")
    check(status == 0 and "the leading sequencer is not asked" in output, "end on the operator's word succeeds without asking", output)
    check_archive(env, install_dir, _SECOND_DAY)
    second_listing = trading_day.day_archive(env, _SECOND_DAY) / "listing.txt"
    check(second_listing.is_file() and f"on the word of operator {_OPERATOR}" in second_listing.read_text(encoding="utf-8"),
          "the listing records the operator's word")

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
    # Parsed before anything else, so that --help describes the test and does nothing: the build runs every top-level
    # script with --help, and this one, run, clears the installation's WALs and regions and starts the venue.
    argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter).parse_args()
    try:
        sys.exit(main())
    finally:
        stop_whatever_is_running()
