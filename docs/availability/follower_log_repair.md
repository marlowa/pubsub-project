# Repairing a follower's log when it rejoins {#follower_log_repair}

## 1. What this document covers

The two sequencer instances each keep a write-ahead log. The leader writes every record, and sends each
one to the follower, which writes it to its own log and acknowledges it. While the follower stays
connected, its log is a copy of the leader's.

When the follower has been disconnected, or has restarted, or when it is an old leader rejoining
after a change of leader, its log is no longer a copy. This document describes how it is made one
again. It is the fix for [BUG-0097](../bug_list.md#bug_0097) and part 4.5 of
[change_of_sequencer_leader.md](change_of_sequencer_leader.md).

It is also what rule 11 ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md))
depends on. That rule lets a leader say that its follower may lead again only once the follower holds
every record the leader holds, and only a repaired log makes that knowable for a follower that has
rejoined.

## 2. Why a rejoining follower's log needs repairing

- **A follower that was away missed records.** Records the leader wrote while the follower was
  disconnected or down were never sent to it. Receiving only new records, its log would have a gap.
- **An old leader holds records its new leader does not have.** An instance that led, wrote records its
  follower never received, and then stopped leading, still holds them when it rejoins. The new leader
  may have written different records under the same numbers.
- **The highest acknowledgement is not the highest unbroken one across connections.** Within one
  connection, records arrive in order, so the highest record the follower has acknowledged means "every
  record up to here". A follower with a gap would acknowledge new records too, so after a reconnection
  the leader could not tell from acknowledgements alone that records were missing.

A follower whose log had a gap, or held records the leader does not, could be elected. When it led, the
matching engine would catch up from its log and miss the records in the gap, and the publishers and the
order activity recorder would read a log that disagrees with the one the venue acted on.

## 3. What must hold

**G5.** When an instance rejoins as a follower, its log ends up identical to its leader's.

And, for rule 11 and for the running-alone decision in part 4.2: **when the leader counts the follower as
having acknowledged record N, the follower holds exactly the leader's records 1 to N.**

## 4. The design

This is how Raft and Kafka repair a follower's log, and nothing in it is new. Kafka's form of it is the
closer fit, because, like this venue, it numbers records itself and keeps the leader's epoch beside them.

### 4.1 Each record says which leadership wrote it

`WalRecord` carries a field, `leader_epoch`: the epoch in which the leader that sequenced the record held
the lead. The follower stores records exactly as it receives them, so the field is in both logs.
Records written before the field existed read it as zero: the field is optional and last, and the
generated decoder reads a message that ends where an optional field would begin as lacking it
([serialisation_dsl.md](../framework/serialisation_dsl.md)).

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
left with has the same epoch as the leader said, the logs agree up to it, and the follower asks once
more; the leader, seeing that the follower's last record is the one it would answer with, answers that
the logs agree and sends what follows (4.5). If the epochs differ, the follower discards the whole run
of its records from the epoch of its record there, and asks again. Some of those records may be in the
leader's log too; discarding them is safe, because the leader sends them again, and doing so needs
nothing to be true of the order of epochs in either log. Each round leaves the follower fewer records,
so the exchange ends, and it takes a round for each run discarded rather than one for each record.
Records from different epochs differ only when an instance whose log lacked records led, which rule 11
prevents once it is in force, or after an epoch went backwards.

A follower whose log is empty sends zero, and the answer is zero. A follower whose logs are not known to
agree asks again every second, so that a request or reply lost with a connection does not leave it
waiting.

Until the leader has answered a follower's request, it sends that follower no live records, and the
follower writes none. A live record sent before the two logs agreed would be written after a gap. On
the follower, the handler on the reactor thread declines every replicated record until the logs agree,
which passes it to the sequencer thread, and the sequencer thread discards it and logs that it did.

When the leader stops sending live records to a follower whose log agreed with its own, because a peer
connection opened or closed or it has just taken the lead, it tells the follower so with a reply marked
`ask_again`, and the follower asks again. Each instance has two peer connections, one opened by each,
and the leader sends records on one of them; a change of connection is a point at which records could
otherwise arrive out of order.

Once the logs agree, the follower writes a replicated record only if it is the next one its log needs.
One it already holds, sent again after the logs were found to agree, is acknowledged and not written
twice. One that would leave a gap is not written; the follower stops writing, as when the logs are not
known to agree, and asks again.

**Which thread writes.** A follower's replicated records are written by the handler on the reactor
thread when it can, and otherwise passed to the sequencer thread. The handler counts every record it
passes on, the sequencer thread counts each one back when it has written or discarded it, and the
handler writes a record itself only while that count is zero, so records are written in the order
they arrived. The count alone did not stop the two threads writing at the same moment: a follower's
log was found with one entry blank and the next record written twice
([BUG-0123](../bug_list.md#bug_0123)). So every write of a replicated record goes through
`ReplicatedRecordWriter::write_if_next`, which decides whether the record is the next one the log
needs and writes it in one step under a lock, and every other change to the log while records may be
arriving, such as discarding records (4.4), goes through `change_log` under the same lock.

### 4.4 The follower discards the records after that point

This uses an operation of the framework's write-ahead log, `Wal::truncate_after(seq_no)`. It finds
where record `seq_no` ends, reading from the segment that holds it rather than from the start of the
log; overwrites everything after it in that segment with zeros, so that a reader stops there and no
older entry beyond it can be read again once new records are appended; deletes every later segment and
the snapshot, which may point beyond the new end; and reopens the writer at that point. The sequencer
trims its table of epochs to match.
Only a follower calls it, and only before it writes anything the leader sends it. It runs on the
sequencer thread, inside `ReplicatedRecordWriter::change_log`, while the reactor thread's handler is
declining every record (4.3), so the two never write the log at the same time.

Discarding is safe. A record the follower holds that its leader does not was written by an old leader
and never reached a majority. Under part 4.2's option A the matching engine never acted on it, and no
member was answered for it, so the gateway still holds the command and sends it again under part 4.3.
The follower logs at Warning how many records it discarded and their numbers and epochs.

### 4.5 The leader sends the follower everything after that point

Having answered that the logs agree, the leader reads its own log from the segment that holds the first
record to send, and sends the follower every record after the agreed one, in order, exactly as stored,
and only then starts sending it live records. The segment is found by reading only the first entry of
each segment file (`Wal::scan_start_for`). The publishers and the matching engine catch up from the
sequencer in the same way (`handle_wal_subscribe_request`, `handle_me_position_request`), and the cost is
of the same kind: the sequencer thread reads the log, and the order path waits while it does. Section 6
gives the cost and section 7 the open question about it.

The follower acknowledges each record as now. On answering, the leader sets `peer_acked_through_` to the
agreed record, replacing whatever it held from an earlier connection. From then on it is again "every
record up to here", because records arrive in order on one connection. Until the follower's
acknowledgements reach the leader's latest record, the leader treats the follower as behind: under part
4.2 it runs as if alone after 100 milliseconds without an acknowledgement, and under rule 11 it goes on
saying that the follower may not lead.

### 4.6 Taking the lead

A follower that takes the lead numbers new records from its highest record plus one, as part 4.1 already
does. With its log repaired, that is the leader's latest record that reached it. An instance does not
move its next sequence number up to its peer's: doing so is what would create a gap, and the repair
makes it unnecessary.

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
| 4.5 sending what it missed | The order path waits while the leader reads its log | Proportional to the records the follower missed, from the start of the segment holding the first of them |

## 7. Open question

**Reading the log on the sequencer thread.** The leader reads the records the follower missed on the
sequencer thread, and the order path waits while it does. Reading starts at the segment holding the first
of them, so the cost is proportional to what the follower missed rather than to the size of the log. A
follower that has been down for long in a busy day misses many records: at 50 million orders a day,
an hour's absence is several million records, and the startup measurement recorded under
[BUG-0107](../bug_list.md#bug_0107) puts reading 5.5 million records at 2.6 seconds with the files
already in memory. Reading and sending from a thread of its own, handing the leader the point where it
stops so that live records follow without a gap, would remove the wait. The publishers' and the matching
engine's catch-ups, which still read their log from its start, have the same question.

## 8. Tests

Each scenario was run with the repair disabled, by building the sequencer with the leader sending no
missed records and the follower discarding nothing, and each failed; with the repair in place each
passes.

| Test | What it requires | With the repair disabled |
|---|---|---|
| `ha_test.py` scenario 61: a follower restarts | The follower is killed, the leader takes 20 orders, the follower is restarted. Once its log agrees with the leader's, it holds the same records, with the same contents, as the leader's from where it stopped, without a gap. Then the leader is killed, and the new leader holds every record the old leader wrote while it was down | Fails: the follower's log lacks all 42 of the records written while it was down |
| `ha_test.py` scenario 62: an old leader rejoins | The leader's sends to its follower are blocked with `libblock_sends_to_ports.so`, so that it writes three orders the follower does not have. It is killed, the follower takes the lead and takes orders, and the old leader is restarted. It discards the three, and its log ends identical to the new leader's | Fails: the old leader never reaches agreement with the new leader |
| `LogEpochTableTest` | The epoch of each record; the leader's answer and the follower's step for a follower that is behind, an old leader, a divergence across two changes of leader, a follower whose records are from a later epoch, an empty follower, and an epoch that went backwards | |
| `WalClassTest`, `truncate_after` and `scan_start_for` | A truncated log reads back up to the record and no further, records appended after it follow it, an older entry beyond the new end is never read again, later segments and the snapshot are removed, and reading from the scan start reaches the record | The test of an older entry fails when the zeroing is left out |
| `WalRecordEncodingTest` | A record carries its epoch, and a record written before the field existed still decodes | The second fails with the generator's earlier handling of optional fields |

Comparing two logs uses the checksum stored with each entry: a follower stores the bytes its leader
sends it unchanged, so the same record carries the same checksum in both logs.

## 9. Order of the work

All six steps are done.

1. `leader_epoch` on `WalRecord`, and the table of epochs.
2. `Wal::truncate_after`, with its unit tests.
3. The gap check at startup.
4. `LogPositionRequest` and `LogPositionReply`, the follower's discarding, the leader's sending, and the
   removal of the step that moves the next sequence number up.
5. The comparison of logs and the scenarios.
6. Step 4 of [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md) section 8,
   which switches rule 11 on.

Related: [change_of_sequencer_leader.md](change_of_sequencer_leader.md),
[a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md), [wal_and_ha.md](wal_and_ha.md).
