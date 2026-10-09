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

Two more locks are taken by reactor threads during the trading day, but not for each order:
`Reactor::timer_registry_mutex_`, when an application thread starts or cancels a timer, and
`Reactor::thread_registry_mutex_`, in the reactor's once-a-second check of its threads. Whether these
are allowed is open question 1 below.

### F3. The reactor thread runs only framework code

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
writing the leader's records (F1).

### F4. No order-handling thread waits for the disk

**The principle.** No order-handling thread syncs a file to disk, or does anything else that can wait
for the disk, during the trading day.

**Why.** A sync can take hundreds of milliseconds (BUG-0070, BUG-0107), and the thread can do nothing
else meanwhile.

**Where it is meant to hold.** The sequencer's log is written into memory-mapped segments with no
sync per record; a record is safe because it is held on two machines, not because it is on disk
([wal_and_ha.md](availability/wal_and_ha.md), "Two levels of commit"). New segments are created ahead
of time by the log's helper thread. Lease promises are written and synced by
`BackgroundPromiseRecorder` on a thread of its own.

**What checks it.** Nothing in the build. The metric `wal_segments_filled_inline` counts the times
the helper was not ready and the writing thread had to create a segment itself.

**Today: broken in two places, both uncommon.**

- `BackgroundPromiseRecorder::record()` writes and syncs a record on the calling thread, which is the
  sequencer's or the matching engine's application thread, when the record held does not cover the
  promise about to be made: when an instance first promises its vote to another, or when a background
  write has not finished within five seconds. Recorded as [BUG-0126](bug_list.md#bug_0126).
- When the log's helper has not prepared the next segment in time, the thread writing the log creates
  it itself, which writes the whole segment to disk. Open question 2 below.

### F5. Nothing on the order path allocates from the heap

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
allocations for each such report. No bug records these yet. Other standard containers on the order
path have not been checked.

### F6. Incoming messages are not copied

**The principle.** A message's bytes are read from the socket straight into a slab chunk, and that
chunk is handed to the application thread; the payload is not copied on its way through the
framework.

**Why.** Copying costs time in proportion to the message's size, on every message.

**Where it is meant to hold.** In `PduParser`, which reads the payload directly into the slab chunk,
and in the hand-over of that chunk to the application thread ([reactor.md](framework/reactor.md),
"Inbound PDU path").

**What checks it.** Nothing.

**Today: holds in the framework; broken in the sequencer** for execution reports that wait for the
follower, which are copied as described under F5. The two lists this page replaces disagreed about
this principle: one said no copy on any inbound **or outbound** path, the other only that incoming
messages are not copied. Which is meant is open question 3 below.

### F7. Threads stay on their cores

**The principle.** Each hot-path thread is pinned to its own processor core, chosen at start-up from a
registry of cores shared by every process on the machine, and stays there.

**Why.** A thread moved between cores loses what it had in the core's caches, and a thread sharing a
core with an unrelated one waits for it.

**Where it is meant to hold.** In `CpuRegistry` and the CPU layout applied by the reactor
([cpu_pinning.md](framework/cpu_pinning.md), [cpu_pinning_anti_affinity.md](framework/cpu_pinning_anti_affinity.md)).

**What checks it.** Not yet checked.

**Today: not yet checked.**

### F8. Shutdown is deterministic

**The principle.** A process shuts down in a bounded time: each thread's queue stops accepting
messages, the thread is woken and leaves its run loop, and the reactor joins every thread within a
fixed timeout.

**Why.** A process that cannot be relied on to stop cannot be relied on to be restarted.

**Where it is meant to hold.** The thread lifecycle state machine in `ApplicationThread`, the
descriptor that wakes `epoll`, and `ThreadWithJoinTimeout` ([threading.md](framework/threading.md)).

**What checks it.** Not yet checked.

**Today: not yet checked.** [BUG-0119](bug_list.md#bug_0119) records an application thread that can
go on using its reactor after the reactor has been destroyed.

---

## Venue principles

Several of these are the guarantees G1 to G5 listed in section 3 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md), which says that each holds
at every moment, not only during a change of leader.

### V1. One order of events

**The principle.** The leading sequencer alone decides the order in which the matching engine sees
commands. Every component that needs the order of events takes it from the sequencer.

**Why.** Two components deciding the order independently could disagree, and the venue would have no
single history.

**Where it is meant to hold.** In the sequencer, which numbers every command it logs, and in the
matching engine, which acts on commands in the sequencer's order.

**What checks it.** Not yet checked.

**Today: not yet checked.**

### V2. The log is the record

**The principle.** The leading sequencer's write-ahead log holds every command the venue accepted.
Everything else, including the matching engine's book, can be rebuilt from it.

**Why.** It gives the venue one authority to recover from, whatever else is lost.

**Where it is meant to hold.** In the sequencer's log ([wal.md](durability/wal.md)) and in the matching
engine's catch-up from it.

**What checks it.** Not yet checked.

**Today: not yet checked.**

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

1. **Two reactor locks taken during the day, but not for each order.** `Reactor::timer_registry_mutex_`
   is taken when an application thread starts or cancels a timer; the FIX gateway does that for session
   events and both gateways once for each batch of 500 cancels when a session departs.
   `Reactor::thread_registry_mutex_` is taken once a second by the reactor thread alone, so it is never
   contended. Should F2 allow these two?
2. **Creating a log segment on the writing thread (F4).** When the helper is late, the thread writing
   the log creates the next segment itself, which waits for the disk. The alternative is for that
   thread to wait for the helper, which is also a wait. Should this stay as the one accepted exception,
   counted by `wal_segments_filled_inline`, with the segment size chosen so that it does not happen in
   a normal day?
3. **F6: inbound only, or outbound as well?** Is the principle that no message's payload is copied on
   any path, or only on the way in?
4. **V12 is written down here for the first time.** Is it stated correctly?
