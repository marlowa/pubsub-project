# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
operator_requests.py — what the operator's tools share: finding each sequencer's listener for operators, connecting to it
over TLS with the operator's certificate, sending one request and reading its answer, starting a trading day, reading
the day's schedule from a file, and replacing the schedule during the day.

Used by operator_halt.py, trading_phase.py, devenv.py and ha_test.py. Each sequencer's listener accepts only TLS, and
only a client presenting a certificate signed by the operator's certificate authority. deploy.py makes the authority, the
operator's certificate and each listener's certificate ([operator_certificates] in the environment file).

By default everything is found in the install directory: the sequencers' deployed configurations
(etc/sequencer/sequencer_primary.toml and sequencer_secondary.toml, their [operator] sections), the operator's
certificate and key and the authority's certificate (etc/operator), and the generated Python messages (lib/python).
"""

from __future__ import annotations

import datetime
import socket
import ssl
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Tuple

SEQUENCERS = ("sequencer_primary", "sequencer_secondary")

# The phases a schedule may give a time to, in order, as a schedule file names them, with the name the messages use.
SCHEDULE_PHASES = (("pre_open", "PreOpen"), ("open", "Open"), ("closed", "Closed"), ("end_of_day", "EndOfDay"))

# The value a schedule file gives a phase that is to be entered as soon as its conditions hold, at no particular time.
WHEN_READY = "when ready"

# A schedule as the operator's requests carry it: (phase, earliest moment in nanoseconds since 1970 UTC, or 0 for as soon
# as its conditions hold), in phase order, with the phase named as the messages name it ("PreOpen", ...).
Schedule = List[Tuple[str, int]]


def _tomllib():
    """Return the TOML parser: `tomllib`, from Python 3.11, or the `tomli` package on an older Python.

    Imported when a file is first read, not when a script starts, so that --help works without either."""
    try:
        import tomllib  # pylint: disable=import-outside-toplevel
    except ImportError:
        import tomli as tomllib  # type: ignore[no-redef]  # pylint: disable=import-outside-toplevel
    return tomllib


def deployed_listeners(install_dir: Path) -> list[tuple[str, str, int]]:
    """Each sequencer's listener for operators, as its deployed configuration gives it: (name, host, port).

    A listener on every interface is reached at this machine's fully qualified name, which deploy.py puts in its
    certificate."""
    listeners = []
    for name in SEQUENCERS:
        config_path = install_dir / "etc" / "sequencer" / f"{name}.toml"
        if not config_path.is_file():
            continue
        with config_path.open("rb") as config_file:
            operator = _tomllib().load(config_file).get("operator")
        if operator is None:
            continue
        host = operator["listen_host"]
        if host in ("0.0.0.0", "::"):
            host = socket.getfqdn()
        listeners.append((name, host, int(operator["listen_port"])))
    return listeners


def load_messages(install_dir: Path):
    """The generated modules leader_follower and pdu_framing, from lib/python in the install directory."""
    python_dir = str(install_dir / "lib" / "python")
    if python_dir not in sys.path:
        sys.path.insert(0, python_dir)
    import leader_follower  # pylint: disable=import-outside-toplevel,import-error
    import pdu_framing  # pylint: disable=import-outside-toplevel,import-error
    return leader_follower, pdu_framing


def tls_context(install_dir: Path, certificate: Path | None = None, key: Path | None = None, ca: Path | None = None) -> ssl.SSLContext:
    """A TLS context presenting the operator's certificate and trusting only the operator's certificate authority."""
    operator_dir = install_dir / "etc" / "operator"
    context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=str(ca or operator_dir / "operator_ca.crt"))
    context.load_cert_chain(str(certificate or operator_dir / "operator.crt"), str(key or operator_dir / "operator.key"))
    return context


def ask(listener: tuple[str, str, int], context: ssl.SSLContext, request, answer_type, pdu_framing, timeout: float):
    """Sends request to one sequencer and returns its answer, decoded as answer_type, or a string saying why there is
    none."""
    name, host, port = listener
    request_bytes = pdu_framing.encode_pdu(request.MESSAGE_ID, request.MESSAGE_VERSION, request.encode())
    try:
        with socket.create_connection((host, port), timeout=timeout) as raw:
            with context.wrap_socket(raw, server_hostname=host) as connection:
                connection.sendall(request_bytes)
                reader = pdu_framing.PduReader()
                while True:
                    data = connection.recv(65536)
                    if not data:
                        return f"{name} ({host}:{port}) closed the connection without answering"
                    for header, body in reader.feed(data):
                        if header.pdu_id == answer_type.MESSAGE_ID:
                            answer, _ = answer_type.decode(body)
                            return answer
    except (OSError, ssl.SSLError) as error:
        return f"{name} ({host}:{port}) could not be asked: {error}"


@dataclass
class PhaseAnswer:
    """What the sequencer that leads answered to a TradingPhaseRequest, or why no such answer came."""
    answered_by: str | None
    answer: object | None
    lines: list[str]


def _zone_info(name: str):
    """The time zone called name, from `zoneinfo`, which Python has from 3.9, or the `backports.zoneinfo` package on an
    older Python. Imported when a schedule is first read, so that a script that reads none needs neither."""
    try:
        from zoneinfo import ZoneInfo  # pylint: disable=import-outside-toplevel
    except ImportError:
        from backports.zoneinfo import ZoneInfo  # type: ignore[no-redef]  # pylint: disable=import-outside-toplevel
    return ZoneInfo(name)


def schedule_entries(leader_follower, schedule: Schedule | None) -> list:
    """schedule as the TradingScheduleEntry messages a request carries."""
    return [leader_follower.TradingScheduleEntry(phase=leader_follower.TradingPhase[phase_name], earliest_utc_ns=earliest)
            for phase_name, earliest in (schedule or [])]


def schedule_in_words(schedule: Schedule) -> str:
    """schedule in words, with each time in UTC, as the sequencer logs it."""
    if not schedule:
        return "no scheduled phase: every phase is entered when an operator asks"
    words = []
    for phase_name, earliest in schedule:
        if earliest == 0:
            words.append(f"{phase_name} as soon as its conditions hold")
        else:
            moment = datetime.datetime.fromtimestamp(earliest / 1e9, tz=datetime.timezone.utc)
            words.append(f"{phase_name} at {moment:%Y-%m-%d %H:%M:%S} UTC")
    return ", ".join(words)


def read_schedule_file(path: Path, trading_day: int) -> Schedule:
    """The schedule for trading_day (YYYYMMDD) from the schedule file at path, each time turned from the venue's local time
    into a moment in UTC.

    The file is TOML. time_zone names the zone the times are written in, as the IANA time zone database names it
    ("Europe/London"). The table [phases] gives each phase a time of day, "HH:MM" or "HH:MM:SS", or "when ready" for as soon
    as its conditions hold. The phases are pre_open, open, closed and end_of_day, and the schedule stops at the first one
    left out: that phase and every one after it are entered only when an operator asks, so a phase after one left out is an
    error. Daylight saving is the time zone's: 07:50 in London is 07:50 UTC in winter and 06:50 UTC in summer. Raises
    ValueError, saying what is wrong, for a file that does not give a schedule."""
    with path.open("rb") as schedule_file:
        contents = _tomllib().load(schedule_file)
    zone_name = contents.get("time_zone")
    if not isinstance(zone_name, str):
        raise ValueError(f"{path}: time_zone is missing; it names the zone the times are in, such as \"Europe/London\"")
    try:
        zone = _zone_info(zone_name)
    except (KeyError, ValueError) as error:
        raise ValueError(f"{path}: time_zone {zone_name!r} is not a time zone this machine knows: {error}") from error
    phases = contents.get("phases", {})
    unknown = sorted(set(phases) - {name for name, _ in SCHEDULE_PHASES})
    if unknown:
        raise ValueError(f"{path}: [phases] names {unknown}, which are not phases a schedule gives a time to; they are "
                         f"{[name for name, _ in SCHEDULE_PHASES]}")
    day = datetime.date(trading_day // 10000, trading_day // 100 % 100, trading_day % 100)
    schedule: Schedule = []
    left_out = None
    for file_name, phase_name in SCHEDULE_PHASES:
        if file_name not in phases:
            left_out = left_out or file_name
            continue
        if left_out is not None:
            raise ValueError(f"{path}: [phases] gives {file_name} a time but leaves out {left_out}, which comes before it; a schedule stops at "
                             "the first phase it leaves out")
        value = phases[file_name]
        if value == WHEN_READY:
            schedule.append((phase_name, 0))
            continue
        try:
            time_of_day = datetime.time.fromisoformat(str(value))
        except ValueError as error:
            raise ValueError(f"{path}: [phases] {file_name} = {value!r} is neither a time written HH:MM or HH:MM:SS nor {WHEN_READY!r}") from error
        local = datetime.datetime.combine(day, time_of_day, tzinfo=zone)
        schedule.append((phase_name, int(local.timestamp()) * 1_000_000_000))
    return schedule


def ask_for_phase(install_dir: Path, trading_day: int, phase_name: str, operator: str, *,
                  listeners: list[tuple[str, str, int]] | None = None, context: ssl.SSLContext | None = None,
                  timeout: float = 5.0, schedule: Schedule | None = None) -> PhaseAnswer:
    """Sends a TradingPhaseRequest for phase_name ("StartOfDay", "PreOpen", ...) on trading_day to every sequencer, and
    returns the answer of the one that leads, with a line describing each sequencer's answer. With the start of day,
    schedule is the day's schedule; with any other phase it is not sent."""
    leader_follower, pdu_framing = load_messages(install_dir)
    request = leader_follower.TradingPhaseRequest(trading_day=trading_day, requested_phase=leader_follower.TradingPhase[phase_name],
                                                  operator_name=operator,
                                                  schedule=schedule_entries(leader_follower, schedule) if phase_name == "StartOfDay" else [])
    context = context or tls_context(install_dir)
    listeners = listeners if listeners is not None else deployed_listeners(install_dir)
    result = PhaseAnswer(answered_by=None, answer=None, lines=[])
    for listener in listeners:
        answer = ask(listener, context, request, leader_follower.TradingPhaseAnswer, pdu_framing, timeout)
        if isinstance(answer, str):
            result.lines.append(answer)
            continue
        state = (f"trading day {answer.trading_day}, phase {answer.phase.name}, trading {'halted' if answer.halted else 'not halted'}"
                 if answer.day_started else "no start of day")
        result.lines.append(f"{listener[0]}: {answer.outcome.name} -- {answer.explanation} ({state})")
        if answer.outcome != leader_follower.TradingPhaseOutcome.NotLeader:
            result.answered_by = listener[0]
            result.answer = answer
    return result


def enter_phases(install_dir: Path, trading_day: int, phase_names: tuple[str, ...], operator: str, *, timeout: float = 60.0,
                 say: Callable[[str], None] = print, schedule: Schedule | None = None) -> PhaseAnswer:
    """Asks for each phase in phase_names in turn ("StartOfDay", "PreOpen", ...), going on to the next once the venue is in
    or past it, until every one is entered or timeout seconds have passed. Returns the last answer the sequencer that leads
    gave, or one with no answer when none led. The start of day is sent with schedule, the day's schedule; with none, the
    day is run by hand.

    Each request is sent again while no sequencer leads, or while the one that leads answers NotReady: pre-open waits until
    the matching engine has acted on the start of day, and the end of day until it has acted on the close. Every other
    refusal ends the attempt. A request sent again does no harm, so this also serves a venue restarted during the day, or
    left part of the way through by a change of leader: each phase it is in or past already is answered as such, and the
    rest are asked for."""
    leader_follower, _ = load_messages(install_dir)
    outcome = leader_follower.TradingPhaseOutcome
    done = {outcome.Written, outcome.AlreadyInPhase, outcome.AlreadyPast}
    deadline = time.monotonic() + timeout
    context = tls_context(install_dir)
    listeners = deployed_listeners(install_dir)
    result = PhaseAnswer(answered_by=None, answer=None, lines=[])
    if not listeners:
        say(f"no sequencer to ask: none is deployed in {install_dir}")
        return result
    for phase_name in phase_names:
        while True:
            result = ask_for_phase(install_dir, trading_day, phase_name, operator, listeners=listeners, context=context, schedule=schedule)
            if result.answer is not None and result.answer.outcome in done:
                for line in result.lines:
                    say(f"  {phase_name}: {line}")
                break
            if result.answer is not None and result.answer.outcome != outcome.NotReady:
                for line in result.lines:
                    say(f"  {phase_name}: {line}")
                return result
            if time.monotonic() > deadline:
                for line in result.lines:
                    say(f"  {phase_name}: {line}")
                say(f"  {phase_name}: no sequencer that leads accepted it within {timeout:.0f}s")
                return result
            time.sleep(0.2)
    return result


def entered(install_dir: Path, result: PhaseAnswer) -> bool:
    """Whether result says the venue is in or past the phase asked for."""
    leader_follower, _ = load_messages(install_dir)
    outcome = leader_follower.TradingPhaseOutcome
    return result.answer is not None and result.answer.outcome in (outcome.Written, outcome.AlreadyInPhase, outcome.AlreadyPast)


def start_trading_day(install_dir: Path, trading_day: int, operator: str, *, timeout: float = 60.0,
                      say: Callable[[str], None] = print) -> bool:
    """Starts trading day trading_day, run by hand: asks for the start of day with no schedule, then pre-open, then open,
    until the venue is open or timeout seconds have passed (enter_phases). Returns whether the venue is open, or past it,
    on trading_day."""
    return entered(install_dir, enter_phases(install_dir, trading_day, ("StartOfDay", "PreOpen", "Open"), operator, timeout=timeout, say=say))


@dataclass
class ScheduleAnswer:
    """What the sequencer that leads answered to a TradingScheduleRequest, or why no such answer came."""
    answered_by: str | None
    answer: object | None
    lines: list[str]


def replace_schedule(install_dir: Path, trading_day: int, schedule: Schedule, operator: str, *, timeout: float = 60.0,
                     say: Callable[[str], None] = print) -> ScheduleAnswer:
    """Asks for the times that remain on trading_day to be replaced by schedule, which starts at the phase after the one the
    venue is in. The request is sent to every sequencer, again while none leads, until timeout seconds have passed. Returns
    the answer of the one that leads, or one with no answer when none led."""
    leader_follower, pdu_framing = load_messages(install_dir)
    request = leader_follower.TradingScheduleRequest(trading_day=trading_day, entries=schedule_entries(leader_follower, schedule),
                                                     operator_name=operator)
    context = tls_context(install_dir)
    listeners = deployed_listeners(install_dir)
    deadline = time.monotonic() + timeout
    result = ScheduleAnswer(answered_by=None, answer=None, lines=[])
    if not listeners:
        say(f"no sequencer to ask: none is deployed in {install_dir}")
        return result
    while True:
        result = ScheduleAnswer(answered_by=None, answer=None, lines=[])
        for listener in listeners:
            answer = ask(listener, context, request, leader_follower.TradingScheduleAnswer, pdu_framing, 5.0)
            if isinstance(answer, str):
                result.lines.append(answer)
                continue
            result.lines.append(f"{listener[0]}: {answer.outcome.name} -- {answer.explanation}")
            if answer.outcome != leader_follower.TradingScheduleOutcome.NotLeader:
                result.answered_by = listener[0]
                result.answer = answer
        if result.answer is not None or time.monotonic() > deadline:
            break
        time.sleep(0.2)
    for line in result.lines:
        say(f"  schedule: {line}")
    if result.answer is None:
        say(f"  schedule: no sequencer that leads answered within {timeout:.0f}s")
    return result
