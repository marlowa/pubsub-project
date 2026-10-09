# Restoring the design: what is wrong, and the plan {#recovery_plan}

> **Status: agreed, 9 October 2026.** Each phase ends with a review at which Andrew decides whether
> the next one starts. Phase 1, agreeing the rules, is under way.

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
3. The rules the design depends on are written nowhere as one list, so a fix can break one without
   anyone noticing; and one of them, never sequencing a command twice, is enforced by several
   separate mechanisms instead of one.
4. The way of working has been to fix defects one at a time, each where it showed up, without
   checking the fix against the design as a whole. That is what produced the first three.

**The plan.** Stop adding features and fixing bugs. Write the rules down and agree them. Make the
build enforce the rules it can. Then repair the design in a fixed order, one phase at a time,
starting with the order path, each phase reviewed before the next begins. Only after that, go back
to the bug list, judging each entry against the rules.

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

### 3. The rules are not written down together

The guarantees the venue gives its members, and the rules about threads, are each stated somewhere,
but in a dozen documents, each describing one mechanism. A person fixing a defect reads the document
for the mechanism in front of them, and does not see the rule they are about to break.

The clearest consequence is the rule that a command is never sequenced twice. It is enforced by three
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
- **Every change is checked against the rules** in [design_rules.md](design_rules.md). A change that
  would break a rule, or needs a new one, is a design question and is raised as one.
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

### Phase 1. Agree the rules

**For:** one list of the rules the design keeps, which every later phase is checked against.

**Involves:** reviewing [design_rules.md](design_rules.md), a draft that lists five rules about threads
and ten about what the venue guarantees, says where each is enforced and tested, and where each is
broken today. It ends with three questions that need Andrew's decision.

**Finished when:** Andrew has answered those questions and agreed the document.

### Phase 2. Make the build enforce the thread rules

**For:** the rules about threads checked by a machine rather than by memory, so they cannot be broken
again unnoticed.

**Involves:** making the standards check reject any lock outside a short, named list of start-up and
shut-down code; and adding checks, in Debug and sanitizer builds, that each important piece of state is
changed by only one thread. At first these checks fail on the code as it stands. That is intended: the
list of failures is the list of work for phases 3 and 4.

**Finished when:** the checks exist, have been shown to fail on the known breaches, and those breaches
are listed as the only permitted exceptions, each with the phase that removes it.

### Phase 3. One writer for the follower's log

**For:** the order path back to its intended design: one thread owns the sequencer's log.

**Involves:** writing the leader's records on the follower by the sequencer's own thread only; removing
the handler that runs component code on the reactor thread, and the framework facility that allows it;
removing the two locks and the two coordinating flags that existed only for the second writer. Some of
the follower's delay may return; Andrew has accepted that, and it is measured.

**Finished when:** no lock is taken for each order in the sequencer; its exception is removed from the
phase 2 list; the high-availability scenarios pass; and the round trip has been measured before and
after.

### Phase 4. The remaining locks

**For:** no lock or wait left on any order-handling thread.

**Involves:** replacing the lease promise recorder's lock with a hand-over between two threads that
needs none, and keeping its disk sync off the order-handling threads (BUG-0126); making sure memory
pools do not run out during a trading day (BUG-0127); and whatever Andrew decides in phase 1 about the
two reactor locks.

**Finished when:** the phase 2 list of exceptions holds only what Andrew has agreed to keep.

### Phase 5. Divide the sequencer into parts

**For:** a sequencer a reader can understand, and where a change to one job cannot disturb another.

**Involves:** dividing `SequencerThread` into parts, each with one job and its own state, all still run
by the same single thread, so no new threads and no locks. Where a part's situation is now spread
across several true-or-false members, it becomes one explicit state with the moves between states
written down. Behaviour does not change: this is a restructuring only, done in small steps, with the
whole test suite passing after each.

**Finished when:** each part has one job, its states are written down, and the tests pass.

### Phase 6. Divide the matching engine into parts

**For:** the same, for `MatchingEngineThread`.

**Involves:** as phase 5.

**Finished when:** as phase 5.

### Phase 7. One check against sequencing a command twice

**For:** the rule "a command is sequenced at most once" enforced in one place, for every command,
however it arrived.

**Involves:** replacing the separate checks with one check on every command (BUG-0122), and making the
same check refuse a member who reuses an identifier, as the specification requires (BUG-0114). Measure
the check's cost on the order path.

**Finished when:** the separate checks are gone, the high-availability scenarios that test repeated
commands pass, and the cost is measured.

### Phase 8. Check the models and documents against the rules

**For:** the written descriptions agreeing with the repaired code.

**Involves:** checking the TLA+ models against the agreed rules; bringing the high-availability, sequencer
and matching engine documents up to date with the code, as part of the documentation audit already
under way; removing descriptions of mechanisms that phases 3 to 7 removed.

**Finished when:** the documentation audit marks those documents done.

### Phase 9. Return to the bug list

**For:** the remaining defects, now fixed against a design that is written down.

**Involves:** going through every open entry and deciding, against the rules, whether it is already
fixed by phases 3 to 7, is a breach of a rule, or is a new requirement. Then agreeing an order for
those that remain, with Andrew. Each fix from then on follows the same way of working as above.

**Finished when:** every open entry has been judged and the order agreed.

---

## What waits until the plan is finished

- **OAR**, the external publishing of the venue's events, whose design is otherwise ready.
- **New features of any kind.**
- **The bug list**, except as phase 9 describes.
- **The documentation audit of documents the plan does not touch**, which can carry on alongside,
  since it changes no code.

## Already done

- The metric handles record without a virtual call, measured before and after, at Andrew's request,
  before this plan was written.
- The code is tagged `pre-recovery-2026-10-09`.
- The draft of the rules, [design_rules.md](design_rules.md), exists for phase 1.
