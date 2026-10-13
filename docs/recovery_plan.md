# Restoring the design: what is wrong, and the plan {#recovery_plan}

> **Status: agreed, 9 October 2026.** Each phase ends with a review at which Andrew decides whether
> the next one starts. Phase 1, agreeing the design principles, is finished: Andrew agreed them on
> 11 October 2026. Phase 2, making the build enforce the framework principles, is finished: Andrew
> agreed its results on 12 October 2026. Phases 3 to 9 follow the order of work in
> [ha_library_boundary.md](availability/ha_library_boundary.md), which Andrew agreed on 12 October 2026:
> the high availability machinery moves from the applications into the library, one step at a time.
> Phase 3's three steps are done, and [BUG-0147](bug_list.md#bug_0147), which step 3 made strike more
> often, is fixed (decision 4 in [lease_classes_in_the_library.md](availability/lease_classes_in_the_library.md)).
> The order's round trip is unchanged by phase 3, measured before and after (section 6.4 of that design).
> Phase 3 is finished: Andrew reviewed it on 13 October 2026 and asked for
> [BUG-0148](bug_list.md#bug_0148), which made scenario 40 fail at random, to be fixed before phase 4
> starts, so that every later phase starts from a run in which every high availability scenario passes.
> It is fixed, and all 73 scenarios pass. Phase 4's design,
> [replicated_wal_in_the_library.md](availability/replicated_wal_in_the_library.md), waits for Andrew's review.

## In one page

**What is sound.** The framework at the bottom of the venue is in good shape and is not part of the
problem: the reactor, the lock-free queues between threads, the allocators, the write-ahead log, the
FIX codec and the gateways' session handling. So are the rules for deciding which instance leads,
which live in a few small classes of their own and are checked by unit tests, a randomised
simulation and TLA+ models. None of this needs to be rebuilt, and nothing below proposes starting
again.

**What is wrong.** Four things, described in the next section:

1. Locks and a second writing thread have appeared on the path every order takes, in a design built
   to have neither.
2. Two classes, the sequencer's and the matching engine's application threads, have absorbed almost
   every high-availability fix, and no longer have a shape a reader can hold in mind.
3. The principles the design depends on are written nowhere as one list, so a fix can break one
   without anyone noticing; and one of them, never sequencing a command twice, is enforced by several
   separate mechanisms instead of one.
4. The way of working has been to fix defects one at a time, each where it showed up, without
   checking the fix against the design as a whole. That is what produced the first three.

**The plan.** Stop adding features and fixing bugs. Write the design principles down and agree them.
Make the build enforce the principles it can. Then repair the design in a fixed order, one phase at a time,
by moving the high availability machinery out of the sequencer and the matching engine and into the
library, each phase reviewed before the next begins. Only after that, go back to the bug list, judging
each entry against the principles.

---

## What is wrong

### 1. Locks and a second writing thread on the order path

The framework is built so that each piece of state is changed by exactly one thread, and threads hand
work to each other through lock-free queues. No thread that handles orders ever waits for another.

On a following sequencer that no longer holds. The records the leader sends are written into the
follower's log by two threads: the reactor thread, through a handler the sequencer installs to run
inside it, and the sequencer's own thread, for the records the handler passes on. Two writers of one
log produced a damaged log, so two locks were added to keep them apart, and two shared flags to
coordinate them. One of those locks is also taken on every write to the leader's log. Because the
leader now waits for the follower to acknowledge each order, the follower's write, and its lock, sit
inside every order's round trip.

There are two further locks on order-handling threads, smaller and separate: the lock around the
lease promise record, taken every tenth of a second and occasionally held across a disk sync, and the
lock taken when a memory pool runs out. Two more are taken by reactor threads during the day, but not
for each order.

All of them are listed, with the evidence, in `lock-audit-report.txt` and in the bug list as BUG-0124,
BUG-0126 and BUG-0127.

### 2. Two classes have absorbed the high-availability fixes

`SequencerThread.cpp` is 3,813 lines; on 1 September it was 2,578. The class now does about fifteen
separate jobs: writing and replicating the log, leadership, holding orders until the follower has them,
running without a follower, keeping and forwarding execution reports, finding out what the matching
engine holds after a change of leader, the record of commands already logged, gateway connections,
sessions and resends, deferring and refusing orders, and streaming the log to outside subscribers.

Which situation the sequencer is in is spread across ten separate true-or-false members. Nothing says
which combinations are possible, so every change has to be reasoned about against all of them. The
matching engine is the same, with fourteen. Most high-availability fixes added one more such member
and a few tests of it.

### 3. The principles are not written down together

The guarantees the venue gives its members, and the principles about threads, are each stated somewhere,
but in a dozen documents, each describing one mechanism. A person fixing a defect reads the document
for the mechanism in front of them, and does not see the principle they are about to break.

The clearest consequence is the principle that a command is never sequenced twice. It is enforced by three
separate checks, one for each way a command can arrive twice, and each new way has needed a new check
(BUG-0122). The matching engine has a fourth, which stops working once the first order has ended
(BUG-0114).

### 4. The way of working

Defects have been fixed one at a time, in the order they were found, each in the place where it showed
up. Each fix was correct where it was made. But none was checked against the design as a whole,
because there was no written statement of the design to check it against, and each fix made the next
one more likely to be local too. The bug list has 127 entries, 31 of them open; working through it in
the same way would make all three problems above worse.

---

## How the work is to be done

These apply to every phase below.

- **Design before code.** Each phase starts with a short written design: what will change, what will
  not, how it will be tested, and what it is expected to cost. Andrew agrees it before any code is
  written.
- **One phase at a time.** A phase ends with a review. The next phase does not start until Andrew says
  so.
- **No new features and no bug fixes outside the plan**, unless a defect is found that loses or
  duplicates orders, in which case it is raised with Andrew first.
- **Every change is checked against the principles** in [design_principles.md](design_principles.md). A
  change that would break a principle, or needs a new one, is a design question and is raised as one.
- **The tests decide.** The whole test suite, including the high-availability scenarios, passes at the
  end of every phase. A new check is made to fail on purpose before it is trusted.
- **Measure what matters.** Any phase that touches the order path measures the order's round trip
  before and after, by the method in `docs/operations/latency_findings.md`.
- **A starting point is kept.** The code as it stood when this plan was written is tagged
  `pre-recovery-2026-10-09`, so any phase can be compared with it, or undone.

---

## The plan

Each phase says what it is for, what it involves, and how it is known to be finished. The detail of
each is decided in its own design at the start of the phase, not here.

### Phase 0. Stop and take stock

**For:** a fixed starting point.

**Involves:** stopping feature work and bug fixing; tagging the code; writing this plan.

**Finished when:** this plan is agreed. Done: agreed 9 October 2026, and the tag exists.

### Phase 1. Agree the design principles

**For:** one list of the design principles, which every later phase is checked against.

**Involves:** reviewing [design_principles.md](design_principles.md), which states eight principles
for the framework and fourteen for the venue, says where each is enforced and tested, and where each
is broken today.

**Finished when:** Andrew has answered those questions and agreed the document. Done: agreed 11 October
2026.

**What it found.** Checking each principle against the code found breaches not recorded before, now in
the bug list:

- breaches of the framework principles, taken in the framework fixes done alongside the phases, except
  BUG-0133, which phase 5 removes: [BUG-0128](bug_list.md#bug_0128) (every timer
  start creates a timer and every cancel destroys one, under a lock), [BUG-0129](bug_list.md#bug_0129)
  (the check for stuck threads takes a lock it does not need, and reads two values without
  synchronisation), [BUG-0130](bug_list.md#bug_0130) (the log's helper keeps only one segment ready),
  [BUG-0131](bug_list.md#bug_0131) (the leading sequencer allocates from the heap for every order) and
  [BUG-0133](bug_list.md#bug_0133) (catch-ups read the log on the thread that sequences orders);
- gaps in the venue principles, to be ordered with the rest of the bug list in phase 11 unless Andrew
  brings one forward: [BUG-0132](bug_list.md#bug_0132) (orders open at the end of the day are never
  cancelled), [BUG-0134](bug_list.md#bug_0134) to [BUG-0136](bug_list.md#bug_0136) (a venue running
  without a standby) and [BUG-0137](bug_list.md#bug_0137) (no start-of-day or end-of-day procedure).

### Phase 2. Make the build enforce the framework principles

**For:** the principles about threads checked by a machine rather than by memory, so they cannot be broken
again unnoticed.

**Involves:** making the standards check reject any lock outside a short, named list of start-up and
shut-down code; and adding checks, in Debug and sanitizer builds, that each important piece of state is
changed by only one thread; and making the standards check reject a raw `std::thread` in a venue
component (F8). At first these checks fail on the code as it stands. That is intended: the
list of failures is part of the list of work for the later phases.

**Finished when:** the checks exist, have been shown to fail on the known breaches, and those breaches
are listed as the only permitted exceptions, each with the phase that removes it. Done: agreed 12
October 2026.

**What it built.**

- `scripts/check_standards.py` refuses any lock in venue code that its table `_PERMITTED_LOCKS` does not
  list, with the reason for each (start-up or shutdown only, a thread that handles no orders, an error
  path, or a breach naming its bug and the phase that removes it). Each entry names the lock or function
  it permits; an entry covering a whole file is refused, and so is an entry that no longer matches a lock.
- It refuses a raw `std::thread` in venue code.
- In Debug and sanitizer builds, `ThreadConfinementCheck` refuses a second thread on the log writer,
  the slab allocator, the ring buffer, the incremental rehash map, the sequencer's table of epochs, the
  gateways' unanswered commands and session tables, and the reactor's handler and connection tables.
  The known second writer of the follower's log (BUG-0124) is counted rather than refused, published as
  a metric, and warned about once.

Each check was made to fail on purpose before it was relied on.

**Checked in the running venue.** Every high-availability scenario was run against a Debug install, so
that the ownership checks ran inside the real venue. 67 of the 72 scenarios pass.

- The checks refused one thing: the matching engine's order book, which is filled while the engine is
  built and then used by the engine's own thread. That is a hand-over, not a second thread, and the
  engine now hands the book over on its first event ([BUG-0140](bug_list.md#bug_0140), fixed). No other
  second thread was found. The counted second writer of the follower's log appears in 15 scenarios, as
  expected.
- The five failures, and one crash in a scenario that passed, have causes outside the checks, recorded
  as [BUG-0141](bug_list.md#bug_0141) (a follower that discards records while its record of command
  identifiers is being built stops), [BUG-0142](bug_list.md#bug_0142) (the arbiter and the matching
  engine wait seconds for the disk while handling leases), [BUG-0143](bug_list.md#bug_0143) (a matching
  engine whose lease runs out while starting cancels every order it recovered),
  [BUG-0144](bug_list.md#bug_0144) (`ha_test.py` misses a log line split between two reads) and
  [BUG-0145](bug_list.md#bug_0145) (scenarios 59 and 62 stop the leader before it has their orders).

### Phase 3. Move the lease classes and the voting service into the library

**For:** the first pieces of high availability machinery in the library, moved with the least risk.

**Involves:** three steps. First, moving the lease classes and their messages from
`applications/fix_common` into the library, without changing how they behave. Second, moving what the
arbiter and the witness do into the library as a voting service, with the pairs they vote in set by
configuration, again without changing how they behave. Third, replacing the lock in the lease promise
recorder with a hand-over between threads that needs no lock, and never writing a promise record on a
thread that handles orders ([BUG-0126](bug_list.md#bug_0126)).

**Finished when:** every high availability scenario passes after each of the three steps, and the
promise recorder's locks are removed from the phase 2 list of exceptions.

**Design:** [lease_classes_in_the_library.md](availability/lease_classes_in_the_library.md), agreed 12
October 2026.

### Phase 4. The library's replicated WAL, with one writing thread

**For:** the order path back to its intended design: one thread owns the sequencer's WAL.

**Involves:** building the library's replicated WAL, in which one thread writes every record, and moving
the sequencer onto it. On the follower, the leader's records are then written by that one thread only.
The handler that runs component code on the reactor thread is removed, with the framework facility that
allows it, and so are the two locks and the two coordinating flags that exist only for a second writing
thread ([BUG-0124](bug_list.md#bug_0124)). Some of the follower's delay may return; Andrew has accepted
that, and it is measured.

**Finished when:** no lock is taken for each order in the sequencer; its exceptions are removed from the
phase 2 list; the high availability scenarios pass; and the order's round trip has been measured before
and after.

**Design:** [replicated_wal_in_the_library.md](availability/replicated_wal_in_the_library.md), a draft
for Andrew's review.

### Phase 5. Following the WAL

**For:** one way, provided by the library, for an application to read the WAL in order and catch up.

**Involves:** giving the library's WAL its own way of being followed, on a thread of its own, and moving
the catch-ups of the matching engine and of the publishers onto it, so that catch-ups no longer read the
WAL on the thread that sequences orders ([BUG-0133](bug_list.md#bug_0133)).

**Finished when:** the matching engine and the publishers catch up through the library, no catch-up runs
on the sequencer's thread, and the high availability scenarios pass.

### Phase 6. Make the matching engine deterministic

**For:** a matching engine that, given the same commands in the same order, always produces the same
book and the same reports.

**Involves:**

- making identifiers from the position of the command in the WAL, scrambled with a reversible cipher
  whose key is fixed for the trading day and shared by both engines
  ([BUG-0139](bug_list.md#bug_0139)); this includes designing the cipher and how the day's key is
  created and given to both engines;
- turning the decisions the leading engine now takes alone into commands in the WAL: halting after an
  absence, acting on promotion, and refusing orders while halted. This includes designing how the
  absence rule is measured, so that a restarted engine never cancels its book because of its own
  downtime ([BUG-0146](bug_list.md#bug_0146)).

**Finished when:** both engines, run on the same WAL, produce identical reports, and the high
availability scenarios pass.

### Phase 7. Run the matching engine pair in lockstep

**For:** losing the leading engine costs a short pause and nothing else, without copying the book from
one engine to the other.

**Involves:** both engines apply every committed command, and only the leading engine's reports are sent
on (way B in [ha_library_boundary.md](availability/ha_library_boundary.md)). The copying of the book
(`BookUpdate`), the reconciliation on promotion and the cancelling of inherited orders on promotion are
removed.

**Finished when:** the follower engine's book is built from the WAL alone, book copying is removed, and
the high availability scenarios that promote an engine pass.

### Phase 8. One check against sequencing a command twice, and the gateways as clients

**For:** the principle "a command is sequenced at most once" enforced in one place, for every command,
however it arrived; and one way for a gateway to send commands and recognise their answers.

**Involves:** replacing the separate checks with one check in the library's WAL, on every command
([BUG-0122](bug_list.md#bug_0122)), and making the same check refuse a member who reuses an identifier,
as the specification requires ([BUG-0114](bug_list.md#bug_0114)); moving the gateways' record of
unanswered commands and their resending onto the library's client side. Measure the check's cost on the
order path.

**Finished when:** the separate checks are gone, the high availability scenarios that test repeated
commands pass, and the cost is measured.

### Phase 9. Divide what is left of the sequencer and the matching engine

**For:** a sequencer and a matching engine a reader can understand, and where a change to one job cannot
disturb another.

**Involves:** dividing what remains of `SequencerThread` and `MatchingEngineThread`, once phases 3 to 8
have moved the high availability machinery into the library, into parts, each with one job and its own
state, all still run by the same single thread, so no new threads and no locks. By then the sequencer
holds the venue's own business: which session a member is bound to, and where reports go. Where a
part's situation is spread across several true-or-false members, it becomes one explicit state with the
moves between states written down. Behaviour does not change: this is a restructuring only, done in
small steps, with the whole test suite passing after each.

**Finished when:** each part has one job, its states are written down, and the tests pass.

### Framework fixes done alongside the phases

**For:** the breaches of the framework principles that have nothing to do with high availability.

**Involves:** these can be done at any point, each with its own design agreed first:

- the reactor's timer lock, removed by creating timers at start-up and only starting and stopping them
  afterwards ([BUG-0128](bug_list.md#bug_0128));
- the reactor's thread registry lock, taken during the trading day ([BUG-0129](bug_list.md#bug_0129));
- the WAL's helper thread keeping only one segment ready ([BUG-0130](bug_list.md#bug_0130));
- the leading sequencer's heap allocations ([BUG-0131](bug_list.md#bug_0131));
- making sure memory pools do not run out during a trading day ([BUG-0127](bug_list.md#bug_0127)).

**Finished when:** each is fixed and its exceptions are removed from the phase 2 list.

### Phase 10. Check the models and documents against the principles

**For:** the written descriptions agreeing with the repaired code.

**Involves:** checking the TLA+ models against the agreed principles; bringing the high-availability, sequencer
and matching engine documents up to date with the code, as part of the documentation audit already
under way; removing descriptions of mechanisms that phases 3 to 9 removed.

**Finished when:** the documentation audit marks those documents done.

### Phase 11. Return to the bug list

**For:** the remaining defects, now fixed against a design that is written down.

**Involves:** going through every open entry and deciding, against the principles, whether it is already
fixed by phases 3 to 9 or the framework fixes, is a breach of a principle, or is a new requirement. Then agreeing an order for
those that remain, with Andrew. Each fix from then on follows the same way of working as above.

**Finished when:** every open entry has been judged and the order agreed.

---

## What waits until the plan is finished

- **OAR**, the external publishing of the venue's events, whose design is otherwise ready.
- **New features of any kind.**
- **The bug list**, except as phase 11 describes.
- **The documentation audit of documents the plan does not touch**, which can carry on alongside,
  since it changes no code.

## Already done

- The metric handles record without a virtual call, measured before and after, at Andrew's request,
  before this plan was written.
- The code is tagged `pre-recovery-2026-10-09`.
- Phase 1: the design principles, [design_principles.md](design_principles.md), agreed 11 October 2026.
- Phase 2: the build enforces the framework principles about locks and threads, agreed 12 October 2026.
