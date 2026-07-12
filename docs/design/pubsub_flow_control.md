# Pub/sub flow control, backpressure & the control channel — design {#pubsub_flow_control}

**Status:** Decided in discussion 2026-07-11. Resolves requirement dimension **D4** (fanout &
backpressure) and the slow-consumer half of **D5**. Companion to
[pubsub_requirements.md](pubsub_requirements.md) (which frames the six dimensions) and
[dsl_topic_catalog.md](dsl_topic_catalog.md) (topic identity). **Not yet implemented.**

## Context

The first-cut `topics.dsl` sketched **ack-per-page** delivery: `TopicPage` carries
`page_number`/`total_pages`, the subscriber sends `TopicAck` after each page, and the publisher
sends the next page on receiving the ack. That is treated as a draft to challenge. On reflection,
given we run over **TCP** with a **per-publisher WAL**, per-page acks are not justified.

## Decisions

### F1 — Delivery is streamed, paced by the TCP socket, not by acks.

- **Reliability / ordering:** TCP already guarantees no loss / no duplication / in-order within a
  connection. No ack or NAK is needed on the data path. (NAKs are what raw *lossy* multicast
  needs; TCP removes that problem.)
- **Backpressure:** the socket itself paces. The publisher pushes records to a subscriber as fast
  as its socket accepts them; a slow subscriber's socket fills → a non-blocking send returns
  `EAGAIN` → the publisher stops sending to *that* socket and resumes on writability (`EPOLLOUT`),
  reading the next record straight from the WAL. **The WAL is the backlog**; a lagging subscriber
  costs the publisher a cursor (an int), not RAM. Because a reactor subscriber
  reads-then-processes-then-reads, its socket-drain rate reflects its *processing* rate, so TCP
  backpressure genuinely tracks how fast it consumes — not merely network speed.
- **Per-page acks are rejected:** in a window-of-one design they add a full round-trip of latency
  *per page* and double message volume, for no reliability or flow-control benefit.

This matches the reliable-multicast reference: the publisher sends at its own rate; retention
absorbs slack; consumers do not positively-ack every message.

> Implementation note: the exact "send until EAGAIN, resume on EPOLLOUT" handling depends on the
> reactor's outbound-flush / backpressure API (see `RawBytesBackPressureIntegrationTest` and the
> earlier TLS EPOLLOUT fix). Confirm that API at implementation time.

> **Page batching (perf, 2026-07-12).** The first cut sent exactly one record per
> `on_connection_writable()`, so throughput was bounded by one epoll writable round-trip *per
> record* -- `topic_pubsub_bench` measured ~120 K records/sec regardless of payload size (the cost
> was the round-trip, not bandwidth). `TopicPublisher::pump_data` now batches up to
> `max_records_per_page` records (default 256) into a single `TopicPage` per writable, bounded by
> an encoded-payload budget kept well under the 64 KiB outbound slab / decode arena. Still one page
> per writable, so it stays socket-paced; the round-trip is just amortised over the batch. Measured
> lift: ~120 K -> ~5.7 M records/sec (~47x), ~8.2 us -> ~0.18 us per record. When only one record is
> available (the live 1-at-a-time path) a page carries one, so batching never adds latency. The
> `TopicPage` wire format already carried a record *list* plus `page_number`/`total_pages`, so this
> was a `pump_data` change only, no protocol change.

### F2 — `TopicAck` demoted to a periodic truncation cursor.

The one thing the publisher still needs from a subscriber is **how far it has consumed**, so it
can safely trim old WAL segments (retention). This is coarse: the subscriber reports its cursor
**periodically** (every N records / every T / on disconnect), NOT per page. Off the hot path.

### F3 — Slow-consumer handling: terminate past a lag threshold, with explicit recovery.

Transient slowness is fine (absorbed by the WAL + retention window). "Too slow" = the
subscriber's cursor lags far enough that the next record it needs is about to fall out of the
retention window. Fire the threshold **slightly before** actual truncation, so there is still a
clean recovery target. Then:

- **Best-effort notice:** send a `TopicLagged` event (reason + oldest-retained seq_no) and close
  gracefully with a timeout. On a single connection this queues *behind* the data backlog, so a
  wedged consumer may never see it — hence "best-effort". (Delivered promptly via the control
  channel, F4.)
- **Reliable signal is on resubscribe:** however the consumer dropped, when it reconnects and
  asks for a cursor older than the oldest retained record, the publisher tells it it has a **gap**
  and where the stream now starts (`accepted_from_seq_no` > requested). A reconnecting consumer is
  by definition reading, so this always lands.

Data loss is therefore **explicit and recoverable** (reconnect; accept the gap or pull a
snapshot), never silent. The lag threshold and retention window are per-topic **policy** (R3
TOML), not mechanism.

### F4 — A separate control channel: a second TCP connection to the SAME port.

To deliver signals promptly even when the data connection is backed up, use a **second, physically
separate TCP connection** kept nearly empty (its socket never backs up). Real out-of-band
delivery, without TCP urgent-data's fragility.

- **TCP urgent / OOB rejected:** it is a 1-byte in-stream marker (`SIGURG` / `EPOLLPRI`), not a
  real channel, and RFC 6093 discourages it (inconsistent urgent-pointer semantics, middlebox
  mangling). SCTP and QUIC have real multiplexed streams but are out of scope for now (SCTP is a
  *transport*, not a security protocol — poor middlebox/OS support; QUIC is a larger undertaking).
- **Single listener port (decided).** The subscriber opens **two** connections to the publisher's
  one port. Each is classified by a **`role` field added to `TopicSubscribeRequest`** —
  `role ∈ {Data, Control}` — carrying the same `subscriber_id`. The publisher correlates the pair
  by `subscriber_id` (`subscriber_id → { data_conn, control_conn }`). This reuses the existing
  "awaiting `TopicSubscribeRequest`" state; we only add the role.
- **Bidirectional (decided).** The control channel carries: publisher→subscriber lag warnings and
  `TopicLagged`/termination; subscriber→publisher the periodic truncation cursor (F2) and a
  heartbeat. All control chatter stays off the data path.
- **Lifecycle:** the two connections are **one logical subscription** — if either drops, tear the
  pair down. Handle the two arriving in either order, with a timeout if one never appears.
- Works with TLS unchanged (each connection does its own handshake).

## Open questions (still to settle)

- **Lag-threshold semantics:** a single threshold = the retention boundary, or a separate, tighter
  threshold that fires earlier? (Leaning: a configurable threshold that fires *before* truncation.)
- **A soft WARNING stage** before the hard terminate? (Undecided.)
- **Control-channel arrival timeout:** what to do if only the data channel connects (proceed
  data-only? reject? wait?).

## Implementation sequence (proposed)

1. **DSL:** add a `TopicChannelRole` enum `{Data, Control}` and a `role` field to
   `TopicSubscribeRequest` in `topics.dsl`; regenerate.
2. **Streamed delivery (F1/F2):** replace `TopicPublisher`'s burst replay with EPOLLOUT-paced
   streaming from the WAL (send next-after-cursor on writable / on a new record; per-subscriber
   sent-cursor + idle flag). Demote `TopicAck` to periodic truncation-cursor handling.
3. **Control channel (F4):** role-based connection classification + `subscriber_id` correlation;
   the control PDUs (`TopicLagged`, lag warning, heartbeat, periodic cursor).
4. **Slow-consumer policy (F3):** lag-threshold check → `TopicLagged` + graceful close;
   gap-on-resubscribe.
5. **Tests:** backpressure rung (a slow acker must not stall a fast co-subscriber);
   termination + recovery.

**Deferred:** a larger send window (pipelining) if throughput ever needs it; a snapshot mechanism
for gap recovery.
