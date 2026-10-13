# Write-Ahead Log (WAL) {#wal}

The WAL is the framework's one durable, append-only record of things that have happened.
It is a **primitive**, reused by two otherwise-unrelated subsystems:

- the **sequencer**, where it is the authoritative order log that leader-follower high
  availability is built on (see [WAL and High Availability](../availability/wal_and_ha.md)); and
- the **matching engine publisher**, where it is the retained backlog that pub/sub subscribers
  stream from and replay (see [Pub/Sub](../pubsub/pubsub.md)).

This document describes the WAL as a data structure — its format, its guarantees, and the
cursor/replay model. The two subsystems that build on it are documented separately.

---

## Design principle

The WAL exists to **separate the irreversible decision from its replayable effects.** A
record, once appended, is a fact: it happened, it has a sequence number, and it will read
back byte-for-byte forever. Everything downstream — matched trades, execution reports,
published topic pages — is a *reconstructable function of the log*. Nothing downstream is
allowed to be the source of truth for anything the WAL already records.

Two consequences follow, and they shape the whole design:

1. **Reading is trivial and total.** Recovery, replication, and pub/sub replay are all the
   same operation at bottom: read records from sequence number *N* onward. The venue does not
   yet have one path for it: a follower's repair, the matching engine's catch-up and the
   publishers' subscriptions each have code of their own, all run on the sequencer's thread
   ([BUG-0133](../bug_list.md#bug_0133)); one way of following the log is part of the design in
   [ha_library_boundary.md](../availability/ha_library_boundary.md), section 4.
2. **Appending is the only privileged act.** Whoever may append is a policy decision that
   lives *outside* the log (leadership, for the sequencer; leadership again, for the
   publisher). The log itself does not know or care who the writer is — it only guarantees
   that what was written reads back intact.

---

## On-disk format

Each WAL entry is a 24-byte header, the payload, and a checksum (`WalWriter.cpp`):

```
[ magic : 4 | length : 4 | seq_no : 8 | reserved : 8 | payload | checksum : 4 ]
```

- **magic** — a fixed marker (`0xFEEDFACE`) that begins every entry; lets a reader recognise a
  record boundary and detect where a torn write ends.
- **length** — the payload byte count, so a reader can find the checksum and the next entry
  without parsing the payload.
- **seq_no** — the record's sequence number, assigned by the writer. Monotonic and gap-free
  within a WAL.
- **reserved** — eight bytes, always zero.
- **payload** — opaque bytes. The WAL does not interpret them; what goes in the payload is
  the writer's convention (see *Payload conventions* below).
- **checksum** — a CRC32 covering the entry. A reader that fails the checksum treats this
  entry, and everything after it, as *never having happened*.

### Replay is scan-from-zero, stop-on-damage

Replay scans from offset 0. On any checksum or bounds failure it **stops** and reports the
last good sequence number. Entries past the failure point are treated as "did not happen".

This makes the failure modes clean and symmetrical:

- **Tail corruption** (a torn final write after a crash) is *identical in effect to a clean
  crash before that entry committed.* The prefix up to the last good record is authoritative;
  the torn tail is discarded. No special-casing.
- **Mid-log corruption** is a genuine fault — a hole in an otherwise-valid log. The reader
  halts rather than skipping it; recovery is to promote a replica whose prefix is clean (a
  sequencer-HA concern, covered in [wal_and_ha.md](../availability/wal_and_ha.md)).

### Segmentation

A WAL is not one growing file but a sequence of fixed-size segments (`wal_000001.log`,
`wal_000002.log`, …). Each segment can be archived on its own.
Segmentation:

- **localises corruption** to a single segment,
- makes **truncation** a matter of deleting whole segments that are fully behind a safe
  point rather than rewriting a file, and
- makes the **disk-full** case predictable — a bounded, countable set of files.

---

## The writer

The WAL is **single-writer**. Exactly one thread appends to a given WAL, so no locking is
needed on the append path and `seq_no` is trivially monotonic and gap-free. Who that writer
*is* can change (a follower is promoted; a publisher gains leadership), but at any instant
there is one. On a following sequencer, too, the sequencer's thread writes every record the
leader sends. Builds with thread checks refuse an append from any other thread.

**`seq_no` continues across a change of writer; it does not restart.** A follower moves its
counter past every record it writes, and a sequencer that takes the lead numbers from the
highest record it holds plus one, so the sequence spans the failover unbroken. This is what
makes `seq_no` alone the total order over the log's whole life. The leader epoch is not part
of the WAL frame and not part of the order key: the sequencer carries it inside its record's
payload (`WalRecord.leader_epoch`), where it is used only to repair a rejoining follower's log
([follower_log_repair.md](../availability/follower_log_repair.md)). A record is located by
`seq_no` and nothing else. Restarting the numbering on promotion, or ordering by
`(epoch, seq_no)`, would both break this.

**No `fsync` per record.** Forcing each append to physical disk would cost tens to hundreds
of microseconds and is not how durability is achieved here. The log is written into
memory-mapped segments, and the operating system writes them back to disk in the background;
nothing in the venue syncs them while it runs. Where stronger durability is required
it comes from *replication to a second machine*, not from `fsync` on one — a WAL that exists
on two disks is safer than one `fsync`'d to a single disk that can still fail as a unit. The
cross-machine durability protocol is a sequencer-HA concern; see
[wal_and_ha.md](../availability/wal_and_ha.md).

---

## The cursor and replay model {#wal_cursor_replay}

A reader does not consume the WAL destructively; it holds a **cursor** — a position — and
advances it. This is the single mechanism behind recovery, replication, and pub/sub replay.

- A cursor is opened at a position and reads the next record on demand
  (`WalCursor::read_next(record_id, payload, size)`), yielding the sequence number and the
  payload bytes.
- Reaching the end simply means *caught up to the current head*. A cursor can be re-opened at
  its last position to pick up records appended since — so "wait for more" and "replay from
  the past" are the same code path, distinguished only by how far behind the head the cursor
  starts.
- **A lagging reader costs a cursor, not memory.** Because the backlog lives in the WAL on
  disk, a slow or replaying consumer holds only an integer position. There is no per-consumer
  buffer to grow; the retained WAL *is* the buffer, shared by every reader.

This last point is why the WAL is the natural backing store for topic pub/sub: fan-out to
many subscribers at different positions is just many cursors over one log. See
[Pub/Sub](../pubsub/pubsub.md).

### Retention and truncation

**Nothing reclaims the WAL while the venue runs.** `Wal::truncate_below` deletes the segments
wholly below a **safe floor**, the lowest sequence number any reader still needs, but nothing
in the venue calls it. The snapshot both the sequencer and the publisher take
(`Wal::take_snapshot`) records where the log has reached and deletes nothing. Every trading day
starts with an empty log, so a log holds one day at most (principle V12); whether it may be
reclaimed during the day waits on a decision about how long execution reports must remain
available (see [wal_and_ha.md](../availability/wal_and_ha.md)).

The rule any reclaiming must obey: **never delete history that a legitimate reader has not yet
passed.**

---

## Payload conventions

The WAL frame carries opaque payloads. `Wal::append`, which both the sequencer and the
matching engine publisher use, puts a small header at the front of every record's payload:

```
[ wall_time_ns : int64 ][ pdu_id : int16 ][ pdu payload... ]
```

so a subscriber's page can carry the record's publish timestamp and its message type without
a separate catalog lookup. The DSL-encoded PDU follows. A reader that only wants to skip a
record needs nothing but the WAL frame's `length`; a reader that wants to *route* it reads
this header. For the sequencer, the PDU is the `WalRecord` envelope around an order, a cancel
or an execution report.

### `wall_time_ns` is data, never an ordering key

The timestamp in that payload header records *when* the writer stamped the record. It says
nothing about **order** — that is `seq_no`'s job, and only `seq_no`'s.

The distinction matters because `wall_time_ns` comes from `WallClock`, which in production is
`std::chrono::system_clock`. That clock is not monotonic: NTP can step it backwards, so two
records can carry timestamps that disagree with the order in which they were appended.
Sorting or comparing records by `wall_time_ns` is therefore always wrong, however sensible it
looks in a debugger. Downstream code treats the stamp as an opaque value to carry (FIX
`TransactTime`, a subscriber page's publish time) and never as something to order by.

The related rule for consumers of the stamp is in [replay.md](replay.md): any
`now() - recorded_timestamp` is meaningless under replay, because the recorded side may be
hours stale.

---

## See also

- [WAL and High Availability](../availability/wal_and_ha.md) — the sequencer's use of the WAL: two-tier
  commit, replication, snapshots, leader election, and failover.
- [Pub/Sub](../pubsub/pubsub.md) — the matching engine publisher's use of the WAL as a fan-out backlog.
- [Sequencer](../venue/sequencer_app.md) — the sequencer application.
- [Serialisation DSL](../framework/serialisation_dsl.md) — the encoding used for record payloads.
