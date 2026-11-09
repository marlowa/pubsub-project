# Project status {#project_status}

The version is 0.5.0. New features are frozen while the design is restored, phase by phase, under
[the plan for restoring the design](recovery_plan.md). Phases 1 to 11 of that plan are finished, and
phase 12, working through the bug list, is under way. This page says what the project is, what has
been shown to work, what is known to be wrong, and where anyone picking it up should start.

## What exists

- **The framework**, `libraries/pubsub_itc_fw`: a reactor, inter-thread queues, pool and slab
  allocators, TCP inter-process communication, timers, a memory-mapped write-ahead log, pub/sub
  over that WAL, and a serialisation code generator.
- **A trading venue built on it**, in `applications/`: FIX and binary order gateways, a sequencer,
  a matching engine, an authentication service, arbiters and a witness. The sequencer, matching
  engine and arbiters each run as a pair of instances.
- **A Java admin service and FIX test client**, in `java/`.
- **A functional specification**, `docs/book`, stating 157 requirements.
- **One list of design principles**, [design_principles.md](design_principles.md), agreed, with
  where each holds and where it is broken today.

## What has been shown to work

- **A trading day under load.** `perf_run.py --profile profiles/trading_day.toml` runs a shaped day
  of several million orders with cancels.
- **Failover and restart of each pair.** `ha_test.py` has 102 scenarios: killing each component,
  restarting each in each role, stopping a whole machine, a member's open orders surviving a restart
  of the matching engine and a change of engine leader, a command that crashes both engines being
  voided, orders held while no engine leads, and reports held for a gateway surviving the death of
  the sequencer holding them, and a follower sequencer that has lost the first file of its WAL being
  sent every record again. All 102 pass, except that scenario 80's check of the matching engines on its WAL
  sometimes loses reports, as [BUG-0234](bug_list.md#bug_0234) records; and the 39 that also run with the secondary matching
  engine leading first pass that way too. After each scenario both matching
  engines are run on its WAL and must give the reports the WAL holds.
- **The FIX session layer.** Resend requests are answered with the real execution reports, a
  member's numbering continues across a reconnect and a change of gateway, and gaps in what a
  member sends are detected and asked for.

## What is known to be wrong

The [bug list](bug_list.md) holds 64 open entries, 15 of them high severity. Each one is judged,
with its evidence, in [the review of the bug list](bug_review.md), which also sets the order in which
they are being fixed. The ones most likely to matter first, by design area:

- **The specification is mostly untested.** 40 of 158 requirements are verified by a scenario, and
  one more by a TLA+ model (`scripts/check_book_requirements.py` counts them; [BUG-0068](bug_list.md#bug_0068)). A scenario
  passing has repeatedly been found to say less than it appeared to, so treat untested requirements
  as unverified.
- **Locks and waits on the order path.** The design is meant to have none. The memory pools take a
  lock when a pool runs out ([BUG-0127](bug_list.md#bug_0127)), and the reactor takes a lock for its timers
  ([BUG-0128](bug_list.md#bug_0128)), though not for each order; principle F2 in
  [design_principles.md](design_principles.md) records each.
- **High availability.** The machinery is in the library, as agreed in
  [ha_in_the_library.md](availability/ha_in_the_library.md) and
  [ha_library_boundary.md](availability/ha_library_boundary.md). Open defects include a matching engine
  that is connected but has stopped working looking healthy ([BUG-0194](bug_list.md#bug_0194)), a
  matching engine catching up from far back sending its reports faster than the sequencer reads them,
  so that the connection is closed and the rest of the reports are lost ([BUG-0234](bug_list.md#bug_0234)),
  growing the order book needing more memory than the machine has ([BUG-0028](bug_list.md#bug_0028)),
  a restarted matching engine waiting 2 seconds for the sequencer to connect to it again
  ([BUG-0237](bug_list.md#bug_0237)), and a venue running without a standby saying so too quietly
  ([BUG-0134](bug_list.md#bug_0134)).
- **Members can lose information.** A restarted gateway stops honouring cancel-on-disconnect
  ([BUG-0090](bug_list.md#bug_0090)).
- **The trading day has no start or end procedure.** Nothing archives the day's files, a file left
  from an earlier day is taken as a restart ([BUG-0137](bug_list.md#bug_0137)), orders open at the
  close are never cancelled ([BUG-0132](bug_list.md#bug_0132)), and a trading halt is told to a
  member only in the reply to an order, and cannot be applied to one instrument
  ([BUG-0065](bug_list.md#bug_0065)).
- **The write-ahead log is never reclaimed during the day** ([BUG-0048](bug_list.md#bug_0048)).
  Whether it may be waits on a decision about how long history must be kept.

## What comes next

The order is set by [the plan for restoring the design](recovery_plan.md), and each phase ends with a
review at which Andrew decides whether the next starts. Phases 1 to 11 are finished; the plan's
"Already done" section says what each one delivered. Together they wrote down and agreed the design
principles, made the build enforce the ones about locks and threads, moved the high availability
machinery from the sequencer and the matching engine into the library, made the matching engine
deterministic with both engines applying the WAL, put the check against sequencing a command twice in
one place, divided the sequencer and the matching engine into parts with one job each, and checked the
models and documents against the principles.

Phase 12, returning to the bug list, is under way. Every open entry has been judged against the
principles in [bug_review.md](bug_review.md), and the entries are being fixed in the order agreed there:
first the breaches of a principle whose remedy is small, then those that need a short design first.
The phase is finished when there is an agreed set of the guarantees the venue gives, evidence for which
of them hold, and a recorded decision about which remaining risks are accepted.

New features, including further pub/sub work, the order activity recorder and the trading phases, wait until the
plan is finished.

## Where to start

1. [The documentation contents](../docs/README.md), then [orientation](orientation/README.md) for what the
   venue is and how to build it.
2. Build, release, deploy and run with `./scripts/devsetup.sh`. The filesystem holding the
   sequencer's WAL must be mounted `lazytime`; see
   [filesystem requirements](operations/filesystem_requirements.md).
3. Run `python3 scripts/ha_test.py --scenario all` against the deployed venue.
4. Build and check the specification with `make -C docs/book check`.
5. Read `coding-rules-for-ai-chatbots.txt` in the repository root before changing code; the build
   enforces much of what it says.
