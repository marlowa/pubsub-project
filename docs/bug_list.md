# Bug List

| | |
|---|---|
| Bugs recorded | 44 |
| Open | 23 (22 defects, 1 task) |
| Closed | 21 |
| Next id | BUG-0045 |

## Open bugs by severity

5 high, 11 medium, 7 low.

| Id | Severity | Kind | Title |
|---|---|---|---|
| BUG-0009 | high | defect | The venue accepts orders indefinitely with no matching engine, and tells nobody |
| BUG-0010 | high | defect | HA fails over into a condition both nodes share |
| BUG-0028 | high | defect | Growing the order book by doubling needs more memory than the machine has |
| BUG-0029 | high | defect | A process death on the same host takes the machine-death path |
| BUG-0038 | high | defect | Inbound FIX sequence numbers are never checked, so a member's lost order is not noticed |
| BUG-0001 | medium | defect | Shutdown timeout errors in timer tests |
| BUG-0002 | medium | defect | The FIX order gateway's `process_message` exit paths are not audited |
| BUG-0003 | medium | defect | Environment placeholders are missing outside dev |
| BUG-0006 | medium | defect | ResendRequest under load |
| BUG-0015 | medium | defect | `deploy.py` silently ignores a change to an environment file |
| BUG-0018 | medium | defect | The idle-connection reaper tears down the pre-warmed failover link |
| BUG-0030 | medium | task | Restart coverage: what ha_test.py exercises, and what it does not |
| BUG-0037 | medium | defect | The resend scenario never creates a gap, so nothing tests the resend |
| BUG-0039 | medium | defect | ResendRequest ignores EndSeqNo, so every resend runs to the head of the stream |
| BUG-0040 | medium | defect | The order-accounting check reports lost orders when it means it could not count them |
| BUG-0041 | medium | defect | Five ways the venue will not start on a RHEL8 target host |
| BUG-0004 | low | defect | Doxygen 1.8.14 turns `\ref` labels into bare directory links |
| BUG-0005 | low | defect | fix-test-client reports a dead gateway poorly |
| BUG-0011 | low | defect | `cmake --install` re-lays config templates unexpanded |
| BUG-0014 | low | defect | Python style warnings across the top-level scripts, and a lint gate that ignores them |
| BUG-0016 | low | defect | `start_fix_seq_system.py` launches from configs that no longer exist |
| BUG-0017 | low | defect | Slab allocator design notes do not mention the tripwire |
| BUG-0044 | low | defect | Scripts cannot answer `--help` without their plotting dependencies |

---

## Conventions

Defects found and not yet fixed, and defects fixed recently enough to be worth remembering.

**Why this file exists.** BUG-0007, the metrics-inside-CPU-pinning defect, was found on 2026-08-04,
judged not worth fixing at that moment, and then forgotten — it survived only in a working note
nobody else could see, and had to be rediscovered on 2026-08-08 before anything was done about it.
A defect that is known and invisible is worse than one nobody has found: the project carries the
risk without carrying the knowledge.

**Every entry records the date it was found and how it was found.** The second is the more useful
half. "Found by the trading-day load run" tells you which activity is worth repeating; "found by
reading the code" tells you the tests would not have caught it.

Closed entries are kept for one release cycle and then deleted -- the commit is the permanent
record. **An id is permanent and is never reused**, so a citation from the code always resolves.

**Most entries are defects. A few are tracked tasks**, marked with a `Kind` row; an entry with no
`Kind` row is a defect. A task is kept here rather than in a list of its own, deliberately: a
separate list is opened less often than this one, and an item nobody reads is the failure this
file was created to prevent. BUG-0030 is the current example -- a coverage matrix with four of
eighteen cells done, which belongs in neither Open-as-a-defect nor Closed-as-finished without
saying which it is.

**Severity** is about what the defect can do to the venue, not how hard it is to fix.

- **high** -- can lose or corrupt orders, stop the venue trading, or leave an operator believing
  one of those did not happen when it did.
- **medium** -- degrades operation, costs a run or recovery time, or leaves a risk unbounded,
  without losing orders by itself.
- **low** -- documentation, tooling ergonomics, or contained to a developer workflow.

## See Also

- [Roadmap](roadmap.md) — planned work, as distinct from defects
- [Testing and Code Coverage](orientation/testing.md)

---

## Open

### BUG-0001: Shutdown timeout errors in timer tests

| | |
|---|---|
| Severity | medium |
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Timer test logs |
| Impact | Unknown — the errors appear but nothing is known to misbehave |

After the timer SEGV fix, "did not stop within shutdown_timeout" and "failed to join within
shutdown_timeout" still appear in timer test logs. **Root cause not identified.** Worth noting that
`ThreadWithJoinTimeout` exists precisely because a raw `std::thread` terminates on an early return
before join, so a join that times out is not obviously benign.

### BUG-0002: The FIX order gateway's `process_message` exit paths are not audited

| | |
|---|---|
| Severity | medium |
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Code reading |
| Impact | Possible false stuck-thread detection |

`ApplicationThread::process_message` (`ApplicationThread.hpp:726`) is expected to update
`time_event_finished_` before it returns. If any exit path from it skips that, the reactor
watchdog reports the thread as stuck when it is not. **Not yet audited**, on the FIX order
gateway thread or on any other.

Given that the reactor watchdog is what made the order-book rehash stall diagnosable, false
positives from it would be costly: a watchdog that cries wolf gets ignored, and it is the
instrument that turned a year-old latency mystery into a measurement.

### BUG-0003: Environment placeholders are missing outside dev

| | |
|---|---|
| Severity | medium |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Reading `deploy.py` against the environment files while working on config templating |
| Impact | `deploy.py` exits on preprod, prod and test-1 |

`environments/preprod.toml`, `prod.toml` and `test-1.toml` each lack 10–12 of the placeholder
values their component templates require. Only `dev.toml` is complete, so only dev can be deployed.

**Deliberately not fixed**: the missing values are real hostnames, ports and certificate paths for
environments that do not exist yet. Inventing them would produce a file that deploys and then
fails at run time, which is worse than one that refuses to deploy.

**The gap widened by 60 on 2026-08-09**, when the reactor queue pools became templated (BUG-0025,
now closed). Unlike the hostnames, these are safe to fill in from `dev.toml` whenever those
environments are built, because a queue depth is a capacity decision rather than a fact about a
host that has to be looked up — so the count is larger but the difficulty is unchanged.

### BUG-0004: Doxygen 1.8.14 turns `\ref` labels into bare directory links

| | |
|---|---|
| Severity | low |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Building the docs on RHEL8, where 1.8.14 is the newest packaged release |
| Impact | Documentation only; the architecture map's cross-links break |

An unresolved `\ref` collapses to `href="../../"`, which a browser opens as a directory listing —
or, on Windows, a file chooser. 1.8.14 does **not** fail the build on an unresolved reference, so
this is silent. `docs/architecture_map_howto.dox` proposes a post-build check; not written.

### BUG-0005: fix-test-client reports a dead gateway poorly

| | |
|---|---|
| Severity | low |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Manual testing of the logon page |
| Impact | The UI misleads about connection state |

Two undecided items: `lastError` is left empty, and `connected` stays true after the gateway dies.
FIX produces no disconnect message, so there is nothing to display without inferring it.

### BUG-0006: ResendRequest under load

| | |
|---|---|
| Severity | medium |
| Found | before 2026-08 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Noted when the feature was written |
| Impact | Unknown |

Partly overtaken by the 0.3.0 work, which replaced the blanket `SequenceReset-GapFill` with real
resends carrying `PossDupFlag`. The original concern — never exercised under load — still stands,
and the trading-day profile is a natural place to exercise it once it drives the FIX gateway.

### BUG-0009: The venue accepts orders indefinitely with no matching engine, and tells nobody

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (49d9dd1) |
| How | The first clean trading-day load run, after the matching engine was OOM-killed |
| Impact | 924,000 orders accepted and acknowledged to the member with nothing able to process them; 1,087,912 orders deferred over 7 minutes |

When the matching engine connection drops, the sequencer commits each order to the WAL and
defers forwarding it:

```
SequencerThread: no matching engine connected -- order seq=59678842 WAL-committed,
forward deferred until an ME reconnects (recovered via WAL replay on ME promotion)
```

That policy is sound for a brief failover — the orders are durable and a promoted matching engine
replays them. Three things about it are not.

**The assumption can stop holding, and nothing notices.** A matching engine was promoted and did
reconcile, then died two minutes later. The sequencer went on deferring for another five
minutes, waiting for a recovery that could no longer happen because no matching engine
existed at all.

**It is logged at INFO, once per order — 1,087,912 times.** A million lines saying the
venue is degraded, at the level used for routine progress. Volume that large hides the
condition rather than reporting it.

**Nothing propagates to the gateway.** The sequencer knew there was no matching engine for
seven minutes. The gateway kept accepting orders and acknowledging them, logging
`dropped=0` throughout, and the member saw no difference. The sequencer has the knowledge,
the gateway has the member relationship, and there is no path between them.

Suggested shape, not yet designed: deferring a handful of orders across a brief failover
should stay silent; deferring thousands over minutes should escalate — a rate-limited
WARNING, a metric for deferred-order count and age, and ultimately a signal that makes the
gateways stop accepting. **A venue that takes orders it cannot process is worse than one
that refuses them.**

Related: BUG-0010, since the deferral policy assumes a promotion that will
succeed.

#### The gateway's own health line reads green throughout, 2026-08-09

Seen from the gateway's side in run 10, and worse than the sequencer half because this is
the line an operator would actually watch. `GW-PROGRESS` reports accounted, sent, dropped
and orders received. Across the failover it stopped being emitted at all for **2 minutes
19 seconds**, then caught up in a burst of three reports inside 0.2 seconds:

```
18:39:36  accounted=10,247,000  sent=10,247,000  dropped=0  nos_received=9,315,468
          <- no progress report for 2m19s
18:41:55  accounted=10,261,000  sent=10,261,000  dropped=0  nos_received=9,546,040
18:41:55  accounted=10,275,000  ...
18:41:55  accounted=10,289,000  ...
```

**230,572 orders arrived during that window and 14,000 were accounted for.** `dropped`
stayed at zero the whole way through, and it was not lying: nothing was dropped. The orders
were accepted, acknowledged to the member, and queued behind a matching engine that no
longer existed.

Two distinct faults, and the second is the awkward one:

- **The health line goes silent exactly when it is most wanted.** It is emitted per N
  orders accounted, so when accounting stalls the reporting stalls with it. A line driven
  by progress cannot report an absence of progress. It needs a time-based emission as well,
  or a reader watching a terminal sees the last healthy line and nothing after it.
- **`dropped=0` is true and misleading together.** An operator watching for trouble watches
  that counter, and a venue can accept a quarter of a million orders it cannot process
  without moving it. Whatever replaces this needs to distinguish *accepted and processed*
  from *accepted and queued behind nothing* — the gap between `nos_received` and
  `accounted` already carries that information and nothing reports it.

### BUG-0010: HA fails over into a condition both nodes share

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (49d9dd1) |
| How | The first clean trading-day load run — the primary was OOM-killed and the promoted secondary died 2 minutes later. Recurred 2026-08-21 |
| Impact | Failover postponed an outage by about 4 minutes instead of preventing it |

The HA mechanism itself performed correctly and quickly:

```
16:49:30  primary OOM-killed (9.9 GB, book 8,388,608)
16:49:31  secondary: replication connection lost -- replica book now stale
16:49:46  promotion timeout fired, arbitration requested, elected leader
16:49:49  reconciled 58,444 records in 2.8s -- book 8,445,780
16:51:41  reactor watchdog: callback stuck [109105 ms] -- orderly shutdown
```

Detection, arbitration, promotion and reconciliation all worked. **The secondary then died
of the same thing**, because it came up holding a book of 8,445,780 orders on a machine
that had just killed a process for holding 9.9 GB.

**HA protects against independent failures. It cannot protect against a systemic condition
both nodes share** — memory exhaustion, a poison message, a bug reached by the same input,
a shared dependency. Failing over into it converts an outage into a slightly later outage
and burns the standby doing it.

**2026-08-21: the same failure, and this time the standby survived it.** The primary was
OOM-killed again, at a larger book still, and the promoted secondary kept trading:

```
19:08:41  primary OOM-killed (anon-rss 10.3 GB, 2^25-bucket table)
19:08:42  secondary arms the 15s promotion timeout
19:08:57  timeout fires, arbitration requested -- decision received 3 ms later
19:09:04  duplicate ArbitrationDecision correctly ignored
19:11     back to 1926 orders/s, and stayed there for the remaining three phases
```

The difference is not that HA improved. It is that killing the primary **freed 10 GB**, so the
condition the two nodes shared stopped being true the moment one of them died. Memory available
went from zero to 12.9 GB. That is luck rather than design -- had the survivor needed to grow its
own table again it would have gone the same way -- but it is worth recording that the mechanism
recovers cleanly when the shared condition lifts, and that arbitration itself took 3 ms under
genuine exhaustion with a ~10 GB book to take over.

It also sharpens the point above. The systemic condition here is the growth policy, and the entry
on it is *Growing the order book by doubling needs more memory than the machine has*. Reserving
the table instead of growing into it removes the condition rather than surviving it.

Worth designing for explicitly. Open questions rather than a plan:

- Should a promoted node **check its own headroom** before accepting promotion, and decline
  if it is in the same state the primary died in?
- Should the arbiter know *why* the primary went, so it can distinguish "the process died"
  from "the process died of something I am also suffering"?
- Is there a signal a node can publish that means "I am not a safe failover target"?
- What is the right behaviour when there is no safe target — refuse orders rather than
  accept ones that cannot be processed? See BUG-0009, on accepting orders with no
  matching engine.

Note this run's book growth was itself an artefact: the matching engine does no matching and the load
client cannot yet cancel, so nothing removed orders. The **failover behaviour** is the
finding here, not the growth.

#### Revised 2026-08-09: the outcome is not always this bad, and the reason is worth keeping

Run 8 reached the same state — machine exhausted, MemAvailable 0.42 GB, primary matching
engine OOM-killed at 12.9 GB while rehashing — and **failover succeeded**. The venue
completed the trading day:

```
11:17:52  primary OOM-killed (anon-rss 12.9 GB, mid-rehash at 2^23)
11:18:07  ArbitrationDecision (group=matching_engine leader=2 follower=1 epoch=1)
11:19:23  secondary adopts LEADER (91s after the kill)
          phases 5, 6 and 7 all PASS -- every order acknowledged
11:48:51  clean shutdown, every component
```

The secondary went on to reach 14.88 GB — **larger than the primary ever was** — and
survived, because killing the primary freed 7.6 GB.

**When the shared condition is memory, the OOM killer's victim selection is load shedding.**
Removing the largest consumer relieves the survivor, so the systemic condition is not
uniformly fatal the way the original entry implies. That does not make HA a defence against
systemic failure — it made no difference to the *cause*, and phase 4 still failed with a p99
of 107 seconds — but the failure mode is "may or may not survive, depending on which process
the kernel picks and how much its death releases", not "will die too".

This sharpens the open questions above rather than answering them. A promoted node checking
its own headroom would, in run 8, have **wrongly declined** a promotion it went on to
complete successfully, because the headroom it needed did not exist until the moment the
primary died.

### BUG-0011: `cmake --install` re-lays config templates unexpanded

| | |
|---|---|
| Severity | low |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | A trading-day run failed to start immediately after a rebuild |
| Impact | `devenv.py` refuses to start until `deploy.py` is re-run |

Every build re-installs the `${placeholder}` templates over the deployed, expanded configs, so
**`deploy.py` must be re-run after every build**. `devenv.py` does detect it and says so clearly,
which is why this is a trap rather than a fault — but it is easy to hit when iterating on a
component and then starting the venue.

See also BUG-0015, which is the converse and the more dangerous half.

### BUG-0014: Python style warnings across the top-level scripts, and a lint gate that ignores them

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (2174fb6) |
| How | Adding the top-level scripts to the pylint gate after a release check found an undefined variable in `perf_run.py` |
| Impact | Style only today. The errors that matter are now gated; the warnings are not |

The pylint gate ran `pylint dsl fix_dictionary` — two package directories — so every script in
the repository root was checked by nothing. That is how `NameError: name 'prefix' is not
defined` survived in `perf_run.py`'s FIX path until `release_check.py` ran it.

The gate now also runs the top-level scripts, but **errors only**, because that is the bar they
can pass today. `perf_run.py` scores 9.15/10 on style: 26 lines over the limit, imports out of
position, several functions with too many locals or arguments, `subprocess.run` without an
explicit `check=`, and files opened without an encoding.

**The resolution is to fix those warnings and then make the gate fail on them**, so the
top-level scripts are held to the same standard as the DSL. Two of the warning classes are
worth more than tidiness:

- `W1514` unspecified-encoding — a file opened without one takes the locale's default, and
  these scripts read and write config and log files.
- `W1510` `subprocess.run` without `check` — silently ignoring a non-zero exit is how a
  deployment step appears to succeed.

Not urgent, but it should not sit indefinitely: the gate as it stands protects against the
class of defect that stops a script dead, and nothing else.

### BUG-0015: `deploy.py` silently ignores a change to an environment file

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Raising the binary gateway's open-order pool in `environments/dev.toml` before a load run; the deployed config still held the old value after `deploy.py` reported success |
| Impact | A run can execute a whole profile against the *old* configuration while every command in the runbook appears to have succeeded |

`deploy.py` substitutes `${placeholder}` values into the deployed configs; it does not re-copy the
templates. Once a config has been expanded it contains no placeholder, so a later edit to
`environments/dev.toml` has no effect: `deploy.py` reports `0 template(s) expanded` and exits
zero. Nothing warns, and the venue starts happily on the previous values.

The working sequence is a re-install first, so there is something left to expand:

```bash
cmake --install build                     # re-lay the templates, unexpanded
python3 scripts/deploy.py --skip-db --skip-certs  # expand them from environments/dev.toml
```

**Worth fixing rather than documenting**, because the failure is silent and the cost is a whole
run. Either re-copy a template when it or the environment file is newer than the deployed config,
or refuse to report success when a deployment expanded nothing — `0 template(s) expanded` is
almost never what the caller intended, and it is the one case that currently looks identical to
success.

### BUG-0016: `start_fix_seq_system.py` launches from configs that no longer exist

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Tracing what still referenced `matching_engine.toml` before deleting it |
| Impact | The script cannot start a venue; it fails at the sequencer, and nothing says why until you read it |

`start_fix_seq_system.py` in the repository root starts the sequencer from
`etc/sequencer/sequencer.toml` and the matching engine from `etc/matching_engine/matching_engine.toml`.
Neither file exists. They are pre-rename names: `arbiter.toml` and `sequencer.toml` were removed
when the HA `_primary`/`_secondary` pair replaced them, and `matching_engine.toml` was deleted on
2026-08-09 as the last of that family. `ha_test.py` already carries a comment noting these three
are "orphaned and rejected by today's binaries".

It is not obviously dead code — 10 KB, executable, in the root — so the next person to reach for
it will spend time on it before finding out.

**Fix is a choice, not a lookup.** Either repoint it at `sequencer_primary.toml` and
`matching_engine_primary.toml`, the way `ha_test.py` does for its single-engine topology, or delete
it as superseded by `devenv.py` and `perf_run.py`. That question was deliberately left open rather
than answered in passing.

### BUG-0017: Slab allocator design notes do not mention the tripwire

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (f413a72) |
| How | Noticed while fixing the tripwire |
| Impact | Documentation only |

`drain_empty_slab_queue()` carries a safety tripwire that throws and takes the reactor down
with it. That is a significant behaviour of the allocator and it appears nowhere in the
design notes -- only in a comment inside the function. Anyone reasoning about failure modes
from the documentation would not know the allocator can terminate a component.

Worth adding when the allocator documentation is next touched, together with the rule that
the condition needs both a spent budget and a spun loop.

### BUG-0018: The idle-connection reaper tears down the pre-warmed failover link

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | The trading-day load run — the sequencer logged `connection lost` at WARNING every ten minutes while both matching engines were healthy |
| Impact | The secondary's order connection is absent for part of every ten-minute cycle, so failover latency depends on when the failure lands |

`MatchingEngine.cpp` registers the order listener on both roles, and says why: *"Pre-warming
this listener means the sequencer's connection is already established when the secondary is
promoted, so WAL reconciliation can begin without a connect delay."* On the secondary that
connection carries no data — the engine discards sequenced orders while in FOLLOWER mode — so
`InboundConnectionManager::check_for_inactive_connections()` closes it after
`socket_maximum_inactivity_interval_`, 600s. **Being idle is precisely what a standby link
does**, so the reaper defeats the pre-warming it exists to provide.

Twelve teardowns in the first 26 minutes of run 7 — two cycles of six connections, at 08:26:20
and 08:36:23, exactly 600s after start and 600s after each other. All on
`matching_engine_secondary`; none on the primary, whose connections carry orders and so never
idle out. It recurs for the whole life of the process.

The framework already has the mechanism: `register_inbound_listener()` takes an
`IdleTimeoutFlag`, and `matching_engine_publisher` and `fix_order_gateway` already pass
`BypassIdleTimeout` for their quiet infrastructure links. The matching engine's two listeners
take the `UseIdleTimeout` default instead.

**Second-order cost, and the more insidious one.** A WARNING that fires every ten minutes in a
healthy system teaches a reader to skim past `connection lost` — which is the line that matters
when a connection is genuinely lost.

### BUG-0044: Scripts cannot answer `--help` without their plotting dependencies

| | |
|---|---|
| Severity | low |
| Found | Known when `build.py`'s pylint gate was written; the date it was first noticed is not recorded |
| Recorded | 2026-08-24, after a citation sweep found `build.py` pointing at an entry that did not exist |
| How | `check_bug_list.py` compared every citation of the bug list against the entries actually in it |
| Impact | A script that cannot describe itself on a machine without matplotlib or psutil. The gate tolerates it, so nothing fails |

`scripts/build.py` runs each top-level script with `--help` as part of the pylint gate, and treats
a `ModuleNotFoundError` as a skip rather than a failure. The comment there explains why, and is
right to: the Rocky container carries no matplotlib or psutil, and failing a C++ toolchain check
because a visualisation library is absent would be the same mistake as making Prometheus a
dependency of starting the venue.

**What it tolerates is still a defect.** A script imports its plotting stack at module scope, so
`--help` cannot run without it. The fix is a lazy import at the point of use, which
`pubsub_metrics.py` already does so that it can run headless.

The gate is therefore weaker than it looks on exactly the machine that most needs it: on a host
without the optional packages, those scripts are checked for nothing at all.

### BUG-0028: Growing the order book by doubling needs more memory than the machine has

| | |
|---|---|
| Severity | high |
| Found | 2026-08-21 |
| Recorded | 2026-08-21 (875259f) |
| How | The trading-day run that proved the stall cured -- it reached a table size no earlier run survived to |
| Impact | `matching_engine_primary` OOM-killed at 19:08:41 with 10.3 GB resident; 280,901 of 13,016,000 orders lost across the outage |

With latency no longer the limit, memory is. At 18:54:17 both engines allocated 2^25-bucket
tables of 9760 MB. At 19:08:41 the kernel took the primary: `anon-rss 10340720 kB`, zero memory
available on a 31 GB machine.

**The arithmetic was always going to end here, and two things had been left out of it.**

First, **the venue runs two order books, not one.** The secondary maintains a full replica via
BookUpdate PDUs, so every figure in this entry -- and every figure in the memory reasoning that
preceded it -- doubles. Growing into a 2^25 table by doubling costs 14.6 GB transient per engine
while both tables are held: 29.2 GB for the pair.

Second, **the binary gateway reserves 4.3 GB before the first order arrives.** `[open_order_pool]`
is `initial_pools = 21` of `objects_per_pool = 1048576` at `sizeof(OpenOrderEntry)` = 168 bytes =
3.45 GB, and it is resident immediately because building the pool's free list writes a next
pointer into every slot. It reads like a leak in `htop` and is not one, but it is 4.3 GB the
engines cannot have.

**The fix is to reserve rather than to grow.** `[order_book] initial_capacity` already exists and
is set to 262144 -- about four minutes of this profile. Sized to the expected book it removes the
growth steps altogether, and it *lowers* peak memory rather than raising it:

| | peak per engine |
|---|---|
| grow into a 2^25 table by doubling | 14.6 GB transient |
| reserve 2^25 at startup | 9760 MB steady |

That is the difference between 29.2 GB for the pair and 19.5 GB, which is the difference between
being killed and not. `allocate_table()` memsets only the state array -- one byte per slot -- and
leaves entry storage to be faulted in as the book fills, so the reservation costs address space
and one allocation at startup rather than resident memory up front.

This does not make `IncrementalRehashMap` redundant. Sized right, the map never migrates; sized
wrong, it is what makes the mistake survivable instead of a second-long stall. Reservation handles
the expected case, the container handles being wrong about it.

**Not yet changed.** The number to use should come from the book size the venue is meant to
support, not from this profile.

### BUG-0029: A process death on the same host takes the machine-death path

| | |
|---|---|
| Severity | high |
| Found | 2026-08-21 |
| Recorded | 2026-08-21 (875259f) |
| How | The trading-day run that proved the order-book stall cured -- the primary was OOM-killed and the venue stopped trading for 16 seconds |
| Impact | 16 s outage against a design target of under 50 ms for this class of failure. ~280,000 orders lost across it |

`design-notes-for-ha.md` separates two recovery paths and gives them very different targets:

| | Local (process) recovery | Network (machine) failover |
|---|---|---|
| Recovery target | **under 50 ms** | 100 ms to seconds |
| Trigger | the process died; hardware and kernel are healthy | total silence from a node |

**What happened on 2026-08-21 was the first kind and was handled as the second.** The kernel
killed one process on a healthy machine and closed its sockets; the follower saw the
replication connection close within a second. It then took the outer loop:

```
19:08:41  primary OOM-killed
19:08:42  follower sees the socket close, arms a 15 s timer
19:08:57  timer fires, asks the arbiter
19:08:57  ArbitrationDecision received -- 3 ms later
19:11     back to 1926 orders/s
```

Fifteen of the sixteen seconds are `ha_timing.heartbeat_timeout_seconds`, armed at
`MatchingEngineThread.cpp:289` on connection loss. The arbitration it was waiting to perform
took 3 ms.

**The wait bought nothing.** The arbiter decides from live connection state -- `peer_connected`
at `ArbiterThread.cpp:553`, backed by a map erased on disconnect at line 102 -- not from
heartbeat recency. Its own connection to the dead primary closed at the same instant the
follower's did, so it would have returned the same decision at 19:08:42 as it did at 19:08:57.
The follower detected the death immediately, discarded that signal, and waited for a timer
sized for a node that has gone silent.

**Why the timer is not simply wrong.** It is the correct trigger for the outer loop, where the
question is whether a node that has stopped answering is dead or merely unreachable. What is
missing is the distinction: a socket closed by the kernel because the peer process no longer
exists is not silence, it is evidence. The design already draws that line and the code does
not act on it.

**What this does not depend on.** Promotion safety here rests on the arbiter, not on a race
being avoided by waiting -- see `design-notes-for-ha.md` section 10, which records that STONITH
is not implemented and that arbiter-mediated leadership plus epoch fencing stands in its place.
A follower asking sooner is refused just as surely if the peer is alive and still connected to
the arbiter, because leadership goes to the lower instance id when both are connected, and the
primary's id is always the lower one. Asking earlier changes when the answer arrives, not what
it is.

**PARKED 2026-08-21, pending a process-supervision design.** Nothing here should be changed
until that exists -- see the correction below, which reverses this entry's first recommendation.

**The 15 seconds is the local recovery grace period, and it is doing nothing only because
nothing fills it.** `design-notes-for-ha.md` section 7 gives the outer-loop trigger as "the
heartbeat timer expires and the primary fails to reconnect **after the local recovery grace
period**". That period is this timer. Its purpose is to give a locally-restarted primary time
to come back so the follower never has to promote at all. Today the venue has no supervisor --
components are started ad hoc by `scripts/devenv.py` for testing, which is not how a real
deployment would start them -- so nothing restarts a dead engine and the window is simply dead
time before a promotion.

That means **the outage is not fixed by shortening the wait; it is fixed by filling it.** How
long the grace period should be is a consequence of how fast a supervised restart is, which
cannot be answered before process supervision is designed. That design is the next piece of
work, and this entry is blocked on it.

**Possible fixes, and a correction.** These were recorded before the above was understood.

1. **WITHDRAWN: treat a peer-initiated close as evidence and arbitrate at once.** This was
   this entry's original first recommendation and it is wrong. Promoting the moment the socket
   closes pre-empts the local restart, forcing a cross-machine failover for a failure that did
   not need one -- the opposite of what the layered design intends. It would only be right for
   a venue that has decided never to recover a process in place.
2. **Still valid: give the promotion delay its own setting.** It currently borrows
   `ha_timing.heartbeat_timeout_seconds`, which is a different quantity measured for a
   different purpose. Even keeping the outer-loop behaviour, one number serving two meanings
   cannot be tuned for either.
3. **The real fix: build the inner loop the design describes.** Section 7 of `design-notes-for-ha.md` calls
   for local process recovery -- a shared-memory journal, restart in place -- with a sub-50 ms target, and
   it does not exist. Fix 1 shortens the outage to about a second by promoting the peer
   faster; this is what would meet the stated target, by not needing a promotion at all for a
   process that can simply be restarted. Much the largest piece of work of the three, and the
   only one that addresses the design gap rather than the symptom.

   Measured 2026-08-21, and it bears on how this is built: rebuilding the book by replaying
   entries costs 438 ms at 2^21, 921 ms at 2^22 and **2034 ms at 2^23** -- pre-reserved, no
   migration, no decode, no I/O, so a lower bound. A shared-memory *journal* replayed on restart
   therefore cannot reach section 8's sub-50 ms target at any realistic book size. Only the
   book itself living in shared memory, re-attached rather than rebuilt, can. That is a much
   larger change and it belongs after the supervision design, not before it.

**Not investigated:** what a restarted primary does when it rejoins after the secondary has
been promoted. `decide_and_broadcast` recomputes leadership rather than consulting
`leadership_state_`, and the reconnect path at `ArbiterThread.cpp:509` looks the stored state
up under the connecting instance's own key. Whether that yields a clean failback or a
disagreement was not traced, and is a separate question from this entry.

### BUG-0030: Restart coverage: what ha_test.py exercises, and what it does not

| | |
|---|---|
| Severity | medium |
| Kind | task -- a coverage matrix, not a defect |
| Found | 2026-08-21, extended into a full matrix 2026-08-22 |
| Recorded | 2026-08-22 (63889b8) |
| How | Reading every scenario against the restart cases an HA pair actually has |
| Impact | Three defects were found in the two cases that were covered. The uncovered ones have not been looked at |

Every scenario kills a component and leaves it dead, which models **machine** death correctly --
a dead machine does not come back on its own. It leaves **process** death, where the instance is
restarted on a machine that never failed, almost entirely unexercised. That is the half a
supervisor makes normal, and it is where every defect found on 2026-08-21 and 2026-08-22 lives.

**The cases an HA pair has, and where each stands:**

| | sequencer | arbiter | matching engine |
|---|---|---|---|
| **R1** restart the leader inside the peer's grace period; the peer must not promote | **27** | **28** | **26** |
| **R2** restart the leader *after* the peer has promoted -- must rejoin as follower | 14 | **25** | **24** |
| **R3** restart the *follower* -- must stay follower, leader untouched | **30** | **31** | **29** |
| **R4** after R2, kill the new leader -- the rejoined instance must take over | 14 | **33** | **32** |
| **R5** cold start both, in either order -- deterministic leader | **36** | **38** | **34** |
| **R6** restart with no arbiter reachable -- degraded, and said so | **37** | **39** | **35** |

Scenarios 10 to 13 restart a matching engine but run a single one, so no role is ever in
question; they test that it comes back, not what it comes back as.

**What the two covered cells cost to find.** R2 for the matching engine is scenario 24, written
2026-08-22, and it found three defects in a row: the arbiter re-running the cold-start tie-break
on a rejoin, the engine promoting itself on arbiter connect, and the sequencer routing orders by
socket rather than by role. All three are in this file. That is one cell of eighteen.

**Completed 2026-08-23.** Eighteen of eighteen cells. The last four were taken by
ha_test.py scenarios 36-39; the suite went from 23 scenarios to 39.

**Where it stood, 2026-08-22.** Fourteen of eighteen cells. Every cell taken so far found at
least one defect except R1, R3 and R4-arbiter, which passed first time -- and that pattern is
itself informative: the defects all lived in what an instance comes back *as*, so the cells where
nothing has to decide that were the ones that already worked.

**Complete, 2026-08-22.** All eighteen cells. Ten defects were found on the way, every one of
them a consequence of the same omission: process death was not in the model, so nothing decided
what a restarted instance comes back as, and channels, records and fallbacks were all built on
identities that stop being true once roles can move.

**The cells that passed first time are as informative as the ones that did not.** R1, R3 and
R4-arbiter, and both R5/R6 cells for the sequencer and arbiter, needed no fixes. The defects all
lived in what an instance comes back *as*; where nothing has to decide that, the code was already
right. And where a component already had a symmetric design -- the sequencer's `peer` channel,
its unconditional startup election timer -- it had none of the faults its matching-engine
counterpart did.

**One thing worth carrying forward.** The sequencer settles leadership with its peer directly,
through StatusQuery/StatusResponse and the instance-id rule, and needs no arbiter to do it. The
matching engine cannot: it must ask an arbiter or fall back to a unilateral rule. Whether the
engine should gain the same peer-to-peer resolution is a design question this coverage raised and
did not answer.

### BUG-0037: The resend scenario never creates a gap, so nothing tests the resend

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (5bc615c) |
| How | Reading the reconnecting client's own output while planning assertions for it |
| Impact | The FIX resend path has no member-observable test coverage, while a scenario named for it passes |

`ha_test.py` scenario 22, `resend_recovery`, is described as "A reconnecting member is sent the
execution reports it missed" and promises the member "receives real execution reports marked
PossDupFlag=Y instead of a blanket gap-fill".

It asserts neither. What it asserts is that the venue resumed the session's numbering across the
reconnect, which it does correctly and which is worth having. The reconnecting client's output
was checked after a run: **579 bytes, containing one Logon and nothing else.** Zero execution
reports, zero `PossDupFlag`, zero `SequenceReset`, zero `OrigSendingTime`. Scenario 23,
`inflight_gateway_death`, produces an identical 579-byte Logon-only output.

The cause is straightforward once seen. The scenario sends its baseline orders, waits for their
reports, and only then drops the session -- so at the moment of the drop the venue owes the
member nothing. Nothing is missed, so the member has no gap to notice, sends no ResendRequest,
and there is nothing to resend. `orders_during_override=0` and `orders_after_override=0` ensure
no traffic arrives during the window either.

**A gap has to be manufactured, and the choice of how is a design decision.** Three candidates:

- **Reconnect after the cancel-on-disconnect grace expires.** The member leaves orders resting,
  the venue cancels them and emits a report per order into a socket that is gone, and the member
  returns to a real gap of its own orders being cancelled. Uses only behaviour the venue already
  has, and is the most realistic of the three. It needs checking that the session's sequence
  state outlives the grace period -- if it does not, the member's numbering restarts and the
  test fails for an unrelated reason.
- **Stop the client with orders in flight**, so the reports land while it is away. Closest to
  scenario 23's intent, and depends on timing that will vary by machine.
- **Have the venue generate reports for a disconnected session directly**, which is the most
  controllable and the least like anything that happens.

Until one is chosen, three assertions have nothing to run against: that `PossDupFlag` is set
inside the requested gap and absent beyond it, that `OrigSendingTime` accompanies every resent
report, and that the terminating gap-fill leaves the member expecting the number the venue will
send next. All three are cheap once a gap exists, and all three read tags from the client's own
output, which is the member-observable fact rather than the venue's account of itself.

A helper for the first of them, `_client_report_counts`, already exists in `ha_test.py` and is
never called.

**The scenario should not be left describing work it does not do.** Whatever is decided about
manufacturing the gap, the description and expected outcome need to match what is asserted, or
the next person to ask "is the resend path tested?" gets the same wrong answer twice.

### BUG-0038: Inbound FIX sequence numbers are never checked, so a member's lost order is not noticed

| | |
|---|---|
| Severity | high |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | Reading the gateway's session handling against FIXT.1.1 while writing docs/fix/sequence_numbers_and_gaps.md |
| Impact | An order a member believes it placed can fail to reach the venue with neither side detecting it |

`MsgSeqNum` (tag 34) on an inbound message is never compared against an expected value. The
gateway keeps no expected-inbound counter, never detects a gap in what a member sends, and never
sends a `ResendRequest` -- the only `ResendRequest` code in the gateway is the handler for
receiving one from a member.

**What this costs.** A member sends an order; the connection drops before it arrives; the member
reconnects and continues numbering from the next value. The venue was expecting the missing
number and receives the one after it, processes it, and carries on. The order that never arrived
is not requested, not logged and not missed. The member has an order it believes is resting and
the venue has never heard of it. Noticing exactly this is the purpose of the numbering.

A message arriving with a number *lower* than expected is accepted for the same reason. FIXT.1.1
calls that a serious error, on the grounds that the far side has gone backwards and its state
can no longer be trusted.

**A related defect with the same root.** `PossDupFlag` (tag 43) is written on the outbound
resend path and never read on inbound. A member recovering a gap of its own retransmits with
`PossDupFlag=Y`, meaning "you may already have this"; the gateway treats it as a new order.
What prevents a duplicate order today is the matching engine rejecting a repeated ClOrdID
within a session -- the application layer catching a session-layer failure. The member gets a
rejection for an order that does exist, and under load the rejections arrive in bulk: a run has
been observed producing 132,000 duplicate-ClOrdID warnings from a client retransmitting orders
it had not been acknowledged.

**What is needed.** An expected-inbound counter per session, checked on every message, with the
three outcomes the specification defines: equal, process; higher, hold the message and send a
`ResendRequest`; lower, a serious error unless `PossDupFlag=Y`, in which case the message has
already been processed and should be discarded rather than forwarded. The counter has to
survive a reconnect, since the series belongs to the session and not to the connection, and it
has to survive a gateway failover for the same reason the outbound counter does.

Not a small change, and it touches the session state that HA already carries across a failover.
Worth sizing before starting.

### BUG-0039: ResendRequest ignores EndSeqNo, so every resend runs to the head of the stream

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | Reading the resend path against the FIX session-layer rules rather than against its own comments |
| Impact | None in normal operation. A member asking for a bounded range gets an unbounded replay, and the venue pays for it |

`FixOrderGatewayThread::handle_resend_request` reads `BeginSeqNo` (tag 7) and nothing else.
`EndSeqNo` (tag 16) is never read anywhere in the codebase, though the data dictionary carries
it and marks it required on ResendRequest. The gateway replays from `BeginSeqNo` to wherever
the session had reached, which is to say it treats every request as though `EndSeqNo` were 0.

**Recorded as a deviation rather than a bug**, because in the case that actually happens it is
the correct behaviour. `EndSeqNo=0` means "everything from here on" and is what an engine sends
after a disconnect, which is when resends occur. A member that asked for 100-150 and receives
100 onwards sees messages in sequence, and its inbound counter advances exactly as it would
have; nothing in the session breaks.

**Where it stops being benign is a bounded request.** A member asking for fifty messages gets
every report since, and the reports come from the sequencer's WAL rather than from a buffer in
the gateway -- so a small request from one member turns into a large slice read on the shared
path that live traffic depends on. On a session with millions of reports behind it that is a
substantial amount of work, asked for by a message the member was entitled to send.

The gap between "no member does this" and "no member can do this" is the whole of the risk. It
is unusual engines behaving reasonably that find this kind of thing, and they find it in
production.

Three ways to settle it, in increasing effort:

- Honour `EndSeqNo` when it is non-zero, which is what the specification asks for.
- Keep the current behaviour and say so deliberately in a comment, so the next reader knows it
  was considered rather than missed.
- Bound the replay regardless of what was asked for, which addresses the cost but not the
  conformance.

Whichever is chosen, `EndSeqNo` being absent from the code with no comment is the one state
that should not persist, because it cannot be told apart from an oversight.

### BUG-0040: The order-accounting check reports lost orders when it means it could not count them

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | A 113-minute trading-day run ended `FAIL -- the matching engine did not accept every order`, having just said it could not read the counter it needed |
| Impact | A clean run is reported as order loss. That is the conclusion this check exists to prevent being drawn wrongly |

`perf_run.py` finishes a run by comparing the matching engine's `orders_processed_total`
against the orders the profile offered. When the counter cannot be read it sets the count to
-1, prints `COULD NOT READ the matching engine's metrics endpoint`, and then falls into the
same comparison as any other run. -1 is not the expected total, so the run is declared a
failure with the words *the matching engine did not accept every order*.

Nothing had gone wrong with the venue. **"I could not measure this" and "this measurement came
out wrong" are different results, and only one of them is about the venue.** Reporting the
first as the second is the more damaging error, because it sends someone to look for a fault
in the system rather than in the instrument.

That is worth more here than it would be elsewhere. This check exists *because* of a false
loss report: a June run reported 166 orders missing, which was investigated at length and
turned out to be a Quill flushing artefact with every one of the million orders present. The
post-shutdown ground-truth count was the answer to that. It has now produced a false loss
report of its own, by a different route.

**The reporting is fixed. The read failure itself is not**, and the obvious causes are all
eliminated: the counter exists (`orders_processed_total`, registered in
`MatchingEngineThread.cpp`), the scrape happens before `full_shutdown()` rather than after it,
and the metrics port resolves correctly from the deployed configuration.

**A leading hypothesis, and how to test it.** `read_counter` allows the scrape **two seconds**
and turns any `URLError` or `OSError` into `None`, which the caller cannot tell apart from the
metric being absent. At the end of the trading-day run the matching engine held 8.6 million
resting orders in a table whose largest allocation was 9.5 GiB, with 8.0 GB resident. Rendering
the Prometheus exposition under that is plausibly slower than two seconds, or the endpoint's
thread simply did not get scheduled promptly.

The evidence fits: the read failed after 113 minutes and 8.6 million orders, and succeeded twice
immediately afterwards on runs of 1,000 orders. If the hypothesis holds, the failure follows the
size of the book rather than anything about the code path -- so it will reproduce at the end of
a long run and never at the end of a short one, which is the worst possible shape for noticing
it.

Two things follow if it is confirmed. The timeout is too short for a process holding several
gigabytes, and more importantly a timeout and an absent metric should not arrive at the caller
as the same value: one is "ask again", the other is "this venue does not publish that".

Two things to fix, and the first does not depend on diagnosing the second:

- Report an unreadable counter as *not verified*, distinct from both PASS and FAIL. A run whose
  accounting could not be taken is not a run that failed.
- Find out why the counter could not be read.

Related: `component_metrics_port()` calls `die()` when a deployed config still holds
unexpanded `${...}` placeholders, which is the state `build.sh` leaves `installed/etc` in until
`deploy.py` runs again. A stale deployment therefore kills a run at the accounting step, long
after the point where the cause would have been obvious.

### BUG-0041: Five ways the venue will not start on a RHEL8 target host

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-24 |
| Recorded | 2026-08-24 (95b8713) |
| How | Bringing the venue up on a RHEL8 target host -- the first run of `create_db.py` and `devenv.py` outside this machine and the Rocky container |
| Impact | Database creation fails outright; the venue starts only after hand-editing `environments/dev.toml` |

BUG-0026 found this class of thing at build and install time. These are the next
stage along: creating the database, and launching. Nothing here is a fault in the C++ -- the
binaries were fine and the venue came up once the scripts were worked around.

Two of the five are the same mistake made twice: **a tool located by where one distribution's
package manager puts it, rather than by asking the system where it is.**

**1. The Liquibase launcher is found by a hardcoded absolute path.** `_check_jdbc_driver()`
opens with `Path(os.path.realpath("/usr/bin/liquibase"))` (`db/create_db.py:149`) and derives
everything else from it. On a host where Liquibase is installed anywhere else -- under `/opt`,
from the vendor tarball, from an RPM that does not drop a `/usr/bin` symlink -- that resolves
to a path that does not exist, and the rest of the function reasons from it anyway.
`shutil.which("liquibase")` is what this wants, and it is the same question the script asks
again in point 3 by a different means.

**2. The JDBC driver is sought in the wrong directory of the Liquibase tree.** Line 150 takes
`liquibase_real.parent / "lib"`. Liquibase 4.x and later ship their bundled drivers under
`<root>/internal/lib`, so the glob finds nothing even when the driver is present and correct.
The search should start from the Liquibase root and accept either layout rather than assuming
one.

The fallback makes this harder to diagnose rather than easier. When the glob comes up empty the
function copies the JAR bundled in `db/drivers/` to the same place, and calls
`liquibase_lib.mkdir(parents=True, exist_ok=True)` first (line 163). On a system install that
needs root; on a wrong path it quietly creates a plausible-looking empty `lib` directory beside
the real `internal/lib`. **A repair path that acts on an unchecked guess leaves the host in a
state that looks deliberate.** The error text it prints when even that fails names `apt` and
`libpostgresql-jdbc-java`, which is the same single-distribution assumption stated out loud.

**3. `liquibase` as a bare command is not found.** `liquibase_cmd` is `["liquibase", ...]`
(line 294), and running it reports command not found. **Not diagnosed**, and it disagrees with
point 1 in a way that is itself the clue: the script hardcodes `/usr/bin/liquibase` *and*
invokes the bare name, and both fail on the same host. So Liquibase is neither at `/usr/bin`
nor on the `PATH` the script sees -- consistent with an `/opt` install reachable through an
interactive login shell, by alias or a `PATH` set in a profile script, but not through
`subprocess.run` without a shell.

To settle it on the host, run these side by side:

```bash
command -v liquibase
python3 -c 'import shutil; print(shutil.which("liquibase"))'
```

Fixing point 1 with `shutil.which` addresses this one too, and turns it into a clear message:
`which()` returning `None` is the same absence, and the script can say so at the point it looks
instead of failing several steps later on a path it invented.

**4. The install tree is named in the environment file, and the build already disagrees with
it.** `environments/dev.toml:22` sets `install_dir = "installed"`, and `devenv.py` resolves
every binary, config and library path off it. But `build.py:58` and `devsetup.py:40` stage a
gcc-8.5 Rocky/RHEL8 build to `installed-rocky8/` **precisely so it cannot overwrite the host's
`installed/`**. The run side therefore looks in the one tree the build deliberately avoided,
and `authentication_service` is reported missing because it is genuinely not there.

**The fix is a choice, not a lookup**, and the axes are the reason. `environments/*.toml` varies
by deployment environment -- dev, preprod, prod, test-1. The install tree varies by target
platform. An `rhel8.toml` is a dev environment that happens to be RHEL8, and the cross product
arrives the day there is a preprod on one. Three ways to settle it:

- An `--install-dir` override on `devenv.py`, matching the `--db-port` precedent -- cheapest,
  and it keeps the two axes apart.
- Default `install_dir` from the detected platform, so the run side derives what the build side
  already decides.
- A per-platform environment file, accepting the cross product when it comes.

Related: *Environment placeholders are missing outside dev*, which is the same file family seen
from the other side.

**5. `devenv.py --db-port` exists, and is invisible from where it is needed.** It is a global
option, so it must precede the subcommand:

```bash
python3 scripts/devenv.py --db-port 5433 start     # works
python3 scripts/devenv.py start --db-port 5433     # unrecognized arguments
```

`devenv.py start --help` lists only `[-h] [name]`. Its own help text says the flag matches
`deploy.py`'s of the same name so that a host with a non-default cluster "names it the same way
at deploy time and at start time" -- and then does not appear at the point of use. The cost is
not the typing: the environment file was edited instead, which is a change that then has to be
un-picked, and which is how a host ends up running a profile against configuration nobody meant
to keep.

**Not a defect: Prometheus did not start.** `devenv.py:239` skips any component whose command is
absent from `PATH` and prints that it is doing so. The comment above it gives the reason --
refusing to start a trading system because a monitoring tool is absent gets the priority
backwards. Recorded here because it appears alongside the five as a sixth failure and is not
one.

---

## Closed

### BUG-0007: Metrics silently disabled when CPU pinning is off

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-04 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Reading the configuration loaders while adding the Prometheus metrics |
| Fixed | 2026-08-08 |

`config.metrics_configuration = MetricsConfigurationLoader::load(toml)` sat **inside** the
`if (config.cpu_pinning_enabled)` block in the matching engine and both gateway loaders. A
component with pinning turned off exposed no metrics at all — no error, no warning, just an
endpoint that never appeared.

Left alone when first found because nothing depended on it. It became urgent when the test
harnesses moved their ground truth onto those counters, at which point a silent disable would make
them pass while verifying nothing. **This is exactly the defect that motivated this file.**

### BUG-0008: A growing hash map stalls the reactor callback thread for over a second

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | The first clean compressed-trading-day load run — the reactor's own watchdog logged it, and the profile confirmed the cause |
| Impact | p99 of 733 ms at 4.2M orders, over 1 s at 8.4M, after which the pipeline did not recover and 1,167,392 of 9,556,000 orders were never accepted |
| Fixed | 2026-08-21 (68ab2b5) |

The matching engine's order book is a `tsl::robin_map` with `power_of_two_growth_policy<2ul>`. It
doubles, and each doubling rehashes the whole table **on the callback thread**. Stalls land only on
exact powers of two — all five stalls of 200 ms or more did, and none occurred elsewhere.

**This is a framework requirement, not an application bug.** Calling `reserve()` in the matching
engine would remove the symptom from a stub and teach nothing. `pubsub_itc_fw` offers slab and pool
allocators for messages *in flight* and nothing at all for long-lived hot-path state that *grows* —
so every application that keeps state (an order book, a session table, a subscription registry)
will grow a container on a reactor callback thread and eventually stall it.

See [Compressed Trading Day Load Profile](operations/trading_day_load.md).

**The framework half is done, 2026-08-11.** `IncrementalRehashMap` is the growable structure the
design note asked for: when the table must grow it allocates a second one and moves the entries
eight slots per mutating operation, searching both while the move is in flight. The worst case for
any one operation is a probe plus eight moves, whatever the size of the map -- where the old cost
was O(capacity) in the operation that crossed the threshold, and the capacity is what kept
doubling. Rehashing on a background thread was the alternative and was rejected: it puts two
threads on one table, which means a lock on the hot path or a lock-free open-addressed table with
tombstones. Migrating a few slots at a time keeps the structure thread-confined, as the container
it replaces already was -- and confinement is now checked rather than assumed, under
`PUBSUB_ITC_FW_THREAD_CHECKS`.

What remains O(capacity) is clearing the new table's state array: one byte per slot, a memset of
about 8 MB at the size that stalled for over a second, which measures under a millisecond. That is
recorded in the header rather than glossed over.

47 unit tests, counting the ones added for `GrowthReportingAllocator` alongside it: differential
testing against `std::unordered_map` over three seeds and three workload shapes, the migration
boundaries driven one step at a time through a template parameter, adversarial hashes that put
every key in one probe chain, lifetime accounting proving every construction is matched by a
destruction, and -- the property the class exists for -- a bound on entries moved per operation
that is **counted rather than timed**, so a loaded machine cannot make it flaky and a return to
one-pass rehashing fails it immediately. Clean under ASan, and under the C++23 dialect.

**Cured, and measured — see "The measurement that closes this entry" below.** The order book is
on `IncrementalRehashMap`, and a trading-day run on 2026-08-21 shows peak p99 flat at 1.78–4.27 ms
across six growth steps from 305 MB to 9760 MB, against 96 ms, 733 ms and over a second for the
same steps under `tsl::robin_map`.

**Committed, so this entry moves to Fixed.** It previously read: this entry stays under Open
only because the change is uncommitted. It moves to Fixed when the
order-book swap lands. Nothing else is outstanding on it.

What the container does *not* do is reduce peak memory — it holds two tables for *longer*, across
the whole migration rather than one operation. That is now the binding constraint rather than
latency, and it has its own entry: BUG-0028.

#### The 2026-08-16 run measured the old book, not the swap

A trading-day run was made on 2026-08-16 intending to measure the order book on
`IncrementalRehashMap`. It did not: the binary it exercised was the `tsl::robin_map` build
installed on 2026-08-11. The swap was compiled into `build/` at 16:59 that day and never
deployed, and `perf_run.py` takes an install prefix and runs what is already there -- it neither
builds nor deploys. So the run is a second `tsl::robin_map` measurement, and says nothing about
the container it was meant to test.

Three independent confirmations, recorded so this is not re-argued:

1. **Symbols.** `installed/bin/matching_engine`, dated 2026-08-11 20:29, carries 13 `robin_map`
   symbols and no `IncrementalRehashMap` symbol. `build/applications/matching_engine/matching_engine`,
   dated 2026-08-16 16:59, is the reverse: no `robin_map`, 7 `IncrementalRehashMap`.
2. **The log line number.** Every growth report in `matching_engine_primary.log` names
   `MatchingEngineThread.cpp:162`. That is where the `PUBSUB_LOG` sits in the committed source;
   the swap adds comment lines above it and moves it to 164.
3. **Bytes per slot.** The reports are 156, 312, 624, 1248, 2496 and 4992 MB, and 4992 MiB over
   2^24 buckets is exactly 312 bytes each. `sizeof(std::pair<OrderKey, OrderEntry>)` is 304
   (`OrderKey` 134, `OrderEntry` 168), and robin_map adds an 8-byte distance-and-hash word per
   bucket: 312. `IncrementalRehashMap` reports `state_bytes + entry_bytes`, one extra byte per
   slot, which would have logged 5008 MB rather than 4992.

**Correcting a figure this entry carried.** The table did not cost 624 bytes per slot. It cost
**312 bytes per bucket**, and the six growth reports correspond to bucket arrays of 2^19 through
2^24 -- reached, at robin_map's half-full growth policy, somewhere around 2^18 to 2^23 live
orders. The earlier "2^21 / 2^22 / 2^23" labels were counting entries rather than buckets, which
is consistent, but the per-slot figure was double the truth and any sizing argument built on it
was wrong by a factor of two.

**What the run does establish**, all of it about `tsl::robin_map`:

| allocation | buckets | growth step at | p99 then | first run, 2026-08-08 |
|---|---|---|---|---|
| 1248 MB | 2^22 | 18:38:32 | **84.94 ms** | 96 ms |
| 2496 MB | 2^23 | 18:48:37 | **378.32 ms** | 733 ms |
| 4992 MB | 2^24 | 19:08:47 | **997.20 ms** | over 1 s |

Median p99 in each window was ~0.9 ms, so the tail-excursion signature reproduces: the bulk of
orders are untouched and a few are delayed catastrophically. The spread against the first run --
733 ms against 378 ms at the same step, on the same container -- is run-to-run variation, and is
itself worth knowing, because it sets how large a difference the re-run must show before it means
anything.

**The profile is off-CPU.** `perf record` on `matching_engine_primary` across all six growth steps
gives a flat profile for the reactor thread: the top symbol is `handle_new_order_single` at 0.66%,
and no allocation or rehash symbol appears near the top. A one-second *CPU* stall would be
impossible to miss at that sampling rate, so the thread is blocked rather than spinning, and
cycle-based sampling cannot attribute it. This holds for the robin_map book and is the most useful
thing the run produced.

**The allocation hypothesis is untested, not supported.** The idea that the stall is the single
large `operator new` for the entry array was framed around `allocate_table`, which is
`IncrementalRehashMap`'s function and was not in the binary. robin_map's own bucket-array
reallocation is the same shape and the off-CPU evidence is compatible with it, but nothing here
distinguishes it from any other blocking cause. If the re-run with the swap deployed still shows
spikes that grow with table size, timing the two `operator new` calls in `allocate_table`
directly -- logged at Warning, so it survives the load run's log level -- settles it cheaply.

**Status: superseded.** The re-run was made on 2026-08-21, deployed first and with the documented
`--clients 4`. See the next section.

#### The measurement that closes this entry, 2026-08-21

Trading-day run with the order book on `IncrementalRehashMap`, deployed first this time, and with
the documented `--gateway binary --clients 4`. Prometheus started on its own beforehand and load
confirmed reaching it within 20 seconds. p99 is the worst 15-second sample in a window of -90s to
+240s around each growth step, taken from `order_round_trip_nanoseconds_bucket` on
`binary_order_gateway_a`; step times come from the growth-report lines in
`matching_engine_primary.log`.

| buckets | table | peak p99 | median p99 | robin_map 2026-08-16 | first run 2026-08-08 |
|---|---|---|---|---|---|
| 2^20 | 305 MB | 2.50 ms | 1.25 ms | -- | -- |
| 2^21 | 610 MB | 1.78 ms | 0.99 ms | -- | -- |
| 2^22 | 1220 MB | **2.43 ms** | 0.99 ms | 84.94 ms | 96 ms |
| 2^23 | 2440 MB | **4.27 ms** | 2.34 ms | 378.32 ms | 733 ms |
| 2^24 | 4880 MB | **2.44 ms** | 0.98 ms | 997.20 ms | over 1 s, pipeline collapsed |
| 2^25 | 9760 MB | **2.43 ms** | 0.99 ms | never reached | never reached |

**Flat across a 32-fold range of table size.** That is the property the container exists for, and
it is the part that could not be argued from unit tests. The failure signature was a tail
excursion -- median p99 around 0.9 ms while the peak went to 85 ms and then to a second -- and the
median is unchanged at 0.98-0.99 ms while the peak now sits beside it. The excursion is gone
rather than reduced. The 4.27 ms at 2^23 is not the start of a trend: the next doubling came back
to 2.44 ms.

Delivery was 12,735,099 of 13,016,000 orders offered, a 2.16% shortfall entirely inside the OOM
window described in BUG-0028. The first run lost 12.22% to the stall itself.

**The profile shows the migration doing its work.** `step_migration()` at **0.44%** of the reactor
thread, `find()` at 0.52%, `insert_entry()` at 0.41%. Compare the 2026-08-16 profile, where the
whole thread was flat at 0.66% top symbol because it was blocked rather than working.

#### The allocation hypothesis was wrong

Recorded because it was the leading theory for a fortnight and the refutation is one comparison:

| | allocation in one `operator new` | stall |
|---|---|---|
| `tsl::robin_map` at its worst step | 4992 MB | ~1 s |
| `IncrementalRehashMap` at the same step | 4880 MB | 2.44 ms |

Same machine, same profile, near-identical request size. `operator new` reads 0.02% in the profile
and `memset` 0.00%, and `node_pressure_memory_stalled_seconds_total` was 0.000 at every step but
the last, where it reached 0.001 with the machine nearly full. The cost was always the rehash.

Two things follow. The `allocate_table` timing experiment proposed above is not needed. And the
reasoning that the table's bytes-per-slot was the thing to attack was chasing the wrong quantity --
the size of the allocation was never what stalled the thread.

### BUG-0012: Orphaned build directories break coverage capture

| | |
|---|---|
| Severity | low |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Every incremental coverage build was failing; the error named standard-library headers and pointed nowhere near the cause |
| Fixed | `10f4578` |

`applications/binary_gateway/` was renamed on 2026-08-01 and CMake never removed the old target
tree. gcovr searches the whole build tree *before* the report-level excludes apply, found `.gcno`
files with no `.gcda`, could not resolve their compilation directory, searched upward and aborted
at `/`.

### BUG-0013: Trading-day phases reused ClOrdIDs

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | The first trading-day run; `perf_run.py`'s own loss accounting reported the mismatch |
| Fixed | `d47112e` |

The phase scheduler restarts the load client per phase and every invocation numbered its orders
from 1, so 5,256,000 of 8,632,000 orders were rejected as duplicates and four of seven phases
recorded no latency at all. Quiet in the worst way: rejected orders still travel through the
gateway and sequencer, so the load looked real while measuring nothing.

### BUG-0019: A FIX logon arriving before the gateway's sequencer links are up is delayed five seconds

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (b51f84c) |
| How | `release_check.py`'s ha stage: scenario 23 (`inflight_gateway_death`) failed with "FIX logon timed out after 3s" |
| Fixed | 2026-08-10 -- `on_connection_established()` re-announces every session still awaiting its numbering down the link that has just come up |

The client authenticated successfully and was left hanging anyway. From the logs of one run:

- `23:01:57.324` the gateway receives the authentication request
- `23:01:57.326` the authentication service answers `Granted`, and the gateway announces the
  session and arms cancel-on-disconnect
- `23:01:57.326` two warnings, together: *primary sequencer not connected -- PDU not sent*, and
  the same for the secondary
- `23:01:57.367` both sequencer connections come up, 40ms later
- `23:02:00` the test gives up and tears the venue down

`complete_session_establishment()` sends the Logon reply only once the session's sequence state
is settled, which the gateway learns by asking the sequencer. That request is a PDU, and at that
instant there was no sequencer to send it to, so it was dropped with a warning and nothing
retried it when the links came up 40ms later.

**The session is not lost.** `sequence_state_timeout` is 5 seconds; when it fires the gateway
warns and calls `complete_session_establishment()` regardless, so the member gets its Logon after
five seconds instead of two milliseconds. `ha_test.py` waits three (`FIX8_LOGON_WAIT = 3.0`) and
gives up two seconds before the gateway would have recovered — which is the whole reason the
scenario reports a failure.

Confirmed by experiment: raising that constant to 9.0 makes scenario 23 pass. The change was
reverted, not kept — 3s versus a 5s fallback is a real defect in the test, but making the test
wait longer would only make it bless the degraded path.

Two separate things needed deciding, and they were the reason the entry stayed open:

- **The test cannot pass when this path is taken.** Its budget is shorter than the recovery it is
  waiting for, so the outcome is decided before the venue has finished trying.
- **The recovery path is degraded, by its own account.** The warning it emits reads: *"opening
  the session at {} anyway; a member expecting a higher number will see a low sequence and
  disconnect."* So the fallback is not a fix — it opens a session with numbering the sequencer
  never confirmed, and the code itself says that can cost the member its connection. The window
  wants closing at the source: retry the sequence-state request when a sequencer connection is
  established, or refuse logons until the gateway can service them.

The gateway had already announced the session and armed cancel-on-disconnect before discovering
it could not finish, so its own view of the session ran ahead of the member's.

Same family as the venue accepting orders with no matching engine and telling nobody: a PDU
dropped on an absent link, a warning in a log nobody is reading, and no retry.

**Where the delay comes from.** The gateway's outbound sequencer connections are retried on a
cadence — `ReactorConfiguration::connect_retry_interval_`, default 2 seconds — and its first
attempts fail because the sequencer is not accepting until leader election completes. From the
run at 23:07:

| time | event |
|---|---|
| `23:07:37.481` | gateway begins dialling both sequencers |
| `23:07:40.0`, `23:07:40.9` | sequencer execution-report *inbound* connections arrive (sequencer → gateway) |
| `23:07:42.384` | logon → *primary sequencer not connected -- PDU not sent* |
| `23:07:42.452` | primary and secondary sequencer connections established, **68ms later** |

Just under five seconds from first dial to success: the initial attempt plus two retries. The
client logged on 68ms before the last one landed.

So the gateway holds its FIX listener open while its upstream link is down, and a member that
logs on during a retry gap has its sequence-state request dropped. That the execution-report inbound
connections were already up at 23:07:40 makes it worse — the gateway looks connected to a
sequencer, in the direction that does not serve this request.

Not yet understood: it passed twice earlier the same evening under `--scenario all`, then failed
under `--scenario all` and three times standalone. A clean rebuild happened in between, but the
deployed configs were checked and contain no unexpanded placeholders, so that is not the
difference. The window is a race, so what matters is that it exists at all; but what moved the
timing enough to change the outcome consistently is not established.

**The fix, 2026-08-10.** `retry_pending_session_binds()` is called from
`on_connection_established()` for each sequencer link as it comes up, and re-sends `SessionBound`
for every session whose `awaiting_sequence_state` is still true. The first of the two options
above; refusing logons was not taken, because holding the FIX listener open and serving the
session a moment later costs the member nothing, whereas a rejection costs it a reconnect.

It re-sends down the one link that has just come up, deliberately, rather than to both
sequencers. A `SessionBound` arriving at a leader that already holds the binding is read as the
previous session having died, and the resume figure is then raised by `ers_since_report` plus the
admin allowance — right for a real failover, wrong for a retry. A link that already took the
announcement must therefore not be sent it twice.

Proved by A/B against a deterministic reproduction (the venue started with no sequencer at all, a
member logged on into the gap, the sequencers started 1.5s later), same venue and same script,
only the binary differing:

| | pre-fix binary | with the fix |
|---|---|---|
| re-announce on connect | never | 14µs after the link came up |
| 5s degraded fallback | ran — *"opening the session at 1 anyway"* | never ran |
| session established | ~5s after logon, on numbering the venue never confirmed | 756µs after the link came up |

Scenarios 1, 22 and 23 pass, as do the 30 `fix_order_gateway_tests`. Scenario 1 is the one worth
naming: it fails a sequencer over, so a link comes back up long after the sessions on it were
settled, and the retry correctly finds nothing to do.

**The test's budget, fixed the same day.** `FIX8_LOGON_WAIT` was 3.0 against a 5s fallback, so a
logon that took the fallback was failed two seconds before the venue had finished trying — and
reported as a timeout, which points at the gateway or the auth service rather than at what
actually happened.

Raising the constant was rejected on 2026-08-09 for a good reason: on its own it only teaches the
test to bless the degraded path. So both halves were done together.

- `FIX8_LOGON_WAIT` is now 8.0, and is documented as an upper bound rather than an expectation.
  It costs a passing run nothing — `wait_for_fix_logon()` returns the moment it sees an outcome,
  so scenario 23 still completes in about 31 seconds.
- `wait_for_fix_logon()` gained a fourth outcome, `degraded`, set when
  `_GW_SEQ_STATE_FALLBACK` appears before the session is established. The scenario now dies
  naming the fallback instead of passing quietly.

That last part is the point of the change. Before it, a session opened on unconfirmed numbering
was indistinguishable from a healthy one at the instant of logon: the run looked clean, and the
two diverged only later, when the member saw a sequence below the one it expected and dropped the
session. The test could not have caught the original bug even with a longer wait; now the longer
wait is safe, because taking the fallback is itself the failure.

The gateway warning it keys on carries a `TEST CONTRACT` comment, like the other lines `ha_test.py`
matches.

Detection proved against synthetic logs rather than a real venue, the degraded path not being
something that can be produced on demand: a clean logon reads `ok`; the fallback warning followed
by establishment reads `degraded`, including when the two land in separate reads of a log still
being written; and a previous session's fallback sitting before `from_byte` is correctly ignored.
Both marker strings were checked to appear verbatim in `FixOrderGatewayThread.cpp`, so a reworded
log line fails the check rather than silently ceasing to match.

### BUG-0020: The Rocky container deployed its gcc-8.5 binaries over the host's install tree

| | |
|---|---|
| Severity | high |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (5ab42ad) |
| How | A full `release_check.py` run in which `ha` scored 0/23 and `perf` died, immediately after the `rocky` stage passed for the first time |
| Fixed | 2026-08-09 |

`devsetup.py` runs build, release, stop and deploy. The first three qualify their directory by
target platform, so a Rocky build stages to `installed-rocky8/` and cannot overwrite the host's
gcc-13 tree. The fourth did not: `deploy.py` takes its destination from the env TOML, where
`install_dir = "installed"` is unqualified — and correct, because a real target host deploys to
that name whatever compiler built the artefact. Right for `deploy.py` alone, wrong as the last
step of a sequence whose other three had agreed on a different directory.

The Rocky stage bind-mounts the repository, so the container's deploy wrote gcc-8.5 binaries
into the host's `installed/`. They carry `RPATH=/opt/deps/...`, a path that exists only inside
the container, so every one of them failed to start with exit 127.

Two things hid it. The stage that caused the damage **passed** — it did its own job correctly
and corrupted a tree it was not testing — and the failures appeared in `ha` and `perf`, which
had passed an hour earlier and contain nothing to do with containers. The reason it had never
been seen is that the Rocky stage had never before run to completion: it had been failing early
on a permissions error, and the corrupting step came after that.

`devsetup.py` now resolves the staging directory once, from `build.platform_tag()`, and passes
it to `deploy.py` as `--install-dir`. On the development host it resolves to `installed`, so
nothing there changes.

The same fault had a second half, fixed the same day at the user's instruction. The release
artefact name carried **no platform tag**, so the container wrote a tarball into the bind-mounted
`release/` that could not be told from a host build — and both `devsetup.py` and
`build-release-deploy.py` chose "the newest tarball", which after a release check is the
container's. `release.py` now appends the platform tag, and both selectors filter by it rather
than by date. `build-release-deploy.py` was worse than its sibling: it hardcoded `installed` and
`rmtree`s it before building, so run in the container it would have deleted the host's tree
before replacing it.

The name is unchanged on the development host, whose tag is empty; only a cross-compiled
artefact is qualified, exactly as the staging directory works.

The third part, also fixed the same day: nothing verified that what a stage was about to test
could start. `release_check.py` gained a `runnable` stage, between `deploy` and `ha`, which runs
`ldd` over every installed binary with the same library path `devenv.py` launches components
under, and fails with the offending names and the `readelf -d` command to confirm the cause. It
takes 0.2s and guards `perf` as well, since that follows `ha`.

Checked in both directions before being trusted: it passes on the repaired tree, and pointed at
`installed-rocky8/` — the actual gcc-8.5 binaries that caused this entry, unmodified — it fails
20 of 23. That is what makes it worth having: `ha` answered the same question by reporting 0 of
23 scenarios failed, which reads as a catastrophic regression in the high-availability code and
was nothing of the kind.

### BUG-0021: `pubsub_metrics.py` built query labels from a module global, and fell back to a table describing the wrong venue

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (126df5d) |
| How | Fixing `--application`, which had the same shape and was silently inert; the user then asked why a static table existed at all when the tool does discovery |
| Fixed | 2026-08-09 |

Two faults with one cause — a value decided at import rather than passed to the code that
uses it.

**The global.** `APPLICATION` was a module-level name that `main()` reassigned, and the label
builders read it rather than being given it. That is exactly what made `--application` inert,
and it would have misled again the moment anything read the global before `main()` ran — a
module imported for its functions rather than executed, which is how a test would use it. The
application is now a parameter throughout and the global is gone.

**The fallback was worse.** When discovery found no series, the tool fell back to a built-in
table describing *this* venue and listed it as though it belonged to the application asked
for:

```
$ pubsub_metrics.py --application nosuchapp --list
note: ... returned no series for application=nosuchapp; using the static table
components (static table):
  binary_order_gateway_a
  fix_order_gateway_a
  ...
```

Those components do not exist for `nosuchapp`. The same would happen pointing the tool at any
other application: a confident, specific, wrong answer in the tool's own voice — and it bought
nothing, because if Prometheus cannot be reached for discovery the queries that follow cannot
reach it either.

Discovery is now the only source for anything that will be queried, and failure is an error
naming what was asked for. The built-in table survives solely for `--demo`, which synthesises
data and never queries, and is now named `build_demo_component_config()` so its one purpose is
visible at the call site.

### BUG-0022: `--application` never reached metric discovery

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (63bddac) |
| How | The user challenged a claim that the metric registry was pubsub-specific; checking it showed discovery was correct and the flag feeding it was not |
| Fixed | 2026-08-09 |

`discover_component_config(prom_url, application=APPLICATION)` bound its default **when the
module was imported**, so it was always `"pubsub"`. `--application` set the global, was
reported faithfully in every message, and never reached the query. Passing
`--application nosuchapp` discovered pubsub's components and listed them.

The failure was invisible in the one place someone would look: the "returned no series for
application=X" message read the global and named the application the caller asked for, while
the query had used a different one.

Fixed by resolving the application when the function runs rather than when it is defined, and
threading it explicitly from `main()` through `resolve_component_config()` and
`_add_comparison_views()` instead of letting them read a global that `--application` mutates
after import.

**Also narrowed the fallback's `except Exception`.** It reported a `NameError` introduced while
making this very fix as "could not discover from http://localhost:9090" — a defect in the
discovery code wearing a connection failure's clothes. It now catches `RequestException`,
`ValueError` and `KeyError`, so an unreachable Prometheus or a malformed response still falls
back to the static table while a programming error surfaces as itself.

### BUG-0023: The band chart drew a flat line across periods with no data

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (5975a90) |
| How | First run of `--metrics bands` against a live Prometheus, over a window spanning two load runs and the three idle hours between them |
| Fixed | 2026-08-09 |

Those three idle hours — when no venue existed at all — were drawn as a steady p99 at about
1 ms: the most reassuring part of the chart, describing the interval with no data behind it.

**The cause was not interpolation between known points.** Prometheus omits empty regions from a
range query rather than returning them as null, so the percentile query returned *no series at
all* across the gap. The fetched timestamps jumped straight from 11:48 to 14:43, and those two
points are adjacent samples as far as the plot is concerned — the line between them is what
`step()` draws between any two neighbours. The renderer already broke lines on `None`; there
were simply no `None` values to break on.

Fixed by `align_to_step_grid()`, which places every fetched series on the full step grid of the
requested window and marks absent steps `None`. The rule is then exact — a step with no sample
is a hole — rather than a heuristic about which gaps look suspicious. The existing `None`
handling in `draw_latency_bands()` does the rest.

It also fixes a latent misalignment: the percentile tracks, the breach counts and the
observation counts are fetched by separate queries and indexed as parallel lists, so a point
present in one and missing from another shifted a track against its own breach bars. All three
now share one grid.

**`--from` / `--to` added at the same time**, because `--since MINUTES` could only frame a run by
arithmetic against the current time, and the breach total then described everything in view
rather than one run. Both accept `HH:MM` for today, `YYYY-MM-DD HH:MM`, a date alone, or an
epoch second, in local time — the axis is local and so is the reader. Scoped to run 8 alone the
count reads 831,554 orders over the 2.5 ms ceiling, which is a statement about a run.

### BUG-0024: The slab allocator had a hard message ceiling, below the performance target

| | |
|---|---|
| Severity | high |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Writing a drain-loop test that forced continuous slab rotation; it failed on a limit nobody was looking for, and a follow-up measurement quantified it |
| Fixed | 2026-08-09 |

`ExpandableSlabAllocator` issued registry slots monotonically and never reused them. The slot
indexes a fixed two-level directory — fixed because deallocating threads read it without a lock
and it must never reallocate under them — so `1024 pages × 256 slots` was a hard ceiling on slab
**rotations for the lifetime of the process**, and therefore on the bytes it could ever receive:
**16 GiB at the then-default 64 KB slab.** Run 8 put ~14.3M messages through one gateway in 113
minutes, close enough that the next failure could have been `allocate()` throwing.

Consumption was throughput, not concurrency: `allocate()` resets the current slab in place only
when it is completely idle at that instant, and under sustained load a PDU is always in flight,
so every slab-full cost a slot.

**Fixed by a generation-tagged handle.** `SlabHandle` packs the slot in the low 32 bits and a
generation in the high 32. Slots are recycled through an **intrusive** free list threaded via the
slots themselves — no allocation, and no synchronisation, because both ends are reactor-only.
Releasing a slot bumps its generation, so a handle outliving its slab is rejected rather than
freeing into the slab that now owns the slot.

Measured, at a 64 KB slab and 1 KB PDUs:

| in-flight depth | slots for 200,000 messages, before | after |
|---|---|---|
| 1 | 3,125 | **3** |
| 8 | 3,125 | **3** |
| 64 | 3,125 | **3** |
| 256 | 3,125 | **6** |

Consumption now scales with in-flight depth rather than throughput, and
`SlabSlotsAreRecycledSoConsumptionIsBoundedByInFlightDepth` pins the property that matters:
**100,000 messages and 1,000,000 messages both cost 3 slots.** The registry now bounds how many
slabs may be live at once, not how many a process may handle.

`SlabHandle` is an `enum class`, not a `using` alias, and that was not cosmetic: as an alias it
converted implicitly to `int`, and the first cut of this change compiled cleanly with the
generation being silently truncated at four stages between `PduParser` and `deallocate()` — under
`-Wall -Wextra -pedantic -Werror`. It would have indexed the right slot and then been rejected as
stale the moment a slot was recycled. A distinct type makes each of those a compile error.

**Also raised `inbound_slab_size` from 64 KB to 256 KB** (`ReactorConfiguration`), which was the
interim mitigation and remains a better default: a slab stays mapped while any one of its chunks
is outstanding, so worst-case retention rises with slab size, and 256 KB keeps that four times
lower than 1 MB.

### BUG-0025: Reactor queue pool sizes were not configurable in any environment

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | `mep_primary.log` filled with `MepCommandPool exhausted: chaining new pool slab (1024 objects)` during a load run |
| Fixed | 2026-08-09 — awaiting a `cmake --install` + `deploy.py` to verify, which cannot run until the trading-day run in flight finishes |

Every application template carried its `[event_queue_pool]` and `[command_queue_pool]` sizes as
**literals**, so no environment could tune them. Only `open_order_pool` was ever templated. The
publisher was the component that made this visible because it was left at the framework default
of 1024 while its peers had been raised by hand — the sequencer to 81,920/1,500,000, the FIX
gateway to 80,000/1,000,000 — which is exactly the drift that having no single place to set them
produces.

All sixteen templates now take `${placeholder}` values seeded from what they held, so the change
is behaviour-neutral: 60 values unchanged, verified by comparing each placeholder's value against
the literal it replaced in `git HEAD`.

**Four values changed deliberately**, both publisher instances:

| | was | now | matched to |
|---|---|---|---|
| `event_queue_pool.objects_per_slab` | 1,024 | 81,920 | the sequencer that feeds it |
| `command_queue_pool.objects_per_slab` | 1,024 | 500,000 | the matching engine that feeds it |

**Still open, and worth a look when someone is next in there:** `binary_order_gateway_*` has a
command pool of 4,096 where `fix_order_gateway_*` has 1,000,000. The two gateways do the same job
over different protocols, so one of those two numbers is wrong, and 4,096 is the suspicious one.

### BUG-0026: Five ways 0.3.0 would not build or run on an RHEL8 target host

| | |
|---|---|
| Severity | high |
| Found | 2026-08-11 |
| Recorded | 2026-08-11 (9aafd07) |
| How | Installing the 0.3.0 release tarball on an RHEL8 target host -- the first time a published artefact was built anywhere other than this machine or the Rocky container |
| Fixed | 2026-08-11 |

Every one of them is a property of that host rather than of the source, which is why the
`rocky` stage of `release_check.py` passed and none of them showed up before. That stage
reproduces the **compiler** -- gcc 8.5 -- and nothing else. It does not reproduce the host's
Python toolchain, its filesystem, its third-party tree layout, its database, or the condition
of having started from a release tarball rather than a git checkout. `release_check.py`'s own
docstring says as much for the Python half, having been written after 0.2.0 failed the same
way; it said it about pylint versions and pytest, and it was right both times.

**1. The pylint gate failed on a message it had not asked for.** The errors-only invocation
(`--disable=all --enable=E`) reported `R0022 useless-option-value` for two inline disables in
`sca.py` naming `bad-whitespace`, a check pylint removed years ago. A message filter cannot
suppress a complaint about the filter itself, so the gate stopped a build over two stale words
in a comment. Fixed at both ends: the stale disables are gone, and `run_command` grew
`tolerated_exit_bits` so a gate that asked for errors is no longer failed by pylint's
warning, refactor and convention exit bits. pylint's exit status is a bitmask, not a verdict.

The versions are the mechanism: this machine has pylint 3.0.3 on Python 3.12, the Rocky image
installs whatever `pip3 install pylint` resolved to at image build time on Python 3.8, and the
work host has a newer one again. Three machines, three linters.

**2. Twenty-two DSL tests failed on NFS, having passed.** `compile_and_load()` builds a `.so`,
dlopens it, and lets `TemporaryDirectory` clean up -- unlinking a file that is still mapped.
On a local filesystem the inode outlives the name and nothing notices. On NFS the server
silly-renames it to `.nfsXXXX` in the same directory, so the following `rmdir` fails with
ENOTEMPTY and the error surfaces from the `with` block **after every assertion has passed**.
The scratch build sits inside the project tree because RHEL8 mounts `/tmp` noexec; on such a host
the project tree is itself the NFS one. Cleanup is now allowed to fail without failing a test, and
leftovers are reaped on the next run.

**3. prometheus-cpp was not found.** The work third-party tree holds it directly under
`THIRDPARTY_DIR` and names the directory after the metric system rather than after the C++
client, so the single prefix `CMakeLists.txt` offered did not exist there. Both prefixes are
now offered, and the comment records that `prometheus-cpp_DIR` in the environment covers any
third layout.

**4. The PostgreSQL port was hardcoded in four places.** `deploy.py` and `devenv.py` pass
`[db].port` from the environment file, which is why this looked plumbed. `perf_run.py`,
`callgrind_run.py` and three `psql` calls in `ha_test.py` passed the literal `"5432"`, and
`db/liquibase.properties` a literal URL. `create_db.py` and `export_credentials.py` now take
their defaults from libpq's own `PGHOST`/`PGPORT`, which covers every script that has no
environment file to read.

**`PGPORT` does not cover a deploy, and that is the case that failed.** `deploy.py` and
`devenv.py` pass `[db].port` from the environment file explicitly, and an explicit argument
beats an environment default -- so exporting `PGPORT` would have changed nothing about the
credential export that broke on RHEL8. Both now take `--db-port`, forwarded by `devsetup.py`
and `build-release-deploy.py` so that the flag survives a build+release+deploy run, which is
how the venue is actually deployed. It is applied to the parsed environment before anything
reads it, so `create_db.py`, `export_credentials.py`, the `${db_port}` placeholder and the
admin service's JDBC URL all move together. Overriding only the psql calls would have left the
Java service pointed at a port with nothing on it: a deploy that succeeds and fails later. The liquibase properties file cannot honour an
environment default -- liquibase substitutes `${env.VAR}` but has no syntax for a fallback --
so it says so in a comment instead.

Found while fixing it: `[admin_service] db_url` repeats the host, port and database name from
the `[db]` section, held together by nothing but a comment in each environment file. On a host
that is not on 5432 both must change, and missing one gives a deploy that succeeds while the
Java admin service alone cannot connect. `deploy.py` now checks them and refuses to deploy on
a mismatch.

**5. A failing step reported an exit code and nothing else.** `--sudo-postgres` failed with no
detail, because `create_db.py`'s database-exists probe captures output and its handler printed
only the status, and because `devsetup.py` exited bare -- so the last thing on screen was the
banner of the step that was *starting*. Both now name the command and repeat what it said.
`devenv.py` additionally names where the connection details came from, since psql reports the
host and port it could not reach but not that they came from the environment file.

### BUG-0027: Dismissed: the gateway's per-session `OpenOrderMap` is not the shape the order book was

| | |
|---|---|
| Severity | low |
| Found | 2026-08-16 |
| Recorded | 2026-08-21 (875259f) |
| How | Swept for other instances of the growing-hash-map pattern after moving the order book off `tsl::robin_map` |
| Impact | **None. Measured 2026-08-21 and dismissed** -- see the closing paragraph |
| Dismissed | 2026-08-21 (875259f) -- measured, and found not to be a problem |

`open_orders::OpenOrderMap` (`applications/fix_common/OpenOrderEntry.hpp`) is a `tsl::robin_map`
held per session (`FixSession::open_orders`) and mutated on the gateway's reactor callback thread
— `insert_or_assign` on each non-terminal execution report, `erase` on each terminal one. That is the same
structure, on the same kind of thread, with the same power-of-two growth policy as the order book
that stalled for over a second.

**Why it is weaker than the order book was.** It is scoped per session rather than venue-wide, and
entries leave on terminal execution reports rather than resting indefinitely. A venue with many small sessions
never grows any one map large enough to matter.

**Why it is not dismissible.** The bound is the number of orders one session leaves resting, and
nothing caps that. Under the trading-day profile it is worse than the general case, not better:
the load client runs a single session, so one `OpenOrderMap` accumulates every resting order in
the run — the same millions the order book holds, in the same growth steps, on the gateway's
callback thread instead of the engine's. A member that rests a large book behaves the same way.

**Settled 2026-08-21: it is not a problem, and the resemblance was superficial.**
`robin_hash::rehash_impl` on the gateway's callback thread reads **0.06%** of a trading-day
profile. The reason is in the type, which the sweep that raised this had not looked at closely
enough: `OpenOrderMap` is `robin_map<std::string_view, OpenOrderEntry*>`, so a bucket is about 32
bytes and the entries live in a pool. The order book stored `OrderEntry` inline at 304 bytes a
slot. A doubling here copies pointers; a doubling there copied the book. Nothing needs doing.

Worth keeping rather than deleting, for two reasons. It records that the pattern was looked for
elsewhere and where it was found, so the sweep is not repeated. And it is a reminder that "same
container, same thread, same growth policy" is not enough to make two structures behave alike --
what mattered was what each bucket held.

**Correction, 2026-08-23. The dismissal was right about the cost and wrong about one of its
premises.** Left above as it was written, because how it was reached matters.

The premise that failed is this one: *"entries leave on terminal execution reports rather than resting
indefinitely."* They did not. A cancel report names the retired order in OrigClOrdID and the
cancel request in ClOrdID, and the gateway looked up the latter -- a key that had never been
filed. Nothing was ever removed. Measured on a trading-day run: the map grew at 1,925 entries a
second against an order rate of 1,925 a second, with a profile specifying a tenth of orders
cancelled. Fixed the same day; see "remove the order a cancel names, not the cancel itself" in
the git history.

So the map was unbounded for any session, not only for the single-session load client the entry
worried about. The reasoning that "a venue with many small sessions never grows any one map
large enough to matter" did not hold, because every session grew without limit.

The second correction is about what was measured rather than what was assumed. `rehash_impl`
reading 0.06% of a profile is still true and still not the whole cost. A rehash was observed
taking p99 from 0.99 ms to about 2.3 ms for eleven minutes, with the load unchanged, recovering
to exactly 0.99 ms afterwards -- three times, at 2Mi, 4Mi and 8Mi entries, each step predicted
before it happened. A CPU profile cannot see it: the cycles in `rehash_impl` really are
negligible, and the cost is in the memory system afterwards, most likely huge pages being
re-formed under a freshly mapped table.

**What survives.** The conclusion that a doubling here copies pointers rather than order records
stands, and 32 bytes a bucket against 304 is still the reason this was not the order book's
problem. The entry's closing point survives too, and gains a second half: what each bucket held
was what mattered -- and a dismissal resting on a claim about behaviour needs that claim checked
against the code, not against what the code was meant to do.

### BUG-0031: Rejoin after a promotion re-runs the cold-start tie-break

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, designing process supervision |
| Recorded | 2026-08-22 (4ba3314) |
| How | Reading the arbiter against the rule a restarted primary needs to follow |
| Impact | A restarted primary -- which by definition has just lost its book -- would be handed leadership back from a healthy secondary that has it |
| Fixed | 2026-08-22 (81300d9) |

**Two different questions are answered by one rule.** "Which instance should lead when neither
is leading yet?" is a cold-start tie-break, and lowest instance id is the right answer for it:
the two can start in either order with a delay between them, and the preference makes that
deterministic. "Which instance should lead when one of them already is?" is a different
question, and lowest id is the wrong answer to it.

`decide_and_broadcast` (`ArbiterThread.cpp:550`) answers both the same way:

```cpp
const bool peer_connected = component_connections_.count(peer_key) > 0;
const int64_t leader_id = peer_connected ? std::min(self_instance_id, peer_instance_id) : self_instance_id;
```

It writes `leadership_state_` and never reads it. The arbiter therefore has no notion of an
incumbent, and recomputes leadership from scratch every time it is asked.

**Why that is dangerous rather than merely untidy.** The primary always holds the lower id. So
after the secondary has been promoted, a primary that restarts and triggers arbitration is
handed leadership back -- and a restarted primary has lost its book, because losing it is why
it restarted. The venue would move leadership from an instance holding the book to one holding
nothing.

**The rule agreed 2026-08-22**, which distinguishes the two questions:

```
if (an incumbent leader is recorded for this group AND that instance is connected)
    keep the incumbent
else
    lowest instance id wins          // cold start, or the incumbent is gone
```

| situation | incumbent | connected | outcome |
|---|---|---|---|
| cold start, either order | none | -- | lowest id: primary leads |
| primary restarts, secondary leads | secondary | yes | **primary becomes follower** |
| primary restarts, secondary's machine died | secondary | no | falls through: primary leads |
| primary restarts, secondary never promoted | primary | yes | primary leads again |

The last row is the *common* case once a supervisor exists, not an exotic one: the follower's
grace period is there so that a quick local restart happens before any promotion does.

**What the fix needs.**

1. **Re-key `leadership_state_` by group.** It is currently keyed `(group, instance_id)`
   (`ArbiterThread.hpp:84`), so "who leads the matching engine?" is not a single lookup and the
   incumbent cannot be consulted.
2. **Consult it in `decide_and_broadcast`**, with the connectivity check above.
3. **Gate resumption of leadership on reconciliation, not on the decision arriving.** A
   restarted primary that becomes leader again with an empty book is a leader that does not
   know what is resting: a member's cancel for a live order is rejected and the venue has
   silently lost state it still notionally holds. The `Reconciling` state and WAL catch-up
   exist; the ordering is what matters.
4. **Learn the current epoch from the arbiter before emitting anything.** Epochs are checked on
   every PDU, so a node rejoining with a stale one has its traffic discarded -- the right
   outcome, but it should be deliberate.

**Not traced:** what the code does on rejoin today, end to end. The reconnect path
(`ArbiterThread.cpp:509`) sends a *stored* decision keyed by the connecting instance's own id,
and after a promotion nothing is stored under the primary's key. Whether that yields silence, a
stale decision or a fresh arbitration was not followed through. It does not change the design
above, which is needed either way.

#### Fixed 2026-08-22

The rule is now `applications/arbiter/LeadershipDecision.hpp`, a pure function that reads no
state and sends nothing, so it is tested directly rather than through a running venue.
`ArbiterThread` gathers the inputs from its connection tables and carries the decision out.

`leadership_state_` is re-keyed by group. That fixed a second fault found on the way: the
reconnect path looked the stored decision up under *the connecting instance's own id*, so an
instance rejoining after a promotion found nothing recorded against itself and was told nothing
at all. Any instance reconnecting is now told who leads its group.

The epoch advances only when leadership actually changes. Confirming an incumbent returns the
epoch already in force, because epochs are checked on every PDU and superseding a leader's own
epoch would have its traffic discarded while it was doing nothing wrong. When the epoch does
advance it is taken from the higher of the arbiter's record and the reporter's, so an instance
that has been away cannot wind the sequence backwards with a stale report.

12 tests in `applications/arbiter/tests/LeadershipDecisionTest.cpp`, including a sweep asserting
that leader and follower are always the two distinct instances whatever the inputs. The rejoin
tests were checked against the previous rule and fail on it, so they catch the defect rather
than merely describing the new behaviour.

**Still to do: the integration scenario.** The unit tests prove the rule; they do not prove the
arbiter is asked at the right moments. That needs the `ha_test.py` scenario recorded in "Every HA
scenario models machine death; none models process death".

### BUG-0032: Ninety application tests were built, installed, and never run

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, after adding a test target to the arbiter and noticing it did not appear in the build output |
| Recorded | 2026-08-22 (fbce587) |
| How | `build.py` names each test binary it runs, and application binaries were never added to that list |
| Impact | Five suites, 90 tests, built and shipped by every release without once being executed |
| Fixed | 2026-08-22 (fbce587) |

`build.py` ran five test binaries by name -- the framework's unit and integration suites,
`scram_crypto`, and the two `fix_codec` ones. Every application suite was built, installed to
`bin/`, and then ignored:

| suite | tests |
|---|---|
| `fix_order_gateway_tests` | 39 |
| `matching_engine_tests` | 18 |
| `sequencer_tests` | 12 |
| `binary_order_gateway_tests` | 9 |
| `arbiter_tests` | 12 (added the same day) |

`ctest` was never invoked either, so the `gtest_discover_tests` registrations went nowhere. All
90 passed when run by hand, so nothing was being hidden -- but nothing was being checked, and a
regression in any of them would have reached a release unremarked.

**Fixed 2026-08-22.** `run_application_tests()` runs them, and the standard build goes from 893
tests to 983.

**The binaries are discovered, not listed**, and that is the point of the fix rather than an
implementation detail. A list is how the gap arose: each new test target had to be remembered in
`build.py`, and across five targets nobody remembered once. Discovery means a test target in any
application gates from the moment it builds.

**Finding nothing is an error, not a pass.** A glob that has stopped matching looks exactly like
a suite in which everything succeeded, which is the failure the gate exists to prevent -- so the
gate must not be able to fail that way itself. Both behaviours were checked: five suites found
and run, and exit code 1 with a message naming the directory when the glob matches nothing.

### BUG-0033: A restarted arbiter forgets who leads, and reverts to the cold-start rule

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, building the restart coverage matrix |
| Recorded | 2026-08-22 (63889b8) |
| How | Asked what the arbiter's new leadership state depends on, and what happens when it is lost |
| Impact | Undoes the incumbent-wins fix: a restarted arbiter hands leadership back to a restarted primary |
| Fixed | 2026-08-22 (c7b0d5d) |

`leadership_state_` lives only in memory. There is no snapshot, no WAL, and nothing reads
anything back at startup. It is replicated to the peer arbiter as decisions are taken
(`ArbiterStateRecord`), but that is a live feed and not a catch-up: an arbiter that starts, or
restarts, has an empty map and is told nothing about decisions made while it was away.

**Why that is worse than it was yesterday.** Before the incumbent rule, the arbiter was
stateless in effect -- it recomputed leadership from instance ids every time, so forgetting cost
nothing. Now the map is the thing that stops a restarted primary taking leadership back from a
working secondary. An arbiter with an empty map finds no incumbent, falls back to the cold-start
tie-break, and answers exactly as the code did before the fix.

The exposure is real but narrow while both arbiters do not restart together: the peer holds the
state and is the one that answers. It becomes live the moment the surviving arbiter is the one
that restarted, or both are.

**What it needs, and it is a design question rather than a patch.** Either the state is
recovered on startup -- from the peer, by asking, in the way a restarting sequencer syncs its
sequence number from its peer through StatusResponse -- or it is durable. The peer-sync route
looks the better fit: it is a pattern the venue already has, it needs no new persistence, and an
arbiter with no reachable peer genuinely has nothing to go on and is right to fall back to the
cold-start rule.

#### Fixed 2026-08-22

The arbiter is now told rather than remembering. `LeadershipLease` (id 118) carries what
`Heartbeat` used to imply -- an assertion of leadership by the instance holding it, at a stated
epoch -- and a restarted arbiter rebuilds its map from the leases it receives. Peer replay on a
link coming up is kept as an accelerator, not the mechanism, so the case with no peer to ask
still works. Full reasoning in `design-notes-for-ha.md` section 11c.

Scenario 25 covers it, and the ordering inside that scenario is the substance of it: both
arbiters must be killed before either is restarted, or the state survives through the peer and
nothing is exercised. Demonstrated:

```
10:48:44  both arbiters dead
10:48:48  group=matching_engine is led by instance 2 at epoch 1 (learned from its lease)
10:49:07  replayed 1 leadership record(s) to peer
```

**An expectation this disproved, recorded because the reasoning looked sound.** The lease
interval is 30s and the arbiter's learning window 10s, which appeared to leave twenty seconds in
which a restarted arbiter would stop declining and start guessing before it could have been told
anything. It cannot arise: an arbiter restarting is what closes its components' connections, and
a leader sends a lease immediately on connecting rather than waiting for the timer. The interval
was left alone rather than shortened on principle.

### BUG-0034: The pair does not survive a second failure: replication is wired by identity, not by role

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 32 |
| Recorded | 2026-08-22 (32060cc) |
| How | Failover, rejoin, then fail the new leader -- the surviving instance never takes over |
| Impact | After one failover the pair is finished. A second failure leaves the venue with no matching engine leader at all |
| Fixed | 2026-08-22 (5a47932) |

`design-notes-for-ha.md` section 11 says the pair must be able to swap repeatedly: if the
secondary is promoted and later dies, the primary -- by then a follower -- must take over again.
It does not.

**Replication is hard-wired primary to secondary.** The primary connects out to
`me_secondary_replication`; the secondary listens. Nothing about that follows the role. So after
a failover the leader is the secondary, which has no replication channel *to* the primary, and
the primary has no channel *from* a leader.

**Failure detection inherits the same wiring.** A follower arms its promotion timer when
`primary_replication_conn_id_` is lost -- the connection it receives replication *on*. A primary
acting as a follower has no such connection, so nothing arms. It does notice the socket go, and
draws the wrong conclusion from it:

```
ME-secondary replication connection 3 lost: peer closed connection on service
'me_secondary_replication' -- book updates paused until secondary reconnects
```

It reads the leader's death as "my secondary has gone, book updates paused". Nothing promotes,
and the venue has no matching engine leader.

**This is the same class as the sequencer routing defect fixed earlier the same day**, and the
same omission behind all of them: *primary* and *leader* were interchangeable while nothing ever
restarted, so channels were named after identities. Once roles move, every channel named for an
identity is pointing at the wrong instance half the time.

#### Fixed 2026-08-22: both directions held permanently

Replication is now symmetric. Each instance listens on its own port and dials its peer's, both
connections stand at all times, and which one carries book updates follows the role because the
leader is the sender. A role change therefore needs no connection work at the moment the venue
can least afford it -- the alternative, having the leader dial the follower and re-establish on
every change, puts a reconnect on the failover path and leaves a newly promoted leader serving
without a replica while it completes.

Failure detection then works in both directions without special handling, which is the point.
A follower arms its promotion timer on losing the connection it *receives* replication on, and
with both directions held, whichever instance is the follower always has one.

**Three places had to stop branching on the configured identity**, and each was somewhere the
design had quietly baked in "primary means leader":

* the environment and both TOML templates, which gave the primary only a dial and the secondary
  only a listener;
* `MatchingEngineConfigurationLoader`, which read the two halves under an `is_secondary` test
  and now reads both for both roles;
* `MatchingEngine`, which registered the replication listener only on the secondary.

All three `is_secondary` checks are gone. Scenario 32 passes: leadership moves primary to
secondary, the primary rejoins as follower, and when the secondary then dies the primary takes
it back -- 15.2 s each way, the promotion timeout in both cases.

**A smaller fault visible in the same logs.** An ArbitrationDecision arrived as
`leader=2 follower=0`, and the instance correctly ignored a decision that did not mention it. The
zero comes from a leadership record rebuilt from a lease, which names only the leader; the
follower field is filled with 0. Harmless today because a correct decision follows immediately,
but it is a wrong value on the wire.

**Scenario 32 fails on this and is left failing**, as scenario 24 was before its defect was
fixed. It says so in place.

### BUG-0035: Role announcements were routed to the wrong socket, and then to a dead one

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 26, in the routing added earlier the same day |
| Recorded | 2026-08-22 (abf7914) |
| How | A supervised restart made the ordering visible; the failover scenarios had hidden it |
| Impact | Orders sent down the wrong channel, and then to the connection of a process that had just died |
| Fixed | 2026-08-22 (4339a47) |

Two faults in the sequencer's handling of `RoleAnnouncement`, both introduced when routing was
first made role-aware and neither caught by the failover scenarios.

**The announcement arrives on the wrong socket to route by.** A matching engine opens an execution-report
connection *to* the sequencer and announces on it. Orders travel the other way, on a connection
the sequencer opens *to* the engine. Routing by the connection an announcement arrived on
therefore aimed orders down the execution-report channel. The announcement names an instance, so the fix is to
map that to this sequencer's own order connection for that instance.

**And an ordering race the first fault concealed.** A restarted engine announces immediately on
a connection it opens itself, while the sequencer's order connection to it is re-established a
couple of seconds later. So the announcement arrived while the only order connection on record
for that instance was the one belonging to the process that had just died:

```
11:09:32  instance 1 leads at epoch 1 -- orders now route to connection 15   <- dead process
11:09:34  matching engine order connection 25 established                    <- the live one
```

The announced leader is now remembered, so the order connection to that instance is routed to
when it appears:

```
11:09:34  order connection 25 to instance 1, which had already announced leadership -- routing there
```

**Why the failover scenarios missed both.** In a failover the promoted instance already holds a
pre-warmed standby connection, so the socket exists before the announcement and the ordering
never arises. It takes a restart -- where the connection is genuinely absent and then appears --
to expose it. That is the argument for the restart cells of the matrix in one paragraph.

### BUG-0036: With no arbiter reachable, a starting engine never promoted and never said so

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 35 |
| Recorded | 2026-08-22 (eb2acec) |
| How | Killed both arbiters, then restarted the matching engine that had been leading |
| Impact | The venue has no matching engine leader, indefinitely, announced by nothing but connection-refused retries |
| Fixed | 2026-08-22 (eb2acec) |

`design-notes-for-ha.md` is explicit that a two-node system with no arbiter falls back to
"lowest instance id wins". The fallback existed, and was unreachable.

The degraded self-promotion lives inside `send_arbitration_report()`, which was only ever called
from `request_startup_arbitration()` -- and that was called **when an arbiter connection came
up**. With the arbiter pool down, no connection comes up, so a starting engine never asked, never
degraded, and sat in `UNKNOWN` forever:

```
HA enabled, starting as UNKNOWN (instance_id=1, configured primary)
service 'arbiter_primary' failed to connect; retrying every 2000ms
service 'arbiter_secondary' failed to connect; retrying every 2000ms
```

Nothing else logged. The venue was not serving and said so only by omission -- the failure mode
section 13 of the design notes describes as the one that does not announce itself.

**Fixed 2026-08-22.** The startup arbitration timer is armed when the thread starts rather than
when an arbiter connects. It fires, finds the role still unresolved, and applies the instance-id
rule.

**Only the primary arms it**, which is worth stating because it is what makes the fallback
correct rather than merely present. The secondary starts as a follower and waits, so the instance
that promotes unilaterally is always the lower id -- the rule the design specifies, satisfied by
construction rather than by a comparison that could be written the wrong way round. Scenario 35
asserts both halves: the primary degrades and says so, and the secondary does not also promote.

### BUG-0042: A restarted primary matching engine promotes itself, producing two leaders

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by the ha_test.py scenario written to prove the arbiter fix |
| Recorded | 2026-08-22 (c7b3459) |
| How | Scenario 24 restarts the primary after the secondary has been promoted, and asserts it comes back a follower |
| Impact | Two instances hold LEADER at once, and the venue stays that way -- the correct decision is discarded as a duplicate |
| Fixed | 2026-08-22 (193ad36) |

`MatchingEngineThread::on_connected` (`MatchingEngineThread.cpp:225`) does this when the first
arbiter connection comes up:

```cpp
if (first_arbiter && is_primary_) {
    // Primary holds leadership by heartbeating the arbiter to renew its lease.
    adopt_leader_role();
}
```

**The primary declares itself leader before asking anyone.** On a cold start that is harmless,
because the arbiter would name the same instance anyway. On a restart it is not: the secondary
may already be leading and serving traffic.

**And the correct answer is then thrown away.** `handle_arbitration_decision` suppresses a
decision that arrives when the instance is already Leader or Reconciling, which exists to stop a
second reply from both arbiters re-running reconciliation. It cannot tell that reply apart from
the first authoritative answer this instance has ever received. Observed:

```
08:54:10.598297  arbiter-primary connection 4 established
08:54:10.598297  adopting LEADER role (epoch=0)
08:54:10.598385  ArbitrationDecision received (group=matching_engine leader=2 follower=1 epoch=1)
08:54:10.598386  already LEADER -- ignoring duplicate ArbitrationDecision
```

The secondary had been promoted two seconds earlier and holds the book. The restarted primary
holds nothing, believes it leads, and has just discarded the message telling it otherwise.

**This is the failure the design says cannot happen.** `design-notes-for-ha.md` section 11: "The
arbiter decides in every case. A restarting node never promotes itself, so no sequence of
restarts can produce two leaders." The code does the opposite.

**The arbiter is not at fault, and the fix above is working.** The same run shows it answering
correctly, and answering at all only because `leadership_state_` is now keyed by group:

```
08:54:10  component group=matching_engine instance_id=1 registered on connection 9
08:54:10  ArbitrationDecision sent to connection 9 (group=matching_engine leader=2 follower=1 epoch=1)
```

**The shape of the fix.** On first arbiter connection a primary should send an
ArbitrationReport rather than adopt a role, and wait. The arbiter already answers both cases --
lowest id when no incumbent is recorded, the connected incumbent when one is -- so nothing new
is needed there. The duplicate guard needs to distinguish a genuine repeat from the first answer
received, which the epoch already carries.

**Blast radius, which is why this is recorded rather than immediately changed.** It alters
startup for the matching engine: a cold-start primary would become leader a round trip later
than it does today, and every scenario that waits on "adopting LEADER role" would see it at a
different moment. The sequencer pair should be checked for the same pattern before either is
touched.

**Scenario 24 fails against the current code**, which is what it is for.

**Restored 2026-08-24.** This entry was deleted in `63889b8`, when three defects found by
scenario 24 were folded into the restart-coverage matrix. Its substance did not survive the fold:
nothing in the matrix records the self-promotion, and three comments in `MatchingEngineThread.cpp`
and `.hpp` went on citing it by title. That is what ids and `check_bug_list.py` exist to prevent.

### BUG-0043: A cold-start primary routed through reconciliation strands the venue

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, uncovered by the fix for BUG-0042 |
| Recorded | 2026-08-24, written from `193ad36` after the code comment citing it was found to name an entry that had never existed |
| How | Reading what a starting instance does when the arbiter names it leader, having just changed what a restarting one does |
| Impact | A venue starting cold gets no matching engine leader at all: the instance waits on a connection it will never be given |
| Fixed | 2026-08-22 (193ad36) |

Reconciliation exists for a **promotion**. A follower that is told to lead already holds a replica
book and an open replication connection to the instance it is taking over from, and reconciliation
uses that connection to close the gap between the replica and the truth.

A cold start has neither. There is no peer serving, no replica to reconcile against, and no
connection to wait on -- so an instance routed through reconciliation on the way to leadership
waits for something that will not arrive, and the venue comes up with no matching engine leading.

The fix is stated as a rule rather than a special case: **reconciliation is entered only when
promoting from Follower.** A start is not a promotion, so it adopts the leader role directly.
`MatchingEngineThread.cpp` carries the reasoning at the branch that decides between the two.

**Why this had no entry until 2026-08-24.** It was found and fixed inside the change for BUG-0042
and recorded only in that commit's message. A comment at `MatchingEngineThread.cpp:1119` cited it
as though it were in this file, naming *"A cold-start primary that is told it leads should not
reconcile"* -- a title `git log -S` finds nowhere except in the commit that wrote the comment. A
citation to an entry nobody wrote is indistinguishable from a citation to one that was deleted,
which is the argument for checking citations mechanically.
