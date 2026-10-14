# Project status {#project_status}

The version is 0.4.0. New features are frozen while the design is restored, phase by phase, under
[the plan for restoring the design](recovery_plan.md). This page says what the project is, what has
been shown to work, what is known to be wrong, and where anyone picking it up should start.

## What exists

- **The framework**, `libraries/pubsub_itc_fw`: a reactor, inter-thread queues, pool and slab
  allocators, TCP inter-process communication, timers, a memory-mapped write-ahead log, pub/sub
  over that log, and a serialisation code generator.
- **A trading venue built on it**, in `applications/`: FIX and binary order gateways, a sequencer,
  a matching engine, an authentication service, arbiters and a witness. The sequencer, matching
  engine and arbiters each run as a pair of instances.
- **A Java admin service and FIX test client**, in `java/`.
- **A functional specification**, `docs/book`, stating 148 requirements.
- **One list of design principles**, [design_principles.md](design_principles.md), agreed, with
  where each holds and where it is broken today.

## What has been shown to work

- **A trading day under load.** `perf_run.py --profile profiles/trading_day.toml` runs a shaped day
  of several million orders with cancels.
- **Failover and restart of each pair.** `ha_test.py` has 74 scenarios: killing each component,
  restarting each in each role, stopping a whole machine, and a member's open orders surviving a
  restart of the matching engine.
- **The FIX session layer.** Resend requests are answered with the real execution reports, a
  member's numbering continues across a reconnect and a change of gateway, and gaps in what a
  member sends are detected and asked for.

## What is known to be wrong

The [bug list](bug_list.md) holds 56 open entries, 13 of them high severity. The ones most likely to
matter first, by design area:

- **The specification is mostly untested.** 30 of 148 requirements are verified by a scenario
  (`scripts/check_book_requirements.py` counts them; [BUG-0068](bug_list.md#bug_0068)). A scenario
  passing has repeatedly been found to say less than it appeared to, so treat untested requirements
  as unverified.
- **Locks and waits on the order path.** The design is meant to have none; the follower's second
  writer of the log, the lease promise recorder, the memory pools, the reactor's timers and its thread
  registry still take locks, each recorded under principle F2 in
  [design_principles.md](design_principles.md) with the phase of the plan that removes it.
- **High availability is built in the applications, not the library.** Moving it is the direction
  agreed in [ha_in_the_library.md](availability/ha_in_the_library.md); the design of the boundary,
  [ha_library_boundary.md](availability/ha_library_boundary.md), is agreed, and phases 3 to 9 of the
  plan carry it out. Open defects
  include failover into a condition both instances share ([BUG-0010](bug_list.md#bug_0010)), a
  process death on the same host handled as a machine death ([BUG-0029](bug_list.md#bug_0029)), and a
  venue running without a standby saying so too quietly ([BUG-0134](bug_list.md#bug_0134)).
- **Members can lose information.** A restarted gateway stops honouring cancel-on-disconnect
  ([BUG-0090](bug_list.md#bug_0090)), and an engine that restarts or is promoted reuses report
  identifiers ([BUG-0139](bug_list.md#bug_0139)).
- **The trading day has no start or end procedure.** Nothing archives the day's files, a file left
  from an earlier day is taken as a restart ([BUG-0137](bug_list.md#bug_0137)), orders open at the
  close are never cancelled ([BUG-0132](bug_list.md#bug_0132)), and the venue cannot declare a
  trading halt ([BUG-0065](bug_list.md#bug_0065)).
- **The write-ahead log is never reclaimed during the day** ([BUG-0048](bug_list.md#bug_0048)).
  Whether it may be waits on a decision about how long history must be kept.

## What comes next

The order is set by [the plan for restoring the design](recovery_plan.md), and each phase ends with a
review at which Andrew decides whether the next starts. Phase 1, agreeing the design principles, is
done. Phase 2, making the build enforce the framework principles, is done. Phases 3 to 9 move the high
availability machinery from the sequencer and the matching engine into the library, in the order agreed
in [ha_library_boundary.md](availability/ha_library_boundary.md); phase 3, moving the lease classes and the
voting service into the library, is finished; its design is
[lease_classes_in_the_library.md](availability/lease_classes_in_the_library.md). Phase 4, the library's
replicated WAL with one writing thread, has had its three steps done: the sequencer keeps its WAL
through the library's `ReplicatedWal`, whose thread is the only one that writes it. Its design is
[replicated_wal_in_the_library.md](availability/replicated_wal_in_the_library.md), and it waits for
Andrew's review. New features, including pub/sub, the order activity recorder and the trading phases, wait
until the plan is finished.

## Where to start

1. [The documentation contents](../docs/README.md), then [orientation](orientation/README.md) for what the
   venue is and how to build it.
2. Build, release, deploy and run with `./scripts/devsetup.sh`. The filesystem holding the
   sequencer's log must be mounted `lazytime`; see
   [filesystem requirements](operations/filesystem_requirements.md).
3. Run `python3 scripts/ha_test.py --scenario all` against the deployed venue.
4. Build and check the specification with `make -C docs/book check`.
5. Read `coding-rules-for-ai-chatbots.txt` in the repository root before changing code; the build
   enforces much of what it says.
