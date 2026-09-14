# Project status {#project_status}

Development is paused at version 0.4.0. This page says what the project is, what has been shown
to work, what is known to be wrong, and where anyone picking it up should start.

## What exists

- **The framework**, `libraries/pubsub_itc_fw`: a reactor, inter-thread queues, pool and slab
  allocators, TCP inter-process communication, timers, a memory-mapped write-ahead log, pub/sub
  over that log, and a serialisation code generator.
- **A trading venue built on it**, in `applications/`: FIX and binary order gateways, a sequencer,
  a matching engine, an authentication service, arbiters and a witness. The sequencer, matching
  engine and arbiters each run as a pair of instances.
- **A Java admin service and FIX test client**, in `java/`.
- **A functional specification**, `docs/book`, stating 139 requirements.

## What has been shown to work

- **A trading day under load.** `perf_run.py --profile profiles/trading_day.toml` runs a shaped day
  of several million orders with cancels.
- **Failover and restart of each pair.** `ha_test.py` has 57 scenarios: killing each component,
  restarting each in each role, stopping a whole machine, and a member's open orders surviving a
  restart of the matching engine.
- **The FIX session layer.** Resend requests are answered with the real execution reports, a
  member's numbering continues across a reconnect and a change of gateway, and gaps in what a
  member sends are detected and asked for.

## What is known to be wrong

The [bug list](bug_list.md) holds 33 open entries, 12 of them high severity. The ones most likely to
matter first:

- **The specification is mostly untested.** 24 of 139 requirements are verified by a scenario
  ([BUG-0068](bug_list.md#bug_0068)). A scenario passing has repeatedly been found to say less
  than it appeared to, so treat untested requirements as unverified.
- **High availability has open defects.** A degraded promotion advances a generation the arbiter
  never learns ([BUG-0085](bug_list.md#bug_0085)); failover can move into a condition both
  instances share ([BUG-0010](bug_list.md#bug_0010)); and a process death on the same host is
  handled as a machine death ([BUG-0029](bug_list.md#bug_0029)).
- **Members can lose information.** An execution report produced while a session is unbound is
  never delivered ([BUG-0088](bug_list.md#bug_0088)), and a restarted gateway stops honouring
  cancel-on-disconnect ([BUG-0090](bug_list.md#bug_0090)).
- **The write-ahead log grows without limit** ([BUG-0048](bug_list.md#bug_0048)). Reclaiming it
  waits on a decision about how long history must be kept.
- **The venue cannot declare a trading halt** ([BUG-0065](bug_list.md#bug_0065)).

## What was to come next

In order:

1. **Pub/sub.** The requirements are in the framework chapter of the specification, and none is
   tested. The next step was a test script for simple publish and subscribe with high availability
   off, then with it on. The alternatives to unicast fanout are named there but not yet assessed.
2. **Technical events and trading phases** -- start of day, end of day, and a market halt. The
   shape is settled in [trading phases](venue/trading_phases.md), and the halt should be decided
   together with [BUG-0065](bug_list.md#bug_0065).
3. **The order activity recorder**, slice 11 of the [roadmap](roadmap.md), a subscriber to the
   matching engine's published topics.
4. **Log retention** ([BUG-0048](bug_list.md#bug_0048)), once the period is decided.

## Where to start

1. [The documentation contents](../docs/README.md), then [orientation](orientation/README.md) for what the
   venue is and how to build it.
2. Build, release, deploy and run with `./scripts/devsetup.sh`. The filesystem holding the
   sequencer's log must be mounted `lazytime`; see
   [filesystem requirements](operations/filesystem_requirements.md).
3. Run `python3 scripts/ha_test.py --scenario all` against the deployed venue.
4. Build and check the specification with `make -C docs/book check`.
5. Read `coding-rules-for-ai-chatbots.txt` and `project-summary-for-ai-chatbots.txt` in the
   repository root before changing code; the build enforces much of what they say.
