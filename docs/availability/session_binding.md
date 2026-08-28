# Session binding: how a session outlives its connection {#ha_session_binding}

A member's FIX session is not its TCP connection. The connection dies often — a network blip, a
gateway restart, a gateway being killed — and the session is supposed to carry on across that,
with its sequence numbering intact and its resting orders untouched. This document describes the
protocol the gateways and the sequencer use to make that true, and what each message in it is for.

It is the layer underneath [Resend provenance](resend_provenance.md) and the implementation half
of [Gateway High Availability](gateway_ha.md). Read this first if either of those does not make
sense.

## What a session is

**A session is a comp id and a protocol.** Not a socket, not a process, not a gateway instance.
`fix_common::SessionIdentity` is that pair, and it is what the venue files everything under:
a member's resting orders in the matching engine's book, its destination in the sequencer, and its
sequence numbering.

It used to be the triple `(protocol, instance, connection id)`, which names *a socket on a
process*. That dies when the socket does, is renumbered on reconnect, and is not even the same
number at the member's backup gateway — so a returning member could see its orders but not cancel
them, because they were filed under an address that no longer existed.

The triple is still how a report is **delivered**. It is no longer what a session **is**.

## Why the sequencer holds the session's state

Because it is the only component that every instance of every gateway talks to.

A gateway knows where a session's numbering has reached while it holds the session, and knows
nothing about it before or after. When a member reconnects — to the same instance or, after a
failure, to a different one — the gateway taking it on has no idea what number to use. Somebody has
to remember across that change, and the sequencer is the only candidate that is still running.

So the gateways **report** and the sequencer **remembers**. That direction matters and is not
arbitrary: the sequencer cannot derive the numbering itself. A FIX outbound sequence number counts
*every* message the member is sent, and the heartbeats, Logons, Logouts and session-level rejects
among them never come near the sequencer.

## The messages

Six PDUs, defined in `libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl`.

| Id | Message | Direction | What it says |
|---|---|---|---|
| 120 | `SessionBound` | gateway → sequencer | this session is now mine, at this connection |
| 122 | `SessionBoundAck` | sequencer → gateway | here is what the venue remembers about it |
| 126 | `SessionSequenceUpdate` | gateway → sequencer | here is where it has got to (every 2s) |
| 121 | `SessionUnbound` | gateway → sequencer | its connection has gone; here is the final state |
| 123 | `SessionReplayRequest` | gateway → sequencer | give me this session's reports, so I can answer a resend |
| 124 | `SessionReplayRecord` | sequencer → gateway | one report from that replay |
| 125 | `SessionReplayComplete` | sequencer → gateway | that is all of them |

### The ordinary life of a session

```
   member                gateway                        sequencer
     |                      |                               |
     |--- Logon ----------->|                               |
     |                      |--- SessionBound (120) ------->|   "CLIENT/FIX is on
     |                      |                               |    instance 1, conn 9"
     |                      |<-- SessionBoundAck (122) -----|   "next number is 4065,
     |                      |                               |    and these numbers
     |<-- Logon (34=4065) --|                               |    held reports"
     |                      |                               |
     |--- NewOrderSingle -->|                               |
     |<-- ExecReport -------|                               |
     |         ...          |--- SessionSequenceUpdate ---->|   every 2 seconds
     |                      |          (126)                |
     |                      |                               |
     |    [connection drops]|--- SessionUnbound (121) ----->|   "conn 9 is gone;
     |                      |                               |    final state is ..."
```

**The Logon reply cannot go out until `SessionBoundAck` comes back**, because it is itself a
numbered message and the gateway does not yet know what number it should carry. Between binding and
the ack the session is authenticated but not established, and nothing may be sent to the member.

### `SessionUnbound` (121), which is the one to understand

It says: *the connection carrying this session has gone.* It does **not** say the session is over.
The identity and its orders outlive the connection — that is the whole point of keying on the
identity — so the sequencer stops addressing reports at a connection that no longer exists, and
keeps everything else.

It carries three things beyond the identity, and each is there for a reason that was learned:

- **The connection id.** So a late unbind cannot tear down a newer binding. A member that
  reconnects fast enough for its new `SessionBound` to overtake the old connection's
  `SessionUnbound` — two gateways racing, which is exactly what a failover produces — would
  otherwise be unbound a moment after binding, and would sit there receiving nothing while
  appearing perfectly connected. The sequencer ignores an unbind that does not name the current
  binding.
- **`outbound_seq_num`.** Where the member's numbering had reached, so the next gateway to hold
  the session carries on from there instead of restarting it at 1. A member whose numbers reset on
  every reconnect sees a break it cannot reconcile.
- **`report_seq_nums`.** Which of those numbers held an execution report. This is the newest field
  and the subject of [Resend provenance](resend_provenance.md); the short version is below.

### The two ways a session ends, and why they differ

**Cleanly.** The connection closes, the gateway sends `SessionUnbound`, and the sequencer's record
is exact. A returning member is resumed at precisely the right number.

**Uncleanly.** The gateway is killed. It sends no unbind at all, so the sequencer's record is
whatever the last `SessionSequenceUpdate` left — up to two seconds stale.

This is why the update on a timer exists, and the way it was learned is worth stating. Reporting
only at unbind meant a killed gateway reported **nothing**, so the sequencer said *"sequence state
is new"* and started the returning member at 1. With a client whose own store had also restarted,
both sides sat at 1, no gap was visible, and the member was silently resynchronised while thousands
of its orders were live on the book. It was told nothing.

So after an unclean death the sequencer resumes the member **deliberately high**: the last reported
number, plus the reports it forwarded since, plus a fixed allowance for the admin traffic it cannot
see. Too high and too low are not symmetrical errors. Too high leaves a **gap**, which the member
closes with a `ResendRequest` and the venue answers. Too low sends the member a number below what it
expects, which FIX requires it to treat as fatal — it drops the session, and no amount of replay
helps.

## Answering a resend

When a member notices a gap it sends a `ResendRequest`. The gateway cannot answer it from memory:
it keeps no store of the messages it has sent, deliberately, because the messages in question may
have been sent by a different gateway instance entirely.

So it asks the sequencer, whose write-ahead log already holds every execution report with the
session it belongs to stamped on it. `SessionReplayRequest` (123) names the session and how many
reports are wanted; the reports come back as `SessionReplayRecord` (124) PDUs; `SessionReplayComplete`
(125) ends it, and is sent even when nothing matched, because otherwise the gateway could not tell
"nothing to send" from "still coming".

**The WAL holds reports and nothing else.** Every other message the member was sent — the Logon,
the heartbeats, the rejects — also took an outbound sequence number, and none of them can be
produced again. FIX's rule is that those numbers are skipped with a `SequenceReset-GapFill` rather
than resent, and to obey it the venue has to know which numbers they were. That is what
`report_seq_nums` on the three session PDUs is for, and why it travels through the sequencer rather
than living in the gateway: after a failover the instance answering the resend is not the instance
that sent the messages. See [Resend provenance](resend_provenance.md).

## Where this lives in the code

| | |
|---|---|
| The identity | `applications/fix_common/SessionIdentity.hpp` |
| Gateway side | `FixOrderGatewayThread::announce_session_bound`, `announce_session_unbound`, `report_session_sequence_numbers`, `handle_session_bound_ack` |
| Sequencer side | `SequencerThread::handle_session_bound`, `handle_session_unbound`, `handle_session_sequence_update`, `handle_session_replay_request` |
| The remembered state | `SequencerThread::SessionSequenceState` |
| Tested by | `ha_test.py` scenarios 21, 22, 23 and 40 |

## See also

- [Resend provenance](resend_provenance.md) — which of a session's numbers held a report, and why a resend needs to know
- [Gateway High Availability](gateway_ha.md) — session identity, provisioning, and what a member sees across a failover
- [FIX sequence numbers, gaps and gap fill](../fix/sequence_numbers_and_gaps.md) — the protocol rules this serves

---

Back to [High availability](../availability/README.md).
