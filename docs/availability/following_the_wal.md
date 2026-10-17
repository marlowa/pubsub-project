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
The matching engine and the matching engine publishers both use them (decision 2).

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
  caught up in order to lead (`EngineOrderRouting::caught_up_to_lead`);
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

There is only ever one repair at a time. The follower asks where the two WALs agree on each of its
connections to the leader, so the same request can arrive twice. While the reading thread is sending the
records after a position, a second request from that same position is not answered, or the follower would
be sent every record twice. A request from any other position means the follower no longer agrees with
what it is being sent, for example because a record arrived out of order: the leader tells the reading
thread to stop, and does not answer until the reading thread has confirmed that it has stopped. Every
record the reading thread was still sending is then queued on the connection ahead of the answer, so the
follower discards them before it takes up from the answer, rather than seeing them after it as a gap and
asking again. The leader answers as soon as the reading thread has confirmed, without waiting for the
follower to ask again. This matters because a follower asks again every time its role changes, and a
newly started follower changes role from unknown to follower within its first few seconds, usually while
its first catch-up is still running. [BUG-0155](../bug_list.md#bug_0155) shows why these rules matter: after the
follower missed 1.2 million records the leader sent them twice, and counted the follower as keeping up from
the moment it began, so new orders waited behind the whole repair. No rule of replication, committing or
repair changes; only the thread that reads the missing records.

### 4.2 The matching engine

The engine's catch-up and the two places where the sequencer sends the engine orders from its WAL
become one thing: the engine is served from a position, up to the last order released to it.

- **A starting or promoted engine** asks with `WalFollowRequest`. A promoted engine asks for new records
  too (`wants_new_records`), and a starting one does not.
- **A new leader that learns how far the engine got** (`EnginePosition`) serves the engine from the
  record after that, followed by new orders, instead of reading them itself.
- **Orders that waited in the WAL** because the storage for held orders was full are no longer read back
  by the sequencer's thread: the engine is served from the first such order, followed by new orders.

The engine's book, its checks and its region are unchanged. The engine's own reading of what it is sent
does not change either, because it receives the same `WalRecord` messages on the same connection.

**A limit on how long the engine waits before asking again.** An engine that is catching up asks again
when a whole period of `catch_up_retry_seconds` (15 seconds as deployed) passes without a single record
arriving. It counts the records it receives, not the records the reading thread examines. The reading
thread sends the engine only orders and cancels, and passes over everything else in the WAL, such as
execution reports and lease promise records, without sending anything. So if the WAL held a stretch of
records with no order or cancel among them, long enough that passing over it took the reading thread more
than 15 seconds, the engine would see nothing arrive and ask again. The sequencer would then stop the
serving under way and start a new one from the last order the engine holds, which is before that stretch,
and the same thing would happen at the next period. The catch-up would never finish, and the engine would
stay waiting and never act. No wrong book results, because the engine acts only when it is told it has
caught up. The reading thread passes over hundreds of thousands of records a second, so this needs a
stretch of millions of records without an order or cancel; in this venue's WAL each order is followed by
about one and a half execution reports, so such a stretch does not occur. If it ever did, the remedy is to
raise `catch_up_retry_seconds`. The reading thread does not report how far it has examined, so the engine
cannot tell this case apart from a sequencer that has stopped serving it.

### 4.3 The publishers

Each publisher uses `WalFollowerClient` towards both sequencers, asking for new records too, and is
served up to the last record committed, by the leading instance only. Its `handle_wal_record_from_sequencer`
writes into its own WAL only the record that follows the last it holds: the client drops a record already
held, and asks again after a gap. The publisher sends no acknowledgement.

The reading thread catches the publisher up and the service hands it over. From then on the sequencer's
own thread sends the publisher each record as it is committed, after the commit's work for the order path,
releasing orders and forwarding reports, is done. By the time a record is committed that thread no longer
holds its bytes, so it reads the record back from the WAL it has just written, which is still in memory.
It does this through the library's `WalHandedOverFollowers`. Reading a record back checks its checksum,
which is most of what sending costs, so each record is read once whatever the number of publishers: one
shared reading reads it, converts it once and sends the result to every publisher in step with it. A
publisher that has just been handed over first reads on a cursor of its own, started where the reading thread
stopped reading for it (`WalReadingReport::resume_at`), so that nothing is searched for, and joins the shared
reading once it holds every record the shared reading has read; in normal running that is at the next
commit. This is a read of the WAL on the sequencer's own thread, of records it wrote moments before, and it is
in step 5's table of permitted reads ([BUG-0171](../bug_list.md#bug_0171)).

Both threads turn a record into what a publisher is sent with the same function,
`publisher_record_from_wal`: every record, as the `WalRecord` envelope the WAL holds. A record that is not
an envelope, or whose envelope does not decode, is damaged, and the publisher is not served past it.
Passing over it would leave a gap in the publisher's numbering, which the publisher would take for
records lost on the way and ask about for ever.

With high availability off there is no reading thread. The sequencer's own thread then catches the
publisher up as well, through the same `WalHandedOverFollowers`, before sending it each record as it is
committed, which with high availability off is as it is written.

A publisher therefore publishes a record a little later than if it were sent as soon as the leader wrote
it: after the follower has acknowledged it. On this machine that is the replication round trip, tens of
microseconds. It is the cost of publishing only what the venue's record holds.

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

**What step 2 built.** The leader's `ReplicatedWal` no longer reads the missing records itself when the
application gives it a `PeerCatchUpInterface` (`use_peer_catch_up`): it asks for them to be sent, and is told
when the follower has been handed over (`on_peer_caught_up`) and when the sending has stopped
(`on_peer_catch_up_stopped`). It publishes its write position and its last committed record to
`WalFollowingPositions` (`publish_positions_to`). New records wait for the follower again only once it has
acknowledged every record up to the hand-over. The sequencer implements the interface with a
`WalReadingThread` and a `WalFollowerService`, serving the follower up to the last record written, and stops
reading orders from the gateways if a hand-over falls short three times in a row. An instance that has
stopped leading, and must discard records its new leader does not hold, waits to cut its WAL back until its
reading thread has stopped sending what it was sending while it led.

The tests are three new tests in `ReplicatedWalTest`: a rejoining follower sent what it lacks through the
catch-up and then new records; new records not waiting for a follower until it has acknowledged through the
hand-over; and a request from a new position answered only once the running catch-up has stopped, without
the follower asking again. `ReplicatedWalTest`'s pairs now run with a stand-in for the reading thread
(`SimulatedCatchUp`) as well as without, and the randomised test runs both ways. Each new test was made to
fail by a deliberate fault before it was trusted.

**How it was measured.** The measurement of section 5.1 was repeated with `tmp/phase5/stall.sh`, with the
WAL holding 1.2 million records. While the follower was repaired, orders went on being answered: of 1,978
orders sent at 200 a second in the ten seconds after it started, the longest wait for a first report was
10.2 milliseconds, against 5.2 before the event, and none went unanswered. The follower was sent 1.2
million records in 8.4 seconds and handed over. Before step 2, the same measurement left 1,645 of 1,979
orders unanswered and the venue answering nothing more. The two later events of the measurement, a
publisher subscribing from the start and an engine catching up, still stop the leader for seconds, and
change the leader: they are [BUG-0156](../bug_list.md#bug_0156) and the engine's catch-up, which steps 4 and
3 move.

All 74 scenarios passed, but for scenario 26, whose leading sequencer lost its lease when writing a lease
promise record took 4.6 seconds on this machine's disk; it passed five times of five when run again. Two
defects found on the way are recorded and fixed: [BUG-0163](../bug_list.md#bug_0163), a starting sequencer
ending when a repair cut its WAL back while it was still reading it; and the ordering of a restarted
catch-up, in which records the reading thread was still sending reached the follower after the answer that
restarted it, so that it saw them as a gap, asked again, and was never answered. Two more are recorded, not
fixed: [BUG-0164](../bug_list.md#bug_0164) and [BUG-0165](../bug_list.md#bug_0165).

### 5.3 Step 3: the matching engine

Every scenario that starts, restarts or promotes an engine must pass, among them 10, 16, 24, 50, 53, 54, 59 and 66. The stall
measurement for an engine's catch-up must show no stall.

**What step 3 built.** The sequencer's reading thread now serves the matching engine in every case where
the sequencer's own thread used to read the WAL for it:

1. **The engine's catch-up.** A starting or promoted engine asks every sequencer with `WalFollowRequest`,
   giving the last order it holds. A sequencer that does not lead re-points its own order connection to
   the engine, declines with `WalFollowDeclined`, and remembers it; when it takes the lead it sends the
   engine `WalLeaderChanged`, and the engine asks again at once. The leading sequencer has the reading
   thread serve the engine from that order, up to the last released, with a converter
   (`engine_record_from_wal`) that sends each order and cancel as the same `WalRecord` the sequencer's own
   thread sends, and passes over everything else. When the engine is handed over, the sequencer sends
   `WalFollowCaughtUp` with the order reached, the first record the WAL holds and the count the reading
   thread sent, which the engine checks (R-0101); an engine that asked to lead then has the orders routed
   to it. An engine that holds nothing is placed at the last order released and sent nothing.
2. **After a change of leader,** when the engine says how far it got (`EnginePosition`), the reading
   thread serves it from the order after that.
3. **Orders that waited in the WAL** because the storage for held orders was full: the reading thread
   serves the engine from the first of them. The size of that storage is a setting,
   `commands.orders_held_at_most`, 16,384 as deployed.

One rule joins the three: while the reading thread is catching an engine up, the sequencer's thread sends
that engine no order itself (`WalFollowerService::is_being_caught_up`). An order released meanwhile is at or
below the limit the reading thread serves up to, so the reading thread sends it, and the hand-over happens
only when the position reached equals the limit. The sequencer publishes the last order released
(`publish_released_position`) before the service compares the two, so that the comparison is never made
against a value that is out of date. The reading thread reads the limit once at the start of each round, as
section 3.4 has it ([BUG-0168](../bug_list.md#bug_0168)). With high availability off there is no reading
thread, and the sequencer reads the WAL on its own thread, as `send_logged_orders` and the catch-up's own
path do.

Stopping the follower sequencer's catch-up stops only that serving, with the reading thread's confirmation
(`WalFollowerService::stop_serving`), so that a matching engine being served goes on being served; every
serving stops, with confirmation, when the instance stops leading.

A record the reading thread cannot read stops the follower being handed over: the converter says the
record is damaged (`ConvertedRecord::damaged`), the reading thread stops serving that follower and reports
the record's number, and the service tells the application (`follower_cannot_be_served`) instead of handing
the follower over. The sequencer says so as an Error the first time and as Info when the follower asks
again, and sets the gauge `sequencer_wal_damaged_record` to the record's number until a follower is handed
over past it. A follower already past the record is served as usual. The sequencer's own catch-up, used
with high availability off, does the same (`engine_catch_up_from_wal`).

**How it was checked.** Unit tests of the service (a damaged report: no hand-over, nor a caught-up
message), of `engine_catch_up_from_wal` with a real WAL holding a malformed record, and of the setting for
the storage; an integration test with real threads in which a follower from before a damaged record is never
handed over and one past it is; and a new scenario, 75, which fills the storage for held orders and requires
every order of a burst to be accepted exactly once. Each was made to fail by a deliberate fault first. In the
last full run, from fresh WALs, 72 of 75 scenarios passed. Scenario 34 failed when a lease promise record
took 7.6 seconds to write ([BUG-0167](../bug_list.md#bug_0167)). Scenarios 14 and 48 failed in that run and
the one before it, with no slow write, and passed every time they were run alone (six times between them).
The cause was a defect in the reactor's queue of commands, which could leave the last command of a burst
unseen until another arrived ([BUG-0170](../bug_list.md#bug_0170)); with it fixed, all 75 scenarios pass. The stall measurement, run before the engine moved to the library's messages,
showed every order answered and an engine's catch-up of 1,034 records taking 56 milliseconds; it has not been
repeated since. No scenario withholds a record from a real catch-up, so refusing a hand-over past a damaged
record is shown only by the unit and integration tests.

Two things differ from what is written above. The engine does not use `WalFollowerClient`: it keeps its own
count check (`CatchUpTally`, R-0101) and its own asking of every sequencer, and only its messages change.
And the old messages, `MePositionRequest` and `MePositionAck`, are removed rather than kept alongside.

### 5.4 Step 4: the publishers

Before the fix, a new scenario shows BUG-0153: a publisher restarted during the day must not write any
record twice into its own WAL, and after the leading sequencer is stopped while it holds records its
follower has not acknowledged, the publisher's WAL must hold the same record under each number as the
new leader's WAL. It must fail before the change and pass after it. The stall measurement for a
publisher's subscription must show no stall.

**What step 4 built.** Each matching engine publisher follows the sequencers' WAL through the library, as
section 4.3 describes: it asks both sequencers with `WalFollowRequest` through `WalFollowerClient`, is
declined by the one that does not lead, is caught up by the leader's reading thread up to the last record
committed, and is then sent each record as it is committed by the sequencer's own thread, which reads it back
from the WAL (`WalHandedOverFollowers`, a new library class). To start where the reading thread stopped, the
reading thread's report that it has reached a follower's limit now carries the position in the WAL to read on
from (`WalReadingReport::resume_at`), which the service passes on when it hands the follower over. The
sequencer sends a publisher nothing as it writes a record. A publisher writes into its own WAL only the record
after the last it holds, and sends no acknowledgement. `WalSubscribeRequest`, `WalSubscribeAck`, the
sequencer's record of the publishers' positions, and the library's integration test of the old messages are
removed; their message numbers, 105 and 106, are not to be used again.

**How it was checked.** A new scenario, 76, restarts a publisher during the day and then has the leading
sequencer die holding three orders its follower never received; it requires each publisher's WAL to hold
every record once, without a gap, each the same as the leading sequencer's record of that number. It failed
before the change, the restarted publisher having written record 2001 twice, and passes after it; in the run
after it, the old leader's WAL held the three orders as records 2007 to 2009, the new leader had given those
numbers to other records, and both publishers held the new leader's records there. Unit tests cover
`WalHandedOverFollowers` with a real WAL, among them records read one at a time across segments as they are
written, a damaged record, and a jump in the numbering, and the converter's three outcomes; each test was made
to fail by a deliberate fault, except that removing the check against the WAL's write position changed no
result, because a cursor that reads into the unwritten end of a segment is opened again at its own position and
loses nothing, so that check saves the cost of opening it again rather than preventing a loss. The stall
measurement, with 1.2 million records in the WAL, showed no stall when a publisher was started with an empty
WAL: the longest wait for an order's first report in the ten seconds after was 5.5 milliseconds, against 5.2
before ([BUG-0156](../bug_list.md#bug_0156)). `perf` on the leading sequencer's thread during a fill showed
the reading back of committed records costing about a fifth of that thread's time
([BUG-0171](../bug_list.md#bug_0171)). Each record is now read once whatever the number of publishers, which
halved the checking of checksums and brought that share down to about a sixth. With high availability off a publisher is still caught up on the
sequencer's own thread ([BUG-0172](../bug_list.md#bug_0172)).

**The order's round trip** was measured as in phase 4, swapping the installed binaries between the build
before this step and the build with it, each arm on fresh state with five runs of 6,000 orders at 100 a
second from a load client pinned to processors 0 and 1, the first run discarded. The secondary matching
engine led in every start, with both builds, so every arm was measured with it leading:

| Build | Median round trip in each of four runs, microseconds |
|---|---|
| Before step 4 | 127.7, 127.3, 127.1, 126.2 |
| After step 4 | 99.0, 107.5, 107.6, 107.4 |
| Before step 4, measured again | 124.8, 125.1, 124.5, 126.6 |

The round trip is about 20 microseconds shorter with the step. The leading sequencer's thread received about
103,000 messages a run before it and 55,000 after: the difference is the publishers' acknowledgements, one
for every record from each publisher, which are no longer sent. And before it the sequencer encoded and sent
each record to both publishers as it wrote it, ahead of passing the order on; now it sends them after the
commit's work for the order path.

### 5.5 Step 5: the build refuses a new read

`scripts/check_standards.py` refuses any read of the WAL in the sequencer outside a
table of permitted uses, in the same way as its table of permitted locks, each entry with its reason:
replay mode, which runs with nothing else; the resends and undelivered reports, naming BUG-0113; and the
duplicate check's `LogTailIndex`, naming BUG-0122 and phase 8; and the records sent to a publisher after
it has been handed over, read back moments after they were written, through the library's
`WalHandedOverFollowers`, naming BUG-0171 for its cost. That last read happens inside a library class, so the
check has to look at what the sequencer constructs, not only at the readers it names. It is made to fail on
purpose before it is relied on. The documents that describe the catch-ups (`sequencer.md`, `wal_and_ha.md`,
`follower_log_repair.md`, `change_of_sequencer_leader.md`, `docs/pubsub/mep_oar.md` and
`design_principles.md` for F4) are brought up to date.

**What step 5 built.** Check 30 of `scripts/check_standards.py` fails on any read of the WAL in
`applications/sequencer` that is not in its table `_PERMITTED_WAL_READS`. A read is a use of anything in its
list `_WAL_READERS`: the library's classes that read the WAL (`WalReader`, `WalCursor` and
`WalHandedOverFollowers`), and the sequencer's own functions and members that read it with one of them
(`LogTailIndex`, `LoggedCommandIdentifiersBuilder`, `engine_catch_up_from_wal`, `send_logged_orders`, and the
members holding the first two and the publishers' `WalHandedOverFollowers`). Listing the sequencer's own
readers means that a new call to a function that already reads the WAL is refused, not only a new use of a
library class. Each entry of the table names the function, or in a header the word on the line, that it
permits, and gives its reason:

- replay mode, which runs with nothing else;
- resends and requests for undelivered reports ([BUG-0113](../bug_list.md#bug_0113));
- the duplicate check ([BUG-0122](../bug_list.md#bug_0122), phase 8);
- the record of command identifiers, built on a thread of its own on the background cores;
- records read back for publishers already handed over ([BUG-0171](../bug_list.md#bug_0171));
- every catch-up with high availability off ([BUG-0172](../bug_list.md#bug_0172));
- orders counted as deferred while no matching engine is connected
  ([BUG-0174](../bug_list.md#bug_0174)), which listing the reads found.

An entry that no longer matches any read is reported too, so that the table cannot go on permitting a read
that has been removed.

**How it was checked.** With the table complete the check passes. It was made to fail on purpose by adding,
one at a time, a use of `WalCursor`, a call of `send_logged_orders`, a call of the publishers' `send_to_all`
and a call of `WalReader::replay` to a function not in the table; each was refused, naming the function. A
mention of `WalReader` in a comment was not. An entry naming a function that does not exist was reported as
matching nothing.

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
