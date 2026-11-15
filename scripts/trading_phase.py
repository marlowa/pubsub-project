#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
trading_phase.py — ask for the venue to enter a phase of the trading day, as an operator.

The tool sends a TradingPhaseRequest to both sequencers, over each one's listener for operators, and prints each answer.
Only the sequencer that leads acts; the other answers that it does not lead. Every request is answered from the
sequencer's current state, so a request sent again does no harm. The phases are entered one at a time, in order, and a
phase is entered only once the one before it has been. The design is docs/venue/trading_day_plan.md.

  * start-of-day: the leading sequencer writes the TechnicalEvent entering the start of day as the first record of a WAL
    that holds none, for the trading day it was started for (its --trading-day argument). Members' commands are refused
    until pre-open.
  * pre-open: written once the matching engine has acted on the start of day. Members' orders and cancels are accepted
    from here.
  * open: continuous trading. This venue does no matching, so its matching engine treats pre-open and open alike.
  * closed: members' orders and cancels are refused from here.
  * end-of-day: written once the matching engine has acted on the close.
  * start-day: start-of-day, pre-open and open in turn; this is what devenv.py and ha_test.py do once the venue is up.

While no sequencer leads, or the leading one answers that the phase's conditions do not hold yet (NotReady), the request
is sent again until --timeout passes.

The listener accepts only TLS, and only a client presenting a certificate signed by the operator's certificate
authority; see operator_requests.py for where everything is found.

Exit status: 0 when the leading sequencer wrote what was asked, or the venue was in or past that phase already; 1 when
it declined, saying why; 2 when no sequencer that leads answered.

Usage:
    ./trading_phase.py start-of-day --trading-day 20261116 --operator alice
    ./trading_phase.py pre-open --trading-day 20261116 --operator alice
    ./trading_phase.py open --trading-day 20261116 --operator alice
    ./trading_phase.py closed --trading-day 20261116 --operator alice
    ./trading_phase.py end-of-day --trading-day 20261116 --operator alice
    ./trading_phase.py start-day --trading-day 20261116 --operator alice
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import operator_requests

_SCRIPT_DIR = Path(__file__).resolve().parent
_PHASES = {"start-of-day": "StartOfDay", "pre-open": "PreOpen", "open": "Open", "closed": "Closed", "end-of-day": "EndOfDay"}


def _default_install_dir() -> Path:
    """The install directory: this script's own directory when it was shipped there with lib/python beside it, as a
    release ships it; otherwise the project's installed/ directory, for a script run from the source tree."""
    if (_SCRIPT_DIR / "lib" / "python").is_dir():
        return _SCRIPT_DIR
    return _SCRIPT_DIR.parent / "installed"


def trading_day_argument(text: str) -> int:
    """A --trading-day argument: eight digits, YYYYMMDD."""
    if len(text) != 8 or not text.isdigit():
        raise argparse.ArgumentTypeError(f"expected a date written as YYYYMMDD, not {text!r}")
    return int(text)


def main() -> int:
    install_default = _default_install_dir()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=(*_PHASES, "start-day"), help="the phase to enter, or start-day for start-of-day, pre-open and open in turn")
    parser.add_argument("--trading-day", required=True, type=trading_day_argument, metavar="YYYYMMDD", help="the trading day the request is for")
    parser.add_argument("--operator", required=True, help="your name, which each sequencer writes in its Quill log with the request")
    parser.add_argument("--install-dir", type=Path, default=install_default, metavar="PATH",
                        help=f"the install directory, where the deployed configurations, certificates and lib/python are (default: {install_default})")
    parser.add_argument("--timeout", type=float, default=60.0, metavar="SECONDS",
                        help="how long to keep asking while no sequencer leads or the phase is not ready (default: 60)")
    args = parser.parse_args()
    install_dir = args.install_dir.resolve()

    phase_names = ("StartOfDay", "PreOpen", "Open") if args.action == "start-day" else (_PHASES[args.action],)
    result = operator_requests.enter_phases(install_dir, args.trading_day, phase_names, args.operator, timeout=args.timeout)
    if result.answer is None:
        return 2
    return 0 if operator_requests.entered(install_dir, result) else 1

if __name__ == "__main__":
    sys.exit(main())
