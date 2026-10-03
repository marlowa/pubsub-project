# Binary Gateway {#binary_order_gateway}

## Role

The binary order gateway is the FIX order gateway's peer: same sequencers, same matching engine, same
book, a different client protocol. Where the FIX order gateway speaks ASCII FIX 5.0 SP2, this one
speaks the internal PDU protocol directly -- clients send the very `NewOrderSingle` the
pipeline already carries and receive the very `ExecutionReport`.

Much of what the FIX order gateway does is translation: parsing FIX text, checking it against a
dictionary, and writing FIX back. None of that is needed here, because the member already speaks
the venue's own message format. What is left is what any gateway must do: authenticate the
member, check each command against the venue's rules, keep the session's state, and route.

That makes it a useful control. Any cost the FIX order gateway carries that this one does not is
the cost of FIX specifically, not of being a gateway.

**What the comparison shows.** Driven at identical rates, the binary gateway decodes an order in
about 0.05 microseconds against the FIX gateway's 3.8, and is faster end to end on every measure,
but only by 1.8 microseconds at the median (103.3 against 105.1), because the round trip is
dominated by what the two share: the sequencer, the matching engine, the log and the report path.
The clearer gain is at the tail, about 12 to 14 microseconds at the 90th and 99th percentiles. The
measurement and its method are in [latency_findings.md](../operations/latency_findings.md). Both
gateways log the same markers at the same rate, which must stay true, or a comparison measures
logging rather than protocol.

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
`NewOrderSingle` (1000) and `OrderCancelRequest` (1001) inbound, `ExecutionReport` (1002) and
`OrderCancelReject` (1003) outbound. `OrderCancelReject` answers a refused request to cancel, with
the same fields as FIX's 35=9, and says the order's status rather than reporting the order
rejected (R-0151).

A session is: connect, `Logon`, SCRAM exchange with the authentication service, `LogonAck`,
then orders. Any other PDU before the session is authenticated is refused and the connection
closed, so nothing reaches the book from a session that has not proved who it is.

### Authentication

SCRAM-SHA-256 against the same authentication service the FIX gateway uses, and the same
exchange: the gateway sends a client nonce with the comp id, derives a proof from the
password when the challenge returns, and verifies the ServerSignature that comes back — so
the service authenticates itself to the gateway in turn. The password never leaves the
gateway process and is zeroed the moment the proof is derived.

*Considered and rejected: identifying the member by comp id alone,* on the reasoning that the FIX
gateway already authenticates members. That would leave an order-entry port that anyone who could
reach it could trade through, whatever protocol the orders arrived in.

`target_comp_id` is checked rather than merely recorded. Empty means the client did not mind
which venue it reached; a populated value that does not match the gateway's configured
`sender_comp_id` refuses the logon with `WrongTargetCompId`. A client that has connected
somewhere it did not intend should be told, not quietly traded.

### What the protocol deliberately omits

- **No heartbeats or sequence numbers.** The framework's PDU transport already detects a dead
  connection and delivers messages framed and in order. FIX needs both because it runs over a
  bare byte stream it must itself keep alive and ordered.

## Order and ER flow

**Every member command is decoded and checked**, as the FIX gateway parses and checks every
order, and then the bytes the member sent are passed on unchanged: the gateway wraps the encoded
payload in a `WalRecord` envelope carrying the routing metadata and forwards that to the
sequencers. Decoding the binary layout reads fields at fixed positions, which costs less than
parsing FIX text, so the binary gateway keeps its advantage over the FIX gateway. Outbound reports
are relayed with `send_pdu_payload` as the bytes that arrived; each is decoded as well, to keep the
session's record of open orders.

**The checks**, in order, each refusing the command if it fails (R-0152):

1. **Every field holds a value its definition allows**: an enumerated field (Side, OrdType,
   TimeInForce and the rest, including inside repeating groups) holds a value the protocol
   defines, and a required string is not empty. This is `first_invalid_field`, generated for each
   message from `applications/fix_orders.dd.xml` alongside the decoder, so the two cannot disagree.
2. **Identifiers, Symbol and OrderQty are no longer than the venue holds**: ClOrdID and
   OrigClOrdID at most `fix_order_limits::max_cl_ord_id_length` (64, the matching engine's book
   key), Symbol and OrderQty at most `[order_limits]` in the configuration.
3. **Every quantity and price is a decimal number**, by the FIX codec's own rule
   (`fix_codec::FixField::as_decimal`).
4. **A sequencer is connected, and the venue is accepting orders.** The leading sequencer says
   whether the venue is accepting orders with an `OrderAcceptance` message, on the connection it
   holds to the gateway, as it does to the FIX gateway.
5. **The session is within its throttle limits** ([gateway_throttles.md](gateway_throttles.md)).

Checks 1 to 3 are `BinaryCommandChecks.hpp`; 4 and 5 are the gateway thread's. The texts of the
refusals for check 4 are the FIX gateway's. A command whose ClOrdID is empty, or which cannot be
decoded at all, cannot be named in a reply, so it is dropped and logged at Info.

**Refusals** are answered by the gateway: a new order with a rejected `ExecutionReport` whose
OrderID the gateway assigns (`GW-ORD-n`), and a cancel with an `OrderCancelReject` reporting the
order still open (OrdStatus New). A refused command is never passed on.

**The matching engine's refusal of a cancel** arrives as a rejected `ExecutionReport` carrying the
OrigClOrdID of the order named. The gateway decodes every report it relays, to keep the session's
record of open orders, and sends this one to the member as an `OrderCancelReject` instead, with
CxlRejReason 1, Unknown order. Every other report is relayed as the bytes that arrived.

## Routing

Every order envelope carries `origin_gateway_id` (2 for this gateway, 1 for the FIX gateway; see
`applications/fix_common/GatewayIds.hpp`) and `gateway_instance_id`, and the member's comp id. A
session is identified by its comp id and protocol, so a binary session and a FIX session under the
same comp id are separate sessions with separate books and reports. The sequencer sends each
report to wherever the session is bound when the report is produced, and the matching engine's
book and its replication to the follower engine are keyed on the session too, so a member that
reconnects, to this instance or the other, can cancel what it left resting and receives its
reports there. How this works is in [Gateway High Availability](../availability/gateway_ha.md).

## Instances

Like the FIX order gateway, this one runs as two instances, `binary_order_gateway_a` and
`binary_order_gateway_b`, from one binary and one `etc/binary_order_gateway/` directory.
The suffix is `_a`/`_b` rather than `_primary`/`_secondary` because nothing elects
anything here: a member picks which instance to connect to, so this is caller-selected
redundancy, the same shape the authentication service already uses.

Each instance stamps its own `[gateway] instance_id` onto every order envelope beside the
protocol id. Instance 1 is `_a` and instance 2 is `_b`. Each member is provisioned to a primary and
a backup instance, as for the FIX gateway, and is refused at an instance it is not provisioned for
with `LogonOutcome::NotProvisionedForInstance`.

The binary gateway runs only in the development environment. The other environment files carry
both instances with `enabled = false`, so it can be brought up without editing a template.

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
what every environment except development does.

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

A member that vanishes leaves orders resting on the book that nobody is managing, so the gateway
cancels them on its behalf: the same obligation the FIX order gateway has, and the same mechanism,
shared through `applications/fix_common/OpenOrderEntry.hpp`. The settings are the same too:
`[cancel_on_disconnect] enabled` and `grace_period` (on, 30 seconds), which each comp id may
override, and GoodTillCancel and GoodTillDate orders are never cancelled on disconnect. The binary
protocol has no logout message, so every disconnect waits out the full grace period; a member that
logs on again inside it has nothing cancelled. See
[Gateway High Availability](../availability/gateway_ha.md).

Tracking is driven by the matching engine's acknowledgements, not by when an order was forwarded:
an order is the session's to cancel only once the engine has said it is on the book. A
non-terminal `ExecutionReport` records the order, a terminal one (Filled, Canceled, DoneForDay,
Rejected, Expired) retires it, and a repeated non-terminal report for an order already tracked
updates the entry rather than adding a second.

When the grace period ends, the session's orders are cancelled in batches of 500 on a 1 ms timer,
so a member holding thousands of resting orders cannot hold up the reactor. Each generated cancel
gets a `BGW-CXL-<conn>-<n>` ClOrdID and an envelope carrying the departed session's comp id, so it
is attributed to the member whose order it retires. The acknowledgements come back for a session
that no longer exists and are logged and dropped, which is the expected end of the sequence.

Entries come from a pool (`[open_order_pool]`), so tracking an order allocates nothing. A report
that cannot be decoded is still relayed: the member is its audience, and a gateway that cannot read
a message has no business withholding it.

## Known differences from the FIX order gateway

- **No TLS listener.** The FIX order gateway offers one; this gateway is plain TCP only, so the
  password crosses the wire in the clear on the client-to-gateway hop. SCRAM means it is
  never stored or forwarded, but it is not a substitute for transport encryption. Whether
  to add it -- and whether the argument extends to the internal PDU hops, since order flow
  is itself sensitive -- is an open question; see the transport encryption item in the
  [roadmap](../roadmap.md).
- **No resend.** The binary protocol has no session layer and no message numbering, so a member
  that reconnects is not sent the reports it missed ([BUG-0046](../bug_list.md#bug_0046)). It does
  receive, on its new connection, the reports produced after it is bound again.
