# Binary Gateway {#binary_gateway}

## Role

The binary gateway is the order gateway's peer: same sequencers, same matching engine, same
book, a different client protocol. Where the order gateway speaks ASCII FIX 5.0 SP2, this one
speaks the internal PDU protocol directly -- clients send the very `NewOrderSingle` the
pipeline already carries and receive the very `ExecutionReport`.

Its point is that it has almost nothing in it. The order gateway is largely a translator: a
FIX parser, a dictionary-driven validator, a serialiser, an ER encoder. None of that exists
here, because the client already speaks the pipeline's language. What is left is session
identity and routing, which is the irreducible job of a gateway.

That makes it a useful control. Any cost the order gateway carries that this one does not is
the cost of FIX specifically, not of being a gateway.

## Wire protocol

Every message is a 24-byte `PduHeader` followed by a DSL-encoded payload -- the same framing
the components use between themselves, so the listener is a `FrameworkPdu` one and the
framework does the framing. There is no byte-stream parsing anywhere in this application.

Two message types are specific to this gateway, defined in `applications/binary_session.dsl`
(ids 700-709):

| PDU | Direction | Purpose |
|-----|-----------|---------|
| `Logon` | client → gateway | Carries `comp_id`, the client's identity |
| `LogonAck` | gateway → client | `LogonOutcome`, plus optional text for logs |

Everything else is the DD-derived order messages from `fix_orders.dsl` (ids 1000+):
`NewOrderSingle` and `OrderCancelRequest` inbound, `ExecutionReport` outbound.

A session is: connect, `Logon`, `LogonAck`, then orders. Any other PDU before `Logon` is
refused and the connection closed, so every order in the pipeline carries a comp id.

### What the protocol deliberately omits

- **No password or SCRAM exchange.** The comp id gives session identity for ER routing and
  the audit trail, which is what the pipeline needs. Authenticating a second transport would
  re-tread what the FIX gateway already does against the authentication service.
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
1 = order gateway, 2 = binary gateway). The pair identifies a session across the venue. The
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

## Configuration

`etc/binary_gateway/binary_gateway.toml`. Ports in the dev environment:

| Endpoint | Port | Notes |
|----------|------|-------|
| Client listener | 9890 | The FIX gateway's equivalent is 9879 |
| ER listener | 11110 | Must match `binary_gateway.port` in the sequencer's config |
| Sequencer primary | 11001 | Outbound, dialled by the gateway |
| Sequencer secondary | 11002 | Outbound, when `ha_enabled` |

The sequencer must be told about it: `[binary_gateway] enabled/host/port` in
`sequencer_primary.toml` and `sequencer_secondary.toml`. Setting `enabled = false` runs the
venue with only the FIX gateway, which is what the non-dev environments ship with.

**Startup order** is as for the order gateway: this gateway must start before the sequencer,
which dials its ER listener and otherwise retries.

## Reference client

`bin/binary_client` logs on, sends orders, and prints the ERs that come back:

```
binary_client --host 127.0.0.1 --port 9890 --comp-id BINCLIENT --orders 3
```

It uses plain sockets and the generated codecs rather than the framework, which shows that a
client needs nothing from `pubsub_itc_fw` beyond the PDU header layout. It is the worked
example for the Java client that will eventually drive this gateway from the web UI.

## Cancel-on-disconnect

A client that vanishes leaves orders resting on the book that nobody is managing, so the
gateway cancels them on its behalf -- the same obligation the order gateway has, and the same
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

## Known differences from the order gateway

- **No TLS listener.** The order gateway offers one; this gateway is plain TCP only.
- **No authentication service integration**, as described above.
