#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
trading_day.py — the procedures that start and end a trading day (docs/venue/start_and_end_of_day.md, sections 3 and 4).

Every trading day starts with none of the previous day's files (principle V12 of docs/design_principles.md). The start
procedure checks that for the files a component reads when it starts; an old Quill log, or a credentials file it is
about to overwrite, does not stop it. It writes the run manifest, exports the day's inputs, starts every component for the day and asks for
the start of day with the day's schedule. From then on the leading sequencer moves the venue from one phase to the next
by the schedule in its WAL. The end procedure checks that the venue has reached the end of day, stops every component,
and moves every file the venue wrote during the day into the day's archive, so that the next day starts on none of them.

  start --trading-day D --schedule FILE --operator NAME
      1. Checks that no process of the installation is running.
      2. Looks for every file of a trading day that a component reads when it starts: the WALs and the files beside them,
         the open-order regions, the epoch files, the lease promise records and the arbiters' statements. With none, the
         day starts fresh. With the run manifest
         naming D, and saying the day's inputs are complete, this is a restart during day D: it goes straight to step 6,
         and the schedule is the manifest's. With the manifest naming D and the inputs not complete, an earlier start
         stopped before it had them, and it carries on from step 3. In every other case it refuses, naming each file
         found and the day each belongs to, and moves or deletes nothing.
      3. Writes the run manifest, var/run_manifest.toml under the installation: the trading day, the schedule in UTC,
         the release installed, when it was written, and that the inputs are not complete.
      4. Exports the members' credentials from the database, checks that the file holds as many as the database query
         found, and only then records in the manifest that the inputs are complete.
      5. Starts every component, in the order the environment file gives, each with --trading-day D. A component that
         finds a file of another day refuses to start, naming it.
      6. Asks for the start of day, with the schedule, until the sequencer that leads has written it or says the venue is
         in it or past it.
      7. Reports the trading day, the phase the venue is in, and the schedule, and exits.

  end --operator NAME
      1. Asks the sequencer that leads for the trading state, with a question that writes nothing, and refuses unless the
         venue is in the end of day of the day the run manifest names. The procedure never moves the venue on itself.
      2. Stops every component in the reverse of the order they were started in, and checks by its process identifier
         that each one has exited. If any has not, it stops and archives nothing.
      3. Writes a listing of every file of the day, with its size and SHA-256 checksum, under
         <archive_dir>/<D>/<machine>/listing.txt, then moves each file below that directory, keeping its path below
         the installation; a file outside the installation, such as a WAL on another disk, keeps its whole path below
         outside_the_installation/. Each archived copy is checked against the listing. A move within one filesystem is
         a rename; a move to another is a copy, synced and checked, followed by the removal of the original. The run
         manifest is moved last, so that a procedure stopped part of the way through leaves it in place, and the next
         run of end finishes the work: a file already archived with the same checksum is removed from its place.
      4. Reports where the archive is, how many files and bytes it holds, and that the installation holds no file of
         the day.

The order activity recorder is not built, so the end procedure does not wait for it to confirm the day's events
(docs/venue/trading_day_plan.md, decision 4).

devenv.py start --fresh-wal --fresh-book remains the way to start a development run without these procedures.

Exit status: 0 when the procedure did what it says; 1 when it refused or something failed, saying why.

Usage:
    ./trading_day.py start --trading-day 20261116 --schedule environments/schedules/trading_schedule.toml --operator alice
    ./trading_day.py end --operator alice
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

import devenv
import operator_requests

_MANIFEST = Path("var") / "run_manifest.toml"
_LISTING = "listing.txt"
_OUTSIDE = "outside_the_installation"
_START_DAY_TIMEOUT = 120.0  # seconds to keep asking for the start of day: until a sequencer leads
_STOP_CHECK_TIMEOUT = 5.0   # seconds to wait, after the stop, for every process to have exited
_TRADING_DAY_FIELD = re.compile(rb"^trading_day=(\d{8})[ \n]")


def trading_day_argument(text: str) -> int:
    """A --trading-day argument: eight digits, YYYYMMDD."""
    if len(text) != 8 or not text.isdigit():
        raise argparse.ArgumentTypeError(f"expected a date written as YYYYMMDD, not {text!r}")
    return int(text)


# ── The run manifest ──────────────────────────────────────────────────────────

@dataclass
class Manifest:
    """What the run manifest records: the trading day, its schedule, and whether the day's inputs are complete."""
    trading_day: int
    schedule: operator_requests.Schedule
    inputs_complete: bool


def manifest_path(install_dir: Path) -> Path:
    """Where the run manifest is: var/run_manifest.toml under the installation."""
    return install_dir / _MANIFEST


def read_manifest(install_dir: Path) -> Optional[Manifest]:
    """The run manifest, or None when there is none. Exits, saying why, when there is one that cannot be read."""
    path = manifest_path(install_dir)
    if not path.is_file():
        return None
    try:
        with path.open("rb") as manifest_file:
            contents = devenv._tomllib().load(manifest_file)  # pylint: disable=protected-access
        schedule = [(str(entry["phase"]), int(entry["earliest_utc_ns"])) for entry in contents.get("schedule", [])]
        return Manifest(int(contents["trading_day"]), schedule, bool(contents["inputs_complete"]))
    except (OSError, KeyError, ValueError, TypeError) as error:
        sys.exit(f"error: the run manifest {path} cannot be read ({error}); it is left as it is, and nothing was started or moved")


def write_manifest(install_dir: Path, manifest: Manifest, schedule_file: Optional[Path]) -> None:
    """Writes the run manifest under a temporary name, syncs it, and renames it into place, so that it is never seen part
    written."""
    path = manifest_path(install_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    release: Dict[str, str] = {}
    try:
        release = json.loads((install_dir / "release.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    lines = [
        f"# The run manifest of trading day {manifest.trading_day}, written by scripts/trading_day.py start.",
        "# Every file the venue writes after it belongs to this trading day (docs/venue/start_and_end_of_day.md, section 3).",
        f"trading_day = {manifest.trading_day}",
        f"inputs_complete = {'true' if manifest.inputs_complete else 'false'}",
        f"written_at = \"{datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}\"",
        f"schedule_file = {json.dumps(str(schedule_file) if schedule_file is not None else '')}",
        f"release_version = {json.dumps(str(release.get('version', 'unknown')))}",
        f"release_git_hash = {json.dumps(str(release.get('git_hash', 'unknown')))}",
        "",
        "# The day's schedule: each phase and the earliest moment to enter it, in nanoseconds since 1970 UTC, 0 meaning as",
        "# soon as its conditions hold. A phase not listed is entered only when an operator asks.",
    ]
    for phase_name, earliest in manifest.schedule:
        lines += ["[[schedule]]", f"phase = \"{phase_name}\"", f"earliest_utc_ns = {earliest}",
                  f"earliest = {json.dumps(operator_requests.schedule_in_words([(phase_name, earliest)]))}", ""]
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as manifest_file:
        manifest_file.write("\n".join(lines) + "\n")
        manifest_file.flush()
        os.fsync(manifest_file.fileno())
    os.replace(temporary, path)
    _sync_directory(path.parent)


# ── The files of a trading day ────────────────────────────────────────────────

@dataclass
class DayFile:
    """A file that belongs to one trading day, and the day it records: None when this procedure does not read its day
    (the component that uses it does), zero when it records none.

    read_at_start says whether a component reads the file when it starts, and so could take it for the new day's: the
    WALs and the files beside them, the open-order regions, the epoch files, the lease promise records and the arbiters'
    statements. Only those, and a run manifest of another day, stop a day from starting. The others cannot be taken for
    the new day's: a Quill log or captured output has its day in its name, the credentials file is exported again before
    any component starts, a FIX capture file is emptied as its gateway starts, and a region set aside as unusable is never
    read again. The end-of-day procedure archives them all the same."""
    path: Path
    recorded_day: Optional[int]
    read_at_start: bool


def _recorded_by_text_file(path: Path) -> int:
    """The day a small text file records in its first field, trading_day=YYYYMMDD; zero when it records none."""
    try:
        with path.open("rb") as text_file:
            found = _TRADING_DAY_FIELD.match(text_file.read(64))
    except OSError:
        return 0
    return int(found.group(1)) if found else 0


def _component_workdir(install_dir: Path, comp: dict) -> Path:
    return (install_dir / comp.get("workdir", ".")).resolve()


def _fix_capture_files(env: dict, install_dir: Path) -> List[Path]:
    """The FIX order gateways' capture files: [fix_capture] file in each deployed configuration, which a relative path
    names from the gateway's working directory."""
    found: List[Path] = []
    for comp in env["components"].values():
        if "config" not in comp:
            continue
        config_path = (install_dir / comp["config"]).resolve()
        try:
            with config_path.open("rb") as config_file:
                config = devenv._tomllib().load(config_file)  # pylint: disable=protected-access
        except (OSError, ValueError):
            continue
        value = config.get("fix_capture", {}).get("file")
        if value:
            raw = Path(str(value))
            found.append(raw if raw.is_absolute() else _component_workdir(install_dir, comp) / raw)
    return sorted(set(found))


def _day_in_log_name(name: str, component: str) -> Optional[int]:
    """The trading day in the name of a Quill log or captured output of component, such as sequencer_primary.20261116.log,
    or None when the name is not one of those."""
    found = re.fullmatch(re.escape(component) + r"\.(\d{8})\..+", name)
    return int(found.group(1)) if found else None


def files_of_the_day(env: dict, install_dir: Path, logs_of_day: Optional[int] = None) -> List[DayFile]:  # pylint: disable=too-many-branches
    """Every file present that belongs to one trading day (docs/venue/start_and_end_of_day.md, section 2.1), with the
    day each records where this procedure reads it. The run manifest is last.

    Quill logs and captured output are found by their names, <component>.<YYYYMMDD>.<anything>, for each component the
    environment file names; with logs_of_day, only those of that day. A file in the log directory without a trading day
    in its name is not one the procedures wrote: the test harnesses write those, and they are left alone."""
    _, log_dir, _ = devenv.resolve_paths(env)
    found: List[DayFile] = []

    def add(path: Path, recorded: Optional[int], read_at_start: bool = True) -> None:
        if path.is_file():
            found.append(DayFile(path, recorded, read_at_start))

    for wal_directory in devenv.durable_paths(env, install_dir, "wal", "directory"):
        if wal_directory.is_dir():
            for path in sorted(wal_directory.rglob("*")):
                text = path.name.split(".tmp")[0] in ("epoch.state", "lease_promise.state")
                add(path, _recorded_by_text_file(path) if text else None)
    for region in devenv.durable_paths(env, install_dir, "order_book", "region_path"):
        add(region, None)
        add(Path(f"{region}.unusable"), None, read_at_start=False)
    for record in devenv.lease_records(env, install_dir):
        for path in (record, Path(f"{record}.tmp")):
            add(path, _recorded_by_text_file(path))
    for capture in _fix_capture_files(env, install_dir):
        add(capture, None, read_at_start=False)
    credentials = devenv.credentials_path(install_dir)
    for path in (credentials, Path(f"{credentials}.tmp")):
        add(path, None, read_at_start=False)
    if log_dir.is_dir():
        for path in sorted(log_dir.iterdir()):
            for component in env["components"]:
                day = _day_in_log_name(path.name, component)
                if day is not None and logs_of_day in (None, day):
                    add(path, day, read_at_start=False)
                    break
    add(manifest_path(install_dir), None)
    return found


def _describe(day_file: DayFile, manifest: Optional[Manifest], install_dir: Path) -> str:
    """The file and the day it belongs to, in words, for a refusal."""
    if day_file.path == manifest_path(install_dir) and manifest is not None:
        return f"{day_file.path}: the run manifest of trading day {manifest.trading_day}"
    if day_file.recorded_day is None:
        return f"{day_file.path}: its trading day is read by the component that uses it"
    if day_file.recorded_day == 0:
        return f"{day_file.path}: records no trading day"
    return f"{day_file.path}: trading day {day_file.recorded_day}"


# ── Processes ─────────────────────────────────────────────────────────────────

def running_processes(env: dict, install_dir: Path) -> List[str]:
    """Every process of the installation that is running, in words: those named by the components' process identifier
    files, and any other process running a program from the installation's bin directory."""
    _, _, run_dir = devenv.resolve_paths(env)
    running: Dict[int, str] = {}
    for name in env["components"]:
        for pid in (devenv.read_pid(run_dir, name), devenv.read_launcher_pid(run_dir, name)):
            if pid is not None and devenv.is_pid_alive(pid):
                running[pid] = name
    found = subprocess.run(["pgrep", "-f", "--", f"{install_dir}/bin/"], capture_output=True, text=True, check=False)
    for line in found.stdout.split():
        pid = int(line)
        if pid != os.getpid():
            running.setdefault(pid, "a program in the installation's bin directory")
    return [f"{name} (pid {pid})" for pid, name in sorted(running.items())]


# ── start ─────────────────────────────────────────────────────────────────────

def cmd_start(env: dict, args: argparse.Namespace) -> int:  # pylint: disable=too-many-return-statements,too-many-branches,too-many-statements
    """The start-of-day procedure (docs/venue/start_and_end_of_day.md, section 3)."""
    install_dir, _, _ = devenv.resolve_paths(env)
    day = args.trading_day
    print(f"=== starting trading day {day} ===")

    running = running_processes(env, install_dir)
    if running:
        print(f"refused: processes of the installation are running: {', '.join(running)}. Nothing was started or moved.")
        return 1

    manifest = read_manifest(install_dir)
    present = [day_file for day_file in files_of_the_day(env, install_dir) if day_file.read_at_start]
    restart = False
    if manifest is None and present:
        print("refused: these files belong to a trading day, a component reads each when it starts, and no run manifest says which day "
              "was started on them:")
        for day_file in present:
            print(f"  {_describe(day_file, manifest, install_dir)}")
        print("Every file of a trading day is archived by the end-of-day procedure for that day before another day starts. "
              "Nothing was started or moved. In a development environment, devenv.py start --fresh-wal --fresh-book clears "
              "the WALs, regions and records that a test run leaves.")
        return 1
    if manifest is not None and manifest.trading_day != day:
        print(f"refused: the run manifest names trading day {manifest.trading_day}, not {day}. These files belong to it:")
        for day_file in files_of_the_day(env, install_dir, logs_of_day=manifest.trading_day):
            print(f"  {_describe(day_file, manifest, install_dir)}")
        print(f"Run the end-of-day procedure for trading day {manifest.trading_day} first, which archives them "
              f"(trading_day.py end). Nothing was started or moved.")
        return 1
    if manifest is not None and manifest.inputs_complete:
        restart = True
        print(f"the run manifest names trading day {day}, with its inputs complete: this is a restart during the day, "
              "on the files the day has written")
        if args.schedule is not None:
            given = _read_schedule(args.schedule, day)
            if given is None:
                return 1
            if given != manifest.schedule:
                print(f"refused: the schedule in {args.schedule} ({operator_requests.schedule_in_words(given)}) is not the one the run "
                      f"manifest holds ({operator_requests.schedule_in_words(manifest.schedule)}). During the day the times are changed "
                      "with trading_phase.py schedule, not by restarting. Nothing was started.")
                return 1
        schedule = manifest.schedule
    else:
        if manifest is not None:
            print(f"the run manifest names trading day {day}, but its inputs are not complete: an earlier start stopped before "
                  "it had them, so everything from writing the manifest is done again")
        if args.schedule is None:
            print("refused: --schedule is needed to start a trading day: the file of the day's times for each phase "
                  "(environments/schedules/trading_schedule.toml is the venue's normal day)")
            return 1
        schedule_read = _read_schedule(args.schedule, day)
        if schedule_read is None:
            return 1
        schedule = schedule_read

        write_manifest(install_dir, Manifest(day, schedule, False), args.schedule)
        print(f"  wrote the run manifest {manifest_path(install_dir)}, with the inputs not yet complete")

        print("=== exporting the day's inputs ===")
        found = devenv.export_credentials(install_dir, env)
        held = _credentials_held(devenv.credentials_path(install_dir))
        if held != found:
            print(f"refused: the database query found {found} credential(s), but {devenv.credentials_path(install_dir)} holds {held}. "
                  "The run manifest still says the inputs are not complete, so the next start exports them again. Nothing was started.")
            return 1
        print(f"  {devenv.credentials_path(install_dir)} holds every one of the {found} credential(s) the database holds")
        write_manifest(install_dir, Manifest(day, schedule, True), args.schedule)
        print("  the run manifest now says the inputs are complete")

    written = read_manifest(install_dir)
    if written is None or not written.inputs_complete:
        print("refused: the run manifest does not say the inputs are complete, so no component is started")
        return 1

    devenv.check_configs_expanded(install_dir)
    ha_enabled = env.get("ha", {}).get("enabled", True)
    devenv.start_components(env, ha_enabled, args.delay, day, with_prometheus=not args.no_prometheus, supervised=args.supervised)
    not_running = _components_not_running(env, ha_enabled, not args.no_prometheus)
    if not_running:
        _, log_dir, _ = devenv.resolve_paths(env)
        print(f"failed: these components are not running: {', '.join(not_running)}. Each says why in its Quill log and its captured "
              f"output, {log_dir}/<component>.{day}.log and .stdout; one that found a file of another day names it. The start of day "
              "was not asked for.")
        return 1

    print()
    print(f"=== asking for the start of day {day}{' again' if restart else ''} ===")
    answered = operator_requests.enter_phases(install_dir, day, ("StartOfDay",), args.operator, timeout=_START_DAY_TIMEOUT, schedule=schedule)
    if not operator_requests.entered(install_dir, answered):
        print(f"failed: trading day {day} did not start; the components are running, and every member's command is refused. See the "
              "lines above, and the sequencers' Quill logs.")
        return 1

    state = operator_requests.trading_state(install_dir, args.operator, say=lambda _line: None)
    print()
    if state.answer is not None:
        print(f"trading day {state.answer.trading_day} is in phase {state.answer.phase.name}, trading "
              f"{'halted' if state.answer.halted else 'not halted'}")
    print(f"schedule: {operator_requests.schedule_in_words(schedule)}")
    print("The sequencer that leads enters each scheduled phase when its time has come; trading_phase.py enters any other.")
    return 0


def _read_schedule(path: Path, day: int) -> Optional[operator_requests.Schedule]:
    try:
        return operator_requests.read_schedule_file(path, day)
    except (OSError, ValueError) as error:
        print(f"refused: the schedule file does not give a schedule: {error}. Nothing was started.")
        return None


def _credentials_held(path: Path) -> int:
    """How many credentials the credentials file holds; -1 when it cannot be read."""
    try:
        with path.open("rb") as credentials_file:
            return len(devenv._tomllib().load(credentials_file).get("credential", []))  # pylint: disable=protected-access
    except (OSError, ValueError):
        return -1


def _components_not_running(env: dict, ha_enabled: bool, with_prometheus: bool) -> List[str]:
    _, _, run_dir = devenv.resolve_paths(env)
    missing = []
    for name in devenv.startup_order(env, ha_enabled):
        comp = env["components"][name]
        if comp.get("metrics_only", False) and (not with_prometheus or shutil.which(comp.get("command", "")) is None):
            continue
        pid = devenv.read_pid(run_dir, name)
        if pid is None or not devenv.is_pid_alive(pid):
            missing.append(name)
    return missing


# ── end ───────────────────────────────────────────────────────────────────────

def cmd_end(env: dict, args: argparse.Namespace) -> int:  # pylint: disable=too-many-return-statements
    """The end-of-day procedure (docs/venue/start_and_end_of_day.md, section 4)."""
    install_dir, _, run_dir = devenv.resolve_paths(env)
    manifest = read_manifest(install_dir)
    if manifest is None:
        print(f"refused: there is no run manifest, {manifest_path(install_dir)}, so no trading day was started by trading_day.py start. "
              "Nothing was stopped or moved.")
        return 1
    day = manifest.trading_day
    print(f"=== ending trading day {day} ===")

    state = operator_requests.trading_state(install_dir, args.operator)
    if state.answer is None:
        print(f"refused: no sequencer that leads answered, so it is not known that the venue is in the end of day. If the venue is "
              f"not running, start it again on the day's files with trading_day.py start --trading-day {day}, and run this again. "
              "Nothing was stopped or moved.")
        return 1
    answer = state.answer
    if not answer.day_started or answer.trading_day != day or answer.phase.name != "EndOfDay":
        where = f"phase {answer.phase.name} of trading day {answer.trading_day}" if answer.day_started else "no trading day"
        due = [words for phase_name, earliest in manifest.schedule if phase_name == "EndOfDay"
               for words in [operator_requests.schedule_in_words([(phase_name, earliest)])]]
        print(f"refused: the venue is in {where}, not in the end of day of trading day {day}. "
              f"{'The schedule enters it: ' + due[0] + '.' if due else 'The schedule does not enter it; trading_phase.py end-of-day asks for it.'} "
              "This procedure never moves the venue on itself. Nothing was stopped or moved.")
        return 1
    print(f"the venue is in the end of day of trading day {day}")
    print("the order activity recorder is not built, so there is no confirmation of the day's events to wait for")

    print()
    pids = [pid for name in env["components"] for pid in (devenv.read_pid(run_dir, name), devenv.read_launcher_pid(run_dir, name))
            if pid is not None and devenv.is_pid_alive(pid)]
    devenv.cmd_stop(env)
    deadline = time.monotonic() + _STOP_CHECK_TIMEOUT
    while time.monotonic() < deadline and (any(devenv.is_pid_alive(pid) for pid in pids) or running_processes(env, install_dir)):
        time.sleep(0.1)
    still = [str(pid) for pid in pids if devenv.is_pid_alive(pid)] + running_processes(env, install_dir)
    if still:
        print(f"failed: these processes are still running after the stop: {', '.join(still)}. A running component may have the day's "
              "files open or mapped, so nothing was archived.")
        return 1
    print("every process of the installation has exited")

    print()
    return archive_the_day(env, install_dir, day)


def _archived_path(path: Path, install_dir: Path, archive: Path) -> Path:
    try:
        return archive / path.relative_to(install_dir)
    except ValueError:
        return archive / _OUTSIDE / path.relative_to(path.anchor)


def _checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as data:
        for block in iter(lambda: data.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _sync_directory(directory: Path) -> None:
    descriptor = os.open(str(directory), os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _read_listing(listing: Path) -> Dict[str, tuple]:
    """The listing an earlier run of end wrote: for each original path, its checksum and size."""
    entries: Dict[str, tuple] = {}
    if listing.is_file():
        for line in listing.read_text(encoding="utf-8").splitlines():
            if line and not line.startswith("#"):
                checksum, size, original = line.split("  ", 2)
                entries[original] = (checksum, int(size))
    return entries


def archive_the_day(env: dict, install_dir: Path, day: int) -> int:  # pylint: disable=too-many-locals
    """Moves every file of the day into <archive_dir>/<day>/<machine>/, after writing a listing of each with its size and
    checksum, and checks each archived copy against it (docs/venue/start_and_end_of_day.md, section 4, step 4)."""
    archive_root = devenv.archive_dir(env)
    machine = socket.gethostname().split(".")[0]
    archive = archive_root / str(day) / machine
    files = files_of_the_day(env, install_dir, logs_of_day=day)
    print(f"=== archiving {len(files)} file(s) of trading day {day} into {archive} ===")
    archive.mkdir(parents=True, exist_ok=True)

    listing_path = archive / _LISTING
    listing = _read_listing(listing_path)
    for day_file in files:
        listing[str(day_file.path)] = (_checksum(day_file.path), day_file.path.stat().st_size)
    lines = [f"# Every file of trading day {day} on {machine}: SHA-256 checksum, size in bytes, and where the file was.",
             "# Each is archived below this directory at its path below the installation, or below "
             f"{_OUTSIDE}/ at its whole path."]
    lines += [f"{checksum}  {size}  {original}" for original, (checksum, size) in sorted(listing.items())]
    temporary = listing_path.with_name(_LISTING + ".tmp")
    with temporary.open("w", encoding="utf-8") as listing_file:
        listing_file.write("\n".join(lines) + "\n")
        listing_file.flush()
        os.fsync(listing_file.fileno())
    os.replace(temporary, listing_path)
    _sync_directory(archive)

    for day_file in files:
        original = day_file.path
        checksum, _ = listing[str(original)]
        target = _archived_path(original, install_dir, archive)
        if target.exists():
            if _checksum(target) != checksum:
                print(f"failed: {target} is already in the archive and differs from {original}; neither was changed, and the files "
                      "after it were not moved")
                return 1
            original.unlink()
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if os.stat(original).st_dev == os.stat(target.parent).st_dev:
            os.rename(original, target)
        else:
            temporary_copy = target.with_name(target.name + ".copying")
            shutil.copy2(original, temporary_copy)
            with temporary_copy.open("rb") as copied:
                os.fsync(copied.fileno())
            os.rename(temporary_copy, target)
            if _checksum(target) != checksum:
                print(f"failed: the copy of {original} at {target} does not match its checksum; the original is left in place, and the "
                      "files after it were not moved")
                return 1
            original.unlink()
        _sync_directory(target.parent)
        _sync_directory(original.parent)

    bad = [original for original, (checksum, _) in listing.items() if _checksum(_archived_path(Path(original), install_dir, archive)) != checksum]
    if bad:
        print(f"failed: these archived copies do not match the listing: {', '.join(bad)}")
        return 1

    for wal_directory in devenv.durable_paths(env, install_dir, "wal", "directory"):
        _remove_empty_directories(wal_directory)

    left = files_of_the_day(env, install_dir, logs_of_day=day)
    if left:
        print("failed: these files of a trading day are still in place: " + ", ".join(str(day_file.path) for day_file in left))
        return 1
    total = sum(size for _, size in listing.values())
    print(f"archived {len(listing)} file(s), {total:,} bytes, in {archive}")
    print(f"the installation holds no file of trading day {day}; the next trading day starts on none of them")
    return 0


def _remove_empty_directories(directory: Path) -> None:
    if not directory.is_dir():
        return
    for child in sorted(directory.rglob("*"), key=lambda path: len(path.parts), reverse=True):
        if child.is_dir() and not any(child.iterdir()):
            child.rmdir()
    if not any(directory.iterdir()):
        directory.rmdir()


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> int:
    """Parse the command line and run the procedure asked for."""
    sys.stdout.reconfigure(line_buffering=True)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--env", type=Path, default=devenv.DEFAULT_ENV_FILE, metavar="PATH",
                        help=f"the environment file (default: {devenv.DEFAULT_ENV_FILE})")
    parser.add_argument("--operator", required=True, help="your name, which each sequencer writes in its Quill log with each request")
    subparsers = parser.add_subparsers(dest="procedure", metavar="procedure")
    subparsers.required = True
    start = subparsers.add_parser("start", help="start a trading day, or restart the venue during one")
    start.add_argument("--trading-day", required=True, type=trading_day_argument, metavar="YYYYMMDD", help="the trading day to start")
    start.add_argument("--schedule", type=Path, metavar="FILE",
                       help="the file of the day's times for each phase; needed to start a day, and on a restart, if given, it must match the manifest")
    start.add_argument("--delay", type=float, default=1.0, metavar="SECONDS", help="seconds between component starts (default: 1.0)")
    start.add_argument("--supervised", action="store_true", help="start each component under scripts/launch.py, which restarts it if it dies")
    start.add_argument("--no-prometheus", action="store_true", help="do not start the Prometheus scraper")
    subparsers.add_parser("end", help="end the trading day the run manifest names, once the venue is in the end of day")
    args = parser.parse_args()

    env_path = args.env if args.env.is_absolute() else (devenv.PROJECT_ROOT / args.env)
    if not env_path.is_file():
        print(f"error: environment file not found: {env_path}")
        return 1
    env = devenv.load_env(env_path)
    if args.procedure == "start":
        return cmd_start(env, args)
    return cmd_end(env, args)


if __name__ == "__main__":
    sys.exit(main())
