# Resend provenance: which number carried what {#ha_resend_provenance}

To answer a FIX `ResendRequest` correctly, the venue must know what each outbound sequence number in
the range carried. This document describes how it knows: the FIX gateway records the number each
execution report goes out on, and the record lives with the session's state in the sequencer, so it
survives the gateway that made it.

**Read [Session binding](session_binding.md) first** if the messages named here — `SessionUnbound`,
`SessionBoundAck`, `SessionSequenceUpdate`, `SessionReplayRequest` — are not already familiar.

## What a resend must know

FIX asks for two different things, depending on what a number carried:

- a number that carried an application message, here an execution report, is **sent again for
  real**, marked `PossDupFlag=Y` so the member knows it may be a duplicate;
- a number that carried a session-level message, such as a Logon, a heartbeat or a reject, is **not
  sent again at all**. It is skipped with a `SequenceReset-GapFill`, which tells the member that
  nothing it needs was there and moves its expected number on.

The reports are in the sequencer's write-ahead log, each stamped with the session it belongs to, and
the gateway knows where the numbering has reached. Neither of those says which report went out on
which number, and without that the two cases cannot be told apart.

### What goes wrong without it

Suppose a member asks for numbers 983 to 1003, where 1002 carried the Logon and 1003 a heartbeat,
both already sent. A venue that fills the whole range with reports sends 21 reports, two of them on
numbers that carried something else. The member counts every message it is handed: having received
the Logon, the heartbeat and then 21 reports, it has counted 23 messages where the venue issued 22
numbers, so it ends the resend expecting 1005 while the venue stands at 1004. The venue's next
message carries 1004, which is lower than the member expects, and FIX requires the member to treat
that as fatal:

```
Fatal  Message Sequence too low, received: 1004 expected: 1005 - will logoff
```

Every message-level property of that resend was correct, and the session still died.

The failure is not always that visible. A gap fill *sets* the member's expected number outright
rather than counting one on, so a resend that happens to end with a gap fill repairs the count it has
just broken, and the session survives. What remains is a member holding reports under numbers that
never carried them, which neither side can detect. A test of resend must therefore check which
report arrived on which number, not merely that the session stayed up.

## What the venue does

**The gateway records which outbound numbers carried a report.** That is the one fact everything else
is derived from:

- a number the record covers carried a report, and that report is sent again on it;
- a number the record does not cover either carried a session-level message or is older than the
  venue remembers. Either way the venue cannot produce what was there, so it is gap-filled. The two
  cases need the same treatment, which is why one record answers both, and no second idea of "how
  far back this instance can vouch" is needed;
- a run of uncovered numbers is gap-filled with one `SequenceReset`, so a quiet spell costs the
  member one message rather than one per heartbeat.

For the example above, the venue sends nineteen reports for 983 to 1001 and gap-fills 1002 to 1003,
and the member ends the resend expecting 1004, the number the venue sends next.

The gateway is the only component that can record this: the sequencer never sees the FIX numbering,
and the log is numbered by the venue's own sequence.

### Naming the reports wanted

The gateway tells the sequencer exactly which of the session's reports it wants:

> Skip the *s* most recent reports for this session, then give me the next *n*, oldest first.

where *s* is the number of covered numbers above the range being answered and *n* the number of
covered numbers within it. Those above the range are recent, so they are always covered.
`SessionReplayRequest` carries *s* as `skip_most_recent` and *n* as `max_records`. A request for a
range in the middle of a session's history therefore gets the reports that were sent on those
numbers, not the most recent ones; neither side needs to know the other's numbering.

### Placing the reports

The gateway walks the requested range once, with the reports in the order the sequencer sent them:

1. At the current number, if the record does not cover it, extend a run until a covered number or
   the end of the range, send one `SequenceReset-GapFill` over the run, and move past it.
2. Otherwise send the next report on this number, with `PossDupFlag=Y` and `OrigSendingTime`.
3. At the end of the range, gap-fill whatever remains up to where the session has reached.

### Where the record lives, and how it travels

Per session, the covered numbers are held as **ranges**. Reports go out in runs, interrupted only
when the member is quiet and a heartbeat takes a number, so a burst of ten thousand orders is one
range, and a session trading steadily and heartbeating every thirty seconds collects on the order of
a thousand ranges in a day.

The record lives in **the session state the sequencer already holds**, beside the session's outbound
sequence number. That is the point of it: a resend is answered by whichever gateway instance holds
the session now, which after a failover is not the instance that sent the messages being asked
about. A record held only in the gateway would be empty in exactly the case it is most needed.

It is sent **a range at a time, as each range closes**, on `SessionSequenceUpdate` every two seconds
and on `SessionUnbound`, and comes back on `SessionBoundAck`, which seeds the gateway's copy. Sent in
pieces rather than at unbind, for two reasons: a gateway that dies without warning sends no unbind,
and that is the failover case this exists for; and a record sent whole would make `SessionBoundAck`
carry the session's whole history at every logon. The numbers between the last update and an abrupt
death are lost, and are then simply uncovered: gap-filled, not guessed at.

### How much is remembered

**The most recent 100,000 outbound numbers per session**, the constant
`fix_common::seq_num_ranges::max_remembered`, in a header both the gateway and the sequencer include,
so the two cannot be set differently. Numbers that fall out of it become uncovered and are
gap-filled, the same path as a number that never carried a report.

The record exists to serve replays, so the limit belongs with how long the sequencer keeps reports.
Nothing deletes the log today ([BUG-0048](../bug_list.md#bug_0048)); when how long the log keeps
reports is decided, this limit must be tied to it, or the venue would believe it can serve a range
whose reports it no longer holds.

## What a member gets, and what it does not

Exact resends in every case the venue can serve, including after a failover, with no store of
outbound messages anywhere. Where the record does not reach, the venue gap-fills rather than
guessing.

- **A report must still be in the log to be sent again.** Provenance says which numbers wanted a
  report; it cannot produce one the log no longer holds. It makes the venue honest about what it can
  serve; it does not extend it.
- **A session that begins and ends within one reporting interval** never sends its record to the
  sequencer, so an instance that takes it on after the gateway dies gap-fills the whole range. The
  member keeps its session, its numbering and its orders, and loses those reports.
- **A member that needs what was gap-filled** can only ask the venue what it holds, which the venue
  does not yet answer ([BUG-0089](../bug_list.md#bug_0089)).

## Why there is no outbound message store

A conventional FIX acceptor keeps a store of every message it has sent, indexed by sequence number, and
a resend is a read from it; fix8, QuickFIX and the commercial engines all do this. This venue
deliberately has none, and `FixSession.hpp` says so:

> What is deliberately NOT held here is a store of the messages sent. Recovering them is the
> sequencer's job, from its WAL, because the reports may have been sent by a different instance
> of this gateway entirely.

A store kept by each gateway instance dies with it, which is exactly the case gateway high
availability exists for; keeping the reports in one replicated place is the better structure. But
the log keeps the reports, not the numbering laid over them, and a store does both jobs. The record
described here is the second job.

*Considered and rejected: recording the complement,* the numbers that carried something the venue
could not send again, held in the gateway with a floor below which it declined to vouch. It records
a derived fact, enough to gap-fill and nothing more, so it could never grow into the full answer; it
needs a second idea, the floor, to cover the first one's lifetime; held per comp id and never
removed, it grows without bound; and trimming it moves the floor, so a member asking about older
numbers would quietly be given a gap fill instead of its reports.

## The binary gateway

The sequencer half of this is not FIX: `SessionReplayRequest`, the session state that crosses a
failover, and naming reports by skip and count are independent of protocol, and in-flight report
recovery for the binary gateway ([BUG-0046](../bug_list.md#bug_0046)) should use the same contract
rather than specify a second one.

**The binary gateway cannot have the numbering problem.** It exists because FIX numbers every
message, session-level ones included, so a venue answering a resend must know a numbering it does not
otherwise keep. A binary recovery built on a cursor, "every report after this one", has no numbering
laid over the stream. If a proposed binary mechanism could reproduce the problem, it has copied FIX's
difficulty without FIX's reason for it.

For testing, a binary client can exercise the shared machinery precisely, but not the FIX-specific
behaviour: where `PossDupFlag` goes, `OrigSendingTime`, gap-fill rules, and how the member's engine
counts. That needs a FIX client.

## In the code

| | |
|---|---|
| The record | `fix_common::SeqNumRange` and `fix_common::seq_num_ranges`, shared by the gateway and the sequencer |
| Recorded | `FixOrderGatewayThread::send_execution_report_to_session`, as each report takes its number |
| Held | `FixSession::report_seq_nums`, and `SequencerThread::SessionSequenceState::report_seq_nums` |
| Sent | `SessionSequenceUpdate` (126) every two seconds, and `SessionUnbound` (121), a range at a time |
| Restored | `SessionBoundAck` (122), which seeds the gateway's copy |
| Used | `handle_resend_request` for *s* and *n*, `gap_fill_unreplayable_run` for placing the gap fills |

## Tests

| Scenario | What it requires |
|---|---|
| 22, `resend_recovery` | A heartbeat placed inside the gap is gap-filled: 18 reports sent again, one gap fill inside the range, and a final gap fill leaving the member expecting the venue's next number, with the session still up after the heartbeat that follows |
| 23, `inflight_gateway_death` | The gateway holding a session is killed and the member returns on the other instance, which sent none of the messages asked about, and is sent the 1,000 real reports |
| 40, `bounded_resend` | A range in the middle of the session's history is answered with the reports sent on those numbers, not the most recent ones |

## See also

- [Session binding](session_binding.md) — how a session outlives its connection
- [Gateway High Availability](gateway_ha.md) — session identity across a failover, and the resend
- [Sequence numbers and gaps](../fix/sequence_numbers_and_gaps.md) — what the protocol requires of a resend
- [BUG-0051](../bug_list.md#bug_0051), [BUG-0052](../bug_list.md#bug_0052) and [BUG-0053](../bug_list.md#bug_0053) — the defects this answers

---

Back to [High availability](../availability/README.md).
