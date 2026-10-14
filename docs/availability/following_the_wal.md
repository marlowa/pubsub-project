# Phase 5: following the WAL, through the library, on a thread of its own {#following_the_wal}

> **Status: agreed, 14 October 2026.** This is the design for phase 5 of
> [the recovery plan](../recovery_plan.md). It follows section 4 of
> [ha_library_boundary.md](ha_library_boundary.md), "Design area: following the WAL". Andrew agreed it
> as a whole, with the decisions recorded in section 8. Step 1 is built: the library's way of following
> a WAL exists and is tested on its own (section 5.1, "What step 1 built"); no component uses it yet.

Throughout this page, WAL means the write-ahead log, the file of sequenced records on disk. The text
log each process writes about what it is doing is called the Quill log.

## 1. What phase 5 does

Several components need every record in the sequencer's WAL after some position, in order: the
follower sequencer when it rejoins, a matching engine when it starts or is promoted, and the matching
engine publishers. Today the leading sequencer serves each of them by reading its WAL on its own
thread, the thread that sequences every order. While it reads, it sequences nothing. Each component
also asks in its own way and is served by its own code, and the publishers' way has defects of its own
([BUG-0133](../bug_list.md#bug_0133), [BUG-0153](../bug_list.md#bug_0153)).

Phase 5 gives the library one way to follow the WAL:

- a component that follows the WAL says the position it has reached, and whether it then wants new
  records as they come;
- the library sends it every record after that position, in order, and tells it when it has been sent
  everything, so that it knows it has caught up;
- the reading is done by a thread of its own, never by the thread that writes the WAL;
- the change from records read by that thread to new records sent by the writing thread is made at an
  exact position, so that nothing is missed and nothing is sent twice.

The follower sequencer's repair, the matching engine's catch-up and the publishers' subscription then
all use it, and the sequencer's thread no longer reads its WAL on anyone's behalf, apart from the
exceptions listed in section 6, each of which already has its own bug and its own plan.

Phase 5 has five steps, each tested before the next starts, in the same way as phases 3 and 4:

1. **Make the stall visible, then build the library's way of following the WAL and test it on its own.**
   First a measurement that shows how long the leading sequencer stops sequencing while it serves a
   catch-up today. Then the reading thread, the part that runs on the writing thread, the follower's
   side and their messages, tested without the venue. The framework gains two small facilities
   (section 3.2), and the ThreadSanitizer build is repaired so that the new code runs under it
   ([BUG-0150](../bug_list.md#bug_0150)).
2. **The follower sequencer's repair** moves onto it, inside the library's `ReplicatedWal`.
3. **The matching engine's catch-up**, and the sending of orders to the engine from the WAL, move onto it.
4. **The publishers** move onto it, which fixes BUG-0153; a scenario shows that defect before the fix.
5. **The standards check** refuses any read of the WAL in the sequencer outside a short named list, as
   it refuses locks (section 5.5), and the documents are brought up to date.

## 2. What happens now

### 2.1 Where the sequencer's thread reads its WAL for someone else

| What | Who asks | Where | How far it reads | Removed by |
|---|---|---|---|---|
| Repairing a rejoining follower: every record after the last on which the two WALs agree | The follower sequencer, with `LogPositionRequest` | `ReplicatedWal::handle_position_request` | From the segment holding the first record the follower lacks | Phase 5, step 2 |
| A matching engine's catch-up: every order after the engine's position | A starting or promoted engine, with `MePositionRequest` | `SequencerThread::handle_me_position_request` | From the segment holding the first order the engine lacks | Phase 5, step 3 |
| Orders a new leader sends the engine after asking how far it got | The sequencer itself, after `EnginePosition` | `SequencerThread::send_logged_orders`, from `handle_engine_position` | The orders the engine lacks | Phase 5, step 3 |
| Orders that waited in the WAL because the storage for held orders was full | The sequencer itself, as the orders are released | `SequencerThread::send_logged_orders`, from `release_held_orders_through` | The orders that waited | Phase 5, step 3 |
| A publisher's subscription: every record after the publisher's position | Each publisher, with `WalSubscribeRequest` | `SequencerThread::handle_wal_subscribe_request` | From the first segment of the WAL, whatever the position asked for | Phase 5, step 4 |
| A member's resend request: that member's execution reports in a range | A gateway, with `SessionReplayRequest` | `SequencerThread::handle_session_replay_request` | Segments from the newest backwards until the range is covered | Not phase 5; [BUG-0113](../bug_list.md#bug_0113) (decision 3) |
| Reports produced while a member had no connection | The sequencer, when the member's session is bound again | `SequencerThread::handle_undelivered_reports_request` | Segments from the newest backwards | Not phase 5; BUG-0113 (decision 3) |
| Checking a command sent again against the WAL itself, while the record of identifiers cannot answer | The sequencer, when such a command arrives | `LogTailIndex`, from `SequencerThread::command_already_logged` | The end of the WAL | Not phase 5; phase 8 replaces the duplicate checks with one ([BUG-0122](../bug_list.md#bug_0122)) |

Two other reads are not on the order path: replay mode, a tool run against a WAL with nothing else
running, and the building of the record of command identifiers, which already runs on a thread of its
own (`LoggedCommandIdentifiersBuilder`).

### 2.2 What it costs

Reading is fast when the records are in memory and slow when they are not. The start-up measurement
recorded under [BUG-0107](../bug_list.md#bug_0107) read 5.5 million records in 2.6 seconds with the
files already in memory, about half a second for each million records. At 50 million orders a day, a
follower sequencer that is away for an hour misses several million records, so the leader would stop
sequencing for seconds to repair it, and for longer if the records had to come from disk. A matching
engine that catches up from far back, or a publisher that restarts during the day, does the same. A
publisher's subscription is the worst of these today, because it reads from the first segment of the
WAL whatever position it asked for.

**Measured, 14 October 2026.** With the follower sequencer stopped while 300,000 orders were taken,
about 1.2 million WAL records, and then started again while orders arrived at 200 a second, the venue
stopped answering orders altogether: the follower never caught up, and no order sent after it restarted
was answered for the rest of the run. Before the restart the longest wait for an order's first report
was 5.2 milliseconds. The causes, which go beyond the reading itself, are in
[BUG-0155](../bug_list.md#bug_0155). The publisher's subscription and the engine's catch-up could not be
measured in that run, because the venue was no longer answering by the time they happened; they are
measured separately, without the follower's repair, in step 1.

### 2.3 What is wrong with the way the publishers follow the WAL

BUG-0153, found while preparing this design by reading the code. Each publisher connects to both
sequencers and subscribes to each.

- The leading sequencer sends each record to the publishers as soon as it has written it, before its
  follower has acknowledged it. If the leader dies before the follower holds the record, the follower
  takes over without it, and the publisher has already published a record that the venue's record does
  not hold.
- Both sequencers answer a subscription, whatever their role, so a publisher that subscribes from a
  position below the end of the day's record is sent those records twice and writes each into its own
  WAL twice.
- The follower sends a publisher nothing while it follows, so a publisher depends on the old leader
  having sent it everything up to the change of leader, which it may have done and more.

The library's way of following the WAL, described next, sends only committed records, only from the
leading instance, exactly once and in order, and sends a follower what it lacks at a change of leader.
Moving the publishers onto it fixes all three.

## 3. The library's way of following the WAL

### 3.1 What a component that follows the WAL sees

A component that follows the WAL, called a follower of the WAL below, uses the library's follower side,
`WalFollowerClient`. It asks both instances of the sequencer pair, because it cannot know which one
leads. In order:

1. It sends `WalFollowRequest` to each instance, naming the last record it holds, and saying whether it
   wants new records once it has caught up, or only the records up to now. A position of -1 means it
   holds nothing and wants nothing from the past: it is placed at the newest record and sent nothing
   before it, which is what a matching engine that found no record of its own book, and a publisher
   that asks to start at the newest record, are given today.
2. An instance that does not lead answers `WalFollowDeclined`. It remembers the request, so that if it
   takes the lead later it can tell the follower (step 5).
3. The leading instance sends every record after the named position, in order, each as the
   application's own message (section 3.5).
4. When it has sent everything, it sends `WalFollowCaughtUp`, saying the last record it sent, how many
   records it sent, and the earliest record its WAL holds. The follower side checks that it received
   exactly that many records, which is requirement R-0101 for the matching engine today, and refuses to
   report itself caught up if it did not. If the follower asked for new records, they follow, one by
   one, as they are committed.
5. An instance that takes the lead sends `WalLeaderChanged` on every connection whose follower it
   declined or was serving, and the follower side sends its request again, naming the last record it
   now holds.

The follower side refuses a record that does not follow the last it received: one at or below it is
dropped, and one further on is a gap, which the follower side treats by asking again from the last
record it holds, and reports as a warning in the Quill log.

These four messages are new, and belong to the library's message file, `replication_messages.dsl`.
`MePositionRequest`, `MePositionAck`, `WalSubscribeRequest` and `WalSubscribeAck` are removed once their
users have moved (decision 2).

### 3.2 The reading thread

`WalReadingThread` is a library class: an application thread, created by the application beside the
thread that owns the WAL, as any other application thread is. One reading thread serves every follower
of that WAL. It owns no connection and takes no lock. The thread that owns the WAL never waits for it,
and it never waits for the thread that owns the WAL: each tells the other what it needs through the
framework's queue of messages between threads.

For each follower it is asked to serve, it holds a `WalCursor` at the follower's position, reads the
records after it, and sends them. Three rules govern what it reads and sends.

**It never reads bytes that may still be being written.** The thread that owns the WAL publishes, after
each record it writes, the position in the WAL just after that record, with a release store; the
reading thread loads it with an acquire load and does not read past it. Everything it reads was
therefore completely written before it reads it. Records are written into memory-mapped segments, so
this costs the writing thread one store to an atomic variable for each record, and nothing else.

**It sends only what the follower may be sent.** Each follower is served up to one of three positions,
which the thread that owns the WAL publishes in the same way (section 3.3). The reading thread sends no
record numbered above that position.

**It sends only as fast as the connection takes the records.** A catch-up of several million records
must not be read into memory faster than the network can carry it, or its backlog lives in the
reactor's queue rather than in the WAL. So the reading thread sends a batch of records each time the
follower's connection can take more, as the publishers' `TopicPublisher` already does. Today the
reactor tells only the thread that owns a connection when it can take more. The framework gains one
facility: a request for that notification may name the thread to be told. With the facility below for
placing the thread, these are the only framework changes phase 5 makes.

When the reading thread reaches the end of the segments it knows of, it opens its cursor again at its
position, which finds any new segment. Between catch-ups it has no work and blocks on its queue, so it
needs no core of its own. The framework today gives every application thread a core of its own on the
hot path, so the framework gains a second small facility: an application thread may ask to stay on the
background cores, as the extra threads an application thread starts already do.

Segments are created ahead of the writer and filled with zeros, and `WalCursor` treats zeros at its
position as the end of a finished segment and moves to the next. A cursor that reached the unwritten end
of the segment being written would therefore skip the records written there later. The rule above, never
to read past the published position, is what prevents it, and step 1 tests that case.

### 3.3 How far each follower is served

Each follower is served up to one of three positions:

| Follower | Served up to | Why |
|---|---|---|
| The follower sequencer | The last record written | Replication sends records as they are written. That is what lets them be committed. |
| A publisher | The last record committed | A publisher may publish only what the venue's record holds (BUG-0153). |
| A matching engine | The last order the sequencer has released to the matching engine | The engine may act only on committed orders (V5), and an order held for another reason, while a new leader waits to learn how far the engine got, must not reach the engine early. |

The first two are published by the library's `ReplicatedWal`. The third belongs to the sequencer, which
publishes it through the library: an application may publish a position of its own and serve followers
up to it.

### 3.4 Handing a follower over from the reading thread to new records

The thread that owns the WAL keeps, for each follower, whether the reading thread is sending to it or
the owning thread is. While the reading thread is sending to a follower, the owning thread sends it
nothing. The hand-over goes as follows:

1. The owning thread asks the reading thread to serve the follower from its position, and marks the
   follower as being caught up.
2. The reading thread sends records until it has sent the last one at or below the follower's limit,
   as it read the limit when it last looked. It then tells the owning thread the position it reached and
   how many records it sent, and stops sending to that follower.
3. The owning thread compares that position with its own current limit for the follower. If they are
   equal, nothing has been released since the reading thread looked, and the follower is handed over:
   the owning thread sends `WalFollowCaughtUp` if the follower asked to be told, and from then on sends
   it each new record itself as the record passes the limit. If the position falls short, records were
   released in the meantime, and the owning thread asks the reading thread to carry on to the new limit,
   and the round repeats.

**Why nothing is missed or sent twice.** Every message any thread sends goes through the one queue into
the reactor, which sends them in the order they were queued. The reading thread queues its last record
before it tells the owning thread it has finished, and the owning thread queues its first new record
only after it has been told. So the follower receives every record the reading thread sent before any
record the owning thread sends, and the owning thread starts at the record after the position the
reading thread reached.

**Why it ends.** The reading thread reads far faster than the venue takes orders, so each round is
shorter than the last, and a round ends with the positions equal as soon as no record is released
during the time it takes to pass a message between the two threads and back, a few microseconds. In a
steady stream of orders that can still take several rounds. So, if three rounds in a row fall short,
the library tells the application, and the sequencer pauses reading new orders from the gateways
until the hand-over is complete, using the pause it already has for when too many orders are held.
Records already sequenced are still committed and released, so the limit stops moving soon after, and
the next round ends the hand-over. The pause lasts about one round, and is reported in the Quill log.

**When a follower's connection is lost** during a catch-up, the owning thread tells the reading thread
to stop serving it, and ignores any report about it that is already on its way.

**When the instance stops leading,** the owning thread tells the reading thread to stop serving every
follower. A follower sequencer may later discard records at the end of its WAL that its new leader does
not hold, which shortens a segment or deletes one. The library does not discard any record until the
reading thread has confirmed that it has stopped, so the reading thread never reads a segment that is
being cut back. It asks the leader again later instead of waiting.

### 3.5 What the application supplies, and what it is told

The library sends each record as the application's own message, so the application supplies, for each
kind of follower, a function that turns a WAL record into the message to send, or into nothing if the
record is not for that follower. For the matching engine, that is today's `stream_wal_record_to_me`:
new orders and requests to cancel, rewrapped. For a publisher, it is every record as a `WalRecord`. The
function runs on the reading thread, so it may read only the record it is given and the reading
thread's own working memory, and must touch nothing that belongs to the application's thread.

The application is told, on its own thread:

- that a follower asked while this instance does not lead, and on which connection, so that the
  sequencer can point its connection to the matching engine at the engine that asked, as it does today
  (`EngineOrderRouting::asked_to_catch_up_while_following`);
- that a follower has been handed over, so that the sequencer can route new orders to an engine that
  caught up in order to lead, as it does today on `MePositionAck` (`EngineOrderRouting::caught_up_to_lead`);
- that a hand-over has fallen short three times in a row (section 3.4).

### 3.6 The follower's side

`WalFollowerClient` runs on the follower's thread and holds the follower's position. It sends the
request to both instances, sends it again on `WalLeaderChanged` or after a gap, drops a record at or
below its position, checks the count on `WalFollowCaughtUp`, and passes each record to a handler the
application supplies. It also tells the application when it has caught up, with the earliest record the
leader's WAL holds, so that a matching engine can still tell whether the WAL reaches back to the first
record of the day (R-0123). This is what section 4 of the boundary design means by "a handler for each
committed command, and the position it has reached when it starts".

## 4. How each reader moves

### 4.1 The follower sequencer's repair

`ReplicatedWal` keeps its rules for repair: the follower asks where the two WALs agree
(`LogPositionRequest`), the leader answers (`LogPositionReply`), and the follower discards any record the
leader does not hold. What changes is what happens once they agree. Instead of reading every missing
record on its own thread, the leader asks its reading thread to send them, served up to the last record
written, and hands the follower over to new records when the reading thread has finished.

Until the follower has acknowledged every record up to the hand-over position, it is not keeping up,
and the leader carries on as it does while the follower is absent: it commits a record once a voter has
confirmed that the follower may not lead, as rule 11 requires. Being handed over is not enough, because
the records the reading thread sent may still be on their way to the follower or waiting to be written
there, and new records would wait behind all of them. Once the follower has acknowledged through the
hand-over position, the leader waits for its acknowledgements again, and when it has acknowledged every
record the leader holds, the leader says it may lead again, as it does now.

The follower asks where the two WALs agree on each of its connections to the leader, and the leader
answers only the first request for each time the follower connects: one repair at a time, never two.
[BUG-0155](../bug_list.md#bug_0155) shows both of these mattering: after the follower missed 1.2 million
records the leader sent them twice, and counted the follower as keeping up from the moment it began, so
new orders waited behind the whole repair. No rule
of replication, committing or repair changes; only the thread that reads the missing records.

### 4.2 The matching engine

The engine's catch-up and the two places where the sequencer sends the engine orders from its WAL
become one thing: the engine is served from a position, up to the last order released to it.

- **A starting or promoted engine** asks with `WalFollowRequest`, through `WalFollowerClient`, instead
  of `MePositionRequest`. A promoted engine asks for new records too, and a starting one does not, which
  is what `asking_to_lead` says today.
- **A new leader that learns how far the engine got** (`EnginePosition`) serves the engine from the
  record after that, followed by new orders, instead of reading them itself.
- **Orders that waited in the WAL** because the storage for held orders was full are no longer read back
  by the sequencer's thread: the engine is served from the first such order, followed by new orders.

The engine's book, its checks and its region are unchanged. The engine's own reading of what it is sent
does not change either, because it receives the same `WalRecord` messages on the same connection.

### 4.3 The publishers

Each publisher uses `WalFollowerClient` towards both sequencers, asking for new records too, and is
served up to the last record committed, by the leading instance only. Its `handle_wal_record_from_sequencer`
then writes into its own WAL only records that follow the last it wrote. The sequencer stops sending
records to the publishers as it writes them (`stream_wal_record_to_external_subscribers`), and sends
them instead as they are committed, from the thread that owns the WAL, once each publisher has been
handed over.

A publisher therefore publishes a record a little later than today: after the follower has acknowledged
it, rather than as soon as the leader wrote it. On this machine that is the replication round trip,
tens of microseconds. It is the cost of publishing only what the venue's record holds.

How a publisher's own subscribers are served, from the publisher's own WAL by `TopicPublisher`, does not
change.

## 5. Steps, and how each is known to be right

### 5.1 Step 1: the stall measured, and the library's way of following the WAL built and tested alone

**The measurement first.** With the venue loaded with a steady stream of orders from the load client,
and the WAL holding a few million records, three things are done one after another, and the longest
order round trip during each is recorded against the longest in the same run without it:

- the follower sequencer is stopped, misses about a million records, and is started again;
- a publisher is started with an empty WAL, so that it subscribes from the start;
- a matching engine catches up from far back.

Today each is expected to stop sequencing for a time that grows with the number of records read. The
same measurement is repeated after steps 2, 3 and 4, and the phase is not finished unless the stall has
gone.

**Then the library.** `WalReadingThread`, the part on the owning thread (`WalFollowerService`),
`WalFollowerClient`, the four messages, the framework's notification to a named thread, and an
application thread kept on the background cores. Tested
without the venue:

- tests of each part on its own, named for what each checks;
- a test with real threads and a real reactor, in which one thread writes records at a high rate while
  several followers of each of the three kinds catch up from different positions, connections are lost
  and made again, and the instance stops leading and leads again. At the end, each follower must have
  received every record up to its limit exactly once, in order, with no gap at the hand-over;
- the same test in the ThreadSanitizer build, which checks that the two threads never use the same
  memory without the ordering described in section 3.2. That build does not compile today
  ([BUG-0150](../bug_list.md#bug_0150)), so step 1 repairs it first;
- deliberate faults, each of which must make at least one test fail: the owning thread sending one
  record before the hand-over; the hand-over made without comparing the positions; the reading thread
  reading past the published position; a record sent above the follower's limit; the follower side
  accepting a record that does not follow the last.

**What step 1 built.** The measurements are recorded in section 2.2 and in
[BUG-0155](../bug_list.md#bug_0155) and [BUG-0156](../bug_list.md#bug_0156). The library's way of following a
WAL is in `WalFollowing.hpp`, `WalReadingThread`, `WalFollowerService` and `WalFollowerClient`, with the four
messages in `replication_messages.dsl`, and the two framework facilities in `ApplicationThreadConfiguration`
and `ApplicationThread`. Its tests are `WalFollowerClientTest`, `WalFollowerServiceTest` and
`WalFollowingIntegrationTest`. Repairing the ThreadSanitizer build found three defects in the framework,
[BUG-0157](../bug_list.md#bug_0157), [BUG-0158](../bug_list.md#bug_0158) and
[BUG-0159](../bug_list.md#bug_0159), all fixed, and it now checks the real lock-free queue
([BUG-0160](../bug_list.md#bug_0160)). Four things differ from what is written above:

- **The integration test covers two kinds of follower, not three:** one served up to the last record
  written and one up to the last committed. Serving up to a position the application publishes is the same
  code with a different position, and is tested with the matching engine in step 3. Losing a connection and
  making it again happens in the test only incidentally, and stopping and taking the lead again is tested
  in the service's unit tests rather than with real threads; both are tested with real instances in steps 2
  and 4, where the scenarios do them.
- **The fault of the owning thread sending a record before the hand-over** is a fault in the application,
  which decides when to send, not in the library, and is checked when the sequencer uses it, in step 3.
- **The fault of reading past the published position** could not be seen in normal running. The reading
  thread opens its cursor again at its own position when it runs out of segment files, and a cursor's
  position moves only when it returns a record, so even a cursor that wandered past the end of what was
  written is put back. And ThreadSanitizer cannot see the reading thread and the writing thread touch the
  same bytes, because each maps the files itself, at different addresses. The rule is kept, because without
  it the reading thread could read bytes still being written; it is checked by a test of its own, in which
  the owner publishes a write position behind the limit and the reading thread must stop at it.
- **A batch also ends after 4,096 records examined,** sent or passed over, so that a follower whose
  converter passes over most records, as a matching engine's passes over every execution report, does not
  hold the reading thread for long.

### 5.2 Step 2: the repair

The scenarios that restart or rejoin a follower sequencer (among them 3, 30, 60, 61 and 62) and the library's
`ReplicatedWalTest`, including its randomised test, must pass; the randomised test is extended so that
records are written while a follower is being repaired. The stall measurement for a rejoining follower
must show no stall.

### 5.3 Step 3: the matching engine

Every scenario that starts, restarts or promotes an engine must pass, among them 10, 16, 24, 50, 53, 54, 59 and 66. The stall
measurement for an engine's catch-up must show no stall.

### 5.4 Step 4: the publishers

Before the fix, a new scenario shows BUG-0153: a publisher restarted during the day must not write any
record twice into its own WAL, and after the leading sequencer is stopped while it holds records its
follower has not acknowledged, the publisher's WAL must hold the same record under each number as the
new leader's WAL. It must fail before the change and pass after it. The stall measurement for a
publisher's subscription must show no stall.

### 5.5 Step 5: the build refuses a new read

`scripts/check_standards.py` refuses any use of `WalReader` or `WalCursor` in the sequencer outside a
table of permitted uses, in the same way as its table of permitted locks, each entry with its reason:
replay mode, which runs with nothing else; the resends and undelivered reports, naming BUG-0113; and the
duplicate check's `LogTailIndex`, naming BUG-0122 and phase 8. It is made to fail on purpose before it is
relied on. The documents that describe the catch-ups (`sequencer.md`, `wal_and_ha.md`,
`follower_log_repair.md`, `change_of_sequencer_leader.md`, `docs/pubsub/mep_oar.md` and
`design_principles.md` for F4) are brought up to date.

After every step, every high availability scenario must pass, the library's tests must pass, and the
order's round trip is measured before and after, as in phase 4.

## 6. What phase 5 does not do

- **Resends and undelivered reports** stay on the sequencer's thread (decision 3). Both read from the
  newest segment backwards and stop as soon as they have what they need, so an ordinary resend reads a
  few segments, measured at 7 milliseconds; one that reaches far back still reads far back. BUG-0113
  keeps them open, for the recovery service it describes.
- **The duplicate check's reading of the WAL** stays until phase 8 replaces the checks against sequencing
  a command twice with one check in the library's WAL.
- **Replay mode** is unchanged.
- **Retention** is unchanged: the WAL is still never reclaimed during the day
  ([BUG-0048](../bug_list.md#bug_0048)). The positions that followers report are what retention would
  one day need, but deciding how long history is kept is not part of this phase.
- **No rule of replication, committing, carrying on alone or repair changes.**

## 7. Cost

- **On the order path:** one store to an atomic variable for each record written, one for each change
  of the committed and released positions, and, for each new record, a check of whether each follower is
  being caught up. Each is a few nanoseconds. The round trip is measured before and after each step.
- **Publishing:** a record is published after it is committed rather than as soon as it is written,
  tens of microseconds later (section 4.3).
- **A pause in reading orders**, about one round between two threads, only when a hand-over falls short
  three times in a row (section 3.4).
- **One more thread in each sequencer process.** It blocks when it has no catch-up to serve, and handles
  no orders, so it needs no core of its own.
- **Code:** the four handlers that read the WAL in `SequencerThread` go, and so does the publishers' own
  subscription code on both sides. The library gains the reading thread, the owning side and the
  follower side.

## 8. Decisions

Four separate decisions, all taken by Andrew on 14 October 2026. Decision 1 was a choice between two
alternatives; decisions 2, 3 and 4 were each answered yes or no on their own.

### Decision 1. How is a follower handed over from records read by the reading thread to new records? (Choose A or B.)

**Background.** A follower being caught up is sent old records read from the WAL, and after that new
records as they are released. Today one thread does both, and writes nothing while it reads, so there is
nothing to hand over. With the reading on a thread of its own, records keep being written while the
reading thread reads, and the change from one to the other must be made at an exact position.

**A. One connection, handed over by the thread that owns the WAL.** The reading thread sends the old
records on the follower's existing connection; when it has finished, the owning thread compares
positions and either takes over or asks it to carry on (section 3.4). The followers receive everything
on one connection in order, as they do now, so their own handling of the records does not change. It
needs the framework's notification to a named thread, and the bounded pause when a hand-over keeps
falling short.

**B. Two connections, joined by the follower.** The reading thread has a listening port of its own, and
a follower opens a second connection to it for old records, while new records arrive on its existing
connection from the moment it asks. The follower side holds the new records back until the old ones
reach them, then drops the overlap. No thread ever sends on another thread's connection and no pause is
needed. But the follower must hold every new record that arrives during its catch-up, which in a long
catch-up on a busy day is a large and unbounded amount of memory; every follower needs a second
connection, and the sequencer a second port for each instance, in every configuration and deployment.

**Recommendation: A.** It keeps the followers as they are and bounds both memory and time; the only
price is a pause of a few microseconds that happens only when a steady stream keeps a hand-over from
finishing.

**Decided: A**, by Andrew on 14 October 2026.

### Decision 2. Do the matching engine and the publishers move onto the library's own messages? (Yes or no.)

**Background.** The engine asks with `MePositionRequest` and is answered with `MePositionAck`; the
publishers ask with `WalSubscribeRequest` and are answered with `WalSubscribeAck`. Each has its own
code on both sides. The library's reading thread could serve either set of messages as they are, which
is less work.

**If yes:** both use the library's `WalFollowerClient` and the four messages of section 3.1, and the
old messages and both sides' code for them are removed. The checks each follower needs, the count of
records (R-0101), the refusal of a gap, and asking again at a change of leader, are written once, in the
library.

**If no:** the library serves each set of messages as it is, and each follower keeps its own code. The
publishers' code still has to gain the refusal of a gap and asking again at a change of leader, which is
the part of BUG-0153 that lives on the follower's side.

**Recommendation: yes.** Section 4 of the agreed boundary design says the library provides one way to
follow the WAL, and BUG-0153 shows what happens when each follower has its own: the publishers' way
lacked what the engine's had.

**Decided: yes**, by Andrew on 14 October 2026.

### Decision 3. Do resends and undelivered reports stay on the sequencer's thread in phase 5? (Yes or no.)

**Background.** A member's resend request and the reports sent to a member whose session is bound again
both read the sequencer's WAL on its thread (section 2.1). They are not catch-ups: they look for one
member's execution reports in a range, and are answered on the gateway's connection. BUG-0113 records
them, and the answer it describes is a separate recovery service. Both already stop reading as soon as
they have what they need, so an ordinary resend reads a few segments.

**If yes:** phase 5 leaves them as they are, the build's table of permitted reads names BUG-0113 for
them, and they move once, to the recovery service.

**If no:** phase 5 also runs them on the reading thread. The reports sent to a member whose session is
bound again must then be handed over against the member's new reports, in the way of section 3.4 but
for one member at a time, which is new work; and both move a second time when the recovery service is
built.

**Recommendation: yes.** They are not part of what phase 5 is for, they are bounded already, and moving
them twice is the kind of change on top of a change that the plan is meant to avoid.

**Decided: yes**, by Andrew on 14 October 2026.

### Decision 4. Is the design as a whole right to build, in the order of section 1? (Yes or no.)

**Background.** Sections 3 and 4 say what is built and how each reader moves, and section 5 says how
each step is known to be right. Step 1 includes repairing the ThreadSanitizer build (BUG-0150), because
the new code is the first since phase 2 in which two threads share memory on purpose, and that build is
the check made for it.

**Recommendation: yes.**

**Decided: yes**, by Andrew on 14 October 2026.
