# Recording leaders' statements in the background {#recording_statements_in_the_background}

**Status: designed and agreed, not built.** This is the design for [BUG-0142](../bug_list.md#bug_0142).
Section 7 records its decision, agreed with Andrew on 9 November 2026.

## 1. The problem

In a pair whose leader makes statements, such as the sequencers, the leader says on each of its lease
requests whether its peer may lead. It says the peer may not lead while the leader is acting on
commands the peer does not hold. Each arbiter keeps the newest statement from each pair in a file, so
that an arbiter that restarts still refuses a lease to an instance its leader said may not lead. An
arbiter tells the leader it holds a statement with the message `StatementHeld`, and it sends that only
once the statement is in the file. The leader acts on commands its peer lacks only once its statements
are held.

Today the arbiter writes that file on its own application thread, the thread that answers every lease
request and renews the arbiter's own lease. `ArbiterThread::record_component_statement` calls
`save_component_statements`, which writes a temporary file, syncs it, renames it over the old one and
syncs the directory, all before the arbiter answers. In scenario 8, against a Debug install, one such
write took 4.8 seconds. During it the arbiter answered no lease request, the primary sequencer, whose
follower was down, lost its lease while holding 1,000 orders, and the arbiter's own lease ran out.

This breaks the rule that code which renews leases never waits for the disk. The lease promise records
were moved off the lease-handling thread for the same reason ([BUG-0107](../bug_list.md#bug_0107)); the
statement file was not.

**The other half of BUG-0142 is already dealt with.** The entry also records a matching engine that
spent 5.5 seconds recording a new epoch after it had been granted a lease, so that the lease ran out
during the write. Rule 13 of [majority_leases.md](majority_leases.md), built for
[BUG-0228](../bug_list.md#bug_0228), now has an instance record the epoch it asks in *before* it sends
the request, so the write no longer comes out of the lease. The measurements made for BUG-0029 on 8
November 2026 show it: "recorded epoch 9129 … before asking to lead in it", then the lease granted.
This design covers the statement file only.

## 2. What already exists

- **The grant and the confirmation are already separate messages.** A `LeaseGrant` from an arbiter
  echoes no statement; the arbiter confirms a statement with `StatementHeld`. So a grant never needs to
  wait for a statement to be written.
- **A statement that could not be written is already handled.** If the write fails, the arbiter logs
  an Error, keeps the statement in memory, and does not confirm it. The leader goes on sending the
  statement on each request, and the write is tried again then. So the leader already copes with a
  statement that is held in memory but not yet confirmed.
- **The arbiter refuses a lease using the statement it holds in memory.** It does not wait for the
  statement to be written before acting on it in that way.
- **`BackgroundPromiseRecorder` already writes lease promises on a thread of its own.** The thread
  handling leases puts the record in one slot and increases a count of writes asked for. The writer
  thread, which looks for work every 10 milliseconds, writes and syncs the record and increases a count
  of writes finished. The thread handling leases reads the outcome later. There is no lock, and neither
  thread waits for the other.
- **The arbiter has a timer that rings every 100 milliseconds** (`LeaseTiming::tick_interval`), on which
  it already does its lease housekeeping and sends heartbeats to the other arbiter.

## 3. The design

### 3.1 A writer thread for the statement file

The arbiter gets a second background writer, of the same kind as the promise recorder, for the
statement file. Section 7 asks whether it is the same class made general, or a class of its own.

The slot holds a copy of the whole set of statements, because the file holds one line for each pair
and is always written whole. The number of pairs an arbiter votes in is fixed by its configuration and
is small (the sequencers, the matching engines and the publishers), so the copy is a fixed-size array
sized at start-up, and handing it over allocates nothing.

### 3.2 When a statement changes

A statement changes in two ways: on a leader's lease request, and on an `ArbiterStateRecord` from the
other arbiter. In both cases the arbiter:

1. updates the statement in memory at once, as now, so that a lease is refused at once to an instance
   the new statement says may not lead;
2. marks the set of statements as not yet written;
3. if no write is in progress, copies the set into the slot and starts a write. If a write is in
   progress, it starts the next one when that write finishes, with the set as it is then. The newest set
   is therefore always the last one written, and a burst of changes costs at most one write in progress
   and one waiting.

The arbiter then answers the request at once, granting or refusing the lease exactly as it does now.
Nothing on the arbiter's thread waits for the disk.

### 3.3 Confirming a statement

A statement is confirmed only once a finished, successful write contained it. That is the rule today,
and it is kept.

- **On each 100-millisecond tick**, the arbiter reads the outcome of the write in progress, if it has
  finished. If it succeeded, the arbiter sends `StatementHeld` for each pair whose statement in that
  write had not been confirmed. It sends it on the connection of the most recent request from that
  pair's leader, which the arbiter remembers for each pair. If it failed, the arbiter logs the Error it
  logs today, and starts another write on the next tick.
- **On a lease request whose statement is already written**, the arbiter confirms it at once, as today.

So the confirmation reaches the leader within about 100 milliseconds of the write finishing, rather
than immediately after it, as now. A leader whose follower is down waits that much longer before acting
on commands the follower lacks. It is a small, fixed addition, and it replaces a wait that could
last seconds.

### 3.4 Why this is safe

What a statement protects is this: a leader acts on commands its peer lacks only once the arbiters
hold a statement saying the peer may not lead, so no arbiter can later elect that peer and lose those
commands. The design keeps each part of that:

- **The confirmation still follows the write.** `StatementHeld` is sent only for a statement a finished,
  successful write contained, so a leader never acts on a statement an arbiter would forget on
  restarting.
- **An arbiter that restarts with a statement not yet written** has not confirmed it, so its leader has
  not acted on it. That is exactly the position today when a write fails.
- **Refusing a lease from the statement in memory, before it is written**, can only refuse more, never
  grant more, so it cannot elect an instance that lacks commands.

### 3.5 Why the file is synced to disk, not kept in shared memory

A statement must survive whatever can make an arbiter forget it, or a restarted arbiter can elect an
instance that lacks commands its leader acted on (the TLA+ trace
[behind-4-arbiter-forgets](tla/traces/behind-4-arbiter-forgets.txt)). An arbiter forgets in two ways:

- **Its process dies and its machine stays up.** Shared memory survives this, and so does a file written
  without `fsync`, because its data stays in the kernel's page cache and is written out later whether or
  not the process exists.
- **Its machine dies**, by a loss of power, a kernel panic or a restart. Shared memory and the page cache
  are both lost. Only what `fsync` has put on the disk survives.

Arbiters run on separate machines precisely so that one machine can be lost, so the record must survive
the second case, and `fsync` is the cost of that. Shared memory would make the write fast and lose the
guarantee. With the write on a thread of its own, the wait for the disk delays only the confirmation of
a statement, which must wait for it anyway, and never the answer to a lease request.

### 3.6 At start and at shutdown

At start the arbiter reads the statement file on its own thread, as now, before it answers anything. At
shutdown the writer thread is stopped as the promise recorder's is: the arbiter waits up to a second for
a write in progress, and then leaves it to finish or end with the process. A statement whose write was
cut short was not confirmed.

## 4. The principle this restores

Code that renews leases never waits for the disk. After this change, neither of the arbiter's two
writes, promises and statements, is made on the thread that answers lease requests.

## 5. Steps, and how each is known to be right

1. **The background writer for statements**, with unit tests using a store that can be made to block:
   a write is started and its outcome read later; a second change while a write is in progress is
   written after it, with the newest set; and a failed write is reported and tried again. Each test is
   made to fail on purpose first.
2. **The arbiter uses it**, with unit tests of `ArbiterThread`'s statement handling: a lease request is
   answered at once while a statement write is blocked; `StatementHeld` is not sent until the write
   finishes; it is sent on the next tick after the write succeeds; and it is never sent after a failed
   write.
3. **A test setting that slows the arbiter's statement writes**, `delay_statement_writes_ms` in the
   arbiter's `[test_events]` section, read only by tests, in the same way as the gateways'
   `delay_session_announcements_ms`.
4. **A new `ha_test.py` scenario reproducing scenario 8's failure on purpose.** The secondary
   sequencer is stopped, so that the primary keeps its lease only with the arbiter's vote. The
   arbiter's statement writes are slowed to 5 seconds, longer than the lease. The scenario requires
   the primary to keep leading, and every order to be answered. Run first against the arbiter as it is
   now, where it must fail, then against the new arbiter, where it must pass.
5. **The full high-availability suite**, with the WALs cleared first.

## 6. What this does not change

The statement file's format, the `StatementHeld` message, the leader's side, and the rules for which
statements an arbiter keeps are all unchanged. Only the thread that writes the file, and the moment
`StatementHeld` is sent, change.

## 7. Decision

### Decision 1. Is the statement writer the promise recorder made general, or a class of its own? (Choose one.)

- **A, the same class made general.** `BackgroundPromiseRecorder` becomes a class template over the
  record it writes and the store that writes it, and the promise recorder becomes that template used
  for promises, so its existing users do not change. One mechanism, already tested, and its lock-free
  hand-over is not written twice.
- **B, a class of its own** for statements, copying the same pattern. Nothing that exists changes, at
  the cost of a second copy of the hand-over to test and keep right.

**Decided: A.** The hand-over between the two threads, with its two counters and its slot, is the part
easiest to get wrong, and it is better tested once and used twice.
