# Bug List

Defects found and not yet fixed, and defects fixed recently enough to be worth remembering.

**Why this file exists.** The metrics-inside-CPU-pinning defect below was found on 2026-08-04,
judged not worth fixing at that moment, and then forgotten — it survived only in a working note
nobody else could see, and had to be rediscovered on 2026-08-08 before anything was done about it.
A defect that is known and invisible is worse than one nobody has found: the project carries the
risk without carrying the knowledge.

**Every entry records the date it was found and how it was found.** The second is the more useful
half. "Found by the trading-day load run" tells you which activity is worth repeating; "found by
reading the code" tells you the tests would not have caught it.

Fixed entries are kept for one release cycle and then deleted — the commit is the permanent record.

---

## Open

### Python style warnings across the top-level scripts, and a lint gate that ignores them

| | |
|---|---|
| Found | 2026-08-09 |
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

### Environment placeholders are missing outside dev

| | |
|---|---|
| Found | 2026-07 (exact date not recorded) |
| How | Reading `deploy.py` against the environment files while working on config templating |
| Impact | `deploy.py` exits on preprod, prod and test-1 |

`environments/preprod.toml`, `prod.toml` and `test-1.toml` each lack 10–12 of the placeholder
values their component templates require. Only `dev.toml` is complete, so only dev can be deployed.

**Deliberately not fixed**: the missing values are real hostnames, ports and certificate paths for
environments that do not exist yet. Inventing them would produce a file that deploys and then
fails at run time, which is worse than one that refuses to deploy.

**The gap widened by 60 on 2026-08-09**, when the reactor queue pools became templated (see the
fixed entry below). Unlike the hostnames, these are safe to fill in from `dev.toml` whenever those
environments are built, because a queue depth is a capacity decision rather than a fact about a
host that has to be looked up — so the count is larger but the difficulty is unchanged.

### A growing hash map stalls the reactor callback thread for over a second

| | |
|---|---|
| Found | 2026-08-08 |
| How | The first clean compressed-trading-day load run — the reactor's own watchdog logged it, and the profile confirmed the cause |
| Impact | p99 of 733 ms at 4.2M orders, over 1 s at 8.4M, after which the pipeline did not recover and 1,167,392 of 9,556,000 orders were never accepted |

The matching engine's order book is a `tsl::robin_map` with `power_of_two_growth_policy<2ul>`. It
doubles, and each doubling rehashes the whole table **on the callback thread**. Stalls land only on
exact powers of two — all five stalls of 200 ms or more did, and none occurred elsewhere.

**This is a framework requirement, not an application bug.** Calling `reserve()` in the matching
engine would remove the symptom from a stub and teach nothing. `pubsub_itc_fw` offers slab and pool
allocators for messages *in flight* and nothing at all for long-lived hot-path state that *grows* —
so every application that keeps state (an order book, a session table, a subscription registry)
will grow a container on a reactor callback thread and eventually stall it.

See [Compressed Trading Day Load Profile](design/trading_day_load.md).

### The venue accepts orders indefinitely with no matching engine, and tells nobody

| | |
|---|---|
| Found | 2026-08-08 |
| How | The first clean trading-day load run, after the matching engine was OOM-killed |
| Impact | 924,000 orders accepted and acknowledged to the member with nothing able to process them; 1,087,912 orders deferred over 7 minutes |

When the matching engine connection drops, the sequencer commits each order to the WAL and
defers forwarding it:

```
SequencerThread: no matching engine connected -- order seq=59678842 WAL-committed,
forward deferred until an ME reconnects (recovered via WAL replay on ME promotion)
```

That policy is sound for a brief failover — the orders are durable and a promoted ME
replays them. Three things about it are not.

**The assumption can stop holding, and nothing notices.** An ME was promoted and did
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

Related: the HA entry below, since the deferral policy assumes a promotion that will
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

### HA fails over into a condition both nodes share

| | |
|---|---|
| Found | 2026-08-08 |
| How | The first clean trading-day load run — the primary matching engine was OOM-killed and the promoted secondary died 2 minutes later |
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

Worth designing for explicitly. Open questions rather than a plan:

- Should a promoted node **check its own headroom** before accepting promotion, and decline
  if it is in the same state the primary died in?
- Should the arbiter know *why* the primary went, so it can distinguish "the process died"
  from "the process died of something I am also suffering"?
- Is there a signal a node can publish that means "I am not a safe failover target"?
- What is the right behaviour when there is no safe target — refuse orders rather than
  accept ones that cannot be processed? See the entry above about accepting orders with no
  matching engine.

Note this run's book growth was itself an artefact: the ME does no matching and the load
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

### `cmake --install` re-lays config templates unexpanded

| | |
|---|---|
| Found | 2026-08-08 |
| How | A trading-day run failed to start immediately after a rebuild |
| Impact | `devenv.py` refuses to start until `deploy.py` is re-run |

Every build re-installs the `${placeholder}` templates over the deployed, expanded configs, so
**`deploy.py` must be re-run after every build**. `devenv.py` does detect it and says so clearly,
which is why this is a trap rather than a fault — but it is easy to hit when iterating on a
component and then starting the venue.

See also the entry below, which is the converse and the more dangerous half.

### `deploy.py` silently ignores a change to an environment file

| | |
|---|---|
| Found | 2026-08-09 |
| How | Raising the binary gateway's open-order pool in `environments/dev.toml` before a load run; the deployed config still held the old value after `deploy.py` reported success |
| Impact | A run can execute a whole profile against the *old* configuration while every command in the runbook appears to have succeeded |

`deploy.py` substitutes `${placeholder}` values into the deployed configs; it does not re-copy the
templates. Once a config has been expanded it contains no placeholder, so a later edit to
`environments/dev.toml` has no effect: `deploy.py` reports `0 template(s) expanded` and exits
zero. Nothing warns, and the venue starts happily on the previous values.

The working sequence is a re-install first, so there is something left to expand:

```bash
cmake --install build                     # re-lay the templates, unexpanded
python3 deploy.py --skip-db --skip-certs  # expand them from environments/dev.toml
```

**Worth fixing rather than documenting**, because the failure is silent and the cost is a whole
run. Either re-copy a template when it or the environment file is newer than the deployed config,
or refuse to report success when a deployment expanded nothing — `0 template(s) expanded` is
almost never what the caller intended, and it is the one case that currently looks identical to
success.

### `start_fix_seq_system.py` launches from configs that no longer exist

| | |
|---|---|
| Found | 2026-08-09 |
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
`matching_engine_primary.toml`, the way `ha_test.py` does for its single-ME topology, or delete
it as superseded by `devenv.py` and `perf_run.py`. That question was deliberately left open rather
than answered in passing.

### Shutdown timeout errors in timer tests

| | |
|---|---|
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| How | Timer test logs |
| Impact | Unknown — the errors appear but nothing is known to misbehave |

After the timer SEGV fix, "did not stop within shutdown_timeout" and "failed to join within
shutdown_timeout" still appear in timer test logs. **Root cause not identified.** Worth noting that
`ThreadWithJoinTimeout` exists precisely because a raw `std::thread` terminates on an early return
before join, so a join that times out is not obviously benign.

### OGT `process_message` exit paths not audited

| | |
|---|---|
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| How | Code reading |
| Impact | Possible false stuck-thread detection |

If any exit path from `process_message` skips updating `time_event_finished_`, the watchdog would
report a thread as stuck when it is not. Not yet audited. Given that the reactor watchdog is what
made the rehash stall diagnosable, false positives from it would be costly.

### Doxygen 1.8.14 turns `\ref` labels into bare directory links

| | |
|---|---|
| Found | 2026-07 (exact date not recorded) |
| How | Building the docs on RHEL8, where 1.8.14 is the newest packaged release |
| Impact | Documentation only; the architecture map's cross-links break |

An unresolved `\ref` collapses to `href="../../"`, which a browser opens as a directory listing —
or, on Windows, a file chooser. 1.8.14 does **not** fail the build on an unresolved reference, so
this is silent. `docs/architecture_map_howto.dox` proposes a post-build check; not written.

### ResendRequest under load

| | |
|---|---|
| Found | before 2026-08 (carried over from the roadmap's Known Issues) |
| How | Noted when the feature was written |
| Impact | Unknown |

Partly overtaken by the 0.3.0 work, which replaced the blanket `SequenceReset-GapFill` with real
resends carrying `PossDupFlag`. The original concern — never exercised under load — still stands,
and the trading-day profile is a natural place to exercise it once it drives the FIX gateway.

### fix-test-client reports a dead gateway poorly

| | |
|---|---|
| Found | 2026-07 (exact date not recorded) |
| How | Manual testing of the logon page |
| Impact | The UI misleads about connection state |

Two undecided items: `lastError` is left empty, and `connected` stays true after the gateway dies.
FIX produces no disconnect message, so there is nothing to display without inferring it.

---

### Slab allocator design notes do not mention the tripwire

| | |
|---|---|
| Found | 2026-08-09 |
| How | Noticed while fixing the tripwire |
| Impact | Documentation only |

`drain_empty_slab_queue()` carries a safety tripwire that throws and takes the reactor down
with it. That is a significant behaviour of the allocator and it appears nowhere in the
design notes -- only in a comment inside the function. Anyone reasoning about failure modes
from the documentation would not know the allocator can terminate a component.

Worth adding when the allocator documentation is next touched, together with the rule that
the condition needs both a spent budget and a spun loop.

### The idle-connection reaper tears down the pre-warmed failover link

| | |
|---|---|
| Found | 2026-08-09 |
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

---

## Fixed

### The Rocky container deployed its gcc-8.5 binaries over the host's install tree

| | |
|---|---|
| Found | 2026-08-09 |
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

One related exposure remains open and needs a decision:

- No stage verifies that what it is about to test can actually start. A one-line `ldd` check
  for unresolved libraries before `ha` would have named this in seconds rather than an hour.

### `pubsub_metrics.py` built query labels from a module global, and fell back to a table describing the wrong venue

| | |
|---|---|
| Found | 2026-08-09 |
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

### `--application` never reached metric discovery

| | |
|---|---|
| Found | 2026-08-09 |
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

### The band chart drew a flat line across periods with no data

| | |
|---|---|
| Found | 2026-08-09 |
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

### The slab allocator had a hard message ceiling, below the performance target

| | |
|---|---|
| Found | 2026-08-09 |
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

### Reactor queue pool sizes were not configurable in any environment

| | |
|---|---|
| Found | 2026-08-09 |
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

### Metrics silently disabled when CPU pinning is off

| | |
|---|---|
| Found | 2026-08-04 |
| How | Reading the configuration loaders while adding the Prometheus metrics |
| Fixed | 2026-08-08 |

`config.metrics_configuration = MetricsConfigurationLoader::load(toml)` sat **inside** the
`if (config.cpu_pinning_enabled)` block in the matching engine and both gateway loaders. A
component with pinning turned off exposed no metrics at all — no error, no warning, just an
endpoint that never appeared.

Left alone when first found because nothing depended on it. It became urgent when the test
harnesses moved their ground truth onto those counters, at which point a silent disable would make
them pass while verifying nothing. **This is exactly the defect that motivated this file.**

### Orphaned build directories break coverage capture

| | |
|---|---|
| Found | 2026-08-08 |
| How | Every incremental coverage build was failing; the error named standard-library headers and pointed nowhere near the cause |
| Fixed | `10f4578` |

`applications/binary_gateway/` was renamed on 2026-08-01 and CMake never removed the old target
tree. gcovr searches the whole build tree *before* the report-level excludes apply, found `.gcno`
files with no `.gcda`, could not resolve their compilation directory, searched upward and aborted
at `/`.

### Trading-day phases reused ClOrdIDs

| | |
|---|---|
| Found | 2026-08-08 |
| How | The first trading-day run; `perf_run.py`'s own loss accounting reported the mismatch |
| Fixed | `d47112e` |

The phase scheduler restarts the load client per phase and every invocation numbered its orders
from 1, so 5,256,000 of 8,632,000 orders were rejected as duplicates and four of seven phases
recorded no latency at all. Quiet in the worst way: rejected orders still travel through the
gateway and sequencer, so the load looked real while measuring nothing.

---

## See Also

- [Roadmap](roadmap.md) — planned work, as distinct from defects
- [Testing and Code Coverage](testing.md)
