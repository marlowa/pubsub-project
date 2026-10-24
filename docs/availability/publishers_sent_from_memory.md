# Sending the publishers each committed record from memory, not from the WAL {#publishers_sent_from_memory}

This is the design for [BUG-0171](../bug_list.md#bug_0171). Andrew agreed it on 24 October 2026, answering yes to
all three decisions in section 7. It is being built in the order of section 6.

## 1. The problem

A matching engine publisher is a process that follows the sequencer's WAL. It wants every record the
leading sequencer writes, in order, as soon as each record is committed. Once a publisher has been caught
up by the reading thread and handed over, the sequencer's own thread sends it each record as the record is
committed. This is done by `WalHandedOverFollowers`, which is described in
[following_the_wal.md](following_the_wal.md), section 4.3.

By the time a record is committed, the sequencer's thread no longer has the record's bytes. It encoded them
into one reusable buffer (`wal_encode_buffer_` in `SequencerThread::append_envelope_to_wal`), handed them to
the replicated WAL, and then reused the buffer for the next record. So `WalHandedOverFollowers` reads each
committed record back from the WAL, on the sequencer's thread. Reading a record back checks its checksum.

Each record is read back once for all the publishers together, not once for each of them. Even so, `perf`
on the leading sequencer's thread, during a fill of 300,000 orders at 4,000 a second with both publishers
handed over, puts 16.0 per cent of the thread's samples in sending committed records to the publishers.
Half of that is checking checksums and half is sending. Writing the WAL itself is 12.3 per cent, for
comparison. The measurement and how it was made are in the BUG-0171 entry.

This work happens after the order path's own work for each commit is done, so the order's round trip does
not change. But it is time the sequencer's thread spends on something other than orders, and at a higher
order rate than 4,000 a second it is time the order path no longer has.

Andrew has chosen the third of the three ways the bug entry lists: keep each record's bytes in memory until
it is committed, and send the publishers those bytes, so that nothing is read back from the WAL. The
checksum is not to be removed from reading the WAL. This document says how that would be done.

## 2. What the sequencer writes to its WAL

The design depends on which records are written and what the sequencer already keeps of each one, so this
section states it.

Every record the sequencer writes is a `WalRecord` envelope, encoded and passed to `ReplicatedWal::append`
by `SequencerThread::append_envelope_to_wal`. That function is called from two places:

1. For each order that arrives from a gateway (`SequencerThread.cpp`, the order branch of the inbound
   handler).
2. For each execution report that arrives from the matching engine (`SequencerThread.cpp`, the report
   branch, which builds a new envelope around the report).

What the sequencer keeps of each kind until it is committed differs:

- **An order** is kept in the storage for held orders (`held_orders_`, a `FixedCapacityRingBuffer` of
  `HeldOrder`, with room for `orders_held_at_most` orders, 16,384 by default). A held order keeps the
  decoded envelope and the inbound message's buffer, which the decoded envelope points into. It does not
  keep the encoded bytes that were written to the WAL. When the storage is full, later orders are not held
  at all: they wait in the WAL, and `first_unheld_seq_` says where they begin.
- **An execution report** is not held in that storage. It is kept in `pending_er_`, a map from the number
  of the record whose commit releases the report (usually the order's record, not the report's own record)
  to what is needed to forward the report to its gateway.

The publisher is sent each record's bytes exactly as they lie in the WAL
(`publisher_record_from_wal`, in `applications/sequencer/PublisherRecordFromWal.hpp`, copies the record's
bytes unchanged after checking that the envelope decodes).

## 3. Why the storage for held orders is not used

The obvious place to send from is the storage for held orders, since it already holds every order until it
is committed. It does not serve, for four reasons.

1. **It holds only orders.** Execution reports are records in the WAL too, and every publisher must be
   sent every record, or it sees a gap in the numbering and asks for the missing record again for ever.
   Reports are kept elsewhere, under a different number, and so would still have to be read back.
2. **It does not hold the bytes that were written.** A held order keeps the decoded envelope. Sending it
   would mean encoding it a second time, at commit, on the sequencer's thread. That costs time of the same
   order as the checksum it saves, and the publisher would then be sent a second encoding rather than the
   bytes the WAL holds. Those are the same only while encoding stays deterministic, which nothing checks.
3. **Held orders are released before the publishers are sent.** At each commit,
   `SequencerReplicatedWalEvents::committed_through` releases the held orders to the matching engine first,
   which removes them from the storage, then forwards reports, and sends the publishers last, so that the
   order path's work comes first. Sending the publishers from the held orders would mean sending them in
   the middle of the order path's work, or keeping held orders after they are released.
4. **When the storage is full, orders are not held at all.** That is when the follower is far behind and
   the leader is under most pressure, and those orders would still be read back from the WAL.

## 4. The design

### 4.1 A store of written records, in the library

`WalHandedOverFollowers` gains a store of the records written but not yet sent to the publishers. The store
belongs to the library, not to the sequencer, because the problem belongs to any application that hands
followers over to new records, and the agreed direction is for high availability to live in the library
([ha_in_the_library.md](ha_in_the_library.md)).

The store is two fixed-capacity rings, both allocated once, when the sequencer starts:

- a ring of bytes, holding the encoded bytes of each record one after another;
- a ring of entries, one for each record held, giving the record's number, its pdu id, where its bytes begin
  in the ring of bytes, and how many bytes it has.

Nothing is allocated after start-up. A record that does not fit, because either ring is full, is not stored.
The store's capacity is a setting of the sequencer, given both as a number of records and as a number of
bytes. Its default is chosen in step 1 from the measured size of records (section 6).

### 4.2 Filling the store

After `ReplicatedWal::append` returns, `SequencerThread::append_envelope_to_wal` passes the store the
record's number, pdu id and bytes, which are still in `wal_encode_buffer_`. The store copies the bytes into
its ring of bytes.

The store is filled only while at least one publisher is in step with the shared reading (the single reading
of the WAL that sends each record to every publisher that has caught up). Otherwise nothing would ever empty
it, so nothing is stored and nothing is copied.

The records in the store are always consecutive: no record is missing between the first and the last. Once
a record is not stored, because the store is full, no later record is stored either until the store has
emptied, in the same way as `first_unheld_seq_` works for held orders. This keeps the rule for where a
record comes from simple: everything from the store's first record to its last is in the store, and
everything else is read from the WAL.

### 4.3 Sending from the store

`send_to_all`, called at each commit, sends the publishers in step every record up to the last committed.
For each record, the shared reading takes the bytes from the store if the store holds the record, and reads
it from the WAL as it does now if it does not. A record taken from the store is removed from it once it has
been sent to every publisher in step. Removing records from the front of both rings is all that emptying
the store involves.

The shared reading's cursor still has to be at the right place for the records that are read from the WAL.
When records are sent from the store, the cursor is not moved, and the next record that has to be read from
the WAL is found by the cursor reading on from where it last stopped, passing over the records it has
already sent. Passing over a record still checks its checksum, but this happens only after the store has
been full, which is rare. Step 2 (section 6) measures how rare.

A publisher that is still catching up on its own cursor (section 4.3 of following_the_wal.md) reads from the
WAL as it does now. It is never sent from the store. Once it joins the shared reading, it is sent from the
store along with the others.

### 4.4 What is not checked

A record sent from the store is not read from the WAL, so its checksum is not checked, and the publisher's
converter does not decode it to check it either. Its bytes are a copy, made moments earlier by the same
thread, of the buffer that thread encoded and handed to the WAL.

The checksum is still checked on every read of the WAL: by the reading thread when it catches a follower
up, by a publisher's own cursor, and by the shared reading whenever the store does not hold a record. What
the checksum guards against is the record on disk, or in the mapped segment, differing from what was
written. A copy that was never written to disk is not exposed to that.

This is a change to what a publisher is guaranteed, and it is decision 1 in section 7.

### 4.5 Changes of leader, and high availability off

- **When this instance stops leading**, every publisher is removed, as now, and the store is emptied.
- **When it takes the lead**, the store is empty. The publishers connect to it, are caught up by the
  reading thread and handed over. From then on the store fills as described above.
- **With high availability off**, there is no reading thread, and a publisher is caught up on the
  sequencer's thread (BUG-0172, which this design does not change). Once caught up it joins the shared
  reading and is sent from the store like any other.

## 5. What it costs

- **On the order path**: one copy of each record's bytes into the store, when the record is written. This is
  before the record is committed, so it is on the order path, which reading back at commit is not. A record
  is a few hundred bytes, so the copy should be far cheaper than the checksum it replaces. Step 1 measures
  it rather than assuming so.
- **Off the order path**: at each commit, sending from the store instead of reading and checking each
  record. Sending each record to each publisher stays; it is about half of the 16.0 per cent today.
- **Memory**: the store's capacity, allocated once.

## 6. Steps, and how each is known to be right

1. **Measure the size of records**, from a WAL written by a fill, to choose the store's default capacity.
   Measure the cost of copying a record of that size.
2. **Build the store in `WalHandedOverFollowers`, with unit tests**, before the sequencer uses it. The tests
   include, each made to fail on purpose before it is trusted:
   - a record sent from the store is the bytes passed in, and is sent without the WAL being read: the test
     alters the record in the WAL after it is written, and the publisher must still be sent the original
     bytes;
   - when the store is full, later records are read from the WAL, in order, with none missing and none sent
     twice;
   - a publisher still on its own cursor is never sent from the store;
   - emptying the store when leadership is lost leaves nothing to be sent.
3. **Use it in the sequencer**, behind the new capacity setting. Run `ha_test.py` in full, from fresh WALs,
   with scenario 76 (publishers follow only the record) among them.
4. **Measure before and after** with `tmp/phase5/fill_perf.sh` for the share of the sequencer's thread, and
   the round trip measured as for BUG-0171, on a quiet machine with the load client pinned. Success is the
   share of the thread spent on the publishers falling from about 16 per cent to about the sending half,
   with no change to the round trip.

## 7. Decisions

These were separate yes-or-no questions, not alternatives. All three are decided.

### Decision 1. May a record sent from memory go to the publisher without its checksum being checked? (Yes or no.)

**Background.** Today every record a publisher is sent has been read from the WAL, and reading it checks its
checksum. Sending from memory reads nothing, so there is nothing to check. The checksum is still checked on
every read of the WAL (section 4.4).

**For.** This is the point of the design: the checksum is half the cost being removed. The bytes have never
been on disk, so the thing the checksum guards against has not happened to them.

**Against.** A fault in the sequencer's own memory between the write and the commit would reach the
publisher unchecked. The WAL's copy would still be checked whenever it is read, so the two would differ.

**Recommendation.** Yes.

**Decided: yes** (Andrew, 24 October 2026).

### Decision 2. Is one copy of each record on the order path acceptable, if the round trip is measured as unchanged? (Yes or no.)

**Background.** Copying a record into the store happens when it is written, which is on the order path.
Reading it back happens at commit, after the order path's work.

**For.** A copy of a few hundred bytes is small, and removes a read and a checksum from the same thread.

**Against.** It moves work onto the order path, which the present design keeps off it.

**Recommendation.** Yes, provided step 4 shows no change to the round trip.

**Decided: yes** (Andrew, 24 October 2026).

### Decision 3. Is the design right to build, in the order of section 6? (Yes or no.)

**Decided: yes** (Andrew, 24 October 2026).

Back to the [availability documents](../availability/README.md).
