# Replay — Notes Toward a Design

**Status: NOT DESIGNED. These are constraints and findings to start from, not a plan.**

Replay as a first-class `pubsub_itc_fw` capability, backed by the WAL. Raised 2026-08-06 as a
conversation rather than a task; these notes are from working through it on 2026-08-08 and exist so
the conclusions are not rediscovered.

---

## Decide which replay first — they are different problems

| | needs | status |
|---|---|---|
| **State reconstruction** — "what was the book at 14:03?" | only the events that change replicated state | **already works.** HA reconciliation and the session-slice resend both do it |
| **Faithful re-execution** — "reproduce that bug exactly" | everything: timers, socket outcomes, thread interleaving, randomness | hard, and probably not worth it |

If the answer is state reconstruction — and it should be — then most of the remaining work is
**access**: cursors, filters, a tool. Not determinism. The WAL already holds the events by
construction.

Say which one is wanted in the design, because every question below is answered differently by the
two.

---

## What the framework already gets right

**Time is injected, not read.** `WallClock` is a virtual interface with `now_ns()`, and there are
no direct `system_clock::now()` or `steady_clock::now()` calls on the matching engine or sequencer
processing paths. A `ReplayClock` implementing that interface is a small class.

**Time arrives with the event.** The matching engine does not ask what time it is; it is told:

```cpp
void handle_new_order_single(const NewOrderSingleView&, int64_t seq_no,
                             int64_t sequenced_at_ns, const SessionIdentity&);
```

**The sequencer collapses concurrency into a total order before the WAL is written.** This is the
structural advantage and it is easy to under-value. Replay from the WAL never has to reproduce
thread interleaving, because the ordering is a *recorded fact* rather than something to re-derive.

That last point is why a WAL beats a packet capture for this, and it is not a matter of capturing
more: a capture records **transport**, a WAL records **decisions**. Packets whose processing order
was decided by scheduling nobody can reproduce do not become replayable by capturing more packets.

---

## Timers: replay consequences, not causes

The question that dissolves most of the difficulty is not "how do I replay a timer" but:

> **Does this timer's effect pass through the sequencer?**

- **It does** — a timeout that cancels an order. The cancel is its own WAL record. Replay the
  *consequence*; the timer never enters it and neither does the clock.
- **It does not** — heartbeats, inactivity teardown, retry backoff. These never touched replicated
  state, so there is nothing to reconstruct.

**Audit item before building anything:** find any state-changing effect that bypasses the
sequencer. That is the only residue of this rule, and it is a short audit.

---

## Traps

### Do not resurrect the venue

Timers are kernel `timerfd`, and the framework core reads `steady_clock::now()` directly in four
places — all connection lifecycle (`InboundConnection::last_activity_time_`,
`InboundConnectionManager`'s inactivity check, `OutboundConnectionManager`'s retry backoff). Those
fire on **real elapsed time regardless of any injected clock**.

So if replay means *"start the components and feed them the WAL"*, inactivity teardown will fire
part-way through and connections will drop mid-replay. If replay means *"feed recorded events into
a consumer"*, none of that code is in the path.

Prefer the second. It is also the cheaper one.

### Any `now() - recorded_timestamp` is broken under replay

Already met once, in the metrics work: `order_round_trip_nanoseconds` deliberately drops the
`gateway_ingress_ns` stamp on the WAL replay path, because a replayed order's ingress time is hours
stale and would invent a tail-latency spike at every failover.

**Treat that as a rule rather than one clever fix.** Any code subtracting a recorded timestamp from
a live clock produces nonsense during replay, and the nonsense is plausible-looking, which is worse.

---

## See Also

- [WAL and High Availability](../availability/wal_and_ha.md) — reconciliation, which is state reconstruction already
- [Pub/Sub](../pubsub/pubsub.md) — topic replay from a cursor, the nearest existing capability
- [Metrics](../operations/metrics.md) — the round-trip stamp and why replay drops it
