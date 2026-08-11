#!/usr/bin/env python3
"""
fix_client_burst_test.py -- high-volume burst test with WAL replication active.

Companion to fix_client_smoke_test.py (item 15): the smoke test verifies
correctness at moderate load; this test verifies the sequencer's WAL-replication
path holds up under a burst -- no dropped orders and no slab/pool exhaustion.

It drives the running fix-test-client's REST API with a single Groovy script that
sends a large number of NewOrderSingles in a tight loop (no inter-order spacing),
using fix.uniqueId() for idempotent, re-runnable ClOrdIDs. It then waits for every
order's ExecutionReport to come back and checks the sequencer logs for WAL-path
distress.

ASSUMES THE FULL STACK IS ALREADY RUNNING with ha_enabled = true (sequencer pair +
live WAL replication), the order gateway, matching engine, and auth service, and
the fix-test-client up on its REST port (default 8081).

Under burst the matching engine returns ERs faster than the follower acks WAL
records, so the sequencer's pending_er_ buffer (seq_no -> slab-allocated ER
payload, held until WalAck) grows. This test's purpose is to confirm that buffer
and the WAL TCP channel absorb the burst without exhausting a slab/pool or dropping
an order.

Pass criteria:
  - every order sent came back as a New ER (zero drops);
  - no ER carried an unexpected OrdStatus;
  - no slab/pool exhaustion, error, or drop in the sequencer logs during the burst.

Reports throughput and a PASS/FAIL. Exit 0 on PASS, 1 on FAIL, 2 if it could not
run.

Usage:
    ./fix_client_burst_test.py                      # 20000 orders, localhost:8081
    ./fix_client_burst_test.py --orders 50000
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ORDSTATUS_NEW = "0"
ORDSTATUS_CANCELED = "4"
ALLOWED_ORDSTATUS = {ORDSTATUS_NEW, ORDSTATUS_CANCELED}

FIX_COMP_ID = "APM001"
FIX_PASSWORD = "stubpassword"
FIX_USE_TLS = "true"

# Log patterns that indicate the WAL path failed to absorb the burst. Backpressure
# and EPOLLOUT partial writes are handled correctly (they add queueing, not loss)
# and are reported as informational, not failures.
FAIL_PATTERNS = re.compile(
    r"exhaust|pool.*full|slab.*full|out of (slab|memory)|dropping|dropped|"
    r"\bERROR\b|\bCRITICAL\b|assertion|SIGSEGV", re.IGNORECASE)
INFO_PATTERNS = re.compile(r"backpressure|EPOLLOUT|partial write|retry", re.IGNORECASE)


def log(msg):
    """Print a line to stdout, flushed."""
    print(msg, flush=True)


def die(msg):
    """Print an error and exit with code 2 (the test could not run)."""
    log(f"ERROR: {msg}")
    sys.exit(2)


class Client:
    """Minimal stdlib-only REST client for the fix-test-client HTTP API."""

    def __init__(self, base):
        self.base = base.rstrip("/")

    def get_json(self, path, timeout=30):
        """GET ``path`` and parse the JSON body; die on any transport/HTTP error."""
        req = urllib.request.Request(self.base + path, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                text = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            die(f"GET {path} -> HTTP {exc.code}: {exc.read().decode('utf-8', 'replace')}")
        except urllib.error.URLError as exc:
            die(f"cannot reach fix-test-client at {self.base}{path}: {exc.reason} "
                f"(is the stack running and the fix-test-client up?)")
        return json.loads(text) if text.strip() else None

    def post_form(self, path, form):
        """POST a form-encoded body; return ``(http_status, body_text)``."""
        body = urllib.parse.urlencode(form).encode()
        req = urllib.request.Request(
            self.base + path, data=body, method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"})
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.status, resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read().decode("utf-8", "replace")
        except urllib.error.URLError as exc:
            die(f"cannot reach fix-test-client at {self.base}{path}: {exc.reason}")
        return 0, ""  # unreachable (die exits); keeps returns consistent


_BURST_SCRIPT = '''
def clordId = { "BURST-" + fix.uniqueId() }
if (!session.isLoggedOn()) {
    session.logon("''' + FIX_COMP_ID + '''", "''' + FIX_PASSWORD + '''", ''' + FIX_USE_TLS + ''')
    def deadline = System.currentTimeMillis() + 10000
    while (!session.isLoggedOn() && System.currentTimeMillis() < deadline) { sleep(200) }
}
if (!session.isLoggedOn()) { out.println "ERROR: logon did not complete"; return }
out.println "logged on"

def n = __ORDERS__
def t0 = System.currentTimeMillis()
(1..n).each {
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(clordId()))
    nos.set(new quickfix.field.Symbol("AAPL"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(100.00))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
}
def dt = System.currentTimeMillis() - t0
out.println "SENT_COUNT " + n
out.println "ELAPSED_MS " + dt
'''


def run_script(client, content, poll_timeout=120.0):
    """Submit the Groovy burst, poll to COMPLETED/FAILED, return the output text."""
    code, text = client.post_form("/api/script/run", {"content": content})
    if code == 409:
        die("a script is already running on the fix-test-client -- stop it first")
    if code != 200:
        die(f"/api/script/run -> HTTP {code}: {text}")

    deadline = time.monotonic() + poll_timeout
    while time.monotonic() < deadline:
        state = client.get_json("/api/script")
        name = state.get("state", "")
        if name in ("COMPLETED", "FAILED"):
            output = (state.get("output") or "").strip()
            for line in output.splitlines():
                if not line.startswith(("SENT_COUNT ", "ELAPSED_MS ")):
                    log(f"    [burst] {line}")
            if name == "FAILED":
                die(f"burst script FAILED: {(state.get('errorMessage') or '').strip()}")
            return output
        time.sleep(0.25)
    die(f"burst script did not finish within {poll_timeout:.0f}s")
    return ""  # unreachable (die exits); keeps returns consistent


def scalar(output, marker, default=0):
    """Return the integer echoed after ``marker`` in the script output."""
    match = re.search(rf"^{marker} (\d+)$", output, re.MULTILINE)
    return int(match.group(1)) if match else default


def count_new_ers(rows):
    """Count inbound New ERs and collect any ER with an unexpected OrdStatus."""
    new_ers = 0
    unexpected = set()
    for row in rows:
        if row.get("direction") != "IN":
            continue
        status = row.get("ordStatus", "") or ""
        if not status:
            continue
        if status == ORDSTATUS_NEW:
            new_ers += 1
        if status not in ALLOWED_ORDSTATUS:
            unexpected.add(status)
    return new_ers, unexpected


def wait_for_ers(client, expected, timeout):
    """Poll the blotter until New ERs reach ``expected`` or growth stalls."""
    deadline = time.monotonic() + timeout
    last = -1
    idle = 0
    while time.monotonic() < deadline:
        rows = client.get_json("/api/messages")
        if isinstance(rows, dict):
            rows = rows.get("messages", rows.get("rows", []))
        new_ers, unexpected = count_new_ers(rows)
        if new_ers >= expected:
            return new_ers, unexpected
        idle = idle + 1 if new_ers == last else 0
        last = new_ers
        if idle >= 8:                    # ~4 s with no new ERs -> assume drained
            return new_ers, unexpected
        time.sleep(0.5)
    rows = client.get_json("/api/messages")
    if isinstance(rows, dict):
        rows = rows.get("messages", rows.get("rows", []))
    return count_new_ers(rows)


def scan_seq_logs(log_dir, sizes_before):
    """Read new sequencer-log bytes since ``sizes_before``; return (fails, infos)."""
    fails, infos = [], []
    for name in ("sequencer_primary.log", "sequencer_secondary.log"):
        path = log_dir / name
        if not path.is_file():
            continue
        start = sizes_before.get(name, 0)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            handle.seek(start)
            for line in handle:
                if FAIL_PATTERNS.search(line):
                    fails.append(f"{name}: {line.strip()[:160]}")
                elif INFO_PATTERNS.search(line):
                    infos.append(name)
    return fails, infos


def snapshot_sizes(log_dir):
    """Record current byte sizes of the sequencer logs (to scan only the burst)."""
    sizes = {}
    for name in ("sequencer_primary.log", "sequencer_secondary.log"):
        path = log_dir / name
        sizes[name] = path.stat().st_size if path.is_file() else 0
    return sizes


def report(result):
    """Print results from the ``result`` dict; return the process exit code."""
    sent, new_ers = result["sent"], result["new_ers"]
    send_ms, wall_ms = result["send_ms"], result["wall_ms"]
    send_rate = sent / (send_ms / 1000.0) if send_ms else 0.0
    rt_rate = new_ers / (wall_ms / 1000.0) if wall_ms else 0.0

    log("")
    log("  -- results ---------------------------------------------")
    log(f"  orders sent        : {sent}")
    log(f"  New ERs received   : {new_ers}")
    log(f"  send time          : {send_ms} ms  ({send_rate:,.0f} orders/s submit)")
    log(f"  round-trip time    : {wall_ms:,.0f} ms  ({rt_rate:,.0f} orders/s end-to-end)")
    log(f"  WAL backpressure   : {len(result['infos'])} log event(s) (handled; informational)")

    failures = []
    if sent != result["expected"]:
        failures.append(f"script sent {sent} orders, expected {result['expected']}")
    if new_ers < sent:
        failures.append(f"{sent - new_ers} orders had no New ER (dropped)")
    if result["unexpected"]:
        failures.append(f"ER(s) with unexpected OrdStatus {sorted(result['unexpected'])}")
    if result["fails"]:
        failures.append(f"{len(result['fails'])} WAL-path distress log line(s):")
        failures.extend("    " + f for f in result["fails"][:5])

    log("")
    log("=" * 60)
    if failures:
        log("  RESULT: FAIL")
        for failure in failures:
            log(f"    - {failure}")
        log("=" * 60)
        return 1
    log("  RESULT: PASS")
    log(f"    {sent} orders, all acked; no drops, no unexpected statuses, "
        f"no slab/pool exhaustion under WAL replication")
    log("=" * 60)
    return 0


def parse_args():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description="fix-test-client WAL burst test")
    parser.add_argument("--host", default="localhost", help="fix-test-client host")
    parser.add_argument("--port", type=int, default=8081, help="REST port (default: 8081)")
    parser.add_argument("--orders", type=int, default=20000,
                        help="number of orders in the burst (default: 20000)")
    parser.add_argument("--log-dir", default="installed/log",
                        help="sequencer log directory to scan (default: installed/log)")
    parser.add_argument("--drain-timeout", type=float, default=60.0,
                        help="max seconds to wait for all ERs (default: 60)")
    return parser.parse_args()


def main():
    """Run the burst test; return the process exit code."""
    args = parse_args()
    client = Client(f"http://{args.host}:{args.port}")
    log_dir = Path(args.log_dir)

    log("=" * 60)
    log("  fix-test-client burst test (WAL replication active)")
    log("=" * 60)
    log(f"  target : http://{args.host}:{args.port}")
    log(f"  orders : {args.orders}")
    log("")

    client.get_json("/api/config")
    client.post_form("/api/messages/clear", {})
    log("  blotter cleared")

    sizes_before = snapshot_sizes(log_dir)

    log(f"  firing burst of {args.orders} orders ...")
    wall0 = time.monotonic()
    output = run_script(client, _BURST_SCRIPT.replace("__ORDERS__", str(args.orders)))
    sent = scalar(output, "SENT_COUNT")
    new_ers, unexpected = wait_for_ers(client, sent, args.drain_timeout)
    fails, infos = scan_seq_logs(log_dir, sizes_before)

    return report({
        "expected": args.orders,
        "sent": sent,
        "new_ers": new_ers,
        "unexpected": unexpected,
        "send_ms": scalar(output, "ELAPSED_MS"),
        "wall_ms": (time.monotonic() - wall0) * 1000.0,
        "fails": fails,
        "infos": infos,
    })


if __name__ == "__main__":
    sys.exit(main())
