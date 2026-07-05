#!/usr/bin/env python3
"""
fix_client_smoke_test.py -- end-to-end order-path smoke test.

Drives the running fix-test-client's REST API by submitting Groovy scripts that
place and cancel orders over a live FIX session, then validates the resulting
message blotter. This exercises the full path: fix-test-client -> order gateway
-> sequencer -> matching engine -> back.

The test ASSUMES THE FULL STACK IS ALREADY RUNNING (auth service, order gateway,
sequencer pair, matching engine) and the fix-test-client is up on its REST port
(default 8081). It does not start or stop anything.

Phases (submitted as Groovy scripts and polled to completion):
  1. Rapid successive NOS -- a handful of new orders with minimal spacing.
  2. Cancel a few of them -- expect OrdStatus=Canceled ERs in the blotter.
  3. Settle (~3 s).
  4. Heavy load -- a Groovy loop of >=100 orders using fix.uniqueId() for
     idempotent, re-runnable ClOrdIDs.

The blotter (GET /api/messages) records the inbound ExecutionReports; each Groovy
script echoes the ClOrdID of every order and cancel it sent (SENT_NOS / SENT_CANCEL
lines) so the validator can match what was sent against what came back:
  (a) every sent NewOrderSingle has a matching New ER (none dropped);
  (b) no ER carries an unexpected OrdStatus (only New / Canceled -- the ME stub
      never matches, so there are no fills);
  (c) every cancel produced a Canceled ER for the original order.

Reports PASS or FAIL with counts. Exit code 0 on PASS, 1 on FAIL, 2 if the test
could not run (e.g. fix-test-client unreachable).

Usage:
    ./fix_client_smoke_test.py                 # localhost:8081, defaults
    ./fix_client_smoke_test.py --port 8081 --heavy 120
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# FIX single-char field codes, as the blotter stores them (via getChar()).
ORDSTATUS_NEW = "0"
ORDSTATUS_CANCELED = "4"
# Only these ER statuses are expected: the matching engine is a stub that never
# matches, so orders rest as New until cancelled -- no partials, no fills.
ALLOWED_ORDSTATUS = {ORDSTATUS_NEW, ORDSTATUS_CANCELED}

# FIX credentials the fix-test-client logs on with (the standard test fixture).
FIX_COMP_ID = "APM001"
FIX_PASSWORD = "stubpassword"
FIX_USE_TLS = "true"


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

    def get_json(self, path):
        """GET ``path`` and parse the JSON body; die on any transport/HTTP error."""
        req = urllib.request.Request(self.base + path, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                text = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", "replace")
            die(f"GET {path} -> HTTP {exc.code}: {body}")
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
            with urllib.request.urlopen(req, timeout=15) as resp:
                return resp.status, resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read().decode("utf-8", "replace")
        except urllib.error.URLError as exc:
            die(f"cannot reach fix-test-client at {self.base}{path}: {exc.reason}")
        return 0, ""  # unreachable (die exits); keeps returns consistent


# ── Groovy scripts ────────────────────────────────────────────────────────────
# Templates use __UPPER__ placeholders (not str.format) so the Groovy braces need
# no escaping. Each order/cancel echoes its ClOrdID so the validator can match
# sent-vs-acked from the script output.

_LOGON = '''
def clordId = { prefix -> "SMOKE-" + prefix + "-" + fix.uniqueId() }
if (!session.isLoggedOn()) {
    session.logon("''' + FIX_COMP_ID + '''", "''' + FIX_PASSWORD + '''", ''' + FIX_USE_TLS + ''')
    def deadline = System.currentTimeMillis() + 10000
    while (!session.isLoggedOn() && System.currentTimeMillis() < deadline) { sleep(200) }
}
if (!session.isLoggedOn()) { out.println "ERROR: logon did not complete"; return }
out.println "logged on"
'''

_PHASES_123 = _LOGON + '''
def ids = []
(1..__RAPID__).each {
    def id = clordId("N")
    ids << id
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(id))
    nos.set(new quickfix.field.Symbol("AAPL"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(100.00))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
    out.println "SENT_NOS " + id
    sleep(20)
}
out.println "phase 1 done"
sleep(500)
ids.take(__CANCELS__).each { origId ->
    def ocr = fix.orderCancelRequest()
    ocr.set(new quickfix.field.ClOrdID(clordId("C")))
    ocr.set(new quickfix.field.OrigClOrdID(origId))
    ocr.set(new quickfix.field.Symbol("AAPL"))
    ocr.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    ocr.set(new quickfix.field.OrderQty(100))
    session.send(ocr)
    out.println "SENT_CANCEL " + origId
}
out.println "phase 2 done"
sleep(3000)
out.println "phase 3 done"
'''

_PHASE_4 = _LOGON + '''
(1..__HEAVY__).each {
    def id = clordId("H")
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID(id))
    nos.set(new quickfix.field.Symbol("MSFT"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(50))
    nos.set(new quickfix.field.Price(200.00))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
    out.println "SENT_NOS " + id
}
out.println "phase 4 done"
sleep(2500)
'''


def run_script(client, label, content, poll_timeout=90.0):
    """Submit a Groovy script, poll to COMPLETED/FAILED, return the output text."""
    log(f"  submitting script: {label}")
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
            err = (state.get("errorMessage") or "").strip()
            for line in output.splitlines():
                if not line.startswith(("SENT_NOS ", "SENT_CANCEL ")):
                    log(f"    [{label}] {line}")
            if name == "FAILED":
                die(f"script '{label}' FAILED: {err or '(no error message)'}")
            log(f"  script '{label}' completed")
            return output
        time.sleep(0.25)
    die(f"script '{label}' did not finish within {poll_timeout:.0f}s")
    return ""  # unreachable (die exits); keeps returns consistent


def sent_ids(output, marker):
    """Return the ClOrdIDs echoed after ``marker`` (SENT_NOS / SENT_CANCEL)."""
    return re.findall(rf"^{marker} (.+)$", output, re.MULTILINE)


def validate(sent_nos, sent_cancel, rows):
    """Check sent orders/cancels against the ER blotter; return a failures list."""
    failures = []

    new_er_ids = set()
    canceled_orig_ids = set()
    unexpected = []
    for row in rows:
        if row.get("direction") != "IN":
            continue
        status = row.get("ordStatus", "") or ""
        if not status:
            continue
        if status not in ALLOWED_ORDSTATUS:
            unexpected.append(row)
        if status == ORDSTATUS_NEW:
            new_er_ids.add(row.get("clOrdId", ""))
        elif status == ORDSTATUS_CANCELED:
            canceled_orig_ids.add(row.get("origClOrdId", ""))

    dropped = [cid for cid in sent_nos if cid not in new_er_ids]
    uncancelled = [cid for cid in sent_cancel if cid not in canceled_orig_ids]

    log("")
    log("  -- validation ------------------------------------------")
    log(f"  NOS sent          : {len(sent_nos)}")
    log(f"  New ERs received  : {len(new_er_ids)}")
    log(f"  cancels sent      : {len(sent_cancel)}")
    log(f"  Canceled ERs recv : {len(canceled_orig_ids)}")

    if dropped:
        preview = ", ".join(dropped[:5]) + (" ..." if len(dropped) > 5 else "")
        failures.append(f"{len(dropped)} sent NOS had no matching New ER (dropped): {preview}")
    if uncancelled:
        preview = ", ".join(uncancelled[:5]) + (" ..." if len(uncancelled) > 5 else "")
        failures.append(f"{len(uncancelled)} cancel(s) produced no Canceled ER: {preview}")
    if unexpected:
        seen = sorted({row.get("ordStatus", "?") for row in unexpected})
        failures.append(f"{len(unexpected)} ER(s) with unexpected OrdStatus {seen}")
    return failures


def parse_args():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description="fix-test-client end-to-end smoke test")
    parser.add_argument("--host", default="localhost",
                        help="fix-test-client host (default: localhost)")
    parser.add_argument("--port", type=int, default=8081,
                        help="fix-test-client REST port (default: 8081)")
    parser.add_argument("--rapid", type=int, default=8,
                        help="phase 1 rapid NOS count (default: 8)")
    parser.add_argument("--cancels", type=int, default=3,
                        help="phase 2 cancel count (default: 3)")
    parser.add_argument("--heavy", type=int, default=120,
                        help="phase 4 heavy NOS count (default: 120)")
    args = parser.parse_args()
    if args.cancels > args.rapid:
        parser.error("--cancels must not exceed --rapid")
    return args


def main():
    """Run the smoke test; return the process exit code."""
    args = parse_args()
    client = Client(f"http://{args.host}:{args.port}")

    log("=" * 60)
    log("  fix-test-client end-to-end smoke test")
    log("=" * 60)
    log(f"  target      : http://{args.host}:{args.port}")
    log(f"  phase 1     : {args.rapid} rapid NOS")
    log(f"  phase 2     : {args.cancels} cancels")
    log(f"  phase 4     : {args.heavy} heavy NOS")
    log("")

    client.get_json("/api/config")               # preflight: is it reachable?
    log("  fix-test-client up (config fetched)")
    client.post_form("/api/messages/clear", {})
    log("  blotter cleared")
    log("")

    out1 = run_script(client, "phases 1-3 (rapid NOS + cancels + settle)",
                      _PHASES_123.replace("__RAPID__", str(args.rapid))
                                 .replace("__CANCELS__", str(args.cancels)))
    out2 = run_script(client, "phase 4 (heavy load)",
                      _PHASE_4.replace("__HEAVY__", str(args.heavy)))

    combined = out1 + "\n" + out2
    sent_nos = sent_ids(combined, "SENT_NOS")
    sent_cancel = sent_ids(combined, "SENT_CANCEL")

    time.sleep(1.0)                               # grace for the last ERs to land
    rows = client.get_json("/api/messages")
    if isinstance(rows, dict):
        rows = rows.get("messages", rows.get("rows", []))
    log(f"  blotter rows fetched: {len(rows)}")

    failures = validate(sent_nos, sent_cancel, rows)
    expected_nos = args.rapid + args.heavy
    if len(sent_nos) != expected_nos:
        failures.append(f"script sent {len(sent_nos)} NOS, expected {expected_nos}")

    log("")
    log("=" * 60)
    if failures:
        log("  RESULT: FAIL")
        for failure in failures:
            log(f"    - {failure}")
        log("=" * 60)
        return 1
    log("  RESULT: PASS")
    log(f"    {len(sent_nos)} orders placed and all acked; "
        f"{len(sent_cancel)} cancels all confirmed; no drops, no unexpected statuses")
    log("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
