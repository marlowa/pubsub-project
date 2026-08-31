# Binary Gateway {#binary_order_gateway}

## Role

The binary order gateway is the FIX order gateway's peer: same sequencers, same matching engine, same
book, a different client protocol. Where the FIX order gateway speaks ASCII FIX 5.0 SP2, this one
speaks the internal PDU protocol directly -- clients send the very `NewOrderSingle` the
pipeline already carries and receive the very `ExecutionReport`.

Its point is that it has almost nothing in it. The FIX order gateway is largely a translator: a
FIX parser, a dictionary-driven validator, a serialiser, an ER encoder. None of that exists
here, because the client already speaks the pipeline's language. What is left is session
identity and routing, which is the irreducible job of a gateway.

That makes it a useful control. Any cost the FIX order gateway carries that this one does not is
the cost of FIX specifically, not of being a gateway.

**What that control has measured so far (2026-07-26).** Speaking ASCII FIX costs about **11% of
the FIX gateway's samples** -- parse, validate, walk repeating groups, checksum -- against 0%
here. Real, but far short of what intuition suggests: roughly 70% of both gateways' samples are
in the kernel, so protocol choice can only ever move the remaining third. Two cautions came out
of that exercise. Until it was equalised, *logging* cost more than FIX parsing did (32% of the
FIX gateway's samples against 11% here), so both gateways now emit the same markers at the same
cadence -- keep it that way or any comparison measures logging. And a throughput comparison is
still not available, because the FIX harness infers completion from log polling rather than
measuring it. The metrics that would settle it properly are specified under item 16 in
`pubsub_itc_fw_summary.md`; the comparison is deferred until then.

## Wire protocol

Every message is a 24-byte `PduHeader` followed by a DSL-encoded payload -- the same framing
the components use between themselves, so the listener is a `FrameworkPdu` one and the
framework does the framing. There is no byte-stream parsing anywhere in this application.

Two message types are specific to this gateway, defined in `applications/binary_session.dsl`
(ids 700-709):

| PDU | Direction | Purpose |
|-----|-----------|---------|
| `Logon` | client → gateway | `comp_id`, `password`, `target_comp_id` |
| `LogonAck` | gateway → client | `LogonOutcome`, plus optional text for logs |

Everything else is the DD-derived order messages from `fix_orders.dsl` (ids 1000+):
`NewOrderSingle` and `OrderCancelRequest` inbound, `ExecutionReport` outbound.

A session is: connect, `Logon`, SCRAM exchange with the authentication service, `LogonAck`,
then orders. Any other PDU before the session is authenticated is refused and the connection
closed, so nothing reaches the book from a session that has not proved who it is.

### Authentication

SCRAM-SHA-256 against the same authentication service the FIX gateway uses, and the same
exchange: the gateway sends a client nonce with the comp id, derives a proof from the
password when the challenge returns, and verifies the ServerSignature that comes back — so
the service authenticates itself to the gateway in turn. The password never leaves the
gateway process and is zeroed the moment the proof is derived.

This was not the original design. The protocol first shipped with a comp id alone, on the
reasoning that authenticating a second transport would re-tread ground the FIX gateway
already covered. That reasoning was wrong: it left an order-entry port that anyone who could
reach it could trade through, whatever the transport carrying the orders.

`target_comp_id` is checked rather than merely recorded. Empty means the client did not mind
which venue it reached; a populated value that does not match the gateway's configured
`sender_comp_id` refuses the logon with `WrongTargetCompId`. A client that has connected
somewhere it did not intend should be told, not quietly traded.

### What the protocol deliberately omits

- **No heartbeats or sequence numbers.** The framework's PDU transport already detects a dead
  connection and delivers messages framed and in order. FIX needs both because it runs over a
  bare byte stream it must itself keep alive and ordered.

## Order and ER flow

Inbound orders are **not decoded**. The gateway wraps the encoded payload, exactly as the
client sent it, in a `WalRecord` envelope carrying the routing metadata, and forwards that to
the sequencers. Outbound ERs are likewise relayed with `send_pdu_payload` without being
decoded -- only the envelope around them is read.

This is not just an efficiency point. A relay that does not parse what it carries does not
need rebuilding when a message it merely passes through gains a field, which is exactly what
the DD-driven generator makes likely.

## Routing: why gateways have ids

An `ExecutionReport` must come back to the gateway its order arrived through. The envelope
already carried `gateway_session_conn_id`, but that identifies a connection *within one
gateway* -- each numbers its own client connections from its own counter, so the FIX gateway
and this one will both hand out low integers for unrelated sessions.

So the envelope also carries `origin_gateway_id` (see `applications/fix_common/GatewayIds.hpp`:
1 = FIX order gateway, 2 = binary order gateway). The pair identifies a session across the venue. The
sequencer keeps a connection per gateway id and routes each ER by the id recorded when the
order was sequenced.

The field is optional and trailing, so WAL records written before it existed still decode; a
record without it is treated as having come from gateway 1, which is what was true then.

**These ids are part of the on-disk WAL format once written.** Never reuse one for a
different gateway; add new gateways by taking the next free number.

### Routing has to survive failover too

Getting an ER back to the right gateway in the steady state is the easy half. Three other
paths carry the gateway id, and all three are needed for a binary session to behave correctly
across a failover:

- **WAL replay.** The sequencer rebuilds its `seq_no → session` map as it replays, taking the
  gateway id from the stored envelope. Without this, every ER emitted after a promotion would
  fall back to "gateway 1" and binary sessions would silently lose their reports.
- **The matching engine's book.** Each resting order remembers the gateway id alongside the
  connection id, because the ME generates cancel ERs on promotion (`seq_no = 0`) that no map
  lookup can route -- their only routing information is what the book entry holds.
- **Book replication.** `BookUpdate` carries the gateway id, so a promoted secondary can route
  the cancels for orders it inherited rather than only for ones it accepted itself.

The general point: a connection id was never meaningful on its own once a second gateway
existed, so every place that stored or forwarded one had to start carrying its gateway id
with it.

## Instances

Like the FIX order gateway, this one runs as two instances, `binary_order_gateway_a` and
`binary_order_gateway_b`, from one binary and one `etc/binary_order_gateway/` directory.
The suffix is `_a`/`_b` rather than `_primary`/`_secondary` because nothing elects
anything here: a member picks which instance to connect to, so this is caller-selected
redundancy, the same shape the authentication service already uses.

Each instance stamps its own `[gateway] instance_id` onto every order envelope beside the
protocol id, and the sequencer routes each execution report back to the instance the order
arrived on. Instance 1 is `_a` and instance 2 is `_b`.

Only `_a` is deployed outside dev; `_b` is configured in the sequencer but disabled, so a
second instance can be brought up without editing the template.

## Configuration

`etc/binary_order_gateway/binary_order_gateway_a.toml` and `..._b.toml`. Ports in the dev
environment:

| Endpoint | Port (`_a`) | Port (`_b`) | Notes |
|----------|------|------|-------|
| Client listener | 9890 | 9891 | The FIX gateway's equivalents are 9879 and 9881 |
| ER listener | 11110 | 11111 | Must match this instance's `[[gateway]]` entry in the sequencer's config |
| Sequencer primary | 11001 | 11001 | Outbound, dialled by the gateway |
| Sequencer secondary | 11002 | 11002 | Outbound, when `ha_enabled` |
| Authentication service | 11070 / 11071 | 11070 / 11071 | Outbound, for the SCRAM exchange |

`[binary_session] sender_comp_id` is this gateway's own name (`BINARY-GATEWAY`), checked
against each client's `target_comp_id`. It names the venue, not the process, so both
instances use it.

The sequencer must be told about each instance: a `[[gateway]]` table with `protocol = 2`
and the instance number, in `sequencer_primary.toml` and `sequencer_secondary.toml`.
Setting `enabled = false` on all of them runs the venue with only the FIX gateway, which is
what the non-dev environments ship with.

**Startup order** is as for the FIX order gateway: it does not matter. The sequencer dials this
gateway's ER listener and retries every two seconds until it answers.

## Reference client

`bin/binary_client` logs on, sends orders, and prints the ERs that come back:

```
binary_client --port 9890 --comp-id BINCLIENT --password stubpassword --orders 3
```

It uses plain sockets and the generated codecs rather than the framework, which shows that a
client needs nothing from `pubsub_itc_fw` beyond the PDU header layout.

## Load generator

`bin/binary_load_client` is the counterpart of fix8's `f8test`, which `perf_run.py` drives
against the FIX gateway. It follows the same interface -- a `T` on stdin fires a burst -- so
the harness can drive either, and `perf_run.py --gateway binary` selects it.

```
binary_load_client --sessions 4 --orders-per-burst 1000 --bursts 2
binary_load_client --sessions 4 --orders-per-burst 2000 --bursts 1 --rate 2000
```

Two things it does that `f8test` cannot, because it owns both ends: it reports its own
sent-versus-received counts rather than leaving the harness to infer completion from log
lines, and it measures true per-order round-trip latency by matching each report to its send
time by ClOrdID.

**Give it a `--rate` for any latency measurement.** Without one it offers orders as fast as
the socket accepts them, which is far faster than the pipeline drains, so the reported
latencies are dominated by queueing rather than service time. The tool says so in its own
output, but the distinction is easy to miss and the difference is two orders of magnitude.

Its orders carry the full DD-derived field set including both repeating groups by default,
matching what `f8test` sends. A binary run against a minimal order would flatter this gateway
badly, since most per-order work scales with field and group count.

## Java web test client

The `fix-test-client` drives either gateway. The logon page has a FIX/Binary selector; one
session is live at a time, and the order form, cancel and blotter follow whichever it is.
Its protocol classes are generated from the same DSL as the C++ side at build time, so the
client cannot drift from the gateway it talks to.

The raw-FIX entry page stays FIX-only: there is no such thing as a hand-typed binary PDU.

## Cancel-on-disconnect

A client that vanishes leaves orders resting on the book that nobody is managing, so the
gateway cancels them on its behalf -- the same obligation the FIX order gateway has, and the same
mechanism, shared via `applications/fix_common/OpenOrderEntry.hpp`.

Tracking is driven by the matching engine's acknowledgements, not by order-forward time: an
order is only the session's to cancel once the engine has said it is on the book. A
non-terminal `ExecutionReport` records the order, a terminal one (Filled, Canceled,
DoneForDay, Rejected, Expired) retires it, and a repeated non-terminal report for an order
already tracked updates the entry in place rather than allocating a second one.

On disconnect the session's orders are moved to a queue and drained in batches of 500 on a
1 ms timer, so a client holding thousands of resting orders cannot stall the reactor. Each
generated cancel gets a `BGW-CXL-<conn>-<n>` ClOrdID and rides an envelope carrying the
departed session's connection id and comp id, so it is attributed to the client whose order it
retires. The acknowledgements come back for a session that no longer exists and are logged and
dropped, which is the expected end of the sequence rather than an error.

Entries come from a pooled allocator (`[open_order_pool]`), so tracking an order costs no heap
allocation.

Note this is the one place the gateway decodes a message it relays. It reads the ER to
maintain the open-order set, but the bytes sent to the client are still the ones that arrived;
if a report cannot be decoded it is relayed anyway, because the client is its audience and a
gateway that cannot read a message still has no business withholding it.

## Known differences from the FIX order gateway

- **No TLS listener.** The FIX order gateway offers one; this gateway is plain TCP only, so the
  password crosses the wire in the clear on the client-to-gateway hop. SCRAM means it is
  never stored or forwarded, but it is not a substitute for transport encryption. Whether
  to add it -- and whether the argument extends to the internal PDU hops, since order flow
  is itself sensitive -- is an open question; see the encryption TODO in
  `pubsub_itc_fw_summary.md`.
- **No proprietary logon mode.** The FIX gateway has one; the binary order gateway offers SCRAM
  only. A proprietary mode was considered and rejected: its purpose would have been to test
  a different venue's binary protocol, which this client cannot speak in any case.
