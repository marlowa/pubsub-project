# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
operator_requests.py — what the operator's tools share: finding each sequencer's listener for operators, connecting to it
over TLS with the operator's certificate, sending one request and reading its answer, and starting a trading day.

Used by operator_halt.py, trading_phase.py, devenv.py and ha_test.py. Each sequencer's listener accepts only TLS, and
only a client presenting a certificate signed by the operator's certificate authority. deploy.py makes the authority, the
operator's certificate and each listener's certificate ([operator_certificates] in the environment file).

By default everything is found in the install directory: the sequencers' deployed configurations
(etc/sequencer/sequencer_primary.toml and sequencer_secondary.toml, their [operator] sections), the operator's
certificate and key and the authority's certificate (etc/operator), and the generated Python messages (lib/python).
"""

from __future__ import annotations

import socket
import ssl
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

SEQUENCERS = ("sequencer_primary", "sequencer_secondary")


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


def ask_for_phase(install_dir: Path, trading_day: int, phase_name: str, operator: str, *,
                  listeners: list[tuple[str, str, int]] | None = None, context: ssl.SSLContext | None = None,
                  timeout: float = 5.0) -> PhaseAnswer:
    """Sends a TradingPhaseRequest for phase_name ("StartOfDay", "PreOpen", ...) on trading_day to every sequencer, and
    returns the answer of the one that leads, with a line describing each sequencer's answer."""
    leader_follower, pdu_framing = load_messages(install_dir)
    request = leader_follower.TradingPhaseRequest(trading_day=trading_day, requested_phase=leader_follower.TradingPhase[phase_name],
                                                  operator_name=operator)
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
                 say: Callable[[str], None] = print) -> PhaseAnswer:
    """Asks for each phase in phase_names in turn ("StartOfDay", "PreOpen", ...), going on to the next once the venue is in
    or past it, until every one is entered or timeout seconds have passed. Returns the last answer the sequencer that leads
    gave, or one with no answer when none led.

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
            result = ask_for_phase(install_dir, trading_day, phase_name, operator, listeners=listeners, context=context)
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
    """Starts trading day trading_day: asks for the start of day, then pre-open, then open, until the venue is open or
    timeout seconds have passed (enter_phases). Returns whether the venue is open, or past it, on trading_day."""
    return entered(install_dir, enter_phases(install_dir, trading_day, ("StartOfDay", "PreOpen", "Open"), operator, timeout=timeout, say=say))
