#!/usr/bin/env python3
"""
fix_session_soak.py -- hours of untidy FIX sessions against one running FIX gateway, checking after every round that a
new member can still log on.

What it is for
--------------
BUG-0056: a FIX gateway once went on accepting TCP connections, and passing none of them to its application thread,
after a long stretch of ad-hoc sessions, many of which ended badly. Short bursts of the same sessions did not bring it
back. This runs the same kinds of session for as long as it is asked to, at varying pace with idle stretches between,
because what differed on the day was a venue that had been up for a long time.

Each round runs a few sessions chosen at random from these kinds, by the member CLIENT:

  clean            logon, one order, logout
  abrupt_close     logon, one order, close the socket with no logout (cancel on disconnect starts its grace period)
  reset            logon, then reset the connection (RST, not FIN)
  low_sequence     logon, then a message numbered below what the venue expects (the venue ends the session)
  gap              logon, then a message numbered above it, and no answer to the venue's ResendRequest
  silent           connect and send nothing until the venue gives up on the logon
  garbage          connect and send bytes that are not FIX
  abandon          logon and keep the socket open, unread, until a later round closes it

Then the probe: the member SOAK-PROBE, used for nothing else, opens a new connection and logs on, and must be answered
with a Logon. After the probe the script records the gateway's open descriptors, its threads and its resident memory,
so that something that grows with time is seen growing, not only when it runs out.

If the probe is not answered it is tried once more after 30 seconds, so that a pause of the machine is not taken for
the fault. If that fails too, the gateway's Quill log and the rest of installed/log are copied aside at once, before
anything else can overwrite them, and the run stops.

The venue must already be running, started by scripts/devenv.py, and the credentials of CLIENT and SOAK-PROBE must
already hold empty passwords: --prepare-credentials writes them, and must be run before the authentication service is
started.

Usage:
    ./scripts/fix_session_soak.py --prepare-credentials
    ./scripts/fix_session_soak.py --hours 3 --seed 1
"""

from __future__ import annotations

import argparse
import datetime
import os
import random
import shutil
import socket
import struct
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MEMBER = "CLIENT"
PROBE_MEMBER = "SOAK-PROBE"
SESSION_KINDS = ("clean", "abrupt_close", "reset", "low_sequence", "gap", "silent", "garbage", "abandon")


def log(text: str) -> None:
    """Print a line to standard output with the time of day in front of it."""
    print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] {text}", flush=True)


def gateway_pid(prefix: Path) -> int | None:
    """The process ID of fix_order_gateway_a, from its PID file, or None if it is not running."""
    for candidate in (prefix / "run" / "fix_order_gateway_a.pid", prefix / "var" / "run" / "fix_order_gateway_a.pid"):
        if candidate.is_file():
            try:
                pid = int(candidate.read_text().strip())
            except ValueError:
                continue
            if Path(f"/proc/{pid}").is_dir():
                return pid
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                command = (entry / "cmdline").read_bytes().split(b"\0")
            except OSError:
                continue
            if command and command[0].endswith(b"fix_order_gateway") and any(b"fix_order_gateway_a" in part for part in command):
                return int(entry.name)
    return None


def gateway_health(pid: int) -> str:
    """Open descriptors, resident memory and thread names of the gateway process."""
    try:
        descriptors = len(os.listdir(f"/proc/{pid}/fd"))
        with open(f"/proc/{pid}/status", encoding="ascii") as status:
            resident = next((line.split()[1] for line in status if line.startswith("VmRSS:")), "?")
        names = []
        for task in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{task}/comm", encoding="ascii") as comm:
                names.append(comm.read().strip())
        return f"descriptors {descriptors}, resident {resident} kB, threads {len(names)} ({', '.join(sorted(names))})"
    except OSError as error:
        return f"cannot be read: {error}"


class Soak:
    """One soak run: the sessions, the probe, and what is recorded."""

    def __init__(self, port: int, prefix: Path, seed: int) -> None:
        from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel
        self.client_class = FixRawClient
        self.port = port
        self.prefix = prefix
        self.random = random.Random(seed)
        self.abandoned: list = []
        self.order_number = 0

    def member(self, comp_id: str = MEMBER):
        """A new, unconnected client for the given comp id, with an empty password."""
        return self.client_class("127.0.0.1", self.port, comp_id, "GATEWAY", "")

    def next_order(self) -> str:
        """A ClOrdID not used before in this run."""
        self.order_number += 1
        return f"soak{self.order_number}"

    def run_session(self, kind: str) -> None:
        """Run one session of the given kind. Whatever the venue does with it is right; only the probe is checked."""
        client = self.member()
        try:
            client.connect(timeout=5.0)
            if kind == "silent":
                time.sleep(self.random.uniform(1.0, 35.0))
                return
            if kind == "garbage":
                client.sock.sendall(os.urandom(self.random.randint(1, 400)))
                time.sleep(1.0)
                return
            client.logon(reset_seq_num=True)
            client.receive_until("A", "5", timeout=5.0)
            if kind == "clean":
                client.new_order_single(self.next_order())
                client.receive(timeout=1.0)
                client.send("5")
                client.receive_until("5", timeout=3.0)
            elif kind == "abrupt_close":
                client.new_order_single(self.next_order())
                client.receive(timeout=0.5)
            elif kind == "reset":
                client.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            elif kind == "low_sequence":
                client.new_order_single(self.next_order())
                client.receive(timeout=0.5)
                client.new_order_single(self.next_order(), seq_num=1)
                client.receive(timeout=2.0)
            elif kind == "gap":
                client.new_order_single(self.next_order(), seq_num=50)
                client.receive(timeout=self.random.uniform(1.0, 15.0))
            elif kind == "abandon":
                self.abandoned.append(client)
                client = None
        except (OSError, ValueError) as error:
            log(f"    {kind}: {type(error).__name__}: {error}")
        finally:
            if client is not None:
                client.close()

    def close_some_abandoned(self) -> None:
        """Close a random part of the sockets left open, and keep the rest for later rounds."""
        keep = []
        for client in self.abandoned:
            if self.random.random() < 0.3:
                keep.append(client)
            else:
                client.close()
        self.abandoned = keep

    def probe(self) -> tuple[bool, str]:
        """Whether a new member is answered with a Logon, and what happened."""
        started = time.monotonic()
        client = self.member(PROBE_MEMBER)
        try:
            client.connect(timeout=5.0)
            client.logon(reset_seq_num=True)
            answer = client.receive_until("A", "5", timeout=10.0)
            elapsed = time.monotonic() - started
            if answer is None:
                return False, f"connected, but no answer to the Logon within 10 s ({elapsed:.1f} s)"
            if answer.get(35) != "A":
                return False, f"answered with MsgType {answer.get(35)} rather than a Logon: {answer.get(58, '')}"
            client.send("5")
            client.receive_until("5", timeout=3.0)
            return True, f"logged on in {elapsed:.2f} s"
        except (OSError, ValueError) as error:
            return False, f"{type(error).__name__}: {error}"
        finally:
            client.close()

    def preserve_logs(self) -> Path:
        """Copy installed/log to a new directory under tmp/, named by the time, and return that directory."""
        destination = PROJECT_ROOT / "tmp" / f"fix_session_soak_failure_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}"
        shutil.copytree(self.prefix / "log", destination / "log")
        return destination


def prepare_credentials(prefix: Path) -> None:
    """Write empty-password credentials for CLIENT and SOAK-PROBE into the installation's credentials.toml."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from ha_test import ensure_fix8_credentials  # pylint: disable=import-outside-toplevel
    credentials = prefix / "etc" / "authentication_service" / "credentials.toml"
    for comp_id in (MEMBER, PROBE_MEMBER):
        ensure_fix8_credentials(credentials, comp_id, "")


def main() -> int:
    """Run the soak; 0 if every probe was answered, 1 if one failed twice, 2 if the run could not start."""
    parser = argparse.ArgumentParser(description="Hours of untidy FIX sessions against fix_order_gateway_a, with a logon probe after every round.")
    parser.add_argument("--prefix", default=str(PROJECT_ROOT / "installed"), help="the installation (default: installed)")
    parser.add_argument("--port", type=int, default=9879, help="fix_order_gateway_a's client port (default: 9879)")
    parser.add_argument("--hours", type=float, default=3.0, help="how long to run (default: 3)")
    parser.add_argument("--seed", type=int, default=1, help="seed for the choice of sessions and pauses, so that a run can be repeated")
    parser.add_argument("--prepare-credentials", action="store_true",
                        help="write empty-password credentials for CLIENT and SOAK-PROBE, then stop; run before the authentication service starts")
    args = parser.parse_args()
    prefix = Path(args.prefix)

    if args.prepare_credentials:
        prepare_credentials(prefix)
        return 0

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    soak = Soak(args.port, prefix, args.seed)
    pid = gateway_pid(prefix)
    if pid is None:
        log("fix_order_gateway_a is not running; start the venue with scripts/devenv.py first")
        return 2
    log(f"soak of fix_order_gateway_a, PID {pid}, port {args.port}, for {args.hours} h, seed {args.seed}")
    log(f"  at the start: {gateway_health(pid)}")
    ok, said = soak.probe()
    log(f"  probe before any session: {said}")
    if not ok:
        log("  the probe fails before anything has been done: check the venue and the credentials (--prepare-credentials)")
        return 2

    deadline = time.monotonic() + args.hours * 3600
    round_number = 0
    while time.monotonic() < deadline:
        round_number += 1
        kinds = [soak.random.choice(SESSION_KINDS) for _ in range(soak.random.randint(1, 8))]
        for kind in kinds:
            soak.run_session(kind)
            time.sleep(soak.random.choice((0.0, 0.0, 0.1, 0.5, 2.0)))
        soak.close_some_abandoned()
        ok, said = soak.probe()
        if not ok:
            log(f"round {round_number}: probe FAILED: {said}; trying again in 30 s")
            time.sleep(30)
            ok, said_again = soak.probe()
            if not ok:
                kept = soak.preserve_logs()
                log(f"round {round_number}: probe FAILED again: {said_again}")
                log(f"  gateway: {gateway_health(pid)}")
                log(f"  installed/log copied to {kept}")
                return 1
            log(f"round {round_number}: the second probe succeeded: {said_again}")
        log(f"round {round_number}: {', '.join(kinds)}; {len(soak.abandoned)} left open; probe {said}; {gateway_health(pid)}")
        # Mostly short pauses, sometimes several idle minutes: the fault followed a long-running venue, not a busy one.
        pause = soak.random.choice((1.0, 3.0, 10.0, 30.0, 60.0, 180.0, 300.0))
        time.sleep(min(pause, max(0.0, deadline - time.monotonic())))

    for client in soak.abandoned:
        client.close()
    log(f"finished after {round_number} rounds; every probe answered; gateway: {gateway_health(pid)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
