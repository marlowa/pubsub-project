# Sequencer Design

## Role

The sequencer puts every command the venue accepts into one order. It gives each order and cancel a
sequence number, writes it to its write-ahead log, sends it to the matching engine, and delivers the
engine's execution reports back to the member that placed the order. It is the one place every
gateway instance of every protocol reports to, so it also holds what the venue must remember about
each member session across a gateway's death: where the session is connected, how far its message
numbering has got, and which session placed each order.

Two sequencers run, a leader and a follower. Only the leader numbers commands and sends them to the
matching engine. The follower writes the leader's records into its own log, so the two logs are
identical, and it keeps the reports the engine sends, in case it has to take over. Which of the two
leads is decided by leases; see
[Deciding leadership by majority, with leases](../availability/majority_leases.md).

This document describes what the sequencer does with commands and reports. The ports, the
configuration and how to run it are in [Sequencer Application](sequencer_app.md). How leadership
changes hands, and what each side guarantees while it does, are in the documents under
[availability](../availability/README.md).

---

## A command from a gateway

A gateway sends each order and cancel to both sequencers, wrapped in a `WalRecord` envelope (PDU id
103). The envelope carries the command itself (a `NewOrderSingle` or `OrderCancelRequest`, left
encoded), the identity of the session that sent it (its comp id and gateway protocol, and the gateway
instance), its `ClOrdID`, and the time the gateway read it from the member.
`SequencerThread::on_framework_pdu_message()` decodes only the envelope; the command inside is never
decoded or copied.

What happens next depends on the sequencer's role.

**A follower discards it.** A follower writes its log only from its leader's records, so the gateway's
copy is thrown away before a number is taken.

**The leader**, in order:

1. **Refuses to sequence a command twice.** A gateway sends a command again, marked as sent again,
   when nothing answered it, which happens across a change of leader or when its connection to the
   sequencer is closed and opened again. If the log already holds that command, it is not sequenced
   again, and its answer comes from the matching engine. A command not marked is checked as well
   while the gateway has two connections open, because it may be the original arriving on the old
   connection after its marked copy arrived on the new one. The check uses a table of every command
   identifier in the log, reserved for 200 million commands (`[commands] identifiers_reserved`) and
   filled from the log in the background when the sequencer starts. See
   [Commands during a change of leader](../availability/commands_during_a_change_of_leader.md).
2. **Numbers it and stamps it.** The envelope is given the next sequence number and the current wall
   clock time (`wall_time_ns`), and the leader's epoch. That one time is used wherever the time of
   sequencing is needed: it is written to the log, and it is the `TransactTime` the matching engine
   puts on the reports, so a report produced again from the log carries the same time as the first.
3. **Writes it to the log** (`append_envelope_to_wal`). What is written is the whole stamped
   envelope, so the record in the log, the record sent to the follower and the record sent to
   downstream subscribers are the same bytes.
4. **Remembers which session placed it**, under its sequence number. What is remembered is the
   session's identity, not the connection it arrived on, because by the time a report is ready the
   session may be connected somewhere else.
5. **Sends it to the matching engine, at once or after the follower holds it.**
   - With high availability off, it is sent at once.
   - With a follower connected and keeping up, the record is sent to the follower and the command is
     held until the follower acknowledges the record (`WalAck`). Only then is it sent to the engine,
     so the engine never acts on a command the follower does not hold. Up to 16,384 commands are held
     in memory; beyond that they wait in the log and are sent from there, in order.
   - With no follower keeping up, the leader runs alone: it tells a voter that the follower may not
     lead, and holds commands until that statement is confirmed. See
     [A follower behind does not lead](../availability/a_follower_behind_does_not_lead.md). The leader
     decides the follower is not keeping up when the follower disconnects, when no acknowledgement
     has come for 100 milliseconds while commands are waiting, or when the store of held commands
     fills.
   - With no matching engine connected, the command is not sent. It is in the log, and whichever
     engine acts next catches up from the log before it acts (below). The sequencer logs how long
     commands have been waiting, not a line for each one.
6. **Sends the record to the follower and to every downstream subscriber**, whether or not an engine
   took the command.

---

## A report from the matching engine

The engine sends each execution report to both sequencers, wrapped in a `WalRecord` envelope that
carries the sequence number of the command it answers and the identity of the session the order
belongs to.

**A follower keeps it** (`KeptReportStore`), in case it takes the lead before the leader has
forwarded it. The records the leader sends the follower say how far through the engine's reports
the leader has forwarded, and the follower discards what it holds up to that point.

**The leader forwards it**, in `forward_report_from_engine()`:

1. **Finds the session.** Ordinarily the report's sequence number finds the session that placed the
   order. A report with no originating sequence number, such as a cancel a promoted engine issues
   for an order it inherited, or a report kept while this sequencer was following, is routed by the
   session identity on its envelope.
2. **Gives the report a sequence number of its own and writes it to the log**, sends it to the
   follower and streams it to downstream subscribers, as for a command. An order can have several
   reports, so they cannot share the order's number.
3. **Waits for the follower before releasing it** (`pending_er_`). A report is released to the
   gateway only once the follower has acknowledged the record of the order it answers, so a member is
   never told of an execution the follower does not know the order for. A report with no originating
   order waits instead for the acknowledgement of its own record. When no follower keeps up, the
   waiting reports are released once a voter has confirmed that the follower may not lead, together
   with the held commands.
4. **Sends it to wherever the session is connected now**, found from the session's identity at the
   moment of sending. A session connected nowhere has its report dropped, and the gateway asks for
   undelivered reports when the session binds again (below).

The report is marked as a possible repeat when the engine says it is one, and always when it is a
report the follower kept, because the new leader cannot know whether its predecessor forwarded it.
The gateway writes that mark as `PossResend`.

---

## Sessions

A session is a comp id and a gateway protocol together. The gateways tell the sequencers about each
session with these messages, sent on the order connection:

| Message | Meaning |
|---|---|
| `SessionBound` | The session has logged on at this gateway instance, on this connection. The sequencer answers with `SessionBoundAck`: how far the session's message numbering had got in each direction, and which of its outbound message numbers carried reports, so that a gateway that has never seen the session can carry on its numbering and answer a resend |
| `SessionUnbound` | The session has gone from this connection |
| `SessionSequenceUpdate` | How far the session's numbering has got. Never lowered, so a late update from the instance that has just lost the session cannot wind it backwards |
| `UndeliveredReportsRequest` | Send again the reports produced while the session was connected nowhere |
| `SessionReplayRequest` | Send the session's reports from a point in its history, to answer a member's resend request; the reply ends with `SessionReplayComplete` |

See [Gateway High Availability](../availability/gateway_ha.md) and
[Session binding](../availability/session_binding.md).

The leader also tells every gateway whether the venue is accepting orders (`OrderAcceptance`), which
depends on whether a matching engine is reachable. Only the leader sends it. See
[Order acceptance](../availability/order_acceptance.md).

---

## The matching engine's catch-up

An engine catches up from the leader's log before it acts, whether it has just started or has just
been promoted. It sends `MePositionRequest` with the position it has reached, the leader streams it
every command after that position, and then sends `MePositionAck` saying where it stopped and how many
records it sent, so the engine can check it received them all. See
[Matching Engine](matching_engine.md).

A new leader also asks the leading engine, with `EnginePositionQuery`, which commands it has
received, and sends it any command the log holds that the engine never got.

---

## Downstream subscribers

Other components follow the log over the sequencer's subscriber listener (`[wal_subscriber]`).
The matching engine publishers do so today. A subscriber sends `WalSubscribeRequest` naming the last
sequence number it holds, is sent every record after it from the log, and then receives each new
record as it is written, acknowledging them with `WalAck`. See
[Topology](../framework/topology.md) and the documents under [pubsub](../pubsub/README.md).

---

## The log

The log is the framework's write-ahead log (`Wal`, `WalWriter` and `WalReader` in
`libraries/pubsub_itc_fw`): a series of memory-mapped segment files, `wal_NNNNNN.log`, each 4 MiB
(`[wal] segment_size`). Each entry is:

```
[ header (24 bytes) | payload | CRC32 (4 bytes) ]

header:  magic 0xFEEDFACE (4) | payload_size (4) | record_id (8) | filler (8)
payload: wall_time_ns (8) | pdu_id (2) | PDU payload
```

`record_id` is the sequence number. For the sequencer, `pdu_id` is always that of `WalRecord`, and
the PDU payload is the encoded envelope. The CRC32 covers the header and the payload. Nothing calls
`fsync` on each write: the operating system writes the mapped pages to disk in the background, which
survives the death of the process but not of the machine. The copy in the follower's log is what
survives the loss of a machine.

A snapshot of the position the log has reached is written every 30 seconds
(`[wal] snapshot_interval_seconds`), so a restart does not have to read the whole log to find where
it ends. The log keeps all of its history: nothing is removed from it during the day
([BUG-0048](../bug_list.md#bug_0048)). See
[The write-ahead log](../durability/wal.md) and [WAL and High Availability](../availability/wal_and_ha.md).

**The follower's log is written on two threads.** The reactor's thread writes each record as it
arrives on the connection to the leader, through a handler installed on that connection
(`install_peer_wal_inline_handler`), and sends the acknowledgement straight back, so neither needs a
turn of the sequencer's thread. A record that handler does not write, because the connection has
data waiting to be sent, or the two logs are not yet known to agree, or records passed on earlier
have not all been dealt with, is passed to the sequencer's thread instead. Every write goes through
`ReplicatedRecordWriter`, which writes a record only if it is the next one the log needs, under a
lock, so the log holds every record once and in order whichever thread delivers it. The leader
writes its own log on its own thread alone.

---

## Replay mode

`sequencer <logfile> <config.toml> --replay` reads the whole log, connects to the matching engine
only, waits until both its connections to the engine are up, and sends the engine every order and
cancel in the log, each carrying its original time of sequencing, so the engine produces the same
reports with the same times. Reports in the log are not sent; they are outputs, not inputs. The time
each order was read by its gateway is not sent, so replayed orders do not appear in the latency
metrics.

---

## See Also

- [Sequencer Application](sequencer_app.md) — ports, configuration, startup
- [WAL and High Availability](../availability/wal_and_ha.md) — the commit and replication rules, and failover
- [Change of sequencer leader](../availability/change_of_sequencer_leader.md) — what a new leader does
- [Follower log repair](../availability/follower_log_repair.md) — bringing a rejoining follower's log into agreement with its leader's
- [Socket Communications](../framework/socket_comms.md) — the inline handler mechanism
