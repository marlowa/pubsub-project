# Phase 3: the lease classes and the voting service in the library {#lease_classes_in_the_library}

> **Status: agreed, 12 October 2026.** This is the design for phase 3 of
> [the recovery plan](../recovery_plan.md). Andrew agreed it as a whole, with the three decisions in
> section 9.

## 1. What phase 3 does

Phase 3 is the first step in moving the high availability machinery out of the applications and into
the library, as set out in [ha_library_boundary.md](ha_library_boundary.md). It has three steps, done
and tested one after the other:

1. **The lease classes.** The classes that decide which instance of a pair leads, and the messages they
   exchange, move from `applications/fix_common` into the library. How they behave does not change.
2. **The voting service.** What the arbiter and the witness do moves into the library, so that which
   pairs they vote in is set by configuration rather than written into their code. How they behave does
   not change.
3. **The promise recorder.** The lock and the waits in `BackgroundPromiseRecorder` are removed, so that
   the thread that handles orders never takes a lock and never waits for the disk because of a lease
   ([BUG-0126](../bug_list.md#bug_0126)).

The two steps that change no behaviour come first, so that a failure after either of them is a fault
in that move. The third step then changes behaviour in one class and in how `PairLeaseAgent` uses it,
both already in their new home.

## 2. What the lease classes are, and who uses them

An instance of a pair leads only while a majority of three voters has granted it a lease that has not
run out. The rules are in [majority_leases.md](majority_leases.md). The classes that apply them are
header files in `applications/fix_common`, in the namespace `fix_common`:

| Class | What it does |
|---|---|
| `LeaseVoter` | Decides whether to grant or refuse a request for a lease. |
| `LeaseHolder` | Tracks the grants an instance holds, and whether it leads. |
| `LeaseParticipant` | Combines the two: one instance, which both votes and asks to lead. |
| `PairLeaseAgent` | Drives `LeaseParticipant` every tick: asks to lead, renews, answers the peer, records promises, and tells its owner when it starts or stops leading. |
| `LeaseLinksInterface` | Implemented by the owner, to send lease messages to the other two voters. |
| `LeaseTiming` | The lease period, the drift allowance and the renewal interval, read from the `[lease]` configuration section. |
| `LeaderStatement`, `PeerStatementsFlag` | A leader's statement that its peer may not lead, because the peer lacks commands the matching engine has acted on ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md)). |
| `LeasePromiseRecord`, `LeasePromiseRecorderInterface`, `LeasePromiseStore`, `BackgroundPromiseRecorder` | Keep on disk the promise an instance has made as a voter, and the statement it holds, so that a restart does not forget them. |
| `EpochStore`, `LeaderEpoch` | Keep on disk the highest epoch an instance knows of, and check epochs. |

They already know nothing about the component using them. The sequencer, the matching engine, the
matching engine publisher and the arbiter each own a `PairLeaseAgent`. The arbiter's votes in the
component pairs (`ComponentLeaseVoters`) and the witness use `LeaseVoter` directly.

The messages, `LeaseRequest` (130), `LeaseGrant` (131) and `LeaseRefusal` (132), with the enumeration
`LeaseRefusalReason`, are defined in `libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl`.
That file is in the library's directory, but it is generated into the namespace `pubsub_itc_fw_app` and
also holds messages that belong to the venue alone, such as `WalRecord`, `SessionBound` and
`OrderAcceptance`. Each lease message names its pair with the enumeration `ComponentGroup`, whose values
are the venue's pairs: sequencer, matching engine, matching engine publisher and arbiter.

## 3. Step 1: the lease classes

**Where the classes go.** Every class in the table in section 2 moves to
`libraries/pubsub_itc_fw/include/pubsub_itc_fw/`, into the namespace `pubsub_itc_fw`, keeping its name.
They stay header-only, as they are now. Code that includes them changes from, for example,
`#include <LeaseHolder.hpp>` to `#include <pubsub_itc_fw/LeaseHolder.hpp>`.

**Where the messages go.** `LeaseRequest`, `LeaseGrant`, `LeaseRefusal` and `LeaseRefusalReason` move
out of `leader_follower.dsl` into a new file of the library's own, `lease_messages.dsl`. The library
generates every message definition file in its directory into its own namespace, so this one becomes
`lease_messages.hpp` in the namespace `pubsub_itc_fw`. Their message numbers stay 130, 131 and 132, so nothing on the wire changes.
The code generator wraps the helper functions it writes into every header in a guard, so that two
generated headers can be included together. The guard is named after the namespace, because the
helpers are defined inside the namespace and each namespace needs its own copy.

**How a message names its pair.** The library cannot name the venue's components, so in the library's
messages the pair is a plain number, `i32 pair_id`, in place of `ComponentGroup group`. `ComponentGroup`
stays in `leader_follower.dsl` for the venue's own use, and the venue converts between the two where it
sends and receives lease messages, with `pair_id_of` and `group_of` in
`applications/fix_common/LeasePairNumbers.hpp`. The values stay the same (sequencer 1, matching engine 2, matching
engine publisher 3, arbiter 4), and an enumeration in the message definitions is already sent as a
32-bit number, so the bytes on the wire do not change either. `PairLeaseAgent` takes the pair's number
in its constructor instead of a `ComponentGroup`.

**What stays in the venue after step 1.** The arbiter and the witness, which step 2 moves; and what each
component does when it starts or stops leading, which stays in that component for good.

**The tests move with the classes.** `LeaseRulesTest`, `LeaseSimulationTest`, `PairLeaseAgentTest`,
`LeasePromiseStoreTest`, `EpochStoreTest` and `LeaderEpochTest` move from `applications/sequencer/tests`
to `libraries/pubsub_itc_fw/tests`, and are built into `pubsub_itc_fw_tests`. Only their include lines and
namespaces change.

**What does not change.** Every log line keeps its wording. Several are matched by `ha_test.py` and are
marked in the code as a test contract. The configuration does not change: each component still reads
its `[lease]` section, and its promise and epoch files are where they are now.

**The documents that name the old places** are corrected in the same change:
[majority_leases.md](majority_leases.md), [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md),
[design_principles.md](../design_principles.md), [ha_library_boundary.md](ha_library_boundary.md) and
the comments at the top of `leader_follower.dsl`. The entries for `BackgroundPromiseRecorder` in
`_PERMITTED_LOCKS` in `scripts/check_standards.py` follow the file to its new path.

**How step 1 is known to be right.**

- The moved unit tests pass, and the number of tests is the same before and after.
- The whole build passes `scripts/devsetup.sh --skip-db`, which builds, releases, deploys and runs every
  test.
- Every high availability scenario in `ha_test.py` is run once before the move, to record which pass,
  and again after it. The same scenarios pass. At the end of phase 2, 67 of the 72 passed in a Debug
  install, with the five failures recorded as [BUG-0141](../bug_list.md#bug_0141) to
  [BUG-0145](../bug_list.md#bug_0145); the run before the move establishes the figure for the Release
  install.

## 4. What the arbiter and the witness are

**The arbiter.** Two arbiters, a primary and a secondary, form the arbiter pool, which is the third
voter in each component pair: the sequencers, the matching engines and the matching engine publishers.
The pool votes through whichever arbiter is active. An arbiter is active only while a majority of three
voters, the two arbiters and the witness, has granted it a lease, so the arbiters decide which of them
is active by the same lease rules as every other pair, using a `PairLeaseAgent` of their own. The
active arbiter answers each component's request for a lease with a grant or a refusal; the passive one
stays silent. An arbiter that becomes active grants nothing to any component for one lease period,
because it does not know what the previously active arbiter promised. The two arbiters tell each other
the highest epoch granted in each component pair, and the newest statement held from each pair's
leader, with `ArbiterStateRecord` (400). See [arbiter.md](../venue/arbiter.md).

The arbiter's code is `ArbiterThread` (about 450 lines), `ComponentLeaseVoters` (one `LeaseVoter` for
each component pair), `ComponentStatementStore` (the statements on disk), a configuration and its
loader, and `Arbiter.cpp`, which sets up the reactor, the listeners and the connections, and holds
`main`.

**The witness.** The witness is the third voter in deciding which arbiter is active. It is never a
candidate: it answers each arbiter's request with a grant or a refusal, by `LeaseVoter`, and keeps
nothing on disk. See [witness.md](../venue/witness.md). Its code is `WitnessThread` (about 150 lines), a
configuration and its loader, and `Witness.cpp`.

**What in them is the venue's.** Studying the code shows that the arbiter and the witness already do
nothing but vote. Two things in them name the venue:

- the pairs the arbiter votes in, which are written into the code as the sequencer, the matching engine
  and the matching engine publisher (`is_component_group` in `ArbiterThread.cpp`), and the pair the
  witness votes in, written in as the arbiter pair;
- the names of the pairs and of the arbiter's role printed in the Quill log, which come from the
  venue's enumerations `ComponentGroup` and `Role` in `leader_follower.dsl`. Several of these log lines
  are matched by `ha_test.py`.

So step 2 is mostly a move, like step 1. The one real change is that the pairs are configured.

## 5. Step 2: the voting service

**What moves into the library.** `ArbiterThread`, `ComponentLeaseVoters`, `ComponentStatementStore`,
`WitnessThread`, the two configurations and their loaders, and the setting up of the reactor, listeners
and connections now in `Arbiter.cpp` and `Witness.cpp`, all into the namespace `pubsub_itc_fw`, keeping
their names. Their tests, `ComponentLeaseVotersTest` and `ComponentStatementStoreTest`, move to
`libraries/pubsub_itc_fw/tests`. `ArbiterStateRecord` moves into the library's `lease_messages.dsl`, with the
pair given as `i32 pair_id`, as in step 1; its message number stays 400.

**What the venue keeps.** By decision 3 in section 9, the venue keeps the two programs `arbiter` and `witness`, each now a short `main` that reads its
configuration file and starts the library's voting service. Their names, their configuration files,
how they are deployed and how `ha_test.py` starts and stops them do not change.

**Configuring the pairs.** The arbiter's configuration gains a section naming the pairs it votes in,
each with the name used in the Quill log and the number its members put on their lease messages, and
the pair the two arbiters themselves form. The witness's configuration names the pair it votes in. For
example, in `arbiter_primary.toml`:

```toml
[[voting_pair]]
name    = "sequencer"
pair_id = 1

[[voting_pair]]
name    = "matching_engine"
pair_id = 2

[[voting_pair]]
name    = "matching_engine_publisher"
pair_id = 3

[own_pair]
name    = "arbiter"
pair_id = 4
```

and in `witness.toml`:

```toml
[voting_pair]
name    = "arbiter"
pair_id = 4
```

The shared type is `VotingPair`, in `libraries/pubsub_itc_fw/include/pubsub_itc_fw/VotingPair.hpp`.
A request for a pair not listed is dropped and logged, as a request for an unknown group is now. The
loader refuses a configuration in which a number or a name appears twice, or in which the arbiter's
own pair is also listed among the pairs it votes in.

**The log stays the same.** The names in the configuration are the ones the log prints now, so the log
lines keep their wording, including those matched by `ha_test.py`. The arbiter's role (unknown,
follower, leader) becomes an enumeration of the library's own, with the same three names.

**What does not change.** Who votes in which pair, the voter numbers on grants and refusals (3 for the
arbiter pool and for the witness), the promise file and the statements file and their formats, and every
message on the wire. The statements file is read as before, except that a line is accepted for any positive pair
number rather than only the venue's three, and the arbiter ignores a statement for a pair it is not
configured to vote in.

**How step 2 is known to be right.**

- The moved unit tests pass, and the number of tests is the same before and after.
- New unit tests of the configuration loaders: a correct section is read; a repeated number, a repeated
  name, and the arbiter's own pair listed among the others are each refused. Each refusal is seen to
  fail before the check that causes it is written.
- `scripts/devsetup.sh --skip-db` passes.
- The same high availability scenarios pass as after step 1. Scenarios that stop and restart an arbiter
  or the witness are among them.

## 6. Step 3: the promise recorder without a lock

### 6.1 What happens now

Before an instance grants its vote, it records the promise on disk and syncs it, so that a restart
does not forget it. Writing and syncing can take a long time on a slow disk, so `BackgroundPromiseRecorder`
writes on a thread of its own. The sequencer and the matching engine drive their `PairLeaseAgent` on
their own application thread, which is also the thread that handles every order. On that thread:

1. **Every tick, every 100 milliseconds**, `background_result()` takes the recorder's mutex to read
   whether a background write has finished.
2. **To start a background write**, `record_in_background()` takes the same mutex.
3. **When the record on disk does not cover a promise about to be made**, `record()` waits on a
   condition variable until any background write has finished, and then writes and syncs the record
   itself, on the application thread. This happens:
   - when the instance first grants its vote to its peer, for example at start-up or after it has
     stopped leading;
   - when it starts leading and the record does not say so;
   - when a background write has not finished in time, which means the disk is slow.

While it waits, the thread handles no orders. Principle F4 in [design_principles.md](../design_principles.md)
says a thread that handles orders never waits for the disk, with an exception for changes of role
(`EpochStore::store` and `Wal::truncate_after`).

### 6.2 Passing requests and results without a lock

The request and its result pass between the application thread and the writer thread through one slot
and two atomic counters. Each counter has one writing thread:

- `requests_started`, written only by the application thread;
- `requests_finished`, written only by the writer thread, together with whether the last write
  succeeded.

The application thread may fill the slot only when the two counters are equal, meaning no write is in
progress, so the two threads never use the slot at the same time. To start a write, it fills the slot
and then increases `requests_started`. The writer thread sees `requests_started` ahead of
`requests_finished`, copies the slot, writes and syncs the record, stores the outcome, and then
increases `requests_finished`. The application thread learns of the outcome at its next tick by reading
`requests_finished`. Neither thread ever waits for the other.

**How the writer thread waits for work.** Without a condition variable, the writer thread checks
`requests_started` every 10 milliseconds and sleeps in between. This thread handles no orders, so
sleeping costs nothing, and 100 checks a second is too few to matter. A background write starts at most
10 milliseconds later than it does now. Most writes are started seconds before they are needed, so the
delay does not matter; where a grant waits for a write (section 6.3), it adds at most 10 milliseconds.
The project builds with C++17, which has no way for one thread to wait on an atomic variable without a
lock.

When the recorder is destroyed, a third atomic tells the writer thread to stop, and the destructor
waits up to a second for it to finish, as it does now.

The thread stays a `ThreadWithJoinTimeout`, as principle F8 requires.

### 6.3 Never writing on the application thread

Removing the lock does not on its own remove the wait in point 3 of section 6.1. Decision 1 in section 9
settles it: the application thread never writes and syncs a promise record itself. So:

- **A promise to the peer that the record does not cover.** The agent does not grant at once. It starts
  a background write of a record that covers the promise, and refuses the request with a new reason,
  `recording_promise`, meaning "ask me again shortly". When the write has finished, the next request
  is granted. Refusing is always safe: a vote not given cannot help make two leaders.
- **The candidate, on that refusal,** asks again at its next renewal interval (one second by default),
  rather than waiting out a whole lease period (three seconds by default) as it does after other
  refusals.
- **The record that this instance leads** is written in the background only. It is not needed for
  safety. Without it, an instance restarted within the lease period waits one lease period before
  asking to lead again, instead of asking at once. This is the behaviour the code already describes for
  a record that cannot be written.
- **A slow disk.** If a background write has not finished when the record is needed, the agent goes on
  refusing promises the record does not cover, and goes on renewing promises it does cover. It never
  writes on the application thread.
- **`write_statement_if_needed`**, which writes a statement synchronously when the recorder cannot write
  in the background, no longer does so. Every program that keeps records (the sequencer, the matching
  engine and the arbiter) uses `BackgroundPromiseRecorder`, so the synchronous path is used only by unit
  tests now, and is removed.
- **Two interfaces, so that the agent cannot write synchronously.** `PairLeaseAgent` keeps its promises
  through `BackgroundPromiseRecorderInterface`, which has only `record_in_background()` and
  `background_result()`. `LeasePromiseRecorderInterface` keeps only the synchronous `record()`, which
  `LeasePromiseStore` implements and only the recorder's own writer thread calls.
- **One write at a time.** If a record is needed while another write is in progress, the agent keeps it
  and starts it at the next tick after that write has finished. The record asked for last is the one
  kept, because it describes the promise the instance holds now.

**When this costs anything.** A change of leader usually needs only the third voter's grant: when the
leader has died, its peer asks the arbiter, which grants, and the dead leader cannot be asked anyway.
The new refusal therefore delays a change of leader only when the third voter is unavailable and the
peer must grant. In that case the delay is about one renewal interval, one second by default.

Two further costs follow from every record being written in the background:

- **A restarted former leader, with the third voter down.** An instance restarted with a record saying it
  led asks to lead again at once, and while it asks it refuses its peer's renewals for a lease period, so
  a peer that took over meanwhile loses its lease. The peer asks to lead again at once, and the restarted
  instance gives way, but its record still names itself, so it first refuses with `recording_promise`.
  The pair is without a leader for about one renewal interval. The unit test
  `WithTheArbiterDownARestartedFormerLeaderLeavesThePeerLeadingAfterAShortGap` measures it. The peer
  losing its lease comes from the restarted instance asking to lead, not from the new refusal; the
  refusal only lengthens the time before the peer leads again, from one tick to one renewal interval.
- **A leader's own statement.** A leader that says its peer may not lead acts on a command its peer lacks
  only once its own statement is on disk. That is now known one or two ticks after the statement changes,
  a tenth or two tenths of a second, rather than at once.

### 6.4 How step 3 is known to be right

- **The build refuses the old code.** The three entries for `BackgroundPromiseRecorder` are removed from
  `_PERMITTED_LOCKS`, so `scripts/check_standards.py` refuses any lock left in it. This is checked by
  running the check once with the entries removed and the old code still in place, and seeing it fail.
- **Unit tests of the recorder,** using a store that takes two seconds to write: starting a write and
  reading its outcome both return at once while the store is writing; the outcome is reported once; a
  write is not started while one is in progress; and the destructor returns within its wait while a
  write is stuck. The library's ThreadSanitizer build (`build-tsan`) does not compile, for a reason
  unrelated to this step ([BUG-0150](../bug_list.md#bug_0150)), so the recorder is checked under
  ThreadSanitizer by a separate program: three hundred writes passed between the two threads, and a
  recorder destroyed while its write is stuck. It reports no data race. A copy of the recorder whose
  atomic operations impose no ordering makes it report races, which shows that it can.
- **Unit tests of the agent,** with the same slow store: `on_tick` and `on_request` never wait for it; a
  first request from the peer is refused with `recording_promise` and granted once the write finishes;
  the candidate asks again at its next renewal interval after that refusal.
- **The simulation and the model.** `LeaseSimulationTest` gains voters that refuse with
  `recording_promise` at random, and must still find no moment with two leaders. The TLA+ model of the
  lease rules, in `docs/availability/tla`, is read to check whether it already allows a voter to refuse
  at any time. If it does, it needs no change; if it does not, it is changed to allow it and checked
  again. The model already allows it. In `MajorityLeaseHA.tla` a grant can be lost on the way, and a
  voter that makes the promise and then refuses looks, to the candidate, exactly like a voter whose grant
  was lost: the refusal carries the epoch the candidate itself asked for, so it changes nothing the
  candidate holds. The model is not changed.
- **The high availability scenarios** pass as they did after step 2, and the time each change of leader
  takes is compared with the run after step 2.
- **The order's round trip** is measured before and after, by the method in
  `docs/operations/latency_findings.md`. No change is expected, because the lock removed is taken once a
  tick and is almost never contended. Each build was started on fresh state and sent five runs of 6,000
  orders at 100 a second with nine in ten cancelled, from a load client pinned to processors 0 and 1;
  the first run of each was discarded. The build before step 3 gave a median round trip of 118.3, 113.3,
  113.0 and 112.4 microseconds in its four runs, and the build after step 3 and the fix for
  [BUG-0147](../bug_list.md#bug_0147) gave 109.2, 109.5, 112.7 and 113.0. The two ranges overlap, and
  the difference between them is no larger than the difference between runs of one build. The hop into
  the FIX gateway's application thread had a mean of 2.7 microseconds in every run of both builds, over
  23,469 to 23,531 observations a run. There is no change.

  The comparison holds only when the same matching engine leads in both builds. In one start of the
  build after step 3 the secondary matching engine led, and the median round trip was 133 to 135
  microseconds, about 20 more. The secondary is configured as a standby whose reactor blocks as soon as
  it has nothing to do, instead of spinning for 50 milliseconds first as the primary's does, and that is
  the likely reason. It is recorded as [BUG-0151](../bug_list.md#bug_0151). It is not a cost of step 3:
  the primary led in each of three further starts of each build.

## 7. What phase 3 does not do

- It does not change the lease rules or the timings. The only configuration it adds is the pairs the
  arbiter and the witness vote in (section 5).
- It does not touch [BUG-0142](../bug_list.md#bug_0142), in which the arbiter and the matching engine
  wait for the disk while handling leases through `EpochStore`. That is the exception to principle F4
  for changes of role, and whether to limit it is a separate open question.

## 8. Cost

- Step 1 is mostly moving files and changing include lines and namespaces, plus the conversion between
  `ComponentGroup` and the pair's number where the venue sends and receives lease messages. A few
  hundred lines change, almost all mechanically.
- Step 2 moves about 1,100 lines of the arbiter and the witness, replaces the venue's enumerations in
  them with configured names and numbers, adds the two configuration sections and their checks, and
  leaves two short programs in the venue.
- Step 3 rewrites one class of about 140 lines, changes the three places in `PairLeaseAgent` that write
  on the application thread, adds one refusal reason and the candidate's quicker retry, and adds the
  tests in section 6.4.

## 9. Decisions

### Decision 1. Should the application thread ever write and sync a promise record itself?

**Agreed 12 October 2026: A, never.** The agent refuses a promise the record does not yet cover, with
the new reason `recording_promise`, and grants once the background write has finished; the candidate
asks again at its next renewal interval; the record that an instance leads is written in the background
only. Section 6.3 gives the details. The alternative, B, kept the synchronous write at a change of role,
under the exception principle F4 allows, and was not chosen because a follower sequencer that stops for
the length of a slow sync holds up the leader's commits while it waits.

### Decision 2. Should the arbiter and the witness become a voting service of the library within phase 3?

**Agreed 12 October 2026: yes.** Sections 4 and 5 give the design; it is step 2 of this phase.

### Decision 3. What does the venue keep of the arbiter and the witness? (Choose A or B.)

**Background.** Step 2 moves everything the arbiter and the witness do into the library. Something
still has to be started as a process on each arbiter machine and on the witness machine, read a
configuration file, and run the library's voting service.

**A. The venue keeps its two programs, `arbiter` and `witness`, each a short `main`.** Each reads its
configuration file and starts the library's voting service.

- For: the programs' names, their configuration files, how they are deployed and released, and how
  `ha_test.py` and the supervision design start and stop them all stay exactly as they are. Step 2
  changes no scripts.
- Against: the venue still has two small directories under `applications/` for something that is
  entirely the library's, and another application that wanted a voting service would write the same
  short `main` again.

**B. The library provides the two programs.** They are built and installed with the library, under
names of the library's choosing, and the venue's deployment runs them with the venue's configuration
files.

- For: an application that wants a voting service writes no code at all, only configuration, which is
  what "configured rather than written" in [ha_library_boundary.md](ha_library_boundary.md) means in
  full.
- Against: the release, deployment and test scripts, and the process names they look for, all change
  in the same step, which makes it no longer purely a move, and a failure after it harder to place.

**Recommendation: A for phase 3.** It keeps step 2 a move that changes no scripts. Turning the two short
programs into programs the library provides is then a small separate change that can be made whenever a
second user of the voting service appears.

**Agreed 12 October 2026: A.**

### Decision 4. How are step 3 and BUG-0147 handled? (Choose A, B or C.)

**Agreed 12 October 2026: A.** Step 3 is committed as designed, and
[BUG-0147](../bug_list.md#bug_0147) is fixed next, before the phase 3 review.

**Background.** A sequencer whose role is still unknown writes gateway orders into its own WAL, while,
as a follower, its reactor's thread writes the leader's replicated records into the same WAL; the two
writers damage it. A sequencer learns that it follows when it first grants its leader a lease. Under
step 3 that first grant is usually refused with `recording_promise`, because the record on disk does not
yet cover it, and the leader asks again at its next renewal interval, so the sequencer learns its role
about a second later, when orders are already flowing. In the high availability scenarios after step 3
the WAL was damaged twice, in scenarios 13 and 40; it was damaged once before any change, in scenario
12. The alternatives were B, having a leader refused for this reason ask again at the next tick, which
narrows the window but leaves the fault; and C, holding step 3 back until the fault is fixed.

