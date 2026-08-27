#!/usr/bin/env python3
"""
fix_raw_client.py — a FIX client that will send whatever it is told to, including nonsense.

Why this exists, when the project already drives fix8
-----------------------------------------------------
f8test is an engine. It is trying to be correct: it always writes a valid MsgSeqNum, it never
sends a number below its own expected without marking it, and it will not emit a malformed body.
That is exactly what makes it useful for the conforming flows, and exactly why it cannot test what
the venue does about a member that is WRONG.

Half the rules in docs/fix/inbound_sequence_checking.md are about members behaving badly:

  * a message whose MsgSeqNum cannot be read at all -- Reject, and do not advance the counter;
  * a number below what the venue expects with no PossDupFlag -- a serious error, ending the
    session;
  * a number above it -- a gap, which the venue must ask about rather than process past.

None of those can be produced by a client that refuses to misbehave. So this one has no session
layer at all: it builds the bytes it is asked for, computes BodyLength and CheckSum unless told to
get them wrong, and reads whatever comes back. It keeps no expected-receive number and will never
send a ResendRequest of its own accord, because a test that has to negotiate with its own client's
opinions is a test of the client.

**It is not a replacement for f8test**, which stays for anything that needs volume, a real session
layer, or a second implementation's view of the venue's bytes.

Authentication is a plaintext password on tag 554 of the Logon, and an empty password means the
field is simply absent -- the SCRAM exchange happens between the gateway and the authentication
service, not between the member and the gateway, so there is no crypto here.

Usage:
    ./fix_raw_client.py --port 9879 --comp-id CLIENT       # logon, one order, print what returns
    ./fix_raw_client.py --port 9879 --logon-seq 500        # logon claiming to be at 500
"""

from __future__ import annotations

import argparse
import datetime
import socket
import sys

SOH = "\x01"

# Tags this module names rather than spells out at each use. Anything else is passed through as a
# number, deliberately: a client that only knows the tags someone thought of in advance cannot be
# asked to send the one nobody thought of.
BEGIN_STRING = 8
BODY_LENGTH = 9
MSG_TYPE = 35
SENDER_COMP_ID = 49
TARGET_COMP_ID = 56
MSG_SEQ_NUM = 34
SENDING_TIME = 52
POSS_DUP_FLAG = 43
ORIG_SENDING_TIME = 122
CHECK_SUM = 10


def utc_timestamp() -> str:
    """FIX UTCTimestamp with milliseconds, as the venue writes them."""
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]


def _body_length(body: str) -> int:
    return len(body.encode("latin-1"))


def _check_sum(text: str) -> str:
    return f"{sum(text.encode('latin-1')) % 256:03d}"


def build_message(fields: list[tuple[int, str]], begin_string: str = "FIXT.1.1",
                  bad_body_length: bool = False, bad_check_sum: bool = False) -> bytes:
    """Frame an ordered list of (tag, value) as FIX wire bytes.

    fields is everything between BodyLength and CheckSum, in the order given -- including the
    header fields, because a test that needs them out of order or missing has to be able to say so.

    bad_body_length and bad_check_sum corrupt the framing deliberately, which is the only way to
    see what the venue does with a message it cannot trust the shape of.
    """
    body = "".join(f"{tag}={value}{SOH}" for tag, value in fields)
    length = _body_length(body) + (1 if bad_body_length else 0)
    head = f"{BEGIN_STRING}={begin_string}{SOH}{BODY_LENGTH}={length}{SOH}"
    without_checksum = head + body
    checksum = _check_sum(without_checksum)
    if bad_check_sum:
        checksum = f"{(int(checksum) + 1) % 256:03d}"
    return (without_checksum + f"{CHECK_SUM}={checksum}{SOH}").encode("latin-1")


def parse_messages(buffer: bytes) -> tuple[list[dict[int, str]], bytes]:
    """Split a byte buffer into complete messages, returning them and the unconsumed remainder.

    Messages are found by their CheckSum terminator rather than by BodyLength, so a message whose
    length field this client corrupted on purpose is still readable back.
    """
    messages: list[dict[int, str]] = []
    text = buffer.decode("latin-1")
    while True:
        end = text.find(f"{SOH}{CHECK_SUM}=")
        if end == -1:
            break
        # end is the SOH before "10="; the message ends at the SOH that closes the checksum.
        terminator = text.find(SOH, end + 1)
        if terminator == -1:
            break
        raw = text[: terminator + 1]
        fields: dict[int, str] = {}
        for field in raw.split(SOH):
            if not field or "=" not in field:
                continue
            tag, _, value = field.partition("=")
            if tag.isdigit():
                fields.setdefault(int(tag), value)  # first wins: repeating groups are not parsed
        messages.append(fields)
        text = text[terminator + 1 :]
    return messages, text.encode("latin-1")


class FixRawClient:
    """A socket that speaks FIX only as far as it is told to."""

    def __init__(self, host: str, port: int, sender_comp_id: str, target_comp_id: str,
                 password: str = "") -> None:
        self.host = host
        self.port = port
        self.sender_comp_id = sender_comp_id
        self.target_comp_id = target_comp_id
        self.password = password
        self.sock: socket.socket | None = None
        self.buffer = b""
        # What this client will put on the next message unless told otherwise. It is NOT an
        # expected-receive counter: nothing here reacts to what the venue sends.
        self.next_send_seq_num = 1

    # -- connection ------------------------------------------------------------------------

    def connect(self, timeout: float = 5.0) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.sock.settimeout(timeout)

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self) -> "FixRawClient":
        self.connect()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    # -- sending ---------------------------------------------------------------------------

    def send(self, msg_type: str, body: list[tuple[int, str]] | None = None, *,
             seq_num: int | None = None, omit_seq_num: bool = False, poss_dup: bool = False,
             orig_sending_time: str | None = None, sending_time: str | None = None,
             bad_body_length: bool = False, bad_check_sum: bool = False,
             advance: bool = True) -> bytes:
        """Send one message, with every part of the header available to be got wrong.

        seq_num overrides the running number without disturbing it; omit_seq_num leaves tag 34 out
        entirely, which is a message the venue cannot place at all. poss_dup and orig_sending_time
        are separable on purpose: PossDupFlag without OrigSendingTime is itself a conformance
        failure worth being able to send.
        """
        if self.sock is None:
            raise RuntimeError("not connected")

        number = self.next_send_seq_num if seq_num is None else seq_num
        fields: list[tuple[int, str]] = [(MSG_TYPE, msg_type),
                                         (SENDER_COMP_ID, self.sender_comp_id),
                                         (TARGET_COMP_ID, self.target_comp_id)]
        if not omit_seq_num:
            fields.append((MSG_SEQ_NUM, str(number)))
        if poss_dup:
            fields.append((POSS_DUP_FLAG, "Y"))
        fields.append((SENDING_TIME, sending_time or utc_timestamp()))
        if orig_sending_time is not None:
            fields.append((ORIG_SENDING_TIME, orig_sending_time))
        fields.extend(body or [])

        wire = build_message(fields, bad_body_length=bad_body_length, bad_check_sum=bad_check_sum)
        self.sock.sendall(wire)
        if advance and seq_num is None and not omit_seq_num:
            self.next_send_seq_num = number + 1
        elif advance and seq_num is not None:
            self.next_send_seq_num = number + 1
        return wire

    def logon(self, *, seq_num: int | None = None, reset_seq_num: bool = False,
              heartbeat_interval: int = 30) -> bytes:
        """Send a Logon. An empty password sends no tag 554 at all, which is what the venue's own
        test credentials use."""
        body: list[tuple[int, str]] = [(98, "0"), (108, str(heartbeat_interval))]
        if reset_seq_num:
            body.append((141, "Y"))
        body.append((1137, "8"))
        if self.password:
            body.append((554, self.password))
        return self.send("A", body, seq_num=seq_num)

    def new_order_single(self, cl_ord_id: str, *, symbol: str = "BHP", side: str = "1",
                         quantity: str = "100", price: str = "10.0", ord_type: str = "2",
                         time_in_force: str = "0", **send_args: object) -> bytes:
        """A minimal NewOrderSingle. Deliberately without the party and underlying groups the
        fix8 sample client pads its orders with -- the venue does not require them, and a test
        client that sends more than it must cannot show which fields matter."""
        body = [(11, cl_ord_id), (55, symbol), (54, side), (60, utc_timestamp()),
                (38, quantity), (40, ord_type), (44, price), (59, time_in_force)]
        return self.send("D", body, **send_args)  # type: ignore[arg-type]

    # -- receiving -------------------------------------------------------------------------

    def receive(self, timeout: float = 2.0) -> list[dict[int, str]]:
        """Read whatever has arrived, parsed. Returns an empty list on timeout rather than
        raising: a test that expects silence needs silence to be an ordinary outcome."""
        if self.sock is None:
            raise RuntimeError("not connected")
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(65536)
        except (socket.timeout, TimeoutError):
            return []
        if not chunk:
            return []
        self.buffer += chunk
        messages, self.buffer = parse_messages(self.buffer)
        return messages

    def receive_until(self, msg_type: str, timeout: float = 5.0) -> dict[int, str] | None:
        """Read until a message of this type arrives, or the timeout expires."""
        import time as _time
        deadline = _time.time() + timeout
        while _time.time() < deadline:
            for message in self.receive(timeout=min(0.5, max(0.05, deadline - _time.time()))):
                if message.get(MSG_TYPE) == msg_type:
                    return message
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9879)
    parser.add_argument("--comp-id", default="CLIENT")
    parser.add_argument("--target-comp-id", default="GATEWAY")
    parser.add_argument("--password", default="")
    parser.add_argument("--logon-seq", type=int, default=None,
                        help="MsgSeqNum to put on the Logon (default: 1)")
    parser.add_argument("--reset-seq-num", action="store_true",
                        help="set ResetSeqNumFlag=Y on the Logon")
    parser.add_argument("--orders", type=int, default=1, help="orders to send after logon")
    args = parser.parse_args()

    client = FixRawClient(args.host, args.port, args.comp_id, args.target_comp_id, args.password)
    client.connect()
    try:
        client.logon(seq_num=args.logon_seq, reset_seq_num=args.reset_seq_num)
        reply = client.receive_until("A", timeout=10.0)
        if reply is None:
            print("no Logon reply", file=sys.stderr)
            return 1
        print(f"logon accepted: MsgSeqNum={reply.get(MSG_SEQ_NUM)}")

        for index in range(args.orders):
            client.new_order_single(f"raw{index + 1}")
        for _ in range(args.orders):
            report = client.receive_until("8", timeout=10.0)
            if report is None:
                print("no ExecutionReport", file=sys.stderr)
                return 1
            print(f"execution report: MsgSeqNum={report.get(MSG_SEQ_NUM)} "
                  f"ClOrdID={report.get(11)} OrdStatus={report.get(39)}")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
