#!/usr/bin/env python3
"""Drive the FIX order gateway with load, as binary_load_client drives the binary one.

The venue already has a FIX client -- the Java fix_test_client, which replaced fix8 -- but it
is single-user and single-session, built for interactive and scripted testing rather than for
offering load at a chosen rate. Comparing the two gateways needs something that can, so this
is the FIX counterpart to binary_load_client and deliberately mirrors it: same order shape,
same knobs, same summary.

The order shape matters more than it looks. binary_load_client's own help says a minimal order
is "NOT comparable with a fix8 run, which sends a full order with nested groups", and the same
trap applies here: a FIX order carrying three underlyings compared against a binary order
carrying none measures the groups, not the protocols. Every field and group below is copied
from BinaryLoadClientMain.cpp for that reason, values included.

WHAT THIS DELIBERATELY DOES NOT DO
==================================

This is a load generator, not a FIX engine, and the session layer is cut to the bone on
purpose. Every omission below is a decision, not an oversight:

  * NO RESEND HANDLING. A ResendRequest (35=2) from the gateway is not answered. A client
    that only places, amends and cancels never creates the gap that provokes one, so the
    path would be untested code standing in for a case that does not arise. If the gateway
    ever does send one, this exits loudly rather than limping on -- see fail_loudly().

  * NO PERSISTED SEQUENCE NUMBERS. Every logon sets ResetSeqNumFlag (141=Y) and starts from 1.
    A real client resumes where it left off; this one has no state worth resuming, and
    resetting removes a whole class of "the venue and I disagree about where we are" failure
    that a load run should not be spending its time on.

  * ALMOST NO INBOUND PARSING. Execution Reports are counted, and their ClOrdID is read so
    cancels can name a resting order. Nothing else is examined. The venue's own metrics are
    the measurement; this client's job is to create traffic and notice if it stops working.

  * NO ADMIN MESSAGES BEYOND THE MINIMUM. Heartbeat (35=0) is sent when due and TestRequest
    (35=1) is answered, because a gateway that thinks the session is dead will drop it. Logout
    is sent on exit as a courtesy. Nothing else is implemented.

  * NO TLS. The gateway listens on both a plain port and a TLS port; this uses the plain one.
    Encryption would change nothing about the latency being compared and would add a
    dependency for no return.

The rule for all of it: when reality departs from the narrow path this client understands, say
so and stop. A load client that silently carries on after a protocol surprise produces numbers
that look fine and mean nothing, which is worse than no numbers.

WHERE THIS GATEWAY DIFFERS FROM THE BINARY ONE
==============================================

The cancel this client sends now matches BinaryLoadClientMain.cpp field for field:
ClOrdID, OrigClOrdID, Side, Symbol and TransactTime, with no quantity. That is what FIX
5.0 SP2 asks for -- it marks OrderQtyData optional on an OrderCancelRequest -- and it is
what both gateways accept, so a comparison between the two protocols is comparing the
same message. Pass --cancel-with-order-qty to add a quantity.

The FIX gateway used to require the quantity and, earlier still, dropped a cancel without
one in silence. The silent drop read deceptively: an Info line reported the cancel as
received and only then a Warning discarded it, so repeated failures looked like
alternating success and failure. It was not intermittent -- every cancel was lost. The
gateway now answers a genuinely missing required field with a rejecting ExecutionReport
naming it, and no longer counts the quantity among them. See R-0142 and R-0143 in the
book's applications chapter.

A second difference was a defect and has been fixed. ExecInst "1 G" was rejected as
ValueIsIncorrect because the validator compared the whole field against an enumeration
holding "1" and "G" separately. ExecInst is MULTIPLECHARVALUE -- a space-separated list --
and eight other enumerated fields had the same fault, QuoteCondition and TradeCondition
among them. The dictionary generator now records those types and the validator checks each
element. The default here is "1 G" again, matching the binary client exactly.

Usage:

    python3 scripts/fix_load_client.py --bursts 3 --orders-per-burst 500 --rate 150
    python3 scripts/fix_load_client.py --dry-run          # print one encoded order and stop
"""

import argparse
import random
import socket
import sys
import time

SOH = "\x01"

# Session identity. The gateway's own config is the mirror of this: it sends as GATEWAY to
# CLIENT, so a client sends as CLIENT to GATEWAY.
DEFAULT_SENDER_COMP_ID = "CLIENT"
DEFAULT_TARGET_COMP_ID = "GATEWAY"

# CLIENT authenticates with an empty password: perf_run.py records that fix8's f8test never
# sends tag 554 at all, and the venue's credential for CLIENT was derived to match.
DEFAULT_PASSWORD = ""

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9879          # the plain listener; 9880 is the TLS one

BEGIN_STRING = "FIXT.1.1"
DEFAULT_APPL_VER_ID = "9"    # FIX 5.0 SP2, required in a FIXT.1.1 Logon

# Tags, named so the message builders below read as FIX rather than as arithmetic.
BEGIN_STRING_TAG, BODY_LENGTH, CHECKSUM = 8, 9, 10
MSG_TYPE, SENDER_COMP_ID, TARGET_COMP_ID, MSG_SEQ_NUM, SENDING_TIME = 35, 49, 56, 34, 52
ENCRYPT_METHOD, HEART_BT_INT, RESET_SEQ_NUM_FLAG, TEST_REQ_ID = 98, 108, 141, 112
USERNAME, PASSWORD, DEFAULT_APPL_VER_ID_TAG = 553, 554, 1137
CL_ORD_ID, ORIG_CL_ORD_ID, SYMBOL, SIDE, ORDER_QTY, ORD_TYPE = 11, 41, 55, 54, 38, 40
PRICE, TIME_IN_FORCE, TRANSACT_TIME, TEXT, ACCOUNT = 44, 59, 60, 58, 1
SECURITY_ID, SECURITY_ID_SOURCE, EX_DESTINATION = 48, 22, 100
EXEC_INST, MIN_QTY, MAX_FLOOR = 18, 110, 111
NO_UNDERLYINGS, UNDERLYING_SYMBOL, UNDERLYING_SECURITY_ID, UNDERLYING_QTY = 711, 311, 309, 879
NO_PARTY_IDS, PARTY_ID, PARTY_ID_SOURCE, PARTY_ROLE = 453, 448, 447, 452
NO_PARTY_SUB_IDS, PARTY_SUB_ID, PARTY_SUB_ID_TYPE = 802, 523, 803

MSG_LOGON, MSG_LOGOUT, MSG_HEARTBEAT, MSG_TEST_REQUEST = "A", "5", "0", "1"
MSG_RESEND_REQUEST, MSG_REJECT, MSG_EXECUTION_REPORT = "2", "3", "8"
MSG_NEW_ORDER_SINGLE, MSG_ORDER_CANCEL_REQUEST = "D", "F"


def fail_loudly(reason):
    """Stop, naming what happened. See the module docstring on why this is not recoverable."""
    raise SystemExit(f"fix_load_client: {reason}\n"
                     f"  This client implements only the narrow path a load run needs. It stops\n"
                     f"  rather than continue, because traffic that carries on after a protocol\n"
                     f"  surprise produces numbers that look fine and mean nothing.")


def utc_timestamp():
    """FIX UTCTimestamp with milliseconds, which is what the gateway parses."""
    now = time.time()
    stamp = time.strftime("%Y%m%d-%H:%M:%S", time.gmtime(now))
    return f"{stamp}.{int((now % 1) * 1000):03d}"


def encode(fields):
    """Wrap ordered (tag, value) pairs in BeginString, BodyLength and CheckSum.

    BodyLength counts everything after the BodyLength field up to and including the SOH before
    CheckSum; CheckSum is the sum of those bytes modulo 256, three digits. Both are computed
    here rather than trusted from a caller, because a wrong one is rejected by the gateway
    with a message that does not obviously point at arithmetic.
    """
    body = "".join(f"{tag}={value}{SOH}" for tag, value in fields)
    head = f"{BEGIN_STRING_TAG}={BEGIN_STRING}{SOH}{BODY_LENGTH}={len(body)}{SOH}"
    checksum = sum((head + body).encode("ascii")) % 256
    return f"{head}{body}{CHECKSUM}={checksum:03d}{SOH}".encode("ascii")


class FixSession:
    """One logged-on FIX session: encodes, sends, and counts what comes back."""

    def __init__(self, options, comp_id):
        self.options = options
        self.comp_id = comp_id
        self.socket = None
        self.out_seq = 1
        self.orders_sent = 0
        self.cancels_sent = 0
        self.reports_received = 0
        self.resting = []
        self.inbound = b""
        self.last_sent = 0.0
        self.latencies = []

    # ── session ────────────────────────────────────────────────────────────────
    def header(self, msg_type):
        fields = [(MSG_TYPE, msg_type), (SENDER_COMP_ID, self.comp_id),
                  (TARGET_COMP_ID, self.options.target_comp_id),
                  (MSG_SEQ_NUM, self.out_seq), (SENDING_TIME, utc_timestamp())]
        self.out_seq += 1
        return fields

    def send(self, fields):
        wire = encode(fields)
        if getattr(self.options, "trace", False):
            print(f"  OUT {wire.decode('ascii').replace(SOH, '|')}")
        self.socket.sendall(wire)
        self.last_sent = time.monotonic()

    def connect_and_logon(self):
        self.socket = socket.create_connection((self.options.host, self.options.port), timeout=10.0)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        logon = self.header(MSG_LOGON) + [
            (ENCRYPT_METHOD, 0),
            (HEART_BT_INT, self.options.heartbeat),
            # Every logon starts the numbering again; see the docstring on persisted sequence
            # numbers. Without this the venue remembers a session this client cannot resume.
            (RESET_SEQ_NUM_FLAG, "Y"),
            (USERNAME, self.comp_id),
            (PASSWORD, self.options.password),
            (DEFAULT_APPL_VER_ID_TAG, DEFAULT_APPL_VER_ID),
        ]
        self.send(logon)
        deadline = time.monotonic() + self.options.logon_timeout
        while time.monotonic() < deadline:
            for msg_type, message in self.read_available(0.5):
                if msg_type == MSG_LOGON:
                    return True
                if msg_type == MSG_LOGOUT:
                    fail_loudly(f"{self.comp_id}: gateway refused the logon -- {field(message, TEXT) or 'no reason given'}")
                if msg_type == MSG_REJECT:
                    fail_loudly(f"{self.comp_id}: gateway rejected the Logon -- {field(message, TEXT) or 'no reason given'}")
        fail_loudly(f"{self.comp_id}: no Logon response within {self.options.logon_timeout}s")
        return False

    def logout(self):
        try:
            self.send(self.header(MSG_LOGOUT))
        except OSError:
            pass
        finally:
            if self.socket is not None:
                self.socket.close()

    # ── inbound ────────────────────────────────────────────────────────────────
    def read_available(self, timeout):
        """Yield (msg_type, raw) for whatever has arrived. Answers admin messages itself."""
        self.socket.settimeout(timeout)
        try:
            chunk = self.socket.recv(65536)
        except socket.timeout:
            return
        except BlockingIOError:
            # A zero timeout puts the socket in non-blocking mode, where "nothing has arrived
            # yet" surfaces as EAGAIN rather than as a timeout. That is the common case on the
            # polling call inside the send loop, and it is emphatically not a lost connection.
            return
        except OSError as error:
            fail_loudly(f"{self.comp_id}: connection lost -- {error}")
        if not chunk:
            fail_loudly(f"{self.comp_id}: gateway closed the connection")
        self.inbound += chunk
        while True:
            end = self.inbound.find(f"{SOH}{CHECKSUM}=".encode("ascii"))
            if end == -1:
                break
            end = self.inbound.find(SOH.encode("ascii"), end + 1)
            if end == -1:
                break
            message = self.inbound[:end + 1].decode("ascii", errors="replace")
            self.inbound = self.inbound[end + 1:]
            msg_type = field(message, MSG_TYPE)
            if msg_type == MSG_TEST_REQUEST:
                self.send(self.header(MSG_HEARTBEAT) + [(TEST_REQ_ID, field(message, TEST_REQ_ID) or "")])
                continue
            if msg_type == MSG_HEARTBEAT:
                continue
            if msg_type == MSG_RESEND_REQUEST:
                fail_loudly(f"{self.comp_id}: gateway sent a ResendRequest, which this client does not implement")
            if msg_type == MSG_EXECUTION_REPORT:
                self.reports_received += 1
                cl_ord_id = field(message, CL_ORD_ID)
                if cl_ord_id and not cl_ord_id.endswith("-CANCEL"):
                    self.resting.append(cl_ord_id)
            yield msg_type, message

    def heartbeat_if_due(self):
        if time.monotonic() - self.last_sent >= self.options.heartbeat / 2:
            self.send(self.header(MSG_HEARTBEAT))

    # ── orders ─────────────────────────────────────────────────────────────────
    def order_fields(self, cl_ord_id):
        """A NewOrderSingle whose shape matches binary_load_client's exactly.

        The values are constant across orders on purpose, as they are there: what is being
        measured is the cost of encoding, carrying and decoding the fields, which does not
        depend on what they say.
        """
        fields = self.header(MSG_NEW_ORDER_SINGLE) + [
            (CL_ORD_ID, cl_ord_id),
            (SIDE, "1"),                      # Buy
            (SYMBOL, self.options.symbol),
            (ORD_TYPE, "2"),                  # Limit
            (TRANSACT_TIME, utc_timestamp()),
            (ORDER_QTY, "100"),
        ]
        if self.options.minimal_order:
            return fields
        fields += [
            (PRICE, "100.00"),
            (SECURITY_ID, "GB00B03MLX29"),
            (SECURITY_ID_SOURCE, "4"),
            (TIME_IN_FORCE, "0"),             # Day
            (ACCOUNT, "ACCT-001"),
            (EX_DESTINATION, "XLON"),
            # "1 G" is what binary_load_client sends, so the two protocols carry identical
            # orders. ExecInst is MULTIPLECHARVALUE: a space-separated list, here Not-held AND
            # All-or-none. The FIX validator used to reject this, comparing the whole field
            # against an enumeration holding the elements separately; fixed in fix_codec.
            *([(EXEC_INST, self.options.exec_inst)] if self.options.exec_inst else []),
            (MIN_QTY, "10"),
            (MAX_FLOOR, "50"),
            (TEXT, "fix_load_client"),
        ]
        if self.options.underlyings:
            fields.append((NO_UNDERLYINGS, self.options.underlyings))
            for _ in range(self.options.underlyings):
                fields += [(UNDERLYING_SYMBOL, "UND-SYM"),
                           (UNDERLYING_SECURITY_ID, "UND-SECID"),
                           (UNDERLYING_QTY, "50")]
        if self.options.parties:
            fields.append((NO_PARTY_IDS, self.options.parties))
            for _ in range(self.options.parties):
                fields += [(PARTY_ID, "PARTY-001"),
                           (PARTY_ID_SOURCE, "D"),     # Proprietary
                           (PARTY_ROLE, 1)]            # ExecutingFirm
                if self.options.party_sub_ids:
                    fields.append((NO_PARTY_SUB_IDS, self.options.party_sub_ids))
                    for _ in range(self.options.party_sub_ids):
                        fields += [(PARTY_SUB_ID, "SUB-ID"),
                                   (PARTY_SUB_ID_TYPE, 5)]   # Firm
        return fields

    def send_order(self, cl_ord_id):
        self.send(self.order_fields(cl_ord_id))
        self.orders_sent += 1

    def send_cancel(self, target):
        fields = self.header(MSG_ORDER_CANCEL_REQUEST) + [
            (CL_ORD_ID, f"{target}-CANCEL"),
            (ORIG_CL_ORD_ID, target),
            (SIDE, "1"),
            (SYMBOL, self.options.symbol),
        ]
        # No quantity by default. FIX 5.0 SP2 marks OrderQtyData optional on a cancel and
        # BinaryLoadClientMain.cpp sends none, so omitting it is both correct and what makes
        # the two load clients send the same message for the gateway comparison.
        if self.options.cancel_with_order_qty:
            fields.append((ORDER_QTY, "100"))
        fields.append((TRANSACT_TIME, utc_timestamp()))
        self.send(fields)
        self.cancels_sent += 1


def field(message, tag):
    """The value of tag in a raw FIX message, or None."""
    needle = f"{SOH}{tag}="
    start = message.find(needle)
    if start == -1:
        if not message.startswith(f"{tag}="):
            return None
        start, needle = 0, f"{tag}="
    start += len(needle)
    end = message.find(SOH, start)
    return message[start:end] if end != -1 else message[start:]


def run(options):
    session = FixSession(options, options.comp_id)
    print(f"connecting to {options.host}:{options.port} as {options.comp_id}")
    session.connect_and_logon()
    print(f"  logged on, {options.bursts} burst(s) of {options.orders_per_burst} at "
          f"{options.rate or 'unthrottled'} orders/s")

    gap = 1.0 / options.rate if options.rate else 0.0
    next_id = options.first_cl_ord_id
    started = time.monotonic()

    # With --bursts 0 the session is driven from stdin instead: one burst per "T" line, which
    # is binary_load_client's contract and f8test's before it. That is what lets a caller hold
    # ONE session open for a whole run and vary the rate by how often it writes a T -- rather
    # than starting a fresh client per phase, where each logout triggers cancel-on-disconnect
    # for every order still resting and dumps thousands of cancels on the matching engine at
    # once. Those storms are large enough to show as multi-hundred-millisecond spikes in the
    # round trip of BOTH gateways, because they share the engine.
    bursts = iter(lambda: sys.stdin.readline(), "") if options.bursts == 0 \
        else iter(range(options.bursts).__iter__().__next__, None)

    for burst_index, _ in enumerate(bursts, start=1):
        burst_started = time.monotonic()
        for _ in range(options.orders_per_burst):
            due = time.monotonic() + gap
            session.send_order(f"{options.comp_id}-{next_id}")
            next_id += 1
            if options.cancel_ratio and session.resting and random.random() < options.cancel_ratio:
                session.send_cancel(session.resting.pop(0))
            list(session.read_available(0.0))
            session.heartbeat_if_due()
            while gap and time.monotonic() < due:
                time.sleep(min(0.001, max(0.0, due - time.monotonic())))
        if options.trace or options.bursts:
            print(f"  burst {burst_index}: {options.orders_per_burst} order(s) in "
                  f"{time.monotonic() - burst_started:.3f}s", flush=True)

    # Let the reports catch up before reporting, or the count understates by whatever is
    # still in flight when the last order was sent.
    drain_until = time.monotonic() + options.drain
    while time.monotonic() < drain_until:
        list(session.read_available(0.2))

    elapsed = time.monotonic() - started
    print(f"\n=== fix_load_client summary ===")
    print(f"  comp id        {options.comp_id}")
    print(f"  order shape    {'minimal' if options.minimal_order else 'full'}: "
          f"{options.underlyings} underlying(s), {options.parties} party/parties "
          f"x {options.party_sub_ids} sub-id(s)")
    print(f"  orders sent    {session.orders_sent}")
    print(f"  cancels sent   {session.cancels_sent}")
    print(f"  reports recvd  {session.reports_received}")
    print(f"  throughput     {session.orders_sent / elapsed:.0f} orders/s over {elapsed:.3f}s")
    expected = session.orders_sent + session.cancels_sent
    verdict = "PASS -- every message earned a report" if session.reports_received >= expected \
        else f"INCOMPLETE -- {expected - session.reports_received} message(s) unreported"
    print(f"  RESULT         {verdict}")
    session.logout()
    return 0 if session.reports_received >= expected else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"plain FIX listener (default: {DEFAULT_PORT}); the TLS one is not used")
    parser.add_argument("--comp-id", default=DEFAULT_SENDER_COMP_ID, help="SenderCompID to log on with")
    parser.add_argument("--target-comp-id", default=DEFAULT_TARGET_COMP_ID)
    parser.add_argument("--password", default=DEFAULT_PASSWORD, help="tag 554; empty for CLIENT")
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--orders-per-burst", type=int, default=1000)
    parser.add_argument("--bursts", type=int, default=1,
                        help="fire N bursts then stop; 0 takes one burst per \"T\" line on stdin, which is "
                             "binary_load_client's contract and is how one session is held open across a "
                             "whole run instead of one session per phase")
    parser.add_argument("--rate", type=int, default=0,
                        help="orders per second. Omit for a throughput test: orders go out as fast as "
                             "the socket accepts them, which offers load far faster than the pipeline "
                             "drains, so the reported latencies are dominated by queueing. Set a "
                             "sustainable rate to measure service latency")
    parser.add_argument("--first-cl-ord-id", type=int, default=None,
                        help="starting ClOrdID; defaults to something derived from the clock, because a "
                             "ClOrdID the matching engine has seen before is rejected as a duplicate")
    parser.add_argument("--cancel-with-order-qty", action="store_true",
                        help="add OrderQty to cancels; omitted by default, matching the binary load client")
    parser.add_argument("--cancel-ratio", type=float, default=0.0,
                        help="fraction of orders also cancelled, so the book does not grow without bound")
    parser.add_argument("--underlyings", type=int, default=3, help="NoUnderlyings instances (default 3)")
    parser.add_argument("--parties", type=int, default=1, help="NoPartyIDs instances (default 1)")
    parser.add_argument("--party-sub-ids", type=int, default=1, help="NoPartySubIDs per party (default 1)")
    parser.add_argument("--exec-inst", default="1 G",
                        help="tag 18, a MULTIPLECHARVALUE: a space-separated list of values. The default "
                             "matches binary_load_client so both protocols carry identical orders. Pass an "
                             "empty string to omit the field entirely")
    parser.add_argument("--minimal-order", action="store_true",
                        help="send only the required fields and no groups. Useful to isolate per-field "
                             "cost, but NOT comparable with a binary_load_client run, which sends a full "
                             "order with nested groups")
    parser.add_argument("--heartbeat", type=int, default=30, help="HeartBtInt in seconds")
    parser.add_argument("--logon-timeout", type=float, default=10.0)
    parser.add_argument("--drain", type=float, default=2.0, help="seconds to keep reading reports after the last order")
    parser.add_argument("--trace", action="store_true", help="print every message sent, for debugging")
    parser.add_argument("--dry-run", action="store_true", help="print one encoded order and exit")
    options = parser.parse_args(argv)

    if options.first_cl_ord_id is None:
        options.first_cl_ord_id = int(time.time()) * 1000

    if options.dry_run:
        session = FixSession(options, options.comp_id)
        wire = encode(session.order_fields(f"{options.comp_id}-{options.first_cl_ord_id}"))
        print(wire.decode("ascii").replace(SOH, "|"))
        print(f"\n  {len(wire)} bytes, {wire.decode('ascii').count(SOH)} fields")
        return 0

    return run(options)


if __name__ == "__main__":
    sys.exit(main())
