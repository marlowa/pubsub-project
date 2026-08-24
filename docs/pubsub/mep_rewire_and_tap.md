# Next pub/sub work: rewire the MEP onto TopicPublisher, and build TAP {#pubsub_mep_rewire_and_tap}

**Status (2026-07-12):** Part 1 (MEP rewire) **DONE**. Part 2 (TAP) still planned. The reusable
pub/sub components (`TopicPublisher`, `TopicSubscriberChannel`, `TopicControlChannel`) and the
flow-control design (F1–F4, `docs/pubsub/flow_control.md`) are done and tested.

## 1. Rewire the MEP onto `TopicPublisher` — DONE

Done as described below. Notes on how each point/wrinkle was resolved:
- The MEP now owns `orders_publisher_` and `er_publisher_` (one `TopicPublisher` each, topic name
  taken from `to_string(Topic::...)`, membership from `pdu_in_topic`, both over the shared `wal_`).
  It implements `TopicPublisherHost`. Inbound `TopicSubscribeRequest` is routed by `topic_name`;
  `on_ack` / `on_connection_writable` / `on_connection_lost` are fanned to both publishers (each
  no-ops on a connection it does not own — `on_ack` was made no-op-on-unknown for this). Live
  records call `notify_record_appended` on both. `on_connection_writable` was added.
- HA: `set_publishers_leader(true/false)` on role change; publishers start non-leader until leader
  is adopted; on leader→follower both `drop_all_subscribers()` (new `TopicPublisher` method).
- **Wrinkle A** resolved conservatively: `topic_truncate_wal` is a **no-op**. The MEP's WAL is
  shared by both topics and its retention is governed by the existing `wal_snapshot` (durability)
  timer, not by topic acks, so this is a no-behaviour-change choice. Proper ack-driven reclamation
  would need to truncate to the global min across BOTH publishers' subscribers — deferred.
- **Wrinkle B**: kept the two ports (orders / execution_reports). Routing is by `topic_name`, so
  the ports are effectively redundant now but harmless; no config change.
- Deleted the inline machinery (`publish_wal_record_to_topic_subscribers`, `send_topic_page`,
  `replay_wal_for_subscriber`, `disconnect_all_topic_subscribers`, the two registries, live-conn
  sets, `conn_to_topic_`). Verified: full unit (671) + integration (45) suites pass; MEP builds
  clean. NOT yet live-smoke-tested with a real subscriber (see below).

### Original plan (for reference)

The MEP (`applications/matching_engine_publisher/`) still has its own **inline** topic-publish
logic (`handle_topic_subscribe_request`, `handle_topic_ack`,
`publish_wal_record_to_topic_subscribers`, `send_topic_page`, `replay_wal_for_subscriber`, the
`orders_registry_`/`er_registry_`, the live-conn sets, `conn_to_topic_`). Replace it with the
extracted component. This is the payoff of the whole extraction (deletes duplicated logic and
gives the MEP F1 socket-paced streaming, the control channel, the lag policy, and truncation for free).

Steps:
1. **Own one `TopicPublisher` per topic** (`orders`, `execution_reports`), each with its
   membership predicate `pdu_in_topic(pdu, Topic::orders)` / `Topic::execution_reports` from the
   generated `topics_registry.hpp`, and the MEP's `wal_directory`.
2. **Implement `TopicPublisherHost`** on `MatchingEnginePublisherThread` (one impl serves both
   publishers): `topic_send_subscribe_ack/page/not_leader/lagged` -> `send_pdu`;
   `topic_disconnect` -> reactor Disconnect; `topic_request_writable_notification` ->
   `request_writable_notification`; `topic_truncate_wal` -> **see wrinkle A below**.
3. **Route inbound events.** On `TopicSubscribeRequest`, decode `topic_name` and dispatch to the
   matching publisher. For `on_connection_writable` / `on_ack` / `on_connection_lost` (which arrive
   by `ConnectionID`, not topic), just call **both** publishers -- each no-ops on connections it
   doesn't own. Add `on_connection_writable` override (the MEP doesn't have one yet).
4. **Live records:** `handle_wal_record_from_sequencer` already `wal_.append(...)`s; then call
   `notify_record_appended(seq_no, pdu_id)` on **both** publishers (each filters by membership).
   Remove the inline `publish_wal_record_to_topic_subscribers`.
5. **HA:** call `set_leader(role_ == leader)` on both publishers on every role change; non-leaders
   then reply `TopicNotLeader`.
6. **Delete** the inline topic machinery listed above and its members.

**Wrinkle A — shared-WAL truncation across two topics (IMPORTANT).** The MEP has ONE WAL holding
BOTH `orders` and `execution_reports` records. Each `TopicPublisher` computes `min_cursor` over
*its own* subscribers and would call `topic_truncate_wal` for *its* floor -- but truncating to the
orders floor could delete ER records a slow ER subscriber still needs (and vice versa). So the MEP
must truncate to the **global min across BOTH publishers' subscribers**. Options: (a) the MEP's
`topic_truncate_wal` ignores the per-publisher value and instead computes/keeps a running
`min(orders_floor, er_floor)` and truncates to that; or (b) expose a `min_cursor()` getter on
`TopicPublisher` and have the MEP truncate to the min of the two after any ack. Do NOT truncate to a
single topic's floor.

**Wrinkle B — port model.** The MEP first cut listens on two ports (one per topic). `TopicPublisher`
routes by `topic_name`, so a single port would suffice (matches the pub/sub "single port" design,
and is what the control channel already assumes). Decide: collapse to one port, or keep two. If
kept, a subscriber's data+control connections must both hit the same topic's port.

After the rewire: re-run the Java web fix-test-client end-to-end (login/place/cancel) to confirm no
regression, and consider a subscriber connecting to the MEP for a live smoke test.

## 2. Build TAP (the first real subscriber)

TAP is the first application to subscribe in the pub/sub sense. Per the recorded decisions, TAP
subscribes to **both** `orders` and `execution_reports` published by the MEP.

Steps:
1. New `applications/tap/` with a `TapThread : ApplicationThread` (+ config TOML, CMakeLists, wire
   into `applications/CMakeLists.txt`).
2. TAP opens, per subscribed topic, a **data** connection (via `TopicSubscriberChannel`) and a
   **control** connection (via `TopicControlChannel`) to the MEP's port, distinguished by the
   `role` field and correlated by `subscriber_id` (see the DualChannelSubscriberThread test as the
   working template). TAP implements `TopicSubscriberChannelHost` + `TopicControlChannelHost`.
3. **Record handling** (per the recorded TAP decisions): `orders` records -> forward to the
   enterprise bus via an abstract `BusPublisher` (`StubBusPublisher` for framework validation;
   Kafka/Pulsar later); `execution_reports` -> maintain an internal L3 book. TAP does NOT publish
   ERs to the enterprise bus.
4. **Gap/lag handling:** on a `TopicLagged` (control channel) or a gap-on-resubscribe
   (`accepted_from_seq_no > requested`), TAP logs the gap and resumes (accepting the loss or pulling
   a snapshot -- snapshot mechanism is future).
5. **HA:** TAP is a primary/secondary pair (arbiter-mediated), like the other components. Use
   `TopicNotLeader` from the MEP as the failover-redirect signal (client tries the other MEP
   endpoint).

## Related smaller tails (from F2/step-4/step-5)
- Move the `TopicAck` truncation cursor onto the **control** channel (F4 sub->pub); ack the tail
  (< `ack_interval`) on idle/disconnect so the last records can be reclaimed.
- Optional soft **warning** stage before the hard lag-drop.
- Actual sequencer-side snapshotting for gap recovery.
