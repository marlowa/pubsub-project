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
> [replicated_wal_in_the_library.md](availability/replicated_wal_in_the_library.md), was agreed on
> 13 October 2026. Its three steps are done: the sequencer's thread is the only one that writes its WAL,
> and the two locks of [BUG-0124](bug_list.md#bug_0124) are gone; the library's replicated WAL is built
> and tested on its own; and the sequencer uses it. All 74 scenarios pass, and the order's round trip is
> unchanged. Phase 4 is finished: Andrew reviewed it on 14 October 2026 and asked for phase 5's design
> next. Phase 5's design, [following_the_wal.md](availability/following_the_wal.md), was agreed on
> 14 October 2026. Its step 1 is built: the library's way of following a WAL, tested on its own. Its
> step 2 is built: the follower sequencer is repaired through the reading thread, and the venue goes on
> answering orders while a follower that missed 1.2 million records catches up
> ([BUG-0155](bug_list.md#bug_0155) is fixed). The lock-free allocators and queues, and the hand-over of
> a follower, are now specified in TLA+ and checked with TLC
> ([BUG-0162](bug_list.md#bug_0162), [framework/tla/findings.md](framework/tla/findings.md)). Its step 3 is
> built: the matching engine is caught up by the reading thread, and uses the library's messages to ask. Its
> step 4 is built: the matching engine publishers follow the WAL through the library, are sent only committed
> records, once each, by the leading sequencer only ([BUG-0153](bug_list.md#bug_0153) and
> [BUG-0156](bug_list.md#bug_0156) are fixed). Its step 5 is built: the build refuses any read of the WAL in
> the sequencer that is not in a named table of permitted reads, each with its reason
> ([BUG-0133](bug_list.md#bug_0133) is fixed). Phase 5 is finished: Andrew reviewed it on 20 October 2026,
> with [BUG-0176](bug_list.md#bug_0176), a far-back catch-up of the matching engine flooding the leader with
> repeated reports, left open, and [BUG-0171](bug_list.md#bug_0171) to be taken further by keeping each record's
> bytes in memory until it is committed. Phase 6 is finished: a channel of test events, so that the high
> availability tests no longer read the wording of Quill log messages; every check is moved onto test events,
> what a member receives, or the WAL files, and with every `TEST CONTRACT` message reworded on purpose, except the
> three operator messages decision 8 keeps, the scenarios still pass
> ([test_events.md](availability/test_events.md), section 4.17). Andrew reviewed it on 20 October 2026. In the
> review of phases 7 and 8 the same day, Andrew agreed that phase 7 goes ahead as written. He agreed phase 8 on
> 24 October 2026, once it was written out in full. Phase 7's design,
> [deterministic_matching_engine.md](availability/deterministic_matching_engine.md), was decided on 25 October 2026.

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
one more likely to be local too. On 9 October 2026 the bug list held 127 entries, 31 of them open; working
through it in the same way would make all three problems above worse.

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
- gaps in the venue principles, to be ordered with the rest of the bug list in phase 12 unless Andrew
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

**Design:** [replicated_wal_in_the_library.md](availability/replicated_wal_in_the_library.md), agreed
13 October 2026.

Done: Andrew agreed its results on 14 October 2026.

### Phase 5. Following the WAL

**For:** one way, provided by the library, for an application to read the WAL in order and catch up.

**Involves:** giving the library's WAL its own way of being followed, on a thread of its own, and moving
the catch-ups of the matching engine and of the publishers onto it, so that catch-ups no longer read the
WAL on the thread that sequences orders ([BUG-0133](bug_list.md#bug_0133)).

**Finished when:** the matching engine and the publishers catch up through the library, no catch-up runs
on the sequencer's thread, and the high availability scenarios pass.

**Design:** [following_the_wal.md](availability/following_the_wal.md), agreed 14 October 2026.

### Phase 6. Test events instead of Quill log wording

**For:** high availability tests that check what the venue does, not the wording of what it writes to its
Quill logs ([BUG-0169](bug_list.md#bug_0169)). Placed before the later phases by Andrew on 18 October 2026,
because every finding is harder to establish while the evidence is log text.

**Involves:**

- an inventory of every check in `scripts/ha_test.py` that reads a Quill log, 112 waits for a phrase and about
  40 counts and readings, each with the fact it proves and whether a member or another existing interface
  could prove it instead;
- checking what a member receives wherever it can be checked;
- for the internal steps no member can see, test events sent by each component when a test asks for them,
  defined in the DSL, and switched off otherwise;
- a new generator in the DSL compiler that writes plain Python, beside the generators for C++, Java and
  pybind11 bindings, so that the test harness decodes events exactly as they are encoded (decided by Andrew on
  18 October 2026);
- moving the checks over one kind at a time, and removing each `TEST CONTRACT` comment once no test reads the
  message.

**Finished when:** no high availability test reads a Quill log to decide whether it passes, the scenarios pass,
and changing the wording of any Quill message cannot make a test pass or fail.

**Design:** [test_events.md](availability/test_events.md), with its inventory
[test_events_inventory.md](availability/test_events_inventory.md), agreed 18 October 2026. It starts from Copilot's
proposal, which Andrew supplied.

### Phase 7. Make the matching engine deterministic

**Reviewed.** Phases 7 and 8 change how the engine pair works and are a substantial design project, so
Andrew reviewed whether they still serve the guarantees the venue is to give before either is designed. On
20 October 2026 he agreed that phase 7 goes ahead as written, and on 24 October 2026 he agreed phase 8. The
question of a limit on automatic recovery, recorded in [BUG-0176](bug_list.md#bug_0176), is designed here with
the measurement of the absence rule.

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
  downtime ([BUG-0146](bug_list.md#bug_0146));
- designing what a command that reaches a defect in the engine costs, since a deterministic engine pair meets it
  in both engines at the same command ([BUG-0193](bug_list.md#bug_0193)): catching what can be caught where one
  command is applied and rejecting that command alike in both engines, and fuzzing the engine with well-formed
  commands, checking the book after each. Voiding a command that crashed the engine is designed with phase 8.

**Design:** [deterministic_matching_engine.md](availability/deterministic_matching_engine.md). Its five decisions
were taken by Andrew on 25 October 2026.

**Finished when:** both engines, run on the same WAL, produce identical reports, and the high
availability scenarios pass.

### Phase 8. Run the matching engine pair in lockstep

**Agreed** by Andrew on 24 October 2026.

**For:** losing the leading matching engine costs a short pause and nothing else. The members' resting
orders are still on the book afterwards.

**What happens now.** The venue runs two matching engines; one leads and the other follows. The sequencer
sends every committed order and cancel only to the engine that leads (`EngineOrderRouting`). Instead, the leading
engine sends the follower a `BookUpdate` message for each order it adds to or removes from its book, and the
follower's book is a copy built from those messages. When the leading engine is lost, the follower is
promoted: it catches up from the WAL after the last record its copy reflects, and then, because every
environment is deployed with `open_orders_on_promotion = "cancel"`, it cancels every order it inherited.
Every member is sent a cancel report for every resting order.

**Why the copy cannot be trusted with `"keep"`.** The copy can lose changes without anyone knowing. While the
link between the two engines is down, the updates are not sent, and nothing sends them when the link returns
([BUG-0187](bug_list.md#bug_0187)). The follower cannot see the loss, because the numbers on the updates
have gaps in ordinary running: a refused order changes nothing and produces no update. So a promoted engine
can lack an order a member was told is resting, and hold an order a member was told is cancelled. The copy
also flows only from the engine configured as primary, so after one change of leader it is not kept up to
date at all ([BUG-0188](bug_list.md#bug_0188)).

**What changes.** There is no copy. Both engines apply every committed order and cancel from the WAL
themselves, in the order the WAL gives, whichever one leads; only the leading engine's execution reports are
sent on to the members (way B in [ha_library_boundary.md](availability/ha_library_boundary.md), agreed on
12 October 2026). The two books agree at every moment for two reasons:

1. **Both engines are given the same input, all of it.** The WAL's records are numbered with no gaps, so an
   engine that misses one notices and asks for it by number, as any follower of the WAL does.
2. **Both apply it by the same rules.** That is phase 7: the decisions an engine now takes alone become
   commands in the WAL, and identifiers come from the position in the WAL, so the same records always give
   the same book and the same reports. This is why phase 8 follows phase 7.

On promotion, the follower's book is already the leader's, apart from the few records still on their way to
it, which it applies as any engine catching up does. Keeping the resting orders is then simply correct.

**Removed:** the `BookUpdate` message, the connection between the two engines that carries it, and the code
that sends and applies it; the catch-up on promotion from the end of a copy; and the
`open_orders_on_promotion` setting, with the cancelling of inherited orders on promotion.

**Finished when:**

- the follower engine's book is built only by applying the WAL, and no `BookUpdate` code is left;
- the high availability scenarios that promote an engine pass, and check that the orders resting before the
  promotion are still resting after it;
- a scenario that blocks the link between the engines, places and cancels orders during the break, kills
  the leading engine, and compares the promoted book with what the members were told, passes. It is written
  before the phase starts, and fails today with `"keep"`.
- both engines start alike, whichever is configured as primary: in an unknown role, catching up from the WAL
  when the first sequencer connects, and saying what role they have only once it is settled. Today the
  configured secondary starts as follower at once and catches up only when promoted
  ([BUG-0195](bug_list.md#bug_0195));
- every scenario with both engines passes whichever engine leads at the start. Either engine can win the
  first election, so this is checked by running them normally, and again with
  `ha_test.py --scenario all --secondary-engine-first`, which starts the secondary engine, waits until it
  leads, then starts the primary, and begins the steps once the primary follows. Today three of the 24 fail when
  the secondary leads first ([BUG-0195](bug_list.md#bug_0195));
- an engine that does not lead catches up from the WAL before it acts as leader, whatever role it was in, and it
  leads with exactly the orders the members were told are open. Scenario 78 kills the leader while the other engine
  is caught up and waiting to learn its role, and requires the engine that takes the lead to catch up first and to
  hold every order a member was told is open; it passes. Scenario 79 does the same with the book cancelled on the
  first promotion, and requires the engine that takes the lead to hold none of the orders a member was told were
  cancelled; it fails today ([BUG-0199](bug_list.md#bug_0199)).

**Also designed in this phase:** voiding a command that crashed an engine, so that a command both engines
crash on costs that command and not the venue: the engine records each command before applying it, and one
that finds it died applying a command asks for the command to be voided through the WAL, provided no running
engine has applied it ([BUG-0193](bug_list.md#bug_0193)).

**Also designed in this phase:** what the venue does when no matching engine leads for longer than a normal
failover takes. It halts and says so at once, gives every order it was holding a definite rejection through
the WAL, and resumes when an operator lifts the halt, so that a member is never left not knowing what became
of an order ([BUG-0197](bug_list.md#bug_0197)). It needs the technical event of
[trading_phases.md](venue/trading_phases.md) and a way to declare a halt ([BUG-0065](bug_list.md#bug_0065)),
which this phase's design must place in the plan.

**Still to settle when the phase is designed:** whether the follower compares its reports, or a summary of
its book, with the leader's while the venue runs, so that a difference is noticed at once rather than at a
promotion; and what happens when one is found. Under principle V11 the leader carries on trading either way.

### Phase 9. One check against sequencing a command twice, and the gateways as clients

**For:** the principle "a command is sequenced at most once" enforced in one place, for every command,
however it arrived; and one way for a gateway to send commands and recognise their answers.

**Involves:** replacing the separate checks with one check in the library's WAL, on every command
([BUG-0122](bug_list.md#bug_0122)), and making the same check refuse a member who reuses an identifier,
as the specification requires ([BUG-0114](bug_list.md#bug_0114)); moving the gateways' record of
unanswered commands and their resending onto the library's client side. Measure the check's cost on the
order path.

**Finished when:** the separate checks are gone, the high availability scenarios that test repeated
commands pass, and the cost is measured.

### Phase 10. Divide what is left of the sequencer and the matching engine

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

### Phase 11. Check the models and documents against the principles

**For:** the written descriptions agreeing with the repaired code, and no state copied between the instances
of a pair except through the WAL.

**Involves:**

- checking the TLA+ models against the agreed principles;
- bringing the high-availability, sequencer and matching engine documents up to date with the code, as part of
  the documentation audit already under way, and removing descriptions of mechanisms that phases 3 to 9 removed;
- checking every connection between the two instances of a pair (the two sequencers, the two matching engines,
  the two gateways, and any other pair) for state that one instance sends the other outside the WAL. Such a copy
  does not have the WAL's numbering with no gaps, so it can lose changes without either instance knowing, as the
  matching engine's `BookUpdate` copy does ([BUG-0187](bug_list.md#bug_0187)). For each one found: say what
  state it carries, whether a lost or repeated message can leave the receiver wrong without its knowing, and
  either move that state into the WAL or show that a loss is always noticed and made up. Each finding that is a
  defect is recorded in the bug list.

**Finished when:** the documentation audit marks those documents done, and every connection between the
instances of a pair has been checked, with its result written down.

### Phase 12. Return to the bug list

**For:** the remaining defects, now fixed against a design that is written down.

**Involves:** going through every open entry and deciding, against the principles, whether it is already
fixed by phases 3 to 9 or the framework fixes, is a breach of a principle, or is a new requirement. Then agreeing an order for
those that remain, with Andrew. Each fix from then on follows the same way of working as above.

**Finished when:** there is an agreed set of the guarantees the venue gives, evidence for which of them
hold, and an explicit decision, recorded, about which remaining risks are accepted. An empty bug list is
not the aim. Gaps found along the way are recorded, not fixed within the phase. Agreed 17 October 2026.

---

## What waits until the plan is finished

- **OAR**, the external publishing of the venue's events, whose design is otherwise ready.
- **New features of any kind.**
- **The bug list**, except as phase 12 describes.
- **The documentation audit of documents the plan does not touch**, which can carry on alongside,
  since it changes no code.

## Already done

- The metric handles record without a virtual call, measured before and after, at Andrew's request,
  before this plan was written.
- The code is tagged `pre-recovery-2026-10-09`.
- Phase 1: the design principles, [design_principles.md](design_principles.md), agreed 11 October 2026.
- Phase 2: the build enforces the framework principles about locks and threads, agreed 12 October 2026.
