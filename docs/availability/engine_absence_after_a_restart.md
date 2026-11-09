# Measuring the engine's absence after the whole venue restarts {#engine_absence_after_a_restart}

**Status: agreed, not built.** This is the design for [BUG-0207](../bug_list.md#bug_0207). Option A was agreed with
Andrew on 9 November 2026 (section 6).

## 1. The problem

When the whole venue has been down, sequencers included, the sequencer that leads afterwards measures how long no
matching engine could trade, and if that is longer than the limit on automatic recovery
(`[recovery] automatic_recovery_limit_seconds`, 10 seconds) it voids the orders it holds, cancels the book and halts
trading ([voided_commands.md](voided_commands.md), section 5.3). It measures from a stamp in a small file beside its
own WAL, `engine_routed_at`, which each sequencer writes about once a second while it knows of an engine it routes
orders to (`EngineRoutedStamp`).

A sequencer that was itself down for longer than the limit, while its peer led and an engine traded, holds a stamp from
before it went down. If the whole venue then goes down and comes back, and this sequencer leads, it measures the absence
from its own old stamp, and cancels the book and halts for an absence that did not happen. It can lead only if its WAL
is as long as its peer's, so only if nothing was written while it was down: a market with orders resting and no command
for longer than the limit, which is a quiet market, not an unusual one. This is a breach of V11: a loss of resilience
became a loss of service.

## 2. What already exists

- Every WAL record carries the wall-clock time it was written, and both sequencers' WALs hold the same records.
- A sequencer leads after a restart only if its WAL is at least as long as its peer's.
- The engines count every record a catch-up sends them, and require the count to match (R-0101). They apply commands,
  and count but do not apply the execution reports the WAL also holds; a record of any other kind is dropped with a
  Warning and not counted, so a new kind must be counted as reports are, or every catch-up past it would fail its count.

## 3. Options

**A. The stamp goes into the WAL.** While it routes orders to an engine, the leading sequencer writes a small record,
`EngineRouted`, into the WAL whenever no record has been written for one second, carrying nothing but the record's own
time. In a busy market nothing extra is written, because every order is a record written while an engine is routed; in
a quiet market one record a second is. After a whole-venue restart the leader measures the absence from the time of the
newest record written while an engine was routed: the newest `EngineRouted`, or the newest command released to an engine,
whichever is later.

This removes the fault at its cause. While a sequencer is down, its peer goes on writing `EngineRouted` records, so the
stale sequencer's WAL is shorter and it cannot lead; and if it caught up before the venue went down, it holds them. The
file `engine_routed_at` is then not needed, and is removed. The engines count `EngineRouted` records as they count reports,
and apply nothing; the publishers pass over them; `wal_replay_check` ignores them.

**B. Ask the peer.** The leader takes the later of its own stamp and its peer's, the peer's carried in a message the two
sequencers exchange when they connect. This needs no new record, but it cannot help when the peer is not running at the
restart, which is exactly when the stale sequencer is most likely to lead alone; and the status messages the bug entry
first proposed for this are no longer sent by anything.

## 4. The scenario that shows the fault first

The bug entry asks for a scenario before any fix. The secondary sequencer is killed. Orders rest on the book and nothing
is sent for longer than the limit, so the primary leads and an engine trades throughout, writing nothing. The whole venue
is then stopped, and restarted with the secondary started first, so that it leads. Before the fix, the secondary measures
the absence from its old stamp and cancels the book and halts; the scenario requires that it does not. Run against the
code as it is now, it must fail.

## 5. Steps, and how each is known to be right

1. The scenario in section 4, failing against the code as it is.
2. The `EngineRouted` record and its writing in a quiet spell, with unit tests: written after a second without a record
   while an engine is routed; not written while none is; not written while records are being written anyway.
3. The engines count it and apply nothing; a catch-up across `EngineRouted` records completes, which an existing catch-up
   scenario run with a quiet spell shows; with the counting removed on purpose, it fails.
4. The leader measures from the newest record written while an engine was routed; the file is removed.
5. The scenario in section 4 passes, and the full high-availability suite, with the WALs cleared first.

## 6. Decision

### Decision 1. A or B? (Choose one.)

Agreed: **A**. It makes the WAL, which both sequencers agree on, the record of when an engine was last routed to, so
a sequencer that missed that period either cannot lead or has caught up with it; B depends on the peer being there at
exactly the moment it is least likely to be. The cost of A is one small record a second in a quiet market, nothing in a
busy one, and one more record kind for the engines to count.
