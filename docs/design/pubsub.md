# Pub/Sub {#pubsub}

The framework provides a **topic-based publish/subscribe primitive**: a publisher exposes one
or more named topics; any number of subscribers connect, replay the history they have missed,
and then stream new records live. Delivery is reliable and ordered, backed by a
[write-ahead log](wal.md) and paced by TCP.

This document describes the design and its decisions, then walks two concrete, runnable
examples: **publishing** (the Matching Engine Publisher) and **subscribing** (the `topic_probe`
diagnostic tool).

---

## Overview

A **topic** is a named stream of records. A record is one DSL-encoded message (a PDU) with a
sequence number and a publish timestamp. A **publisher** owns the WAL that records are appended
to and serves them to subscribers; a **subscriber** connects to a publisher, names the topic it
wants and a starting sequence number, and receives every record on that topic from that point
on.

The pieces:

| Piece | Role |
|-------|------|
| `TopicPublisher` | Reusable publisher side, one instance per topic. Streams records from the WAL to subscribers; handles subscribe, backpressure, retention, and leadership. |
| `TopicSubscriberThread` | Reusable subscriber base. Owns connect, the subscribe handshake, dedup, and acking; delivers each fresh record to `on_pubsub_message()`. |
| `TopicSubscriberChannel` | The subscriber-side protocol state machine that `TopicSubscriberThread` drives. |
| The [WAL](wal.md) | The retained backlog. Fan-out is many cursors over one log. |
| The topic **catalog** | The set of recognised topic names and their member message types, generated from the DSL. |

The primitive is transport-agnostic at the seam: `TopicPublisher` decides *what* to send and
*to whom* and defers the actual socket work to its host application through the
`TopicPublisherHost` interface. This is what lets the same publisher run inside any
`ApplicationThread`.

---

## Design decisions

### D1 — Delivery is streamed and paced by the TCP socket, not by acks.

Records are pushed to each subscriber as fast as its socket will accept them. There is **no
per-record or per-page acknowledgement on the data path.** TCP already guarantees no loss, no
duplication, and in-order delivery within a connection, so an application-level ack would add a
round-trip per message for a guarantee the transport already makes.

Backpressure is the socket itself: a slow subscriber's send buffer fills, a non-blocking send
returns `EAGAIN`, and the publisher stops sending to *that* subscriber and resumes when the
socket signals writable again (`EPOLLOUT`). Because a reactor subscriber reads-then-processes-
then-reads, its socket-drain rate tracks its *processing* rate — so TCP backpressure genuinely
reflects how fast the subscriber consumes, not merely network speed.

### D2 — The WAL is the backlog; a lagging subscriber costs a cursor, not RAM.

Each subscriber has its own [WAL cursor](wal.md#the-cursor-and-replay-model). Catch-up and live
delivery are the *same* path: the publisher reads the next record after the cursor, whether that
record is a year old or a microsecond old. A subscriber that falls behind does not cause the
publisher to buffer anything in memory — it simply holds an older cursor position over the same
retained log. The retained WAL *is* the buffer, shared by every subscriber.

This is the decision that makes fan-out to many subscribers at different positions cheap, and it
is why pub/sub is built on the WAL rather than on per-subscriber queues.

### D3 — Pages batch records to amortise the epoll round-trip, without adding latency.

Delivery is still one `TopicPage` per writable notification — that is what keeps it socket-paced
— but a page carries a *batch* of up to `max_records_per_page` records (default 256), bounded by
an encoded-payload budget kept well under the outbound slab and decode arena. Batching amortises
the one-epoll-round-trip-per-page cost over many records, which is the dominant throughput cost
when catching up. When only one record is available (the live, one-at-a-time case) the page
carries one, so batching **never adds latency** on the live path. The `TopicPage` wire format
carries a record list, so this is purely a pacing choice, not a protocol change.

### D4 — Slow consumers are dropped explicitly and recoverably, never silently.

Transient slowness is absorbed by the WAL and its retention window. "Too slow" means a
subscriber's cursor has fallen far enough behind that the next record it needs is about to be
truncated out of the retained window. When that happens:

- **Best-effort notice:** a `TopicLagged` signal (reason + oldest-retained sequence number) is
  sent on the subscriber's control channel and the connection is closed.
- **Reliable signal on resubscribe:** however a subscriber dropped, when it reconnects and asks
  for a sequence number older than the oldest retained record, the publisher clamps the start
  forward and tells it *where the stream now begins* (`accepted_from_seq_no` greater than
  requested). A reconnecting subscriber is by definition reading, so this signal always lands.

Data loss is therefore always **explicit and recoverable** (reconnect and accept the gap),
never a silent hole.

### D5 — A separate control channel keeps signals off the data path.

Each subscriber opens **two** TCP connections to the publisher's single port: a **data** channel
(bulk record delivery) and a **control** channel (rare out-of-band signals — lag warnings, the
periodic truncation cursor, heartbeats). The control channel is kept nearly empty so its socket
never backs up, giving prompt out-of-band delivery even when the data connection is saturated.
Each connection carries a `role` field (`Data` or `Control`) and a shared `subscriber_id`; the
publisher correlates the pair by id. The two connections are **one logical subscription** — if
either drops, the pair is torn down.

### D6 — Acks survive, but only as a coarse truncation cursor.

The one thing a publisher still needs from a subscriber is **how far it has consumed**, so it can
safely reclaim old WAL segments. So `TopicAck` is not on the hot path: a subscriber reports its
cursor *periodically* (every N records, or on disconnect), and the publisher truncates the WAL
only up to the slowest subscriber's acknowledged position. Retention is bounded additionally by a
per-topic window so one wedged subscriber cannot pin the log forever (see D4).

### D7 — Only the leader publishes; on demotion, subscribers are dropped to rediscover.

A publisher runs inside an HA component and publishes only while it holds leadership. A non-leader
that receives a subscribe request answers `TopicNotLeader` and disconnects, so a subscriber learns
which endpoint is live without a separate discovery service. On losing leadership a publisher drops
all subscribers (`drop_all_subscribers`); they reconnect and rediscover the new leader. The WAL and
topic identity are unaffected by the role change.

### D8 — Topic identity is generated from the DSL; policy is hand-written.

Topics are a **recognised catalog**, not free-form strings. The split:

- **Identity** — a topic's name and its member message types (by pdu id) — is declared in the DSL
  and **generated** into the topic registry. It cannot drift from the pdu ids because it is
  derived from the same schema that defines them.
- **Policy** — per-topic retention window, lag threshold, page size, listener port — is
  hand-written configuration, validated at load against the generated registry.

A publisher's membership test (`is_member(pdu_id)`) therefore comes straight from the generated
catalog: a record is served on a topic if and only if its pdu id is a declared member.

---

## Publishing — worked example: the Matching Engine Publisher (MEP)

The **MEP** (`applications/matching_engine_publisher/`) is the reference publisher. It tails the
order flow and republishes it on two topics — `orders` (new-order and cancel PDUs) and
`execution_reports` (ERs) — for downstream consumers that do not sit on the low-latency core
path.

The MEP owns one `TopicPublisher` per topic over a **shared WAL**, and implements
`TopicPublisherHost` so the publishers can send and disconnect through the owning
`ApplicationThread`. The essential wiring:

1. **Construct one publisher per topic**, giving each its topic name, its membership predicate
   (from the generated catalog), and the WAL directory:

   ```cpp
   orders_publisher_(*this, to_string(Topic::orders), pdu_in_topic(Topic::orders), wal_directory);
   er_publisher_(*this, to_string(Topic::execution_reports), pdu_in_topic(Topic::execution_reports), wal_directory);
   ```

2. **Route inbound subscribe requests by topic name** to the matching publisher; fan `on_ack`,
   `on_connection_writable`, and `on_connection_lost` to both (each no-ops on a connection it
   does not own).

3. **On every appended record, notify both publishers** so an idle subscriber's cursor wakes and
   streams it:

   ```cpp
   orders_publisher_.notify_record_appended(seq_no, pdu_id);
   er_publisher_.notify_record_appended(seq_no, pdu_id);
   ```

   Each publisher ignores a pdu id that is not a member of its topic, so the same call is safe on
   both. A live record must be **in the WAL before** `notify_record_appended` — catch-up and live
   delivery share the one cursor path (D2).

4. **Track leadership.** On a role change the MEP calls `set_leader(true/false)` on both
   publishers; publishers start non-leader and only begin serving once leadership is adopted, and
   `drop_all_subscribers()` runs on leader→follower (D7).

Everything else — per-subscriber cursors, page batching, backpressure, retention, the
data/control pairing — is inside `TopicPublisher` and is the same for any publisher. To publish a
**new** topic you declare it in the DSL, add a `TopicPublisher` for it, and route its subscribe
requests; you do not touch the streaming, pacing, or retention logic.

---

## Subscribing — worked example: `topic_probe`

`topic_probe` (`applications/topic_probe/`) is a tiny diagnostic subscriber — the minimal way to
watch a topic live — and it is the reference for how to write one. It connects to a publisher's
topic port, subscribes to one topic, and prints each record as a structured, decoded dump.

A subscriber subclasses `TopicSubscriberThread` and overrides **one** method,
`on_pubsub_message()`. The base class owns all the boilerplate: connecting to the publisher, the
subscribe handshake, routing inbound topic PDUs, dedup, and periodic acking. `topic_probe`'s whole
subscriber is:

```cpp
class ProbeThread : public pubsub_itc_fw::TopicSubscriberThread {
  protected:
    void on_pubsub_message(const pubsub_itc_fw::EventMessage& message) override {
        print_record(topic_name(), message.seq_no(), message.pdu_id(),
                     message.payload(), static_cast<size_t>(message.payload_size()));
    }
};
```

Key points a subscriber author needs:

- **The record is a borrowed, zero-copy view valid only for the duration of the call.** The
  payload points into the framework's buffers; a subscriber that needs to keep the bytes beyond
  the callback must copy them. `topic_probe` decodes and prints within the call, so it copies
  nothing.
- **`from_seq_no` selects the starting point.** `0` replays everything the publisher still retains
  and then streams live; a higher value starts nearer the head. Replay and live are one continuous
  stream to the subscriber (D2) — there is no separate "I am now live" event to handle.
- **Decoding a record is a pdu-id dispatch to the generated view.** `topic_probe` switches on
  `message.pdu_id()` to the matching generated `...View`, decodes it into a `BumpAllocator` arena,
  and prints the generated `to_string`. Because the dump is generated from the same DSL that
  defines the wire format, it cannot drift from the schema:

  ```cpp
  case app::ExecutionReport::message_pdu_id:
      decoded = decode_and_dump<app::ExecutionReportView>(payload, payload_size);
      break;
  ```

- **`TopicNotLeader` is handled for you.** If the endpoint is a follower, the base class logs that
  it is awaiting failover; point the probe at the leader's port. A production subscriber would
  reconnect to the other endpoint.

To run it:

```bash
topic_probe orders                       # replay retained history, then stream live
topic_probe execution_reports --from-seq-no 9000
```

Default ports are per topic (`orders` → 11040, `execution_reports` → 11041); override with
`--host`/`--port` for a secondary. Stop with Ctrl-C.

---

## The wire protocol, at a glance

The topic PDUs are defined in the DSL alongside the application messages. A subscriber and a
publisher exchange:

| PDU | Direction | Purpose |
|-----|-----------|---------|
| `TopicSubscribeRequest` | subscriber → publisher | Names topic, `subscriber_id`, `from_seq_no`, and channel `role` (Data/Control). One is sent on each of the two connections. |
| `TopicSubscribeAck` | publisher → subscriber | Confirms the subscription and reports `accepted_from_seq_no` — where the stream actually begins (may be clamped forward; see D4). |
| `TopicPage` | publisher → subscriber | A batch of records on the data channel (D3). |
| `TopicAck` | subscriber → publisher | The periodic truncation cursor (D6). |
| `TopicNotLeader` | publisher → subscriber | The endpoint is not the leader; reconnect to the leader (D7). |
| `TopicLagged` | publisher → subscriber | Best-effort out-of-band lag notice on the control channel (D4). |

Because the identity of every topic and the pdu ids of its members are generated from the DSL,
these PDUs and the topic catalog are always in step with the messages they carry (D8).

---

## See also

- [WAL](wal.md) — the retained backlog that pub/sub streams from and replays.
- [Serialisation DSL](serialisation_dsl.md) — how records and topic PDUs are encoded; the
  generated views and `to_string` dumps.
- [Sequencer](../applications/sequencer_app.md) and
  [WAL and High Availability](wal_and_ha.md) — the order flow the MEP republishes, and the
  leadership model a publisher runs under.
- [Reactor](reactor.md) — the event loop that drives the writable-pacing and connection paths.
