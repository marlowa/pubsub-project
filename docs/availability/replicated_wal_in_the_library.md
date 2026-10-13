# Phase 4: the replicated WAL in the library, with one writing thread {#replicated_wal_in_the_library}

> **Status: agreed, 13 October 2026.** This is the design for phase 4 of
> [the recovery plan](../recovery_plan.md). It follows section 3 of
> [ha_library_boundary.md](ha_library_boundary.md). Andrew agreed it as a whole, with the decisions in
> section 8.

Throughout this page, WAL means the write-ahead log, the file of sequenced records on disk. The text
log each process writes about what it is doing is called the Quill log.

## 1. What phase 4 does

Phase 4 puts the order path back on the design it was built for: one thread owns the sequencer's WAL
and is the only thread that ever writes it, on the leader and on the follower alike. It also moves the
replication of the WAL out of `SequencerThread` and into the library, as the replicated WAL that
[ha_library_boundary.md](ha_library_boundary.md) describes.

It has three steps, done and tested one after the other, in the same way as phase 3:

1. **One writing thread, in the sequencer as it is.** The follower writes its leader's records on the
   sequencer's own thread instead of on the reactor's thread. The handler that runs the sequencer's code
   on the reactor's thread is removed, and so is the framework facility that allows it, together with
   the two locks and the two atomic flags that exist only because two threads wrote the WAL
   ([BUG-0124](../bug_list.md#bug_0124)). Nothing moves between files in this step. It changes how the
   follower behaves in one respect, the thread its writes happen on, and it is the step that may cost
   latency, so it is measured on its own.
2. **The library's replicated WAL.** A new library class, `ReplicatedWal`, does what `SequencerThread`
   does today to keep two copies of the WAL: numbering and writing records on the leader, sending them to
   the follower, counting a record as committed once the follower has confirmed it, carrying on alone
   when the follower is absent or behind, repairing a rejoining follower's WAL, and deciding which
   instance leads. It is built and tested in the library on its own, with no venue code, before anything
   uses it.
3. **The sequencer on the library's replicated WAL.** `SequencerThread` hands its records to
   `ReplicatedWal` and acts on what it is told, and its own copies of that machinery are deleted. How the
   venue behaves does not change.

The step that changes behaviour comes first and is the smallest, so that its cost is measured with
nothing else changed at the same time. The two steps after it change no behaviour, so a failure after
either of them is a fault in that move.

## 2. What happens now

### 2.1 The WAL inside the sequencer

Everything that keeps two copies of the WAL is written into `SequencerThread`, mixed with the
sequencer's own business of sessions, reports and the matching engine:

- **On the leader**, the sequencer numbers each record (`next_sequence_number_`), writes it
  (`append_to_wal`), and sends it to the follower once the follower's WAL is known to agree with its
  own (`follower_log_matches_`). The follower acknowledges each record with `WalAck`, and the highest
  number acknowledged is `peer_acked_through_`. An order is held (`held_orders_`) and sent to the
  matching engine only once the follower has acknowledged it, and a report is held (`pending_er_`) until
  the follower has acknowledged the order it answers
  ([change_of_sequencer_leader.md](change_of_sequencer_leader.md), part 4.2).
- **When the follower is absent or falls behind**, the leader first has a voter confirm the statement
  that the follower may not lead, and only then acts on records the follower does not hold
  (`say_follower_may_not_lead`, `release_if_confirmed`, `running_alone_`). When the follower holds every
  record again, it says the follower may lead
  ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md)).
- **When a follower rejoins**, it asks its leader where their two WALs agree, by the number and the
  leadership epoch of its last record (`LogPositionRequest`, `LogPositionReply`). It discards what its
  leader does not hold, and the leader then sends it everything after that point. Which leadership wrote
  each record is kept in a table of epochs, `LogEpochTable`
  ([follower_log_repair.md](follower_log_repair.md)).
- **Who leads** is decided by the lease classes, now in the library, through `PairLeaseAgent`, which the
  sequencer owns and calls every tick (phase 3).

### 2.2 Two threads write the follower's WAL

A follower receives its leader's records on its connection to the peer. Most of them are written by the
reactor's thread, inside the parser, by a handler the sequencer installs on that connection
(`install_peer_wal_inline_handler`, using the framework's `ApplicationThread::install_inline_pdu_handler`).
The handler writes the record and sends the acknowledgement at once, without handing the record to the
sequencer's thread. Records it cannot write, because the two WALs are not yet known to agree or because
a record before it was passed on, go to the sequencer's thread, which writes or discards them there.

So two threads write one WAL, which breaks principle F1 in [design_principles.md](../design_principles.md),
and the reactor's thread runs the sequencer's code, which breaks principle F3. Keeping that safe takes:

- the mutex in `ReplicatedRecordWriter`, taken around the writing of every record the leader sends, so
  that the two threads never write at once ([BUG-0123](../bug_list.md#bug_0123));
- `log_epochs_mutex_`, taken on every write on the leader and the follower, because the table of epochs
  is extended on the reactor's thread and read and cut back on the sequencer's;
- two atomic flags, `follower_log_agreed_` and `replicated_records_queued_`, by which the sequencer's
  thread tells the handler when it may write;
- `highest_replicated_seq_no_` made atomic, because both threads raise it;
- a second histogram and two gauges, because a histogram has exactly one thread that records into it,
  and counters of the known breach for builds with thread checks.

The breach also costs work elsewhere. The sequencer's record of the command identifiers its WAL holds
cannot see the records the reactor's thread wrote, so a following sequencer reads its own WAL back from
disk once a second to find them (`read_identifiers_from_log`), and reads the positions of forwarded
reports from it at the same time.

### 2.3 Why the handler on the reactor's thread exists

It was added to shorten the follower's acknowledgement, which is inside every order's round trip
because the leader holds each order until the follower has acknowledged it. Without it, a record
reaching the follower wakes the sequencer's thread, which writes the record and hands the acknowledgement
back to the reactor to send: two hand-overs between threads. With it, there are none. When it was added,
each hand-over cost about 200 microseconds on this machine, because the processors were configured for
power saving. That configuration has since been found and is switched off for measurement, and a thread
that has gone to sleep now costs about 20 microseconds to start again
([latency_findings.md](../operations/latency_findings.md)).

## 3. Step 1: one writing thread in the sequencer

### 3.1 What changes

- **The follower writes every record on the sequencer's thread.** Every `WalRecord` from the leader
  arrives as an ordinary message, and `handle_wal_record` writes it if it is the next record the WAL
  needs, or discards it, and sends the acknowledgement. This is the path that already handles the records
  the handler passes on; it becomes the only path.
- **Removed from the sequencer:** `install_peer_wal_inline_handler`; `ReplicatedRecordWriter` and its
  test; `log_epochs_mutex_`; `follower_log_agreed_` and `replicated_records_queued_`, replaced by an
  ordinary member that says whether the two WALs agree; the atomic on `highest_replicated_seq_no_`; the
  histogram and the gauge for writes on the reactor's thread; and the marks and counters of the known
  breach.
- **Removed from the framework:** `ApplicationThread::install_inline_pdu_handler`, the inline handler in
  `PduParser`, and the reactor's command that installs it, as principle F3 says. Any accessor added only
  for it goes too, if nothing else uses it.
- **The follower notes each record as it writes it.** The identifier of the command a record holds, and
  the position of forwarded reports it carries, are noted when the record is written, as the leader does
  already. The once-a-second read of the follower's own WAL, which exists only because another thread
  wrote records this one never saw, is removed.
- **The build refuses the old code.** The entries for the two locks and for `append_to_wal` are removed
  from `_PERMITTED_LOCKS` in `scripts/check_standards.py`, so a lock put back in the sequencer fails the
  build. The second-writer check in `Wal` and the second-thread check in `LogEpochTable` stop counting
  and refuse instead: a second thread that writes the WAL is then a failure in builds with thread checks,
  not a number in a gauge.

### 3.2 What it may cost

The follower's acknowledgement gains the two hand-overs between threads that the handler removed: the
record goes from the reactor's thread to the sequencer's, and the acknowledgement goes back. At 100
orders a second, the measuring load, an order arrives every 10 milliseconds and the follower's sequencer
thread will usually have gone to sleep in between, so the first hand-over will usually pay the cost of
waking it. The sequencer's reactor spins for 50 milliseconds before it sleeps, so the second hand-over
should be quick. The round trip may therefore rise by something like 20 to 40 microseconds at the
measuring load, and by less at higher rates, where the thread does not have time to sleep. That is an
estimate, not a measurement. The recovery plan records that Andrew has accepted that some of the
follower's delay may return, provided it is measured.

It is measured before and after this step by the method in
[latency_findings.md](../operations/latency_findings.md), with the same matching engine leading in both
arms ([BUG-0151](../bug_list.md#bug_0151)). The "before" arm also answers BUG-0124's request that the
cost of the two locks be measured before they are removed: it is the cost of the locks and the handler
together. If the measured cost is large, the figure comes back to Andrew before step 2, with the one
lever available: letting the sequencer's application thread spin for a configured time before it sleeps,
as its reactor already does, which keeps it awake between orders at the cost of the processor time it
spins for.

**Measured: no cost that can be told apart from the difference between runs.** Each arm was started on
fresh state with the primary matching engine leading, and sent five runs of 6,000 orders at 100 a
second with nine in ten cancelled, from a load client pinned to processors 0 and 1, the first run
discarded. Because the figures drift during a day, the build before this step was measured twice, once
before and once after the build with it:

| Build | Median round trip in each of four runs, microseconds |
|---|---|
| Before step 1 | 124.9, 124.3, 124.5, 124.8 |
| After step 1 | 128.7, 127.9, 124.6, 124.8 |
| Before step 1, measured again | 127.6, 127.1, 127.3, 124.6 |

The two measurements of the same build differ from each other by as much as either differs from the
build after the step. The step took effect: the following sequencer's queue carried about 55,300
messages a run after it and 31,300 before, the difference being the leader's records, which reach its
thread through the queue instead of being written on the reactor's thread. The mean of the hop into the
FIX gateway's application thread was 2.7 to 2.8 microseconds in every run of every arm.

### 3.3 How step 1 is known to be right

- **The build refuses the old code:** with the permissions removed and the old code still in place,
  `scripts/check_standards.py` reports nine locks it does not permit; with the new code, none.
- **A second thread is refused:** two tests, one for the WAL and one for the table of epochs, use each
  from a second thread in a build with thread checks and expect a refusal. With the checks switched off
  in the code, both fail.
- **The follower's records are noted without reading the WAL back:** `ha_test.py` scenario 74 sends an
  order, kills the leading sequencer once the order is accepted, and sends the same order again to the
  follower once it leads, marked as sent again. The follower holds the order only because its leader
  replicated it, so the scenario fails unless the follower noted the command when it wrote the record.
  With the noting removed from the code, the order is sequenced a second time and the scenario fails,
  in two runs of two.
- **Every high availability scenario** passes, on fresh WALs.
- **The order's round trip** is measured before and after, as section 3.2 says.

## 4. Step 2: the library's replicated WAL

### 4.1 What `ReplicatedWal` does

`ReplicatedWal` is a library class, in `libraries/pubsub_itc_fw`, in the namespace `pubsub_itc_fw`. An
application that wants a replicated WAL holds one as a member of its application thread, as section 11
of [ha_library_boundary.md](ha_library_boundary.md) describes. It does what section 2.1 lists, with the
rules unchanged:

- **On the leader**, it numbers each record it is given and writes it, sends it to the follower once
  their two WALs are known to agree, and keeps the highest number the follower has acknowledged. A
  record is **committed** once the follower has acknowledged it, or, while the leader carries on alone,
  once a voter has confirmed that the follower may not lead. It numbers a new leadership's records above
  every record its WAL holds ([BUG-0105](../bug_list.md#bug_0105)).
- **On the follower**, it writes each record its leader sends if it is the next one its WAL needs, and
  acknowledges it. A record that would leave a gap stops the writing until the two WALs are found to
  agree again.
- **Carrying on alone.** When no follower is connected, or the follower has not acknowledged anything
  for the time allowed, it makes the statement that the follower may not lead, and commits records the
  follower lacks only once a voter has confirmed it. When the follower holds every record again, it says
  the follower may lead.
- **Repair.** It keeps the table of epochs, answers a rejoining follower's request for where their WALs
  agree, discards on the follower what the leader does not hold, and sends the follower everything after
  the point of agreement.
- **Who leads.** It owns the `PairLeaseAgent`, the epoch store and the promise recorder, calls the agent
  every tick, and changes role when the agent says so.
- **What it reports.** The metrics and Quill log lines that `SequencerThread` produces for these things
  today, with the same names and wording, so that dashboards and `ha_test.py` see no difference.

It is used on one thread only, the application thread that holds it (decision 2), and it takes no lock.

### 4.2 What the application supplies, and what it is told

**What it supplies:**

- **Its records.** On the leader, the application asks for the number the next record will carry,
  encodes the record with that number, and hands it over. The number is asked for first because the
  venue's own envelope, `WalRecord`, carries its number inside it and is read with that number by the
  matching engine, the gateways and the publishers.
- **Its connections.** The application still sets up its connections, as the sequencer does today. It
  gives `ReplicatedWal` a small interface through which to send on them, `ReplicationLinksInterface`,
  in the same way that it gives the lease classes `LeaseLinksInterface`; tells it when the connection to
  the peer is established or lost; passes it every message of the library's replication and lease
  messages; and calls it every tick.
- **Its configuration:** where its WAL lives, and the settings the sequencer reads today for its peer,
  its voters and its timings.

**What it is told**, through an interface the application implements, `ReplicatedWalEventsInterface`:

- that its role has changed, and to what, with the new epoch;
- on the leader, that the committed position has advanced, and to where. The sequencer then sends the
  matching engine the orders it holds up to that point, and forwards the reports that were waiting for
  it, as it does today on an acknowledgement. The records themselves are not handed back: the sequencer
  already keeps each held order's own buffer for that purpose, and the WAL is not built for reading a
  live record back by its number;
- on the follower, each record it has written, so that the application can note what it needs from it,
  as step 1 arranges for the identifiers and the forwarded positions;
- on the follower, that the records after a given number have been discarded in a repair, so that the
  application can do then whatever it does today when that happens.

The interface for an application that follows the WAL as a state machine, with a function called for
each committed command and functions to save and restore its state, is not built in this phase. It
belongs to phase 5, following the WAL, and phase 6, where the matching engine is the first user.

### 4.3 Each record's number and epoch

The library has to know each record's number and the epoch of the leadership that wrote it: the number
to write and replicate the records in order, and the epoch to repair a rejoining follower's WAL. Today
the number is in the WAL's own entry header, written by `Wal::append`, but the epoch is only inside the
venue's `WalRecord` envelope, as its `leader_epoch` field, and the sequencer decodes the envelope to find
it. Decision 1 is how the library learns it. The recommendation is that the WAL's entry header carries
the epoch, beside the number and the time it already carries, and that the library replicates whole
entries in a message of its own, so that the library never decodes an application's envelope.

Under that recommendation:

- `Wal::append` takes the epoch, and the entry it writes is the time, the epoch, the message type and the
  application's bytes. Every trading day starts with empty WALs, so no WAL written before the change has
  to be read after it.
- A library function splits an entry into its header and the application's bytes. The nine places in
  the sequencer, and the one in the library's `TopicPublisher`, that skip the header by counting its
  bytes use it instead, so the layout is known in one place. The Python checks in `ha_test.py` read only the outer frame and its checksum, so they are not
  affected.
- The replication messages move into a message definition file of the library's own,
  `replication_messages.dsl`: `ReplicatedRecord`, carrying the number, the epoch, the time, the message
  type and the bytes of one entry; and `WalAck`, `LogPositionRequest` and `LogPositionReply`, moved from
  the venue's `leader_follower.dsl` with their message numbers unchanged. `ReplicatedRecord` takes a new
  message number, because the venue's `WalRecord` keeps its own, 103, for the envelope sent to the
  matching engine, the gateways and the publishers.
- The venue's `WalRecord` keeps its number. Its `leader_epoch` field is no longer read by the library.
  Whether anything else reads it is checked during the step, and it is removed if nothing does.

### 4.4 How step 2 is known to be right

- **Unit tests of a pair**: two `ReplicatedWal` objects, each with its own WAL in the build tree, joined
  by test links that deliver messages in memory and can hold, drop or delay them, and a lease agent whose
  voters the test controls. The tests cover, at least: a record committed once acknowledged, and not
  before; the follower writing records in order and refusing one after a gap; the leader carrying on
  alone only after the statement is confirmed, and saying the follower may lead once it holds every
  record; a rejoining follower whose WAL holds records of an older leadership discarding them and taking
  its leader's; a new leader numbering above every record it holds; and a change of leader with records
  not yet acknowledged. Each test is seen to fail with a deliberate fault before it is trusted.
- **A randomised test**, in the manner of `LeaseSimulationTest`: many runs in which messages are lost,
  delayed and reordered, instances stop and restart and leaders change at random, checking after every
  step that the follower's WAL is the leader's up to some point, that no committed record is ever
  missing from the WAL of the instance that leads next, and that no number is used twice.
- **No lock**: the library's standards check finds none in the new files.
- **The TLA+ models** of the sequencer pair, `SequencerPairHA.tla` and `FollowerBehindHA.tla` in
  `docs/availability/tla`, are read against the class. The rules are unchanged, so the models should
  need no change; if the class does anything the models do not allow, that is a fault in the class.

**Checked.** The class is `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ReplicatedWal.hpp`, and its tests
are `ReplicatedWalTest.cpp`: seven tests of a pair, each named for what it checks, and the randomised
test, 20 runs of 400 steps each. All eight pass. Five deliberate faults in the class were each seen to
make at least one test fail: committing a record before the follower acknowledged it; the follower
writing past a missing record; committing without a voter confirming the statement; a rejoining follower
keeping records its leader does not hold; and a new leader numbering its records without rising above
those it holds. The randomised test lost no committed record in any run. It found one defect, in the lease
rules rather than in this class: an instance holding an older statement that its peer may not lead never
granted that peer, even after the peer led at a higher epoch ([BUG-0152](../bug_list.md#bug_0152)). With that
fixed, the randomised test also requires, once everything is restored, that one instance leads and the
other follows.

## 5. Step 3: the sequencer on the library's replicated WAL

### 5.1 What changes

`SequencerThread` holds a `ReplicatedWal` and implements `ReplicatedWalEventsInterface`. Removed from it,
because the library now does them: the numbering and writing of records; sending records to the follower
and receiving acknowledgements; `follower_log_matches_`, `peer_acked_through_` and the acknowledgement
timeout; `running_alone_` and the statement that the follower may or may not lead; the table of epochs
and `LogEpochTable` itself; the handling of `LogPositionRequest` and `LogPositionReply`; and the
ownership of the lease agent, the epoch store and the promise recorder.

What stays in the sequencer, because it is the venue's business: the held orders and when they go to the
matching engine; the reports waiting for a commit and where they go; the reports kept while not leading;
waiting for the matching engine's position after taking the lead; the record of identifiers and the
check against sequencing a command twice (phase 8 moves that); sessions, resends and undelivered reports;
the catch-ups of the matching engine and the publishers (phase 5 moves those); and order acceptance.

### 5.2 How step 3 is known to be right

- **Every high availability scenario** passes on fresh WALs, as after step 1.
- **The Quill log lines and metrics** that `ha_test.py` and the dashboards read are unchanged, checked by
  running the scenarios, which match many of those lines, and by comparing the metrics endpoint's names
  before and after.
- **The order's round trip** is measured before and after this step. No change is expected, because the
  same work is done on the same thread; the measurement confirms it.
- **The size of `SequencerThread`** falls by the lines that moved, which is the point of the phase, and
  the move is checked by reading the diff for anything that changed rather than moved.

## 6. What phase 4 does not do

- **It does not change any rule** of replication, committing, carrying on alone, repair or leadership.
  Every behaviour the scenarios check is the same after it.
- **It does not move the ways of following the WAL.** The matching engine's catch-up, the publishers'
  subscription, the answers to resend requests and the record of identifiers still read the WAL on the
  sequencer's thread, as now ([BUG-0133](../bug_list.md#bug_0133)). Phase 5 gives the WAL its own way of
  being followed.
- **It does not change where the WAL's helper keeps segments ready**
  ([BUG-0130](../bug_list.md#bug_0130)), nor what is reported while the leader runs without a standby
  ([BUG-0134](../bug_list.md#bug_0134)). Both are framework fixes that can be done at any point.
- **It does not move the setting up of connections** into the library. The application still makes its
  connections and passes the library what arrives on them.

## 7. Cost

- **Step 1** removes more than it adds: the handler, one class and its test, two locks, two atomic flags,
  a histogram, two gauges and the once-a-second read, and a framework facility with its reactor command.
  A few hundred lines go, and a few tens change.
- **Step 2** is the largest: a new class of perhaps 1,000 to 1,500 lines, built mostly from code that
  exists in `SequencerThread` today, with its configuration, its message file, the change to the WAL's
  entry header, and the tests in section 4.4, which are new and are most of the value of the step.
- **Step 3** deletes from `SequencerThread` what step 2 built, and adds the calls between the two: a
  large diff, mostly deletions.

## 8. Decisions

These are separate questions, each answered on its own.

### Decision 1. How does the library learn each record's leadership epoch? (Choose A or B.)

**Agreed 13 October 2026: A.**

**Background.** Section 4.3. The library needs each record's epoch to repair a rejoining follower's
WAL. Today the epoch is held only inside the venue's `WalRecord` envelope, which the library cannot read
without knowing the venue's messages.

**A. The WAL's entry header carries the epoch, and the library replicates whole entries in a message of
its own.**

- For: the library never decodes an application's bytes, which is the line
  [ha_library_boundary.md](ha_library_boundary.md) draws: what a command means is the application's.
  The layout of an entry is then known in one place, the library, instead of being counted out by hand
  in nine places in the sequencer and one in `TopicPublisher`.
- Against: the layout of an entry on disk changes, and every reader of the WAL must use the new
  function to find the application's bytes. No old WAL has to be read, because every trading day starts
  with empty ones.

**B. The application gives the library a function that reads the epoch out of its own envelope.**

- For: the WAL's entry layout and the replication messages stay as they are, so step 2 changes less.
- Against: the library depends on application code to do its own job, so a second application using
  the replicated WAL would have to put an epoch in its envelope and write the same function. The venue's
  envelope stays the place the epoch lives, which is the venue's message describing the library's
  business.

**Recommendation: A.** It is the shape the agreed boundary describes, and the cost falls in this phase
only.

**The reply wanted:** "A" or "B".

### Decision 2. Which thread owns the replicated WAL? (Choose A or B.)

**Agreed 13 October 2026: A.**

**Background.** Section 4.1. Principle F1 needs one thread to own the WAL; it does not say which.

**A. The application thread that holds the `ReplicatedWal`.** For the sequencer, that is the thread that
sequences orders, as on the leader today.

- For: no hand-over between threads is added anywhere. On the leader, numbering, writing, replicating
  and acting on a commit all happen on the thread that already does them. It is the arrangement section
  11 of the boundary design shows.
- Against: the application's thread does the WAL's work, so a slow write delays the application's next
  message. That is already true of the leader today, and principle F4's helper thread, which prepares
  segments ahead of the writer, is what keeps writes from waiting for the disk.

**B. A thread of the library's own, to which the application thread hands records.** This is how some
replicated logs are built, Aeron Cluster's consensus module among them.

- For: the application's thread never does the WAL's work.
- Against: every record gains two hand-overs between threads on the leader, one to hand it over and one
  to learn that it is committed, which is the cost the sequencer's own design avoids; and the
  application must hold every record until it hears back, which needs storage and a second queue.

**Recommendation: A.**

**The reply wanted:** "A" or "B".

### Decision 3. Is the design as a whole right to build, in the order of section 1? (Yes or no.)

**Agreed 13 October 2026: yes.**

**Background.** Sections 1 to 7: three steps, the one that changes behaviour first and measured on its
own, then the library's class built and tested alone, then the sequencer moved onto it.

**Recommendation: yes.**

**The reply wanted:** "yes" or "no", with any change to the steps.
