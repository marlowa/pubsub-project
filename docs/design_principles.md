# Design principles {#design_principles}

> **Status: draft, being agreed with Andrew, one principle at a time.** This is phase 1 of
> [the plan for restoring the design](recovery_plan.md). Nothing in the code is to be changed on the
> strength of this page until it is agreed. Where a principle is broken today, the entry says so and
> names the bug that records it.

## What this page is for

This is the one list of the principles the venue's design rests on. There is no other: other
documents describe how particular parts are built, and link here for the principles rather than
stating their own.

For each principle the page says:

- what the principle is, in one or two sentences;
- why the design needs it;
- the place in the code that is meant to make it hold;
- the tests or checks that fail when it is broken;
- whether it holds today, and if not, which bug records that.

Each "Today" line was worked out from the code and the bug list as they stood on 9 October 2026.
The HA scenarios were not run again to write this page; a principle marked as holding is one whose
tests passed when last run and whose code has not changed since. A principle marked "not yet
checked" has not been compared with the code.

The detail stays in the documents linked from each entry. When a principle and another document
disagree, one of them is wrong, and the disagreement is to be raised, not settled quietly in the code.

Every change to the code is to be checked against this list before it is committed. A change that
needs a principle broken, or a new one added, is a change to the design, and is to be raised as one.

The principles come in two groups. The framework principles say how the work inside one process is
done: how it is divided among threads, and how memory, cores and shutdown are handled. The venue
principles say what the venue as a whole promises its members, and how its components keep those
promises when a process or a machine fails. Each principle has a label, F for the framework and V
for the venue, followed by a number, so that other documents and commit messages can refer to it.

## Words used here

- An **order-handling thread** is a thread that every order, cancel or execution report passes
  through on its way through the venue. In each order gateway, the sequencer and the matching engine
  these are the application thread that does the component's work, and the reactor thread that
  carries its network traffic.
- To **own** a piece of state is to be the only thread that changes it. Other threads may read it
  only where the state is built for that, as described under F1.
- A **lock** means a mutex of any kind, a condition variable, a semaphore, a file lock, or a loop
  that waits for another thread to release something. A lock-free queue or a single atomic variable
  is not a lock.

---

## Framework principles

### F1. Each piece of changing state has exactly one owning thread

**Agreed** with Andrew, 11 October 2026.

**The principle.** Only one thread ever changes a given piece of state. Another thread that needs the
state, or needs it changed, sends the owning thread a message through its queue. The one exception
is a value that one thread writes and others only read, held in an atomic variable or in a structure
built for exactly one writer, such as `SingleWriterHistogram`; even then, only one thread writes.

**Why.** It is what lets the framework do without locks. A piece of state with two writers needs a
lock, or a careful protocol between the two threads, and each such protocol has to be reasoned about
on its own. With one writer, nothing needs reasoning about.

**Where it is meant to hold.** In the structure of the code: each `ApplicationThread` subclass owns
its own members and receives everything else through `on_itc_message`, `on_framework_pdu_message`,
`on_timer_event` and the other callbacks listed in [threading.md](framework/threading.md). Helper
threads, such as the log's segment helper (`WalWriter`), the identifier builder
(`LoggedCommandIdentifiersBuilder`) and the FIX capture writer (`FixCapture`), own their own state and
hand results back through atomics written by one side only.

**What checks it.** Only the one-writer check in `SingleWriterHistogram` and in
`IncrementalRehashMap`, compiled into Debug, AddressSanitizer and coverage builds. Nothing checks the
principle in general.

**Today: broken in one place.** On a following sequencer, the sequencer's log has two writers. The
reactor thread writes the records the leader sends, through a handler that the sequencer installs on
the peer connection (`SequencerThread::install_peer_wal_inline_handler`). The sequencer's own thread
writes records the handler passes on to it. The table of epochs in the log has the same two writers.
Two locks were added to keep the two writers apart (see F2), and two atomic variables
(`replicated_records_queued_` and `follower_log_agreed_`) exist only to coordinate them. Recorded as
[BUG-0124](bug_list.md#bug_0124).

### F2. Fast paths take no locks, and no order-handling thread waits for another thread

**Agreed** with Andrew, 11 October 2026.

**The principle.** An order-handling thread never takes a lock and never waits for another thread to
finish something. It may block only in the one place the framework provides, waiting for work when
its queue is empty or its sockets are idle.

**Why.** A thread that waits for another can be put to sleep by the kernel, and waking it costs tens
of microseconds or more, which is a large part of the venue's whole round trip of about 98
microseconds (`docs/operations/latency_findings.md`). A lock that is almost never contended still
puts that cost on the rare order that meets it, and each lock added makes the next one easier to
justify.

**Where it is meant to hold.** In the choice of structures: `LockFreeMessageQueue` between threads,
compare-and-swap in the pool allocators, and single-writer state under F1.

**What checks it.** Nothing in the build. The locks taken today were found by a search of the source
and by recording, with `perf`, every time a thread of the running venue went to sleep waiting for a
mutex (`lock-audit-report.txt`).

**Today: broken in four places.**

| Lock | Where | Taken | Bug |
|---|---|---|---|
| `log_epochs_mutex_` | `SequencerThread` | On every write to the sequencer's log, on the leader and the follower | [BUG-0124](bug_list.md#bug_0124) |
| The mutex in `ReplicatedRecordWriter` | Following sequencer | On every record the leader sends, which is inside every order's round trip | [BUG-0124](bug_list.md#bug_0124) |
| The mutex and condition variables in `BackgroundPromiseRecorder` | Sequencer and matching engine application threads | Every 100 milliseconds, at each lease tick; and, rarely, a wait for a disk sync to finish | [BUG-0126](bug_list.md#bug_0126) |
| The expansion mutex in `ExpandablePoolAllocator` | Every queue between threads, and the gateways' open-order pools | Only when a pool runs out, and then while a new pool is mapped from the operating system | [BUG-0127](bug_list.md#bug_0127) |

The first two exist only because of the two writers described under F1.

Two more locks are taken during the trading day, but not for each order.

`Reactor::timer_registry_mutex_` is taken when an application thread starts or cancels a timer, and
by the reactor thread when it creates or destroys the timer. It is not accepted as an exception. The
framework has no way to create a timer separately from starting it, so every start creates a new
timer and every cancel destroys one, and nothing stops that from happening on the order path. Timers
are to be created during initialisation and only started and stopped afterwards, which leaves the lock
nothing to protect. Recorded as [BUG-0128](bug_list.md#bug_0128).

`Reactor::thread_registry_mutex_` is taken by the reactor thread in its once-a-second checks for
threads that have exited or are stuck in a handler. It is not accepted as an exception either. The
detection is kept; the lock is not needed for it, because the tables it guards change only at start-up
and shutdown. Recorded as [BUG-0129](bug_list.md#bug_0129), which also makes the two handler times the
check reads atomic.

### F3. The reactor thread runs only framework code

**Agreed** with Andrew, 11 October 2026.

**The principle.** A component's own logic runs on its application thread. The reactor thread reads and
writes sockets, runs timers, and delivers events to application threads' queues. It does not run code
that belongs to a component.

**Why.** If a component's code runs on the reactor thread, the component's state has two threads
touching it, which breaks F1, and the component's work delays every other connection the reactor
serves. It also hides from the reader of a component which thread is running which line.

**Where it is meant to hold.** In `Reactor` and `ApplicationThread`: the reactor turns network data
into events on an application thread's queue.

**What checks it.** Nothing; the framework offers a way round it.

**Today: broken in one place.** `ApplicationThread::install_inline_pdu_handler` lets a component run
its own handler on the reactor thread, inside the parser. Only the sequencer uses it, for the follower
writing the leader's records (F1). When the sequencer no longer uses it, `install_inline_pdu_handler`
is removed from the framework, so that the framework no longer offers a way round this principle.

### F4. No order-handling thread waits for the disk

**Agreed** with Andrew, 11 October 2026.

**The principle.** No order-handling thread syncs a file to disk, or does anything else that can wait
for the disk, during the trading day.

**The exception: a change of role.** While an instance is changing role it is not handling orders,
and two things it does then must wait for the disk:

- **Recording a new epoch.** When a sequencer or a matching engine moves to a new generation,
  `EpochStore::store` writes the epoch to a file and syncs it on the application thread before the
  new generation is acted on. The wait is what makes the change safe: without it, a process that
  crashed before the epoch reached the disk could restart believing it still owned a generation
  that has already been used.
- **Cutting back a follower's log.** When a follower sequencer repairs its log to match its leader's,
  `Wal::truncate_after` zeroes the rest of the segment and syncs it on the sequencer's thread.

**Why.** A sync can take hundreds of milliseconds (BUG-0070, BUG-0107), and the thread can do nothing
else meanwhile.

**Where it is meant to hold.** The sequencer's log is written into memory-mapped segments with no
sync per record; a record is safe because it is held on two machines, not because it is on disk
([wal_and_ha.md](availability/wal_and_ha.md), "Two levels of commit"). New segments are created ahead
of time by the log's helper thread, which keeps several segments ready (see below). Lease promises are written and synced by
`BackgroundPromiseRecorder` on a thread of its own.

**What checks it.** Nothing in the build. Two metrics show whether the log's helper keeps pace:
`wal_segments_waited_for` counts the times the writing thread had to wait for the helper to finish a
segment, and `wal_segments_filled_inline` counts the segments the writing thread created itself. The
first should be 0, and the second 1, for the first segment at start-up, which nothing can prepare in
advance.

**Creating log segments.** Each new segment is filled with zeros when it is created, so that later
writes into it allocate nothing; filling is the one step that can wait for the disk. The helper
thread does it ahead of the writer, and keeps a configurable number of filled segments ready, four by
default, so that the writer only waits if the disk stalls for longer than it takes to fill four
segments with records. If the helper has failed, the writing thread creates and fills the next
segment itself. That is the one accepted exception to this principle: it is counted, logged as a
warning, and treated as an incident.

**Today: broken in two places, both uncommon.**

- `BackgroundPromiseRecorder::record()` writes and syncs a record on the calling thread, which is the
  sequencer's or the matching engine's application thread, when the record held does not cover the
  promise about to be made: when an instance first promises its vote to another, or when a background
  write has not finished within five seconds. Recorded as [BUG-0126](bug_list.md#bug_0126).
- The log's helper keeps only one segment ready. If it is still filling that segment when the writer
  needs it, the writer waits for it, spinning until it finishes; and the writer creates a segment
  itself, if the helper has failed, without logging a warning. Recorded as
  [BUG-0130](bug_list.md#bug_0130).

### F5. Nothing on the order path allocates from the heap

**Agreed** with Andrew, 11 October 2026.

**The principle.** Handling an order, a cancel or an execution report allocates no memory from the
general heap. Memory comes from pools, bump allocators, slabs and fixed buffers sized at start-up.

**Why.** The heap allocator takes locks of its own and its cost varies; a pool's does not.

**Where it is meant to hold.** The framework's allocators ([allocators.md](framework/allocators.md)),
slab-backed message payloads, and fixed-capacity structures such as `FixedCapacityRingBuffer`.

**What checks it.** Nothing, on demand. Recorded as [BUG-0087](bug_list.md#bug_0087).

**Today: broken on every order in the leading sequencer.** It records which session each order came
from in `seq_no_to_session_`, a `std::unordered_map`, adding an entry for every order; adding a new
key to a `std::unordered_map` allocates a node from the heap. An execution report that arrives before
the follower has acknowledged the order it answers is copied into `pending_er_`, a `std::multimap`,
with its payload in a `std::vector` and its `ClOrdID` in a `std::string`, which is up to three
allocations for each such report. Both are recorded as [BUG-0131](bug_list.md#bug_0131). Other standard
containers on the order path have not been checked.

### F6. Messages are not copied

**Agreed** with Andrew, 11 October 2026.

**The principle.** A message's bytes are not copied from one buffer into another once they exist, in
either direction. On the way in, they are read from the socket straight into a slab chunk, and that
chunk is handed to the application thread. On the way out, the application thread encodes the
message directly into a slab chunk, and the reactor sends from that chunk. Encoding a message into
its outgoing buffer is producing the message, not copying it.

**Why.** Copying costs time in proportion to the message's size, on every message.

**Where it is meant to hold.** On the way in, in `PduParser`, which reads the payload directly into
the slab chunk, and in the hand-over of that chunk to the application thread
([reactor.md](framework/reactor.md), "Inbound PDU path"). On the way out, in
`PduFramer::send_prebuilt`, which sends from the slab chunk by pointer.

**What checks it.** Nothing.

**Today: holds in the framework; broken in the sequencer in two places.**

- Execution reports that wait for the follower are copied, as described under F5. Recorded as
  [BUG-0131](bug_list.md#bug_0131).
- The following sequencer sends its acknowledgement of the leader's records with `PduFramer::send`,
  which copies the payload into a buffer inside the framer. It does so from the inline handler on the
  reactor thread, which is removed under F1 and F3 ([BUG-0124](bug_list.md#bug_0124)); when it is,
  nothing uses `PduFramer::send` on the order path.

### F7. Threads stay on their cores

**Agreed** with Andrew, 11 October 2026.

**The principle.** Each component's hot-path threads, its reactor thread and its application
threads, are pinned to the cores the machine's layout gives that component, and stay there. A
component's threads share the hyperthreads of one physical core; a physical core is never shared
between components. Every other thread runs in a shared background tier and never on a hot-path core.
The layout is decided before any process starts, not claimed at run time.

**Why.** A thread moved between cores loses what it had in the core's caches. A component's reactor
thread and application thread exchange messages constantly, and on the two hyperthreads of one
physical core they share the level-one cache, so the hand-over is nearly free; measured on the same
load, the round trip's 99th percentile was 471 microseconds with each component's threads together on
one physical core, against 2.4 to 3.0 milliseconds with one thread per physical core. Two different
components on one physical core simply compete for it. Cores cannot be claimed at run time, because
what is free depends on which processes have started so far
([cpu_pinning_anti_affinity.md](framework/cpu_pinning_anti_affinity.md)).

**Where it is meant to hold.** The layout is declared for each environment and resolved into core
numbers on the target machine by `deploy.py`, using `scripts/cpu_layout.py`, which fills each physical
core with one component or leaves it alone. Every process starts confined to the background tier, and
the reactor then moves its hot-path threads onto the component's cores
([cpu_pinning.md](framework/cpu_pinning.md)).

**What checks it.** Nothing in the build. The layout was checked by hand on the development
workstation: every ranked component landed on the cores the layout allocated it, and the JVM
components started in the background tier.

**Today: holds**, as far as that check goes.

### F8. Shutdown is deterministic

**Agreed** with Andrew, 11 October 2026.

**The principle.** A process shuts down within a bounded time. Each thread is told to stop, given a
fixed time to leave its run loop, and joined with a timeout. A thread that does not stop in that time
is never left running: the process is ended deliberately, and the thread that would not stop is
logged.

**Why.** A process that cannot be relied on to stop cannot be relied on to be restarted.

**Where it is meant to hold.** The thread lifecycle state machine in `ApplicationThread`, the
descriptor that wakes `epoll`, and `ThreadWithJoinTimeout` ([threading.md](framework/threading.md)).

**How it is done.** `Reactor::finalize_threads_after_shutdown` cancels each thread's timers, tells each
thread to shut down, waits up to `shutdown_timeout_` (one second by default) for each to stop running,
and then joins each with the same timeout. The waits are made one thread at a time, so the bound is
twice the shutdown timeout for each thread. The log's helper thread is given up to 30 seconds to join,
because it may be in the middle of writing a segment. A thread that does not join is recorded, and
`abort_if_thread_abandoned` ends the process.

**What checks it.** Not checked in the build.

**Today: broken in one known place.** [BUG-0119](bug_list.md#bug_0119) records an application thread that can
go on using its reactor after the reactor has been destroyed.

---

## Venue principles

Several of these are the guarantees G1 to G5 listed in section 3 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md), which says that each holds
at every moment, not only during a change of leader.

### V1. One order of events

**Agreed** with Andrew, 11 October 2026.

**The principle.** The leading sequencer alone decides the order in which the matching engine sees
commands. Every component that needs the order of events takes it from the sequencer.

**Why.** Two components deciding the order independently could disagree, and the venue would have no
single history.

**Where it is meant to hold.** In the sequencer, which numbers every command it logs, and in the
matching engine, which acts on commands in the sequencer's order. The matching engine connects only
to the two sequencers, its peer engine and the two arbiters; the peer connection carries the leading
engine's book to the follower, and that book was built in the sequencer's order.

**What checks it.** Nothing directly.

**Today: holds.** The matching engine changes nothing in its book on its own initiative: its one
recurring timer records only that it is alive. Two kinds of event are to travel through the
sequencer's log when they are built, and are not built yet: changes of trading phase and halts
([trading_phases.md](venue/trading_phases.md); the venue cannot declare a halt at all, which is
[BUG-0065](bug_list.md#bug_0065)), and events driven by the time of day, such as good-till-date
expiry, which the engine records and nothing acts on.

### V2. The log is the record

**Agreed** with Andrew, 11 October 2026.

**The principle.** The leading sequencer's log holds every command the venue accepted during the
trading day, in the order V1 gives them. It is the venue's authority: from it, together with the
reference data loaded at start of day, everything else the venue holds can be rebuilt, the matching
engine's book included. Other durable stores, such as the matching engine's open-order region, are
copies kept so that recovery is fast; they are never a second record.

**Why.** It gives the venue one authority to recover from, whatever else is lost.

**Scope.** The log holds one trading day: every day starts with an empty log, and the logs are
archived after end of day. Every order is a day order, and anything still open at the end of the
trading day is to be cancelled, and its member told, before end of day. Orders that outlive the
trading day, known as long orders, are deliberately not implemented: they are business functionality
that would bring out no new requirement of the library. If they ever are implemented, they are
restored at the start of each day, from a database export as the reference data is, and re-entered as
commands the sequencer logs, so that each day's log still holds everything that day's venue holds.

**Where it is meant to hold.** In the sequencer's log ([wal.md](durability/wal.md)) and in the matching
engine's catch-up from it. A restarted matching engine does not replay the whole day, which takes
about 40 seconds at 40 million orders a day: it reads its open orders back from its open-order region
([open_order_checkpoint.md](durability/open_order_checkpoint.md)) and takes only the tail of the log
beyond the position the region records. If the region cannot be used, it establishes from the
sequencer's record what was open, cancels each order, tells the members and halts.

**What checks it.** Scenarios 50, 51 and 52 of the high availability tests cover the three recovery
paths of the open-order region.

**Today: holds within the day, with one gap at the end of it.** The log keeps the whole day: the
snapshot deletes nothing, and nothing calls `Wal::truncate_below`. But nothing cancels the orders
still open at the end of the trading day; they disappear when the processes are stopped and the logs
archived, and their members are never told. Recorded as [BUG-0132](bug_list.md#bug_0132).

### V3. At most one instance of a pair acts as leader at any moment

**The principle.** An instance of the sequencer, the matching engine or the arbiter acts as leader
only while a majority of its three voters grants it a lease. Requirement R-0146.

**Why.** Two leaders acting at once would sequence or match two different histories.

**Where it holds.** In the lease classes in `applications/fix_common/`: `LeaseVoter`, `LeaseHolder`,
`LeaseParticipant`, driven by `PairLeaseAgent`. Each component supplies only the connections the agent
sends on. The lease rules, and which class holds each, are in sections 3 and 10 of
[majority_leases.md](availability/majority_leases.md).

**What checks it.** `LeaseRulesTest`, `LeaseSimulationTest`, `PairLeaseAgentTest`,
`ComponentLeaseVotersTest` and `LeasePromiseStoreTest`; `ha_test.py` scenarios 8, 9 and 15; and the
TLA+ models, whose counterexamples are rerun on every install ([tla/findings.md](availability/tla/findings.md)).

**Today: holds.** It is the one venue principle enforced in one place, by classes that know nothing of
the component using them.

### V4. A follower that lacks commands the engine acted on never leads

**The principle.** A following sequencer that does not hold every command the matching engine has
acted on does not take the lead. Rule 11 in [majority_leases.md](availability/majority_leases.md).

**Why.** If it led, the venue would continue from a history that is missing orders the engine has
already matched.

**Where it holds.** In the lease classes (the statement that the follower may not lead travels on the
lease requests and is recorded by the voters), and in the leading sequencer, which acts on a command
its follower does not hold only once another voter has recorded that statement
(`SequencerThread::may_act_without_follower`). Design:
[a_follower_behind_does_not_lead.md](availability/a_follower_behind_does_not_lead.md).

**What checks it.** `ha_test.py` scenarios 61, 63 and 64; `LeaseRulesTest`; the simulation; the
model `FollowerBehindHA.tla`.

**Today: holds.**

### V5. The matching engine acts only on commands the logs hold (G2)

**The principle.** The leading sequencer sends a command to the matching engine only once its follower
has written and acknowledged it, or, when it is running without a follower, only under V4.

**Why.** If the leader's machine died, the engine would hold an order that no surviving log holds, and
the venue could not account for it.

**Where it holds.** In the leading sequencer: `hold_until_acknowledged`, `release_if_confirmed` and
the running-alone logic in `SequencerThread`.

**What checks it.** `ha_test.py` scenario 59.

**Today: holds.**

### V6. Every command a gateway accepted is answered (G1)

**The principle.** Every command a gateway accepted from a member is either placed and reported, or
refused with a reply. It is never left unanswered.

**Why.** A member that hears nothing cannot tell whether its order is in the market.

**Where it holds.** In the gateways, which keep each command until it is answered and send again what
they hold when the leader changes or their connection to it is made again
([commands_during_a_change_of_leader.md](availability/commands_during_a_change_of_leader.md),
sections 3.1 and 3.2; `UnansweredCommandStore`). Sending again is safe only because of V7.

**What checks it.** `ha_test.py` scenarios 1, 66, 68 and 71.

**Today: holds**, as far as those scenarios test it.

### V7. A command is sequenced at most once, judged by its identity

**The principle.** The leading sequencer places a command in the log at most once, judged by the
member's comp id, the protocol and the `ClOrdID`, however many times and by whatever path the command
arrives. A command the log already holds is answered, not sequenced again. Requirements R-0006 and
R-0119 cover the member's side: a resubmitted order is refused as a duplicate, for the rest of the day.

**Why.** It is what makes it safe for a gateway to send a command again whenever it is unsure (V6).

**Where it should hold.** In one check, in the leading sequencer, applied to every command.

**What checks it.** `ha_test.py` scenarios 67, 68 and 71; unit tests of `LoggedCommandIdentifiers`
and `LogTailIndex`.

**Today: holds on the paths tested, but by several mechanisms, not one.** The leader checks a command
against its log only when the gateway has marked it as sent again. Each way a command can arrive twice
has then needed its own argument or its own check: after a change of leader, after a connection is
made again, and while a gateway has two connections open to the leader. Recorded as
[BUG-0122](bug_list.md#bug_0122). Separately, the matching engine refuses a repeated `ClOrdID` only
while the first order is still in its book, so an identifier can be used again once its first order
has ended, which R-0119 forbids: [BUG-0114](bug_list.md#bug_0114).

### V8. Every report the matching engine produces reaches the member (G3)

**The principle.** Every execution report reaches the member it is for. A member may receive a report
more than once, provided every repeat is marked as one (R-0122).

**Where it holds.** In the sequencer, which forwards reports to the gateway the session is bound to and
keeps the reports a gateway has not received; in a new leader, which forwards the reports its
predecessor had not; and in the gateways' resend handling. Design: section 4.4 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md), and
[session_binding.md](availability/session_binding.md).

**What checks it.** `ha_test.py` scenario 65, and the session scenarios.

**Today: holds on the paths tested.** It is spread across several of the sequencer's jobs: pending
reports, kept reports, the samples of what the leader forwarded, and the session state.

### V9. Within a log, sequence numbers only go forward (G4)

**The principle.** No sequence number is used twice in a log. A new leader numbers from the highest
record it holds.

**Where it holds.** In the sequencer's numbering when it takes the lead (section 4.1 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md)).

**What checks it.** The numbering checks in `ha_test.py` scenarios 1 and 59.

**Today: holds.**

### V10. A follower's log becomes a copy of its leader's (G5)

**The principle.** When an instance rejoins as a follower, its log ends up identical to its leader's:
records the leader does not hold are discarded, and records it lacks are sent to it.

**Where it holds.** In the follower's log repair ([follower_log_repair.md](availability/follower_log_repair.md)),
in `SequencerThread`.

**What checks it.** `ha_test.py` scenario 62.

**Today: holds**, but the repair is the part of the sequencer where the two writers of F1 meet, and
it is to be checked again when the second writer is removed.

### V11. A fault in the standby is a loss of resilience, not a loss of service

**The principle.** While the leader is working, nothing wrong with the follower stops the venue
trading. The leader carries on alone and reports loudly that it is doing so.

**Why.** Otherwise a fault in the backup becomes an outage, and a follower that is alive but stalled
becomes more dangerous than one that has died.

**Where it holds.** In the leading sequencer's running-alone logic, together with V4, which keeps a
follower that fell behind from leading later. Decided in section 7 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md).

**What checks it.** `ha_test.py` scenario 63.

**Today: holds.**

### V12. Every trading day starts fresh

**The principle.** At the end of a trading day everything is halted, and the logs are archived. A log
never spans two trading days, and no state is carried from one day into the next except through the
start-of-day loading of reference data.

**Why.** It bounds how much any component has to read or recover, and it means a defect cannot carry
damage from one day into the next.

**Where it holds.** In operations: the end-of-day procedure. The phases of the day are in
[trading_phases.md](venue/trading_phases.md).

**What checks it.** Nothing automated.

**Today: holds by procedure.**

---

## Proposed additions, not yet agreed

These are positions Andrew has stated that are not yet on the list. Each is to be accepted as a
principle, or left out.

- **(a)** Primary and secondary are fixed identities, tied to the instance id. Leader and follower are
  roles, and change.
- **(b)** A supervisor starts processes. It never decides which instance leads; only the voters do.
- **(c)** An arbiter that cannot be sure declines, rather than guess.
- **(d)** Nothing the venue needs in order to run may depend on something optional, such as
  Prometheus.
- **(e)** Every matching engine catches up on the log before it acts on anything.
- **(f)** Threads are created only through `ThreadWithJoinTimeout`, never as a raw `std::thread`.
- **(g)** An instance interrupted twice in a short time is treated as a loss of service and is not
  restarted.

## Open questions

These need Andrew's decision before this page is agreed. They are separate questions, not
alternatives.

4. **V12 is written down here for the first time.** Is it stated correctly?
