# Refusing orders the venue cannot process {#ha_order_acceptance}

**Status: steps 1 to 3 of 5 built, 2026-08-28.** It addresses [BUG-0009](../bug_list.md#bug_0009),
which stays open until the member is told — the condition now travels from the sequencer to the
gateway, and the gateway still acknowledges every order it is given. See
[Implementation order](#ha_order_acceptance_steps).

## The problem

When the matching engine connection drops, the sequencer commits each order to the WAL and defers
forwarding it:

```
SequencerThread: no matching engine connected -- order seq=59678842 WAL-committed,
forward deferred until an ME reconnects (recovered via WAL replay on ME promotion)
```

**That policy is right for a brief failover.** The order is durable, and a promoted matching engine
replays the WAL and picks it up. Three things about it are not.

**The assumption can stop holding, and nothing notices.** A matching engine was promoted, did
reconcile, and then died two minutes later. The sequencer went on deferring for another five
minutes, waiting for a recovery that could no longer happen because no matching engine existed at
all.

**It is logged at INFO, once per order — 1,087,912 times.** A million lines saying the venue is
degraded, at the level used for routine progress.

**Nothing reaches the member.** The sequencer knew for seven minutes that there was no matching
engine. The gateway kept accepting orders and acknowledging them, reporting `dropped=0` throughout,
and the member saw no difference. **The sequencer has the knowledge and the gateway has the member
relationship, and there is no path between them.**

## Deferring is cheap for the venue and expensive for the member

Worth stating plainly, because it explains where the harm actually falls and therefore what the
limits are protecting.

A deferred order costs the venue almost nothing: `release_pdu_payload` is called and the handler
returns, so nothing is retained in memory. The order is in the WAL and that is enough.

The member is in a different position entirely. It has been **acknowledged**, so as far as it and
its risk systems are concerned the order is live and working. It may hedge against it. It cannot
cancel it, because a cancel needs the same matching engine. Every second of deferral widens a gap
between what the member believes and what is true.

So the limits below are not about protecting the venue's memory. **They bound how far a member's
picture of its own position is allowed to drift from reality.**

## What is built

### 1. The sequencer escalates rather than repeats

A deferred order no longer logs. Instead the sequencer counts them and records when the condition
began, and emits a **rate-limited WARNING** naming the count and the age. On recovery, one INFO
saying how many orders were deferred and for how long — which is the line an operator wants and
which no amount of per-order logging provided.

### 2. The sequencer tells the gateways

A new PDU, `OrderAcceptance` (127), from the leader to every gateway it holds a connection to. The
sequencer already opens those connections, so there is no new channel.

Sent **on transition** in both directions, and **repeated while degraded**, so that a gateway which
connects during an outage learns the state rather than inheriting a default of "fine".

### 3. The gateway refuses, and says why

While the venue is not accepting, a `NewOrderSingle` is answered with an **ExecutionReport carrying
`OrdStatus=Rejected`** and a reason. That is the FIX-correct answer: the member gets an ordinary
order lifecycle response it already handles, and its risk systems see the order as dead rather than
pending. A `BusinessReject` would be read by many members as a protocol fault rather than an order
outcome.

**Cancels are refused too**, and that deserves saying out loud because it sounds wrong. A cancel
needs the matching engine exactly as an order does. Accepting one the venue cannot act on would
repeat this bug in a worse place: a member believing it had cancelled would be more dangerously
wrong than one believing it had traded.

### 4. The health line gets a clock

`GW-PROGRESS` is emitted per *N* orders accounted, so when accounting stalls the reporting stalls
with it. Across the incident it went silent for **2 minutes 19 seconds** and then caught up in three
lines inside 0.2 seconds. **A line driven by progress cannot report the absence of progress.**

It is emitted on a timer as well, and it reports the gap between `nos_received` and `accounted` —
which already carries "accepted and going nowhere" and is currently thrown away. `dropped=0` stays
true and stops being the only thing an operator can watch.

## When deferring becomes refusing

**Age first, with a count as backstop.**

| | |
|---|---|
| Age | the condition has lasted longer than a failover plausibly takes |
| Count | more orders have been deferred than a member should be allowed to be wrong about |

Age is the honest measure, because it is the member's exposure that matters and exposure is
measured in time. The count is a backstop for the case age alone handles badly: a burst can defer
tens of thousands of orders in the seconds *before* the age threshold trips.

The age threshold must clear a **normal** failover, or the venue rejects orders during routine
recovery that members currently survive. The matching engine pair uses a 15-second peer heartbeat
timeout, then promotion, reconnection and WAL reconciliation. So the threshold is set comfortably
above that, not at it.

**Both are constants with their reasoning beside them**, not configuration, for the same argument
made in [Inbound sequence checking](../fix/inbound_sequence_checking.md): a figure no operator has
a reason to change is a field to keep in step for nothing. If one ever does, the sequencer's
configuration is where it goes.

## Automatic, and automatic to resume

The venue refuses on its own and resumes on its own when a matching engine returns.

This is **not** the graduated, operator-involved judgement that
[BUG-0059](../bug_list.md#bug_0059) argues for. That one is about a member's behaviour, where being
wrong means wrongly locking out someone's trading connection. This is about the venue's own
capacity: there is no matching engine, the fact is not a matter of interpretation, and continuing
to accept orders is the harm.

Requiring an operator to re-enable acceptance was considered and rejected. It protects against a
flapping matching engine reopening the venue repeatedly — but it makes recovery depend on someone
being present, and BUG-0009 is precisely a case where nobody was watching for seven minutes. A
design whose safety rests on the watching that has already failed is not safer.

## What this does not solve

- **A matching engine that is connected but not working.** Everything here keys on the connection.
  An engine that accepts orders and does nothing with them looks healthy throughout, which is
  closer to [BUG-0010](../bug_list.md#bug_0010)'s territory.
- **Orders already acknowledged before the venue noticed.** They stay deferred and are recovered by
  WAL replay, as now. This bounds how many join them; it does not rescue the ones already there.
- **Telling the member when acceptance resumes.** Nothing pushes that; a member discovers it by
  sending an order that is not rejected. Worth revisiting if it proves awkward in practice.

## Implementation order {#ha_order_acceptance_steps}

Each step leaves the venue working.

1. **The sequencer's own accounting** — count, age, rate-limited WARNING, recovery line.
   **Done 2026-08-28.**

   `note_order_deferred` and `note_matching_engine_reachable` in `SequencerThread`, with a
   `steady_clock` because this measures an interval rather than naming a moment — a wall-clock
   adjustment mid-outage would otherwise change how long the venue believed it had been degraded.
   The warning interval is five seconds.

   Measured, both engines killed and one brought back:

   ```
   no matching engine reachable -- orders are being accepted and deferred, starting at seq=42009043
   still no matching engine after  6s --  41 order(s) deferred so far
   still no matching engine after 12s --  81 order(s) deferred so far
   a matching engine is reachable again after 19s -- 120 order(s) were deferred and are recovered by its WAL replay
   ```

   **120 deferred orders, four lines.** Before, that was 120 lines at INFO.

   **The recovery line was wrong first, and the way it failed is worth keeping.** It was reported
   from the forward path, so the venue noticed it had recovered only when the *next order* arrived.
   A venue that recovered while nothing was trading said nothing at all, leaving an operator with
   the last warning and silence — which is the same shape as the defect this is fixing. It is now
   reported when the engine reconnects.
2. **The health line** — timer-based emission and the accepted-versus-accounted gap.
   **Done 2026-08-28.**

   A recurring five-second timer calls `report_order_progress_on_timer`, which skips if the
   count-driven line has just spoken — so a busy venue is not reported twice and a quiet one is
   still reported at all. Both paths write through `emit_order_progress`.

   The line gains **`awaiting`**: orders taken from members and acknowledged for which no execution
   report has been accounted. It is appended rather than inserted, because `ha_test.py` and
   `perf_run.py` both read a prefix of it — a field may be added, the existing four may not be
   reordered.

   Measured, with both engines killed and thirty orders sent into the silence:

   ```
   GW-PROGRESS accounted=1 sent=1 dropped=0 nos_received=1  awaiting=0
   GW-PROGRESS accounted=1 sent=1 dropped=0 nos_received=31 awaiting=30
   GW-PROGRESS accounted=1 sent=1 dropped=0 nos_received=31 awaiting=30
   ```

   `dropped=0` is still true, and now it is no longer the only thing an operator can watch. The
   line also keeps being emitted while nothing progresses, which is the whole point: before this,
   `accounted` stuck at 1 never reached the modulo and the line stopped entirely.

   **It broke a scenario, and the way it broke is worth keeping.** `ha_test.py` scenario 18
   asserted that a gateway serving nobody logged *zero* `GW-PROGRESS` lines, using the line's
   presence as evidence of traffic. A timer makes that false: an idle gateway now emits one every
   five seconds, all zeros. The scenario reads the figures instead of counting the lines.

   The wording of that line is a declared test contract and the comment saying so is honoured here
   -- the format was appended to, never reordered. **The wording turned out not to be the whole
   contract.** When it is emitted mattered too, and nothing said so. Both comments now do.
3. **`OrderAcceptance` (127)** — carried and logged, acted on by nobody.
   **Done 2026-08-28.**

   The leader evaluates acceptance from the age and size of the current deferral, and sends the
   result to every gateway it holds a connection to: on transition in both directions, repeated on
   the same five-second interval as the warning while the venue is not accepting, and to a gateway
   at the moment it connects. The gateway records it and logs the changes; nothing refuses yet.

   The thresholds are `order_deferral_refusal_age` (45 seconds) and
   `order_deferral_refusal_count` (250,000), with the reasoning beside them in
   `SequencerThread.hpp`. Age bounds the outage in time; the count bounds it in volume, because at
   the peak rate measured on this venue the age alone would allow about 1.55 million orders to be
   accepted and not processed before it spoke. **The count is deliberately reachable inside a
   normal failover at peak rate** — a burst is exactly when the volume runs away.

   **The count counts orders, not members**, and the harm this document describes is per member. A
   venue has a few hundred to a few thousand comp ids, and one member holds several; each posts
   continuously, which is how tens of millions of orders a day come from a few thousand sessions.
   So a venue-wide total is a *proxy* for the thing that matters — how far any one member's
   picture of its own position has drifted. It stands in because the sequencer counts deferrals
   globally rather than per session.

   **A per-session count was considered and deferred, 2026-08-28.** Counting deferred orders per
   `SessionIdentity` would model the harm directly, and would stop a quiet member being refused
   because a busy one filled the venue — traffic is nowhere near uniform across comp ids. It was
   not taken because it adds per-session state to the sequencer for a threshold that the age
   almost always reaches first, and the count only has to be a sane bound on volume rather than an
   exact model of exposure. If the proxy proves too blunt, this is the change to make, and the
   threshold that comes with it is a figure to choose rather than derive.

   Acceptance is evaluated on every deferred order rather than on a timer. Only the *log* is
   rate-limited; a threshold crossed between two warnings takes effect when it is crossed. A venue
   with no traffic has nothing to refuse, so there is nothing a timer would discover.

   Measured, both engines killed and orders sent in bursts across the threshold:

   ```
   SequencerThread: still no matching engine after 132s -- 6 order(s) deferred so far
   SequencerThread: no longer accepting orders -- the outage has run longer than a failover
                    plausibly takes (6 order(s) deferred over 132s, thresholds 45s / 250000)
   FixOrderGatewayThread: the venue is no longer accepting orders -- 6 order(s) deferred over 132s
   ```

   The gateway's line lands **400 microseconds** after the sequencer's. Restarting an engine
   produced the matching pair in the other direction.

   **A follower contradicted the leader, and only measurement found it.** A gateway restarted
   during an outage was correctly told "not accepting", and two seconds later told "accepting
   again" — on a venue with no matching engine running at all. The gateway holds a connection to
   *both* sequencers and cannot tell which of them leads. A follower forwards nothing to a matching
   engine, so it never defers, so its state is permanently "accepting"; it was not wrong about
   itself, it was answering a question that was never about it. `send_order_acceptance` now returns
   early unless this instance leads.

   That guard creates two obligations, both met in `adopt_role`. **A newly promoted leader must
   broadcast**, because the gateways may still be holding what the previous leader said before it
   died, and silence would leave a refusal in place that nothing would ever lift. And **an
   instance adopting follower must clear its deferral bookkeeping**, because a deferral begun in
   a previous leadership would otherwise still be open — the recovery that would have closed it
   happened while this instance was not the one watching for it. Without that, a re-promoted
   instance comes back already refusing, with an age measured from an outage that ended long ago.
4. **The gateway refuses**, orders and cancels, with a rejected ExecutionReport.
5. **The scenarios**, written to fail first: a matching engine killed and not restarted must produce
   refusals rather than acknowledgements, and the health line must keep reporting while nothing
   progresses.

## See also

- [BUG-0009](../bug_list.md#bug_0009) — the defect, and the run that found it
- [BUG-0010](../bug_list.md#bug_0010) — the deferral policy assumes a promotion that will succeed
- [WAL and High Availability](wal_and_ha.md) — why a deferred order is durable in the first place
- [Inbound sequence checking](../fix/inbound_sequence_checking.md) — the same argument for constants over configuration

---

Back to [High availability](../availability/README.md).
