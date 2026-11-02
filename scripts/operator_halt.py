#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
operator_halt.py — halt trading, or lift a halt, as an operator.

The tool sends an OperatorHaltRequest to both sequencers, over each one's listener for operators, and prints each
answer. Only the sequencer that leads acts; the other answers that it does not lead. The design is
docs/availability/voided_commands.md, section 6.2.

  * To halt, the leading sequencer writes a TechnicalEvent that halts trading. New orders are refused, cancels are
    carried out, and no order is cancelled: an operator's halt keeps the book.
  * To lift, it writes a TechnicalEvent that lifts the halt, but only if a matching engine leads and the WAL holds a
    report answering every order and cancel written before the halt. Otherwise it declines and says why, and the
    operator asks again later. This is how a halt for a recovery that took too long (R-0117) is ended, once a matching
    engine leads again.

The listener accepts only TLS, and only a client presenting a certificate signed by the operator's certificate
authority. deploy.py makes the authority, the operator's certificate and each listener's certificate
([operator_certificates] in the environment file). The tool checks that each sequencer's certificate was signed by
the same authority, and names the host it connected to.

By default everything is found in the install directory: the sequencers' deployed configurations
(etc/sequencer/sequencer_primary.toml and sequencer_secondary.toml, their [operator] sections), the operator's
certificate and key and the authority's certificate (etc/operator), and the generated Python messages (lib/python).
A sequencer listening on every interface (0.0.0.0) is reached at this machine's name; --sequencer names one
elsewhere.

Exit status: 0 when the leading sequencer wrote what was asked; 1 when it declined, saying why; 2 when no sequencer
that leads answered.

Usage:
    ./operator_halt.py halt --operator alice
    ./operator_halt.py lift --operator alice
    ./operator_halt.py lift --operator alice --sequencer host-a:11050 --sequencer host-b:11051
"""

from __future__ import annotations

import argparse
import socket
import ssl
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
_SEQUENCERS = ("sequencer_primary", "sequencer_secondary")


def _default_install_dir() -> Path:
    """The install directory: this script's own directory when it was shipped there with lib/python beside it, as a
    release ships it; otherwise the project's installed/ directory, for a script run from the source tree."""
    if (_SCRIPT_DIR / "lib" / "python").is_dir():
        return _SCRIPT_DIR
    return _SCRIPT_DIR.parent / "installed"


def _tomllib():
    """Return the TOML parser: `tomllib`, from Python 3.11, or the `tomli` package on an older Python.

    Imported when a file is first read, not when the script starts, so that --help works without either."""
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
    for name in _SEQUENCERS:
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


def parse_sequencer(text: str) -> tuple[str, str, int]:
    """A --sequencer argument, HOST:PORT, as (name, host, port)."""
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit():
        raise argparse.ArgumentTypeError(f"expected HOST:PORT, not {text!r}")
    return text, host, int(port)


def ask(listener: tuple[str, str, int], context: ssl.SSLContext, request_bytes: bytes, timeout: float, modules):
    """Sends the request to one sequencer and returns its OperatorHaltAnswer, or a string saying why there is none."""
    leader_follower, pdu_framing = modules
    name, host, port = listener
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
                        if header.pdu_id == leader_follower.OperatorHaltAnswer.MESSAGE_ID:
                            answer, _ = leader_follower.OperatorHaltAnswer.decode(body)
                            return answer
    except (OSError, ssl.SSLError) as error:
        return f"{name} ({host}:{port}) could not be asked: {error}"


def main() -> int:
    install_default = _default_install_dir()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("halt", "lift"), help="halt trading, or lift the halt")
    parser.add_argument("--operator", required=True, help="your name, which each sequencer writes in its Quill log with the request")
    parser.add_argument("--install-dir", type=Path, default=install_default, metavar="PATH",
                        help=f"the install directory, where the deployed configurations, certificates and lib/python are (default: {install_default})")
    parser.add_argument("--sequencer", action="append", type=parse_sequencer, metavar="HOST:PORT",
                        help="a sequencer's listener for operators; give it once for each sequencer. Default: both, from their deployed configurations")
    parser.add_argument("--certificate", type=Path, metavar="PATH", help="the operator's certificate (default: etc/operator/operator.crt in the install directory)")
    parser.add_argument("--key", type=Path, metavar="PATH", help="its private key (default: etc/operator/operator.key in the install directory)")
    parser.add_argument("--ca", type=Path, metavar="PATH",
                        help="the certificate authority that signed the sequencers' certificates (default: etc/operator/operator_ca.crt in the install directory)")
    parser.add_argument("--timeout", type=float, default=5.0, metavar="SECONDS", help="how long to wait for each sequencer (default: 5)")
    args = parser.parse_args()

    install_dir = args.install_dir.resolve()
    sys.path.insert(0, str(install_dir / "lib" / "python"))
    import leader_follower  # pylint: disable=import-outside-toplevel,import-error
    import pdu_framing  # pylint: disable=import-outside-toplevel,import-error

    operator_dir = install_dir / "etc" / "operator"
    context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=str(args.ca or operator_dir / "operator_ca.crt"))
    context.load_cert_chain(str(args.certificate or operator_dir / "operator.crt"), str(args.key or operator_dir / "operator.key"))

    listeners = args.sequencer or deployed_listeners(install_dir)
    if not listeners:
        print(f"no sequencer to ask: none is deployed in {install_dir}, and none was named with --sequencer", file=sys.stderr)
        return 2

    request = leader_follower.OperatorHaltRequest(halt=args.action == "halt", operator_name=args.operator)
    request_bytes = pdu_framing.encode_pdu(request.MESSAGE_ID, request.MESSAGE_VERSION, request.encode())
    outcome = leader_follower.OperatorHaltOutcome
    wrote = {outcome.Halted, outcome.Lifted}
    status = 2
    for listener in listeners:
        answer = ask(listener, context, request_bytes, args.timeout, (leader_follower, pdu_framing))
        if isinstance(answer, str):
            print(answer)
            continue
        print(f"{listener[0]}: {answer.outcome.name} -- {answer.explanation} (trading is {'halted' if answer.halted else 'not halted'})")
        if answer.outcome in wrote:
            status = 0
        elif answer.outcome != outcome.NotLeader and status == 2:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
