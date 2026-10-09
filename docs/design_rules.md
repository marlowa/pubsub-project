# The rules the design keeps {#design_rules}

> **Status: draft, for Andrew to agree.** Nothing in the code is to be changed on the strength of
> this document until it is agreed. Where a rule is broken today, the entry says so and names the
> bug that records it.

## What this document is for

The venue's design rests on a small number of rules. Each one is stated somewhere already, but in a
different document, often as part of the description of one mechanism, and nowhere are they listed
together. A change made to fix one problem can then break a rule without anyone noticing, because
the person making the change was reading the document about the mechanism, not the rule.

This page lists the rules in one place. For each rule it says:

- what the rule is, in one or two sentences;
- why the design needs it;
- the one place in the code that is meant to make it hold;
- the tests that fail when it is broken;
- whether it holds today, and if not, which bug records that.

Each "Today" line was worked out from the code and the bug list as they stood on 9 October 2026.
The HA scenarios were not run again to write this page; a rule marked as holding is one whose tests
passed when last run and whose code has not changed since.

The detail stays in the documents linked from each entry. This page does not repeat it. When a rule
and a document disagree, one of them is wrong, and the disagreement is to be raised, not settled
quietly in the code.

Every change to the code is to be checked against this list before it is committed. A change that
needs a rule broken, or a new rule added, is a change to the design, and is to be raised as one.

The rules come in two groups. The thread rules say how the work inside one process is divided among
its threads. The venue rules say what the venue as a whole promises its members, and how its
components keep those promises when a process or a machine fails.

## Words used here

- An **order-handling thread** is a thread that every order, cancel or execution report passes
  through on its way through the venue. In each order gateway, the sequencer and the matching engine
  these are the application thread that does the component's work, and the reactor thread that
  carries its network traffic.
- To **own** a piece of state is to be the only thread that changes it. Other threads may read it
  only where the state is built for that, as described under T1.
- A **lock** means a mutex of any kind, a condition variable, a semaphore, a file lock, or a loop
  that waits for another thread to release something. A lock-free queue or a single atomic variable
  is not a lock.

---

## The thread rules

### T1. Each piece of changing state has exactly one owning thread

**The rule.** Only one thread ever changes a given piece of state. Another thread that needs the
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
rule in general. Step 2 of the order of work below is to add owner-thread checks to the state that
matters most, starting with the sequencer's log.

**Today: broken in one place.** On a following sequencer, the sequencer's log has two writers. The
reactor thread writes the records the leader sends, through a handler that the sequencer installs on
the peer connection (`SequencerThread::install_peer_wal_inline_handler`). The sequencer's own thread
writes records the handler passes on to it. The table of epochs in the log has the same two writers.
Two locks were added to keep the two writers apart (see T2), and two atomic variables
(`replicated_records_queued_` and `follower_log_agreed_`) exist only to coordinate them. BUG-0123,
a follower's log found with one entry blank and the next record written twice, is what two writers
produced before the locks were added. Recorded as [BUG-0124](bug_list.md#bug_0124).

### T2. No order-handling thread takes a lock or waits for another thread

**The rule.** An order-handling thread never takes a lock and never waits for another thread to
finish something. It may block only in the one place the framework provides, waiting for work when
its queue is empty or its sockets are idle.

**Why.** A thread that waits for another can be put to sleep by the kernel, and waking it costs tens
of microseconds or more, which is a large part of the venue's whole round trip of about 98
microseconds (`docs/operations/latency_findings.md`). A lock that is almost never contended still
puts that cost on the rare order that meets it, and each lock added makes the next one easier to
justify. [threading.md](framework/threading.md) states the same rule as "there are no mutexes on any
hot path".

**Where it is meant to hold.** In the choice of structures: `LockFreeMessageQueue` between threads,
slab and pool allocators with no lock on their normal path, and single-writer state under T1.

**What checks it.** Nothing in the build. The locks taken today were found by a search of the source
and by recording, with `perf`, every time a thread of the running venue went to sleep waiting for a
mutex (`lock-audit-report.txt`). Step 2 of the order of work below is to make the build fail on any
lock outside a short list of start-up and shut-down code.

**Today: broken in four places.**

| Lock | Where | Taken | Bug |
|---|---|---|---|
| `log_epochs_mutex_` | `SequencerThread` | On every write to the sequencer's log, on the leader and the follower | [BUG-0124](bug_list.md#bug_0124) |
| The mutex in `ReplicatedRecordWriter` | Following sequencer | On every record the leader sends, which is inside every order's round trip | [BUG-0124](bug_list.md#bug_0124) |
| The mutex and condition variables in `BackgroundPromiseRecorder` | Sequencer and matching engine application threads | Every 100 milliseconds, at each lease tick; and, rarely, a wait for a disk sync to finish | [BUG-0126](bug_list.md#bug_0126) |
| The expansion mutex in `ExpandablePoolAllocator` | Every queue between threads, and the gateways' open-order pools | Only when a pool runs out, and then while a new pool is mapped from the operating system | [BUG-0127](bug_list.md#bug_0127) |

The first two exist only because of the two writers described under T1, and go when the second
writer goes.

Two more locks are taken by reactor threads during the trading day, but not for each order:
`Reactor::timer_registry_mutex_`, when an application thread starts or cancels a timer, and
`Reactor::thread_registry_mutex_`, in the reactor's once-a-second check of its threads. Whether these
are allowed is open question 1 below.

### T3. The reactor thread runs only framework code

**The rule.** A component's own logic runs on its application thread. The reactor thread reads and
writes sockets, runs timers, and delivers events to application threads' queues. It does not run code
that belongs to a component.

**Why.** If a component's code runs on the reactor thread, the component's state has two threads
touching it, which breaks T1, and the component's work delays every other connection the reactor
serves. It also hides from the reader of a component which thread is running which line.

**Where it is meant to hold.** In `Reactor` and `ApplicationThread`: the reactor turns network data
into events on an application thread's queue.

**What checks it.** Nothing; the framework offers a way round it.

**Today: broken in one place.** `ApplicationThread::install_inline_pdu_handler` lets a component run
its own handler on the reactor thread, inside the parser. Only the sequencer uses it, for the follower
writing the leader's records (T1). Removing that use, and the facility, is step 3 of the order of
work below.

### T4. An order-handling thread does not wait for the disk

**The rule.** No order-handling thread syncs a file to disk, or does anything else that can wait for
the disk, during the trading day.

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
  it itself, which writes the whole segment to disk.

### T5. Nothing on the order path allocates from the heap

**The rule.** Handling an order, a cancel or an execution report allocates no memory from the general
heap. Memory comes from pools, slabs and fixed buffers sized at start-up.

**Why.** The heap allocator takes locks of its own and its cost varies; a pool's does not.

**Where it is meant to hold.** The framework's allocators ([allocators.md](framework/allocators.md))
and fixed-capacity structures such as `FixedCapacityRingBuffer`.

**What checks it.** Nothing, on demand. Recorded as [BUG-0087](bug_list.md#bug_0087).

**Today: not known.** Some structures on the order path are standard containers that can grow, such
as the sequencer's map of execution reports waiting for the follower (`pending_er_`, a
`std::multimap`). Whether they allocate on the normal path has not been measured.

---

## The venue rules

These are stated in the specification as requirements, and in the documents of the
[high availability](availability/README.md) chapter as guarantees. G1 to G5 are the guarantees listed
in section 3 of [change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md), which
says that each holds at every moment, not only during a change of leader.

### V1. At most one instance of a pair acts as leader at any moment

**The rule.** An instance of the sequencer, the matching engine or the arbiter acts as leader only
while a majority of its three voters grants it a lease. Requirement R-0146.

**Why.** Two leaders acting at once would sequence or match two different histories.

**Where it holds.** In the lease classes in `applications/fix_common/`: `LeaseVoter`, `LeaseHolder`,
`LeaseParticipant`, driven by `PairLeaseAgent`. Each component supplies only the connections the agent
sends on. The rules, and which class holds each, are in section 3 and section 10 of
[majority_leases.md](availability/majority_leases.md).

**What checks it.** `LeaseRulesTest`, `LeaseSimulationTest`, `PairLeaseAgentTest`,
`ComponentLeaseVotersTest` and `LeasePromiseStoreTest`; `ha_test.py` scenarios 8, 9 and 15; and the
TLA+ models, whose counterexamples are rerun on every install ([tla/findings.md](availability/tla/findings.md)).

**Today: holds.** It is the one venue rule enforced in one place, by classes that know nothing of the
component using them.

### V2. A follower that lacks commands the engine acted on never leads

**The rule.** A following sequencer that does not hold every command the matching engine has acted on
does not take the lead. Rule 11 in [majority_leases.md](availability/majority_leases.md).

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

### V3. The matching engine acts only on commands the logs hold (G2)

**The rule.** The leading sequencer sends a command to the matching engine only once its follower has
written and acknowledged it, or, when it is running without a follower, only under V2.

**Why.** If the leader's machine died, the engine would hold an order that no surviving log holds, and
the venue could not account for it.

**Where it holds.** In the leading sequencer: `hold_until_acknowledged`, `release_if_confirmed` and
the running-alone logic in `SequencerThread`.

**What checks it.** `ha_test.py` scenario 59.

**Today: holds.** It is one of the jobs the sequencer's thread class does among many, which is the
subject of step 5 of the order of work.

### V4. Every command a gateway accepted is answered, exactly once (G1)

**The rule.** Every command a gateway accepted from a member is either placed and reported, or refused
with a reply. It is never left unanswered.

**Why.** A member that hears nothing cannot tell whether its order is in the market.

**Where it holds.** In the gateways, which keep each command until it is answered and send again what
they hold when the leader changes or their connection to it is made again
([commands_during_a_change_of_leader.md](availability/commands_during_a_change_of_leader.md),
sections 3.1 and 3.2; `UnansweredCommandStore`). Sending again is safe only because of V5.

**What checks it.** `ha_test.py` scenarios 1, 66, 68 and 71.

**Today: holds**, as far as those scenarios test it.

### V5. A command is sequenced at most once, judged by its identity

**The rule.** The leading sequencer places a command in the log at most once, judged by the member's
comp id, the protocol and the `ClOrdID`, however many times and by whatever path the command arrives.
A command the log already holds is answered, not sequenced again. Requirements R-0006 and R-0119
cover the member's side: a resubmitted order is refused as a duplicate, for the rest of the day.

**Why.** It is what makes it safe for a gateway to send a command again whenever it is unsure (V4).

**Where it should hold.** In one check, in the leading sequencer, applied to every command.

**What checks it.** `ha_test.py` scenarios 67, 68 and 71; unit tests of `LoggedCommandIdentifiers`
and `LogTailIndex`.

**Today: holds on the paths tested, but by several mechanisms, not one.** The leader checks a command
against its log only when the gateway has marked it as sent again. Each way a command can arrive twice
has then needed its own argument or its own rule: after a change of leader, after a connection is made
again, and while a gateway has two connections open to the leader. Recorded as
[BUG-0122](bug_list.md#bug_0122). Separately, the matching engine refuses a repeated `ClOrdID` only
while the first order is still in its book, so an identifier can be used again once its first order
has ended, which R-0119 forbids: [BUG-0114](bug_list.md#bug_0114). Step 6 of the order of work makes
the check one rule, applied to every command.

### V6. Every report the matching engine produces reaches the member (G3)

**The rule.** Every execution report reaches the member it is for. A member may receive a report more
than once, provided every repeat is marked as one (R-0122).

**Where it holds.** In the sequencer, which forwards reports to the gateway the session is bound to and
keeps the reports a gateway has not received; in a new leader, which forwards the reports its
predecessor had not; and in the gateways' resend handling. Design: section 4.4 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md), and
[session_binding.md](availability/session_binding.md).

**What checks it.** `ha_test.py` scenario 65, and the session scenarios.

**Today: holds on the paths tested.** Like V3, it is spread across several of the sequencer's jobs:
pending reports, kept reports, the samples of what the leader forwarded, and the session state.

### V7. Within a log, sequence numbers only go forward (G4)

**The rule.** No sequence number is used twice in a log. A new leader numbers from the highest record
it holds.

**Where it holds.** In the sequencer's numbering when it takes the lead (section 4.1 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md)).

**What checks it.** The numbering checks added to `ha_test.py` scenarios 1 and 59.

**Today: holds.**

### V8. A follower's log becomes a copy of its leader's (G5)

**The rule.** When an instance rejoins as a follower, its log ends up identical to its leader's:
records the leader does not hold are discarded, and records it lacks are sent to it.

**Where it holds.** In the follower's log repair ([follower_log_repair.md](availability/follower_log_repair.md)),
in `SequencerThread`.

**What checks it.** `ha_test.py` scenario 62.

**Today: holds**, but the repair is the part of the sequencer where the two writers of T1 meet, and
it is to be rechecked when the second writer is removed.

### V9. A fault in the standby is a loss of resilience, not a loss of service

**The rule.** While the leader is working, nothing wrong with the follower stops the venue trading.
The leader carries on alone and reports loudly that it is doing so.

**Why.** Otherwise a fault in the backup becomes an outage, and a follower that is alive but stalled
becomes more dangerous than one that has died.

**Where it holds.** In the leading sequencer's running-alone logic, together with V2, which keeps a
follower that fell behind from leading later. Decided in section 7 of
[change_of_sequencer_leader.md](availability/change_of_sequencer_leader.md).

**What checks it.** `ha_test.py` scenario 63.

**Today: holds.**

### V10. Every trading day starts fresh

**The rule.** At the end of a trading day everything is halted, and the logs are archived. A log never
spans two trading days, and no state is carried from one day into the next except through the
start-of-day loading of reference data.

**Why.** It bounds how much any component has to read or recover, and it means a defect cannot carry
damage from one day into the next.

**Where it holds.** In operations: the end-of-day procedure. The phases of the day are in
[trading_phases.md](venue/trading_phases.md).

**What checks it.** Nothing automated.

**Today: holds by procedure.** This rule is not written in any other document. It is stated here
because a design that assumed a log could span two days would be wrong.

---

## The order of work

Agreed on 9 October 2026. The code was tagged `pre-recovery-2026-10-09` before it began.

1. Agree this document.
2. Make the thread rules fail the build where a check is possible: reject a lock outside a short,
   named list of start-up and shut-down code, and add owner-thread checks to the sequencer's log in
   Debug, AddressSanitizer and coverage builds.
3. Remove the second writer of a following sequencer's log, and the facility that let it run on the
   reactor thread (T1, T2, T3). Measure the order's round trip before and after. Some of the
   follower's jitter may come back; that is accepted.
4. Remove the promise recorder's lock and its disk sync from the order-handling threads (BUG-0126),
   and stop a pool running out during the day (BUG-0127).
5. Divide `SequencerThread`, and then `MatchingEngineThread`, into parts, each with one job and an
   explicit statement of which state it is in, all still run by the one thread. Behaviour does not
   change, and the HA scenarios pass after every step.
6. One check against sequencing a command twice, applied to every command (V5; BUG-0122 with
   BUG-0114).
7. Check the TLA+ models against these rules.
8. Only then, return to the bug list, judging each open entry against these rules.

## Open questions

These need Andrew's decision before this document is agreed. They are separate questions, not
alternatives.

1. **Two reactor locks taken during the day, but not for each order.** `Reactor::timer_registry_mutex_`
   is taken when an application thread starts or cancels a timer; the FIX gateway does that for session
   events and both gateways once for each batch of 500 cancels when a session departs.
   `Reactor::thread_registry_mutex_` is taken once a second by the reactor thread alone, so it is never
   contended. Should T2 allow these two, or should they be removed in step 4 with the others?
2. **Creating a log segment on the writing thread (T4).** When the helper is late, the thread writing
   the log creates the next segment itself, which waits for the disk. The alternative is for that
   thread to wait for the helper, which is also a wait. Should this stay as the accepted exception,
   counted by `wal_segments_filled_inline`, with the segment size and the helper's lead chosen so that
   it does not happen in a normal day?
3. **V10 is written down here for the first time.** Is it stated correctly?
