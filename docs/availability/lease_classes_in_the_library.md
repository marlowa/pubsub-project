# Phase 3: the lease classes in the library {#lease_classes_in_the_library}

> **Status: draft for review, not agreed.** This is the design for phase 3 of
> [the recovery plan](../recovery_plan.md). Nothing in the code is to change on the strength of this page
> until Andrew agrees it. Section 7 lists the decisions it needs.

## 1. What phase 3 does

Phase 3 is the first step in moving the high availability machinery out of the applications and into
the library, as set out in [ha_library_boundary.md](ha_library_boundary.md). It has two steps, done and
tested one after the other:

1. **The move.** The classes that decide which instance of a pair leads, and the messages they
   exchange, move from `applications/fix_common` into the library. How they behave does not change.
2. **The promise recorder.** The lock and the waits in `BackgroundPromiseRecorder` are removed, so that
   the thread that handles orders never takes a lock and never waits for the disk because of a lease
   ([BUG-0126](../bug_list.md#bug_0126)).

The move comes first because it changes no behaviour, so any failure after it is a fault in the move.
The second step then changes behaviour in one class, already in its new home.

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

## 3. Step 1: the move

**Where the classes go.** Every class in the table in section 2 moves to
`libraries/pubsub_itc_fw/include/pubsub_itc_fw/`, into the namespace `pubsub_itc_fw`, keeping its name.
They stay header-only, as they are now. Code that includes them changes from, for example,
`#include <LeaseHolder.hpp>` to `#include <pubsub_itc_fw/LeaseHolder.hpp>`.

**Where the messages go.** `LeaseRequest`, `LeaseGrant`, `LeaseRefusal` and `LeaseRefusalReason` move
out of `leader_follower.dsl` into a new file of the library's own, `lease.dsl`, generated into the
namespace `pubsub_itc_fw`. Their message numbers stay 130, 131 and 132, so nothing on the wire changes.

**How a message names its pair.** The library cannot name the venue's components, so in the library's
messages the pair is a plain number, `i32 pair_id`, in place of `ComponentGroup group`. `ComponentGroup`
stays in `leader_follower.dsl`, because the venue's own messages use it (for example
`ArbiterStateRecord`), and the venue converts between the two where it sends and receives lease
messages. The values stay the same (sequencer 1, matching engine 2, matching engine publisher 3,
arbiter 4), and an enumeration in the message definitions is already sent as a 32-bit number, so the
bytes on the wire do not change either. `PairLeaseAgent` takes the pair's number in its constructor
instead of a `ComponentGroup`. About forty places in the venue use `ComponentGroup` with lease
messages, mostly in the arbiter, and each gains a conversion.

**What stays in the venue.** In this phase:

- the arbiter and the witness stay applications. They use the library's classes, but turning them into
  a voting service that the library provides is not part of this phase (decision 2 in section 7);
- `ComponentLeaseVoters` and `ComponentStatementStore`, the arbiter's votes in the component pairs, and
  `ArbiterStateRecord`, which one arbiter sends the other, stay in `applications/arbiter`;
- what each component does when it starts or stops leading stays in that component.

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

## 4. Step 2: the promise recorder without a lock

### 4.1 What happens now

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

### 4.2 Passing requests and results without a lock

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
10 milliseconds later than it does now. Writes are started seconds before they are needed (section
4.1), so the delay never matters, except in decision 1 below, where it adds at most 10 milliseconds to a
change of leader. The project builds with C++17, which has no way for one thread to wait on an atomic
variable without a lock.

When the recorder is destroyed, a third atomic tells the writer thread to stop, and the destructor
waits up to a second for it to finish, as it does now.

The thread stays a `ThreadWithJoinTimeout`, as principle F8 requires.

### 4.3 Never writing on the application thread

Removing the lock does not on its own remove the wait in point 3 of section 4.1. That needs a decision
about what the agent does when it must make a promise the record on disk does not yet cover. Decision 1
in section 7 sets out the two choices. If the answer is A, as recommended:

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

**When this costs anything.** A change of leader usually needs only the third voter's grant: when the
leader has died, its peer asks the arbiter, which grants, and the dead leader cannot be asked anyway.
The new refusal therefore delays a change of leader only when the third voter is unavailable and the
peer must grant. In that case the delay is about one renewal interval, one second by default.

### 4.4 How step 2 is known to be right

- **The build refuses the old code.** The three entries for `BackgroundPromiseRecorder` are removed from
  `_PERMITTED_LOCKS`, so `scripts/check_standards.py` refuses any lock left in it. This is checked by
  running the check once with the entries removed and the old code still in place, and seeing it fail.
- **Unit tests of the recorder,** using a store that takes two seconds to write: starting a write and
  reading its outcome both return at once while the store is writing; the outcome is reported once; a
  write is not started while one is in progress; and the destructor returns within its wait while a
  write is stuck. The same tests run in the ThreadSanitizer build (`build-tsan`), which reports any data
  race between the two threads.
- **Unit tests of the agent,** with the same slow store: `on_tick` and `on_request` never wait for it; a
  first request from the peer is refused with `recording_promise` and granted once the write finishes;
  the candidate asks again at its next renewal interval after that refusal.
- **The simulation and the model.** `LeaseSimulationTest` gains voters that refuse with
  `recording_promise` at random, and must still find no moment with two leaders. The TLA+ model of the
  lease rules, in `docs/availability/tla`, is read to check whether it already allows a voter to refuse
  at any time. If it does, it needs no change; if it does not, it is changed to allow it and checked
  again. Which of the two was found is stated at the review.
- **The high availability scenarios** pass as they did after step 1, and the time each change of leader
  takes is compared with the run after step 1.
- **The order's round trip** is measured before and after, by the method in
  `docs/operations/latency_findings.md`. No change is expected, because the lock removed is taken once a
  tick and is almost never contended; the measurement confirms it.

## 5. What phase 3 does not do

- It does not make the arbiter and the witness into a voting service of the library (decision 2).
- It does not change the lease rules, the timings or the configuration.
- It does not touch [BUG-0142](../bug_list.md#bug_0142), in which the arbiter and the matching engine
  wait for the disk while handling leases through `EpochStore`. That is the exception to principle F4
  for changes of role, and whether to limit it is a separate open question.

## 6. Cost

- Step 1 is mostly moving files and changing include lines and namespaces, plus the conversion between
  `ComponentGroup` and the pair's number in about forty places. A few hundred lines change, almost all
  mechanically.
- Step 2 rewrites one class of about 140 lines, changes the three places in `PairLeaseAgent` that write
  on the application thread, adds one refusal reason and the candidate's quicker retry, and adds the
  tests in section 4.4.

## 7. Decisions needed

These are two separate questions.

### Decision 1. Should the application thread ever write and sync a promise record itself? (Choose A or B.)

**Background.** Section 4.1 lists the three cases in which the thread that handles orders writes and
syncs a promise record itself, and waits for the disk while it does. Removing the lock (section 4.2)
does not change those cases. BUG-0126 asks for a decision on them.

**A. Never.** The agent refuses a promise the record does not yet cover, with the new reason
`recording_promise`, and grants once the background write has finished. The candidate asks again at
its next renewal interval. The record that the instance leads is written in the background only.
Section 4.3 gives the details.

- For: the thread that handles orders never waits for the disk because of a lease, even when the disk
  is slow. Principle F4 holds for leases with no exception.
- Against: a change of leader can take about one second longer, but only when the third voter is
  unavailable and the peer must grant. It needs a new refusal reason and a change to how a candidate
  responds to a refusal.

**B. Only at a change of role.** The synchronous write stays for the first two cases in section 4.1,
which are changes of role and so fall under the exception principle F4 already allows. Only the lock
on every tick is removed. For the third case, a slow disk, the agent refuses rather than waits.

- For: smaller. The lease rules, the refusal reasons and the candidate's behaviour do not change at
  all.
- Against: the thread that handles orders can still wait for the disk at a change of role, for as long
  as a sync takes. BUG-0142 shows that on a slow disk this can be seconds, and a follower sequencer that
  stops for seconds holds up the leader's commits while it waits.

**Recommendation: A.**

### Decision 2. Should the arbiter and the witness become a voting service of the library within phase 3? (Yes or no.)

**Background.** [ha_library_boundary.md](ha_library_boundary.md), section 2, says the voters that hold
no WAL, the arbiters and the witness, become a voting service the library provides, which any pair can
use by configuration. None of the seven phases in the recovery plan says when that is done. Phase 3
moves the classes the arbiter and the witness are built from, but leaves the two programs as they are.

**What "no" means.** In phase 3 the arbiter and the witness use the library's classes and otherwise
stay as they are. The voting service is added to the plan as a step of its own after phase 9. Nothing
in phases 4 to 9 needs it: they concern the replicated WAL, the matching engine and the gateways.

**What "yes" means.** Phase 3 gains a third step, turning the arbiter's and the witness's voting into a
service in the library, configured rather than written. That is a much larger change than the move,
and touches how the arbiters choose which of them is active.

**Recommendation: no**, with the voting service added to the plan after phase 9.
