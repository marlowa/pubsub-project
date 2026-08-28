# Resend provenance: which number carried what {#ha_resend_provenance}

**Status: built, 2026-08-27.** It closed [BUG-0051](../bug_list.md#bug_0051),
[BUG-0052](../bug_list.md#bug_0052) and [BUG-0053](../bug_list.md#bug_0053), which came from one
missing fact between them.

**Read [Session binding](session_binding.md) first** if the messages named here — `SessionUnbound`,
`SessionBoundAck`, `SessionSequenceUpdate` — are not already familiar. This document assumes the
protocol they belong to and only adds a field to it.

**One thing was built differently from the design.** Retention is a named constant,
`fix_common::seq_num_ranges::max_remembered`, rather than configuration — see
[Retention](#ha_resend_retention). A working gateway-local mechanism was written on 2026-08-27 and
deliberately reverted; the reasons are at the end, because they are the argument for the design
below rather than against building anything.

## The problem, in one example

This is the run that found it, on 2026-08-27, with the real numbers. A member had gone away and
come back, and was short of twenty messages.

The venue's side of the conversation, from the gateway log:

```
16:51:22.158874  FIX OUT  8=FIXT.1.1|35=A|34=1002|...      <- Logon, number 1002
16:51:32.206291  FIX OUT  8=FIXT.1.1|35=0|34=1003|...      <- Heartbeat, number 1003
16:51:32.206879  ResendRequest BeginSeqNo=983 -- resending 983..1003, will resume at 1004
16:51:32.223220  resend complete -- 21 report(s) resent, no gap left
```

Read the third line carefully. The member asked for numbers **983 to 1003**. That is twenty-one
numbers, and the venue filled all twenty-one with execution reports out of its log.

**But two of those numbers had not held reports.** Number 1002 held the Logon and number 1003 held
the heartbeat, and both had already gone out — the first two lines above. The venue had no record
of that, so it put a report on each of them. Number 1003 was used twice, for two different
messages, ten milliseconds apart.

The member's engine counts every message it is handed, whatever number is on it. It had received
the Logon and the heartbeat, then twenty-one reports: twenty-three messages where the venue had
issued twenty-two numbers. So the member finished the resend expecting **1005** while the venue
stood at **1004**. Ten seconds later the venue sent its next heartbeat:

```
Fatal  Message Sequence too low, received: 1004 expected: 1005 - will logoff
```

**Every message-level property of that resend was correct.** Twenty-one real reports, all inside
the range asked for, every one marked `PossDupFlag=Y` and stamped `OrigSendingTime`, the range
closed where the venue said it would. The resend reported success. The session was dead ten seconds
later, and nothing in the exchange said why.

## Why the venue could not do better

To answer a `ResendRequest` correctly you must know **what each number in the range carried**,
because FIX asks for two different things depending on the answer:

- a number that held an application message — an execution report — is **resent for real**, marked
  `PossDupFlag=Y` so the member knows it may be a duplicate;
- a number that held a session-level message — a Logon, a heartbeat, a reject — is **not resent at
  all**. It is skipped, by sending a `SequenceReset-GapFill` that tells the member "nothing you need
  was in there, move your expected number to here".

The venue knew the reports: they are in the sequencer's write-ahead log, each stamped with the
session it belongs to. It knew where the numbering had reached. **It did not know the mapping
between them** — which report went out as which number — so it could not tell the two cases apart,
and filled every number in the range with a report.

## What it does now

The gateway records the outbound number each execution report goes out on. That record travels to
the sequencer and comes back when a gateway takes the session on, so it survives the gateway that
made it. Answering a resend then becomes mechanical:

- a number the record covers held a report → replay that report onto it;
- a number the record does not cover → gap-fill it, in runs, so a quiet spell costs the member one
  `SequenceReset` rather than one per heartbeat.

For the range above the venue now resends nineteen reports for 983 to 1001 and gap-fills 1002 to
1003, so the member ends the resend expecting 1004 — the number the venue will actually send next.

**"Not covered" means one thing whether the number held a heartbeat or is simply older than the
venue still remembers.** Both are numbers the venue cannot produce a message for, and both are
gap-filled. That is why one record answers the whole question and there is no second mechanism
about how far back the venue can vouch for.

Measured, in `ha_test.py` scenario 22, which places a heartbeat inside the gap deliberately: 18
reports replayed, one gap-fill inside the range at `NewSeqNo=1003`, a terminating gap-fill leaving
the member expecting 1013 — the venue's next number — and the session still up after the heartbeat
that follows.

## Summary of the design

| | |
|---|---|
| The missing fact | which outbound FIX number carried each execution report |
| Who can record it | the gateway, at the moment it assigns the number |
| Where it must live | the sequencer's session state, so it survives a gateway failover |
| How it travels | incrementally on `SessionSequenceUpdate` (126); returned on `SessionBoundAck` (122) |
| Protocol change | one field on `SessionReplayRequest`: how many recent reports to skip |
| What is retained | the most recent *K* numbers per session, *K* tied to the WAL's retention |
| Uncovered numbers | gap-filled, never guessed at — one rule for "was not a report" and "too old" |
| Tests | `ha_test.py` scenarios 22, 23 and 40 — each failed on this until it was built |

## Why the venue is in this position, and it was a deliberate trade

A conventional FIX acceptor keeps an **outbound message store**: every message it sends, retained
and indexed by `MsgSeqNum`. A resend is then a read from that store, and the question above never
arises. That is what fix8 does -- `Session::handle_resend_request` calls `_persist->get(begin,
end, ...)` -- and what QuickFIX and the commercial engines do.

This venue deliberately has no such store, and `FixSession.hpp` says so:

> What is deliberately NOT held here is a store of the messages sent. Recovering them is the
> sequencer's job, from its WAL, because the reports may have been sent by a different instance
> of this gateway entirely.

That trade is sound and should be kept. A per-instance store dies with its instance, which is
exactly the case gateway HA exists for; putting the reports in one replicated place instead is
the better structure. **What was not priced is that the WAL preserves the reports and not the
numbering laid over them.** The store was doing two jobs, and only one of them was replaced.

## Why the symptom was intermittent

Worth knowing, because it is why this survived as long as it did and why a run can look fine.

A `SequenceReset-GapFill` **sets** the member's expected number outright rather than counting one
on; fix8 does `_next_receive_seq = NewSeqNo - 1` and then the same unconditional increment every
message gets. So a resend that happens to end in a gap-fill repairs the drift it has just caused,
and the session survives with nobody any the wiser. The session dies only when the reports exactly
fill the range and no terminating gap-fill is emitted — the `no gap left` case in the log above.

Measured on 2026-08-27: a range deliberately over-filled by 3883 reports left the session **alive**,
because the trailing gap-fill corrected it. So the reliable symptom was never a dead session. It was
a member holding reports under numbers that never carried them — and since the sequencer returns the
*most recent* reports, after a failover those are recent reports presented as the old missing ones.
A wrong record of its own session, which neither side can detect.

## The design

**Record which outbound numbers carried a report.** That is the primary fact. Everything the
resend needs is derived from it, and nothing else has to be maintained alongside it:

- A number the record covers carried a report, and the report is replayed onto it.
- A number the record does not cover did not carry a report, or is older than the venue keeps.
  Either way the venue cannot produce what was there, so it is gap-filled. **The two cases need
  the same treatment, which is why one record answers both** and no second notion of "how far
  back this instance can vouch for" is needed.
- Runs of uncovered numbers are gap-filled together, so a quiet spell costs the member one
  `SequenceReset` rather than one per heartbeat.

The gateway is the only component that can record it. The sequencer never sees the FIX numbering,
and the WAL is numbered by the venue's own sequence.

### Naming the reports, not counting them {#ha_resend_naming}

Recording the numbers is not by itself enough, because of the way the gateway currently asks for
the reports: it sends a count, and the sequencer returns *the most recent* that many. For a
tail request those are the right ones. For a bounded request in the middle of a session's history
they are not — [BUG-0053](../bug_list.md#bug_0053), reproduced 2026-08-27 by `ha_test.py`
scenario 40: a member asking for numbers 100 to 149 was sent the fifty most recent reports
wearing those numbers, and every other property of the reply was correct.

The record makes the fix small. The gateway knows how many report-carrying numbers lie **above**
the range being replayed, and those are by definition recent, so they are always covered. So it
can say precisely which reports it wants without either side knowing the other's numbering:

> Skip the *s* most recent reports for this session, then give me the next *n*, oldest first.

where *s* is the count of covered numbers above the range and *n* is the count of covered numbers
within it. `SessionReplayRequest` gains one field for *s*; `max_records` already carries *n*. The
sequencer's sliding window becomes a window offset by *s* rather than always anchored at the most
recent record, which is a small change to a loop that already maintains one.

That removes the "most recent that many" heuristic entirely. The venue stops inferring which part
of the stream a member means and is told.

### Placement

At replay time the gateway walks the requested range once, holding the returned reports in the
order the sequencer sent them:

1. At the current number, if the record does not cover it, extend a run until it does or the range
   ends; emit one `SequenceReset-GapFill` over the run and advance past it.
2. Otherwise take the next report and send it on this number with `PossDupFlag=Y` and
   `OrigSendingTime`.
3. At the end of the range, gap-fill whatever remains up to where the session had reached.

Step 3 already exists. Steps 1 and 2 replace "fill every number with a report".

### Representation and where it lives

Per session, the covered numbers as **ranges** — reports arrive in runs, interrupted only when the
member goes quiet and a heartbeat takes a number, so a burst of ten thousand orders is one range.
A session trading steadily and heartbeating every thirty seconds accumulates on the order of a
thousand ranges across a day.

It lives in **the session state the sequencer already holds**, beside `outbound_seq_num`, and
travels on `SessionUnbound` (121), `SessionBoundAck` (122) and `SessionSequenceUpdate` (126).

This is the whole point, and the reason a gateway-local record is not enough. A resend is served
by whichever instance holds the session **now**, which after a failover is not the instance that
sent the messages being asked about. A record held only in the gateway is empty in precisely the
case it is most needed.

**Shipped incrementally on `SessionSequenceUpdate`, a range at a time as each closes**, rather than
in one piece at unbind. Two reasons: a gateway that dies uncleanly never sends an unbind, and that
is the failover case this exists for; and a single-shot record would make `SessionBoundAck` carry
the session's whole history at every logon. The tail between the last update and an unclean death
is lost, and those numbers are then simply uncovered — gap-filled rather than guessed at. The venue
already accepts an approximation there: it biases the resumed number high after an unclean death
for the same reason.

### Retention {#ha_resend_retention}

**Bounded to the most recent *K* outbound numbers per session**, and *K* is tied to the
sequencer's replay cap rather than chosen separately.

**As built, *K* is the constant `fix_common::seq_num_ranges::max_remembered` and not
configuration.** The design called for a configured value; plumbing one through two components'
TOML for a figure that has to be revisited the moment BUG-0048 lands buys nothing, and a constant
in the header both components already include is harder to set inconsistently. When the WAL's
retention is settled the two are tied there, and that is the point at which configuration is worth
having. The record exists to serve replays; keeping
provenance for numbers whose reports the sequencer would no longer return buys nothing, and the
two limits disagreeing would produce a venue that believes it can serve a range and then cannot.

Numbers that fall out of the window become uncovered and are gap-filled, which is the same path as
a number that never carried a report. Nothing degrades silently: uncovered is uncovered.

The sequencer's own retention of the reports is unbounded today
([BUG-0048](../bug_list.md#bug_0048)) and will not stay so. **That is the one ordering constraint
on this work:** *K* cannot be settled before the WAL's retention is, so either BUG-0048 goes first
or *K* is introduced as a configured value with the tie to WAL retention made when BUG-0048 lands.

### What a member gets

Exact resends in every case the venue can serve, including after a failover, with no outbound
message store anywhere. Where the record does not reach, the venue gap-fills rather than guessing,
and the member recovers those orders through an Order Mass Status Request — which is not
implemented, and which [gateway_ha.md](gateway_ha.md) already names as wanted.

### What it does not fix

The reports themselves must still be in the WAL. If they have been truncated, provenance says
which numbers wanted them and the venue still cannot produce them; the answer is the same
gap-fill. Provenance makes the venue **honest** about what it can serve. It does not extend what
it can serve.

## Relationship to the binary gateway

The sequencer half of this design is not FIX. `SessionReplayRequest`, the session state that
crosses a failover, and the contract change under
[Naming the reports](#ha_resend_naming) are all protocol-independent, and
[BUG-0046](../bug_list.md#bug_0046) -- in-flight report recovery for the binary gateway -- will
have to consume the same contract. **The two should be designed together**, or the contract gets
specified twice and the second specification discovers the first was not quite right.

**The binary gateway will not have this defect, and that is a design instruction rather than an
observation.** BUG-0051 exists because FIX numbers every message, session-level ones included, so
a venue answering a resend has to reconstruct a numbering it did not retain. A binary recovery
built on a cursor -- "every report after this one" -- has no numbering laid over the stream and
cannot have the bug. If a proposed binary mechanism turns out to be capable of reproducing it,
that is evidence the mechanism has copied FIX's difficulty without FIX's reason for it.

The practical consequence is about testing. A binary client, which the project controls
completely, can exercise the shared machinery precisely -- and cannot reach the FIX-specific
behaviour: `PossDupFlag` placement, `OrigSendingTime`, gap-fill semantics, and the member engine's
counting, which is what turned BUG-0051 from a numbering error into a dead session. That part
needs a FIX client, and `f8test` is the only one the project has.

## What it is, in the code

| | |
|---|---|
| The record | `fix_common::SeqNumRange` and `fix_common::seq_num_ranges`, shared by both components |
| Recorded | `FixOrderGatewayThread::send_execution_report_to_session`, as each report takes its number |
| Held | `FixSession::report_seq_nums`, and `SequencerThread::SessionSequenceState::report_seq_nums` |
| Shipped | `SessionSequenceUpdate` (126) every 2s, and `SessionUnbound` (121); incremental, from a watermark |
| Restored | `SessionBoundAck` (122), which seeds the gateway's copy |
| Used | `handle_resend_request` for the counts, `gap_fill_unreplayable_run` for placement |

**What the failover case looks like now.** Scenario 23 kills the gateway holding a session and
brings the member back on the surviving instance, which never sent the messages being asked about.
It resends **1000 real reports**. Before, it filled the range with whatever the sequencer returned.

**And what it still cannot do.** A session that lives and dies inside one reporting interval never
reports at all, so the sequencer holds no record and the surviving instance gap-fills the whole
range rather than guessing. The member keeps its session, its numbering and its orders, and loses
those reports. That is this design working, not failing: the alternative is a venue that guesses,
which is what BUG-0051 was.

## The mechanism that was built and reverted

On 2026-08-27 the complement was recorded instead: a per-comp-id list of the numbers that carried
something the venue could not replay, held on the gateway thread, with a floor below which the
instance declined to vouch. It worked -- scenario 22 passed all seven assertions, scenario 23's
failover leg passed, and a negative run confirmed the assertions bit.

It was reverted, and the reasons are worth keeping because they are what this design answers:

- **It recorded a derived fact rather than the primary one.** Enough to gap-fill and not enough
  to do anything else, so it could never grow into the real answer -- it would have to be
  replaced rather than extended.
- **It needed a second concept, the floor, to paper over the first one's lifetime.** A record
  keyed on the primary fact needs one concept: a number is claimed by a report or it is not.
- **It leaked.** Entries were held per comp id and never removed, which is the shape of BUG-0048
  in a new place.
- **It degraded silently.** Trimming an over-long list moved the floor, so a member asking about
  older numbers quietly received a gap-fill instead of its reports, with only a log line to say
  so.

The gateway-local half of it would still be needed here -- something has to record the number as
it is assigned -- but keyed on reports rather than on their complement, and shipped to the
sequencer rather than kept.

## See also

- [Session binding](session_binding.md) -- the protocol this adds a field to: how a session outlives its connection
- [BUG-0051](../bug_list.md#bug_0051) -- the defect, and how it was found
- [Gateway High Availability](gateway_ha.md) -- session identity across a failover, and the
  in-flight report recovery this is part of
- [Sequence numbers and gaps](../fix/sequence_numbers_and_gaps.md) -- what the protocol requires
  of a resend

---

Back to [High availability](../availability/README.md).
