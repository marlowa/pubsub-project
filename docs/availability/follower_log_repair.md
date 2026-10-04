# Repairing a follower's log when it rejoins {#follower_log_repair}

## 1. What this document covers

The two sequencer instances each keep a write-ahead log. The leader writes every record, and sends each
one to the follower, which writes it to its own log and acknowledges it. While the follower stays
connected, its log is a copy of the leader's.

When the follower has been disconnected, or has restarted, or when it is an old leader rejoining
after a change of leader, its log is no longer a copy, and nothing makes it one again. This is
[BUG-0097](../bug_list.md#bug_0097), and part 4.5 of
[change_of_sequencer_leader.md](change_of_sequencer_leader.md) outlines a fix. This document sets out
that fix in full.

It also has to come first. Rule 11 ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md))
lets a leader say that its follower may lead again only once the follower holds every record the leader
holds, and today the leader cannot tell whether that is true for a follower that has rejoined.

## 2. What happens today

These are read from `SequencerThread.cpp`.

- **A rejoining follower skips what it missed.** When the two instances connect, each asks the other
  how far the sequence has reached (`StatusQuery`, `StatusResponse`). An instance whose own next
  sequence number is lower moves it up to the other's, and from then on receives only new records. The
  records it missed are never sent to it, so its log has a gap.
- **The follower writes whatever it is sent.** Replicated records are written by a handler that runs on
  the reactor thread as each one arrives (`install_peer_wal_inline_handler`), and by `handle_wal_record`
  on the sequencer thread for any the handler passes on. Both append the record under the sequence
  number it carries, with no check against what the log already holds.
- **An old leader keeps records its new leader does not have.** An instance that led, wrote records its
  follower never received, and then stopped leading, still holds them when it rejoins. The new leader
  may have written different records under the same numbers.
- **The leader counts the highest acknowledgement, not the highest unbroken one.** `peer_acked_through_`
  is the highest sequence number the follower has acknowledged, on any connection. Within one
  connection that is the same thing as "every record up to here", because records arrive in order. After
  a reconnection it is not: a follower with a gap acknowledges new records, and the leader then believes
  it holds everything up to its latest record.

The consequences are serious. A follower whose log has a gap, or holds records the leader does not, can
be elected. When it leads, the matching engine catches up from its log and misses the records in the
gap, and the publishers and the order activity recorder read a log that disagrees with the one the
venue acted on.

## 3. What must hold

**G5.** When an instance rejoins as a follower, its log ends up identical to its leader's.

And, for rule 11 and for the running-alone decision in part 4.2: **when the leader counts the follower as
having acknowledged record N, the follower holds exactly the leader's records 1 to N.**

## 4. The design

This is how Raft and Kafka repair a follower's log, and nothing in it is new. Kafka's form of it is the
closer fit, because, like this venue, it numbers records itself and keeps the leader's epoch beside them.

### 4.1 Each record says which leadership wrote it

`WalRecord` gains a field, `leader_epoch`: the epoch in which the leader that sequenced the record held
the lead. The follower stores records exactly as it receives them, so the field is in both logs.
Records written before the field existed read it as zero.

Each instance keeps in memory a table of the epochs that appear in its log and the first sequence number
written in each. A sequencer reads its whole log when it opens it, so the table is built then, and it is
kept up to date as records are appended and when a follower discards records (4.4). It changes only
once per change of leader, so it stays small. From it an instance can answer two questions without
reading its log: which epoch wrote record N, and which is the last record written in epoch E or
earlier.

The epoch of two records with the same number tells whether they are the same record. A leader writes
each sequence number once in its epoch, and a follower writes a record only as its leader sends it, so
two logs holding a record with the same number and the same epoch hold the same record, and, once the
logs have been repaired as below, the same records before it too.

### 4.2 A follower whose log has a gap trusts it only up to the gap

Logs written before this change can have gaps (section 2). When a sequencer opens its log it checks
that the sequence numbers run from 1 without a break. If they do not, it discards everything after the
last record before the first gap, logging at Warning how many records it discarded and which numbers
they were. The discarded records were all sent to it by a leader, so the leader still holds them and
sends them again (4.3).

### 4.3 Finding the last record on which the two logs agree

When a follower connects to its leader, or when an instance starts following a leader it is already
connected to, it sends `LogPositionRequest`: the sequence number of its last record and that record's
epoch.

The leader answers with `LogPositionReply`: the last record, at or below the follower's last record,
that the leader's log holds with an epoch no later than the follower's last epoch, and that record's
epoch in the leader's log. The follower discards every record after that one (4.4). If the record it is
left with has the same epoch as the leader said, the two logs agree up to it. If not, the follower asks
again with its new last record, and the answer moves further back. Each round moves the follower's last
record back, or its epoch back, so this ends; in practice it takes one round, or two after a change of
leader.

A follower whose log is empty sends zero, and the answer is zero.

Until the leader has answered a follower's request, it sends that follower no live records, and the
follower writes none. A live record sent before the two logs agreed would be written after a gap. On
the follower, the handler on the reactor thread declines every replicated record until the logs agree,
which passes it to the sequencer thread, and the sequencer thread discards it and logs that it did.

### 4.4 The follower discards the records after that point

This needs a new operation in the framework's write-ahead log, `Wal::truncate_after(seq_no)`. It finds
where record `seq_no` ends, overwrites the header of the entry after it with zeros so that a reader stops
there, deletes every later segment, and reopens the writer at that point; the sequencer trims its table
of epochs to match.
Only a follower calls it, and only before it writes anything the leader sends it. It runs on the
sequencer thread while the reactor thread's handler is declining every record (4.3), so the two never
write the log at the same time.

Discarding is safe. A record the follower holds that its leader does not was written by an old leader
and never reached a majority. Under part 4.2's option A the matching engine never acted on it, and no
member was answered for it, so the gateway still holds the command and sends it again under part 4.3.
The follower logs at Warning how many records it discarded and their numbers and epochs.

### 4.5 The leader sends the follower everything after that point

Having answered, the leader reads its own log from the start and sends the follower every record after
the agreed one, in order, and only then starts sending it live records. This is how the publishers and
the matching engine already catch up from the sequencer (`handle_wal_subscribe_request`,
`handle_me_position_request`), and it has the same cost: the sequencer thread reads the log, and the
order path waits while it does. Section 6 gives the cost and section 7 the open question about it.

The follower acknowledges each record as now. On answering, the leader sets `peer_acked_through_` to the
agreed record, replacing whatever it held from an earlier connection. From then on it is again "every
record up to here", because records arrive in order on one connection. Until the follower's
acknowledgements reach the leader's latest record, the leader treats the follower as behind: under part
4.2 it runs as if alone after 100 milliseconds without an acknowledgement, and under rule 11 it goes on
saying that the follower may not lead.

### 4.6 Taking the lead

A follower that takes the lead numbers new records from its highest record plus one, as part 4.1 already
does. With its log repaired, that is the leader's latest record that reached it. The step in which an
instance moves its next sequence number up to its peer's (`handle_peer_status_response`) is removed: it
is what creates the gap, and the repair makes it unnecessary.

## 5. Where this does not apply

**The matching engine pair** keeps no write-ahead log of its own commands: each engine catches up from
the sequencer before it acts. **The publishers** keep their own logs, which they fill from the
sequencer's by subscription, already starting from the position they hold.

## 6. What it costs

| Part | Cost on the ordinary path | Other cost |
|---|---|---|
| 4.1 epoch on each record | Four bytes per record | A table of epochs in memory, built while the log is read at startup |
| 4.2 gap check | None | A check of the log at startup, which reads it already |
| 4.3 finding the agreed record | None | One exchange, sometimes two, when a follower connects |
| 4.4 discarding | None | A new log operation, used only by a rejoining follower |
| 4.5 sending what it missed | The order path waits while the leader reads its log | Proportional to the size of the log, as the publishers' and the engine's catch-up are today |

## 7. Open question

**Reading the log on the sequencer thread.** The leader reads its whole log to find the records to send,
because the log has no index by sequence number. That is what the other two catch-ups do, and the
startup measurement recorded under [BUG-0107](../bug_list.md#bug_0107) shows the size of it: opening a
log of 5.5 million records, 1.4 GB, took 2.6 seconds with the files already in memory, and noticeably
longer straight after a build, when they were not. A follower that restarts late in a
50-million-order day would hold up the order path for the length of that read. Two ways to remove it, for
later, and not part of this design:

- read and send from a thread of its own, handing the leader the point where it stops so that live
  records follow without a gap;
- keep an index from sequence number to position in the log, so that the read starts at the agreed
  record.

This design takes the cost as the other catch-ups do, and the question applies to all three.

## 8. Tests

Each test must fail on today's code, shown before it is used to judge the change.

| Test | What it requires | Today |
|---|---|---|
| Scenario, new: a follower restarts | The follower is killed, the leader takes orders, the follower is restarted. Once it has caught up, its log holds the same records, in the same order, with the same contents, as the leader's | Fails: the follower's log has a gap |
| Scenario, new: an old leader rejoins | The leader's sends to its follower are blocked with `libblock_sends_to_ports.so`, so that it holds records the follower does not. It is killed, the follower takes the lead and takes orders, and the old leader is restarted. Its log ends identical to the new leader's | Fails: the old leader's records stay in its log |
| Scenario, new: a gap is not taken for agreement | As the first, and then the leader is killed. The instance that takes over holds every record the matching engine acted on | Fails today; with rule 11 switched on it is also the check that the leader says "may lead" only once the follower holds everything |
| Unit tests | `Wal::truncate_after` leaves a log that reads back up to the record and no further, and appends after it; the answer to a position request for each shape of divergence; the gap check | Not yet written |

Comparing two logs needs a tool that prints every record's sequence number, epoch and a checksum of its
contents; the scenarios compare the two outputs.

## 9. Order of the work

1. `leader_epoch` on `WalRecord`, and the table of epochs.
2. `Wal::truncate_after`, with its unit tests.
3. The gap check at startup.
4. `LogPositionRequest` and `LogPositionReply`, the follower's discarding, the leader's sending, and the
   removal of the step that moves the next sequence number up.
5. The log comparison tool and the three scenarios.
6. Then step 4 of [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md) section 8,
   which switches rule 11 on.

Related: [change_of_sequencer_leader.md](change_of_sequencer_leader.md),
[a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md), [wal_and_ha.md](wal_and_ha.md).
