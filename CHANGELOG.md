# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the public API is not yet considered stable and may
change in any release.

## [Unreleased]

### Fixed

- **0.3.0 would not build or run on the work RHEL8 host**, in five separate ways, none of which
  the `rocky` release stage could have caught: it reproduces the gcc 8.5 compiler and nothing
  else about that machine. The pylint gate failed on `R0022 useless-option-value` -- a complaint
  about the message filter, which the message filter cannot suppress -- raised by a newer pylint
  over two stale `bad-whitespace` disables in `sca.py`. Twenty-two DSL tests failed on NFS during
  scratch-directory cleanup, *after* passing, because a dlopen'ed `.so` is silly-renamed to
  `.nfsXXXX` rather than unlinked. `prometheus-cpp` was not found, that tree naming and placing it
  differently. The PostgreSQL port was hardcoded as `"5432"` in `perf_run.py`, `callgrind_run.py`,
  three `psql` calls in `ha_test.py` and `db/liquibase.properties`. And a failed `--sudo-postgres`
  reported an exit code with no detail. See `docs/bug_list.md` for each.
- **An errors-only pylint gate is no longer failed by a non-error.** `run_command` takes
  `tolerated_exit_bits`; pylint's exit status is a bitmask of finding categories, so a gate that
  asked for errors now fails on errors alone. A check retired by some future pylint stops the
  build no longer.
- **`create_db.py` and `export_credentials.py` take their default host and port from libpq's own
  `PGHOST`/`PGPORT`**, so a cluster that is not on localhost:5432 needs one exported variable
  rather than an edit per script. An explicit `--db-port` still wins, and `deploy.py` and
  `devenv.py` go on passing `[db].port` from the environment file.
- **`deploy.py` checks `[admin_service] db_url` against the `[db]` section** and refuses to deploy
  when they disagree. The two name one database and were held together by a comment; missing one
  gives a deploy that succeeds while the Java admin service alone cannot connect.
- **A failing step says what failed.** `create_db.py` repeats the command and its captured stderr,
  `devsetup.py` names the step that failed rather than exiting bare on its status, and `devenv.py`
  says that a refused database connection used the details in the environment file.

### Changed

- **`--no-pylint` describes what it now skips** -- all project Python, not just the DSL -- in both
  `build.py` and `devsetup.py`. The gate widened in 0.3.0 and the help text did not follow.

## [0.3.0] - 2026-08-10

### Added

- **The build checks the top-level scripts.** `pylint` ran against two package directories,
  so every script in the repository root — the ones that deploy the venue and run the tests —
  was checked by nothing at all. They are now linted for **errors**, and each must answer
  `--help` without failing. Both gates are green as added; the style warnings are recorded in
  `docs/bug_list.md` rather than fixed, and raising the bar to include them is the stated
  resolution there.
- **A trading-day load profile.** `perf_run.py --profile profiles/trading_day.toml` runs a shaped
  day -- pre-open, an opening auction at the ceiling, steady morning, a sustained elevated hour,
  a midday lull, an afternoon burst and a close above the ceiling -- with a cancel stream so the
  book reaches a working size instead of only growing. Phase rates are fractions of a measured
  ceiling, so the same profile means the same thing on a different machine. It is the reason
  almost everything else in this release was found: nine runs of up to two hours each, exercising
  the framework through load transitions no steady rate produces.
- **The order round-trip histogram is charted as percentile bands over time**, against a stated
  ceiling, with an exact count of the orders that exceeded it -- refused rather than estimated
  when no bucket bound sits on the ceiling value.
  - `--from` / `--to` scope a chart to one run or one incident. `--since MINUTES` could only frame
    a window by arithmetic against the current time, and a chart spanning two runs made its own
    breach total meaningless.
  - Under `--graphic`, a strip along the top of each band chart reports the time and latency
    under the pointer, and says `no data at this time` over an interval where nothing was
    observed rather than reading a value off the cursor's height.
- **Bucket bounds now cover a third regime: an order caught across a failover.** A trading-day run
  put 223,824 orders (1.87%) past the old 5s top bound, where the histogram could say only "more
  than five seconds" -- the `_sum` series puts their mean at 62s or more. Bounds extend to 250s,
  deliberately coarse above a minute, so the overflow bucket returns to meaning "beyond anything
  anticipated".
- **The order book's growth is visible.** `GrowthReportingAllocator` warns as the book's storage
  doubles; previously it reached 9.9 GB and was OOM-killed having logged no memory warning at all.
  A resource monitor samples every component's RSS to `resource_usage.csv` for the length of a run.
- **A coverage baseline**, reported and never gated. `coverage_baseline.py` with a committed
  baseline, a release stage that fails on a stale baseline rather than on a number, and the
  policy written down in `docs/testing.md`.

- **A reconnecting member is sent the execution reports it missed.** A ResendRequest is now
  answered with the real reports, carrying `PossDupFlag=Y` and `OrigSendingTime`, and only the
  administrative remainder is gap-filled -- the split FIX actually prescribes. Previously every
  ResendRequest was answered with a blanket `SequenceReset-GapFill`: the session survived and
  the member was told nothing about what had happened to its orders.
  - **Sequence continuity across a reconnect**, without which a member never asks. A session's
    outbound number is reported to the sequencer when it unbinds and handed back when a gateway
    binds it, so a reconnect -- to the same instance or to the backup -- continues the member's
    numbering instead of restarting at 1. `ResetSeqNumFlag=Y` is honoured, for the member that
    wants the opposite.
  - **The replay is a filtered WAL scan**, not a second copy of anything: every report is
    already in the WAL with its originating session on the envelope. Measured at 18 ms to scan
    a 4 MB retained WAL and return 3,223 records for one session. The sequencer returns the
    most recent records up to the gap width the member described -- what a member misses is the
    tail of its stream, so serving the oldest matches first fills the gap with ancient history
    and never reaches what it actually missed.
  - New `SessionBoundAck`, `SessionReplayRequest`, `SessionReplayRecord` and
    `SessionReplayComplete` PDUs. Live reports arriving mid-resend are held and delivered
    behind it, so the numbered sequence the member is being handed stays ordered.
  - **Limits, stated plainly:** only execution reports are replayable and only while the WAL
    retains them; there is no outbound message store; the remembered sequence numbers do not
    survive a venue restart; the binary gateway is unchanged, having no session layer to hang a
    resend on.
- `binary_client` gained `--cl-ord-id` and `--cancel` (step 5), and the FIX encoder gained
  optional `PossDupFlag`/`OrigSendingTime` for resends.


- **A session is now an identity, not an address, and its reports follow it.** A member that
  reconnects -- to the same gateway instance or to its backup -- receives execution reports for
  orders it placed on the connection it has lost, and can cancel orders it left resting. Neither
  was possible before: an order was filed under the connection it arrived on, which is a socket
  on a gateway process, so it died with the connection and did not exist at the backup at all.
  - The sequencer's routing entry is split in two: `seq_no -> session identity`, and
    `session identity -> current destination`. The destination is resolved when a report is sent
    rather than remembered when the order was placed.
  - The identity is `(comp id, protocol)`. An instance failover moves a session between instances
    of one protocol, so the instance cannot be part of it; a FIX and a binary session sharing a
    comp id are two sessions, so the protocol must be.
  - Gateways announce sessions to the sequencer with new `SessionBound` / `SessionUnbound` PDUs.
    An unbind naming a connection that is no longer the current one is ignored, so a fast
    reconnect cannot be unbound by the old connection's late notice.
  - The matching engine's book key (`OrderKey`) and its `BookUpdate` replication carry the
    identity, and the engine no longer stamps a destination on any report -- on the
    cancel-on-failover path any address it remembered would name the process that just died.
- `binary_client` gained `--cl-ord-id` and `--cancel`, so a second run on a new connection can
  cancel an order the first one placed. That is how the above was verified from a client rather
  than from a log.

- **Sessions are provisioned against gateway instances.** A comp id gains a primary and an
  optional backup gateway instance (`pubsub_comp_id.primary_gateway_instance` /
  `backup_gateway_instance`, editable on the admin service's comp-id form), and both gateways
  refuse a logon that arrives at an instance the session is not provisioned for. The values
  travel the same path as cancel-on-disconnect -- database, `export_credentials.py`,
  `credentials.toml`, authentication service, `AuthenticationResult` -- so they arrive with the
  session rather than needing a lookup the gateway has no database access to make.
  - They name an *instance*, not a protocol: instance 1 of the FIX gateway and instance 1 of the
    binary gateway hold the same position in their own protocol, and the pinning applies to
    whichever the member speaks. This keeps the authentication service protocol-agnostic.
  - **Not pinned means any instance**, and is the default: both columns are nullable, and null
    means the member expressed no preference. No existing comp id is locked out by the change.
  - The refusal has its own binary `LogonOutcome::NotProvisionedForInstance`, and the FIX Logout
    text names the instances the member should be using. Reusing `AuthenticationFailed` would
    have sent members off rotating a password that was never the problem.
- **Fixed: the authentication service was discarding per-comp-id provisioning on every credential
  change.** `persist_credentials` rewrites `credentials.toml` in full from its SCRAM map, so an
  admin setting, removing or restoring any credential silently stripped every member's
  cancel-on-disconnect settings and left them on gateway defaults, with nothing in any log to say
  so. It now writes the session policy back out beside the credential it belongs to.

- **Cancel-on-disconnect now has a grace period** (`[cancel_on_disconnect] enabled` and
  `grace_period`, defaulting to on and 30 seconds, in both gateways). A dropped session's
  resting orders are held rather than cancelled, and if the same comp id reconnects inside
  the window nothing is cancelled at all. Previously the whole book went the instant a
  socket closed, so a member whose connection blipped came back flat -- and a gateway
  failure flattened every book on the instance, the high-availability mechanism producing
  exactly the outcome high availability exists to prevent.
- GoodTillCancel and GoodTillDate orders are never cancelled on disconnect; they were
  placed to outlive the session. A clean FIX Logout still cancels immediately, since a
  member that logs out has said what it wants.
- **Cancel-on-disconnect is provisioned per comp id**, not only venue-wide. Two new
  `pubsub_comp_id` columns reach the gateway on `AuthenticationResult`, so they arrive with
  the session. The grace period is nullable and null is *not* zero: null defers to the
  gateway's configured default, zero cancels immediately. Editable on the admin service's
  comp-id form.
- **The matching engine echoes `TimeInForce` on execution reports.** It previously did not,
  which is what made the exemption above impossible to implement.

- **The binary gateway now runs as two instances, `binary_order_gateway_a` and
  `binary_order_gateway_b`**, for the same reason the FIX gateway does: losing one
  instance should not take every session on that protocol with it. The sequencer
  carries four `[[gateway]]` entries in dev — FIX 1 and 2, binary 1 and 2 — and
  routes each execution report back to the instance its order arrived on. Only the
  `_a` instance of each protocol is deployed outside dev; the `_b` entries are
  configured but disabled.
- **`perf_run.py --gateway-instance` now applies to the binary gateway too**, not
  only to FIX. The load generator's target port is read from the chosen instance's
  deployed configuration rather than from a constant in the script, so it cannot
  drift from the deployment.

### Changed

- **Reactor queue pool sizes are configurable per environment.** Every application carried
  `[event_queue_pool]` and `[command_queue_pool]` as literals, so no environment could tune them,
  and the values had drifted apart by three orders of magnitude between components doing the same
  job. Behaviour is unchanged except for the matching engine publisher, whose queues were still at
  the framework default of 1,024 and exhausted repeatedly under load.
- **The reactor's inbound slab is 256 KB, up from 64 KB.** A slab stays mapped while any one of its
  chunks is outstanding, so worst-case retention rises with slab size; 256 KB keeps that four times
  lower than 1 MB while giving four times the headroom of 64 KB.
- **`pubsub_metrics.py --application` is required wherever Prometheus is consulted**, and there is
  no longer a fallback to a built-in component table. The table describes this venue, so offering
  it when an application returned no series answered a question about pubsub that the caller did
  not ask -- listing component names that do not exist for them.

- **`perf_run.py --clients N` now gives each FIX client its own comp id**, credential and
  generated session config, as the binary load client already did. N clients under one comp id
  are one session to the venue, and f8test numbers its ClOrdIDs from one in every process, so
  under an identity-keyed book all but the first client's orders would be duplicates. It only
  worked before because the book key included the gateway connection id.
- Log lines that `ha_test.py` and `perf_run.py` assert on now carry a `TEST CONTRACT` comment at
  the source. The harness matches log text across seven processes, which makes the wording an
  interface no compiler checks; 24 sites in the matching engine, sequencer, arbiter and both
  gateways now say so where the line is written.

- **Both gateways renamed so the name says the protocol and the job.**
  `order_gateway` is now `fix_order_gateway` and `binary_gateway` is now
  `binary_order_gateway`. The old names said which wire format a gateway spoke, or
  didn't, but neither said what kind of gateway it was — and order entry is not the
  only kind a venue needs. A gateway carrying nothing but risk-parameter changes
  should not have to compete with members placing orders, and naming the existing
  pair for what they actually are leaves room for that without a second rename
  later. The change runs all the way through: directories, binaries, CMake targets,
  namespaces, class names, config files, component names, deployed paths and docs.

### Fixed

- **A FIX logon arriving before the gateway's sequencer links were up waited five seconds for a
  session the venue had already granted.** The gateway sends the Logon reply only once it knows
  where the session's numbering stands, which it learns by announcing the binding to the
  sequencer. That announcement is a PDU, and a member logging on while the gateway was between
  outbound connect retries had it dropped for want of anywhere to send it — with nothing to ask
  again when the links came up milliseconds later. The session was not lost: a five-second
  timeout opened it regardless. But it opened it on numbering the sequencer never confirmed, and
  the gateway's own warning says what that costs — *a member expecting a higher number will see a
  low sequence and disconnect*. The gateway now re-announces every session still awaiting its
  numbering as each sequencer link is established, down the link that has just come up. The
  five-second path remains as a backstop and is no longer the ordinary way out of the race.
  Measured against a reproduction that starts the venue with no sequencer at all: the retry goes
  out 14µs after the link comes up and the session establishes 756µs later, where the fallback
  would not have fired for another second.
  - The re-announcement is deliberately not sent to both sequencers. One arriving at a leader
    that already holds the binding is read as the previous session having died, which raises the
    resume figure by the reports since the last one plus an allowance — right for a real
    failover, wrong for a retry.
  - **`ha_test.py` could not have caught this even with a longer wait.** Its logon budget was
    three seconds against that five-second fallback, so the scenario was failed two seconds
    before the venue had finished trying, and reported as a timeout — which points at the
    gateway or the authentication service rather than at what happened. Raising the budget alone
    would only have taught the test to bless the degraded path, so both halves were done
    together: the budget is now eight seconds and documented as an upper bound rather than an
    expectation, and a session that establishes only because the fallback fired is now reported
    as its own outcome and fails the scenario. A session opened on unconfirmed numbering was
    previously indistinguishable from a healthy one at the instant of logon; the two diverged
    later, on the member's side, where no test was looking.
- **The coverage baseline named the directory the project used to live in.** `coverage_baseline.py`
  trimmed the checkout's path prefix off each file by searching for the literal
  `pubsub-project-10-copilot/`. Once the project was moved out of that directory the marker
  matched nothing, every path stayed absolute, and all 302 files read as new — so the release
  check reported a stale baseline and said not to tag, when coverage had not moved at all
  (function coverage identical, four lines' difference across two files the tool already
  describes as run-to-run variance). The prefix is now taken from the script's own location, and
  both sides are resolved through symlinks so a checkout reached by one still matches. A named
  directory was only ever going to be right until the project was moved.
- **The Rocky container deployed its gcc-8.5 binaries over the development host's install tree.**
  `devsetup.py`'s build, release and stop steps qualify their directory by target platform;
  its deploy step took the destination from the env TOML, where `install_dir = "installed"` is
  unqualified — correct for a real target host, wrong as the last step of a sequence whose other
  three had agreed on `installed-rocky8/`. Because the release check bind-mounts the repository
  into the container, the deploy overwrote the host's binaries with ones whose `RPATH` points at
  a path that exists only inside the container, and they then failed to start with exit 127. The
  stage that caused it passed; the damage surfaced in the two stages that ran afterwards.
  `devsetup.py` now resolves the staging directory once and passes it to `deploy.py`. On the
  development host it resolves to `installed`, so nothing there changes.
- **The release check asks whether the system can start before spending ten minutes proving it
  cannot.** A new `runnable` stage runs `ldd` over every installed binary, under the same library
  path `devenv.py` launches components with, and reports the unresolved names. Without it, a tree
  of binaries from the wrong platform surfaced as `ha` reporting 0 of 23 scenarios failed — which
  reads as a catastrophic regression in the high-availability code, and was every component
  dying at startup with exit 127. The stage takes 0.2 seconds.
- **A release artefact now says which platform it was built for.** The name carried no platform
  tag, so the container's tarball sat in the shared `release/` directory indistinguishable from a
  host build — and both scripts that deploy one picked "the newest", which after a release check
  is the container's. `release.py` appends the platform tag, and `devsetup.py` and
  `build-release-deploy.py` select by platform rather than by date. `build-release-deploy.py`
  additionally deleted the unqualified `installed/` before building, so in the container it would
  have removed the host's tree outright. Names on the development host are unchanged: only a
  cross-compiled artefact is qualified, as with the staging directory.
- **`perf_run.py --gateway fix` could never have run.** `run_fix8_session()` referenced
  `prefix`, which is a local of `main()` and was never passed in, so the FIX performance path
  died with `NameError` the moment it reached the matching engine's metrics port. Found by
  `release_check.py`, which is the only thing that runs it.
- **Eleven scripts had a usage block that was not their docstring**, including `deploy.py`,
  `devenv.py`, `ha_test.py` and `release.py`. A `from __future__ import annotations` preceding
  the triple-quoted block demotes it to an ordinary string expression, so `__doc__` was `None`
  — and eight of those scripts pass `__doc__` to argparse as the description. Their `--help`
  printed usage and options with no explanation of what the script was for.
- **The slab allocator had a hard limit on how much a process could ever receive.** Registry slots
  were issued monotonically and never reused, and the slot indexes a fixed directory -- fixed
  because deallocating threads read it without a lock -- so 262,144 slab rotations was a ceiling on
  the bytes a process could handle in its lifetime: **16 GiB at the then-default slab size**. A
  gateway put roughly 14.3M messages through in 113 minutes. Slots are now recycled through an
  intrusive free list, and `SlabHandle` carries a generation so a handle outliving its slab is
  rejected rather than freeing into the slab that replaced it. Measured: 100,000 messages and
  1,000,000 messages now cost the same three slots.
- **The empty-slab drain tripwire fired on elapsed time alone.** A machine under memory pressure
  descheduled the gateway's reactor thread, and the tripwire threw on its first iteration against
  an empty queue -- blaming "a corrupted lock-free queue state" while its own diagnostics reported
  nothing of the sort. The gateway died and its order-entry port with it. The condition now
  requires both a spent budget and a loop that has actually spun.
- **The idle-connection reaper tore down the pre-warmed failover link.** The matching engine's
  order listener is pre-warmed so a promoted secondary can reconcile without a connect delay, but
  on the secondary it carries no data, so the reaper closed it every 600 seconds -- destroying the
  pre-warming it exists to provide, and logging a WARNING every ten minutes in a healthy venue.
- **The band chart drew a line through intervals with no data.** Prometheus omits empty regions
  from a range query, so two samples three hours apart were adjacent points and the line between
  them read as steady latency across a period when no venue was running.
- **`--application` never reached metric discovery.** Its default bound at import, so the flag set
  a global, was reported faithfully in every message, and never reached the query.
- **The band chart's breach count could be hidden by its own bars**, reading 20,354 where the
  value was 720,354 -- an order of magnitude, to a glancing reader.
- **A build directory left behind by a renamed target broke coverage capture**, with an error that
  named standard-library headers and pointed nowhere near the cause.

- **`ha_test.py` scenario 17 opened its fresh session under the baseline comp id.** Since the
  matching engine keys an order on `(comp id, protocol, ClOrdID)`, and f8test numbers its
  ClOrdIDs from one in every process, every recovery order collided with one still resting on
  the book and was correctly rejected as a duplicate. The fresh logon now uses its own comp id.
  A regression introduced by the step 5 re-keying and missed at the time, because the step 5
  sweep did not include scenario 17.

## [0.2.1] - 2026-07-31

A patch release fixing three separate failures on the RHEL8 target. **v0.2.0 does
not build on RHEL8**; this release does. There are no functional changes.

All three shared a cause: each was a property of the *target toolchain* rather than
of the source, so a development machine could not see any of them, and no unit test
could have caught them.

### Fixed

- **`fix_codec` did not compile under gcc 8.5.** `FixReject::describe` used
  `snprintf`, and gcc 8.5 rejects it under `-Werror=format-truncation` at a call
  site with a small constant capacity — which the test suite deliberately
  provides, because truncation there is the documented behaviour. Replaced with
  `fmt::format_to_n`, which has the same bounded, non-allocating contract but is
  not a printf format string. Newer gcc does not emit the diagnostic at all, which
  is why the development build stayed green.
- **The Python lint gate disagreed between machines.** pylint 3.3 split
  `too-many-arguments` into a total count (`R0913`) and a positional count
  (`R0917`); a `disable` comment written before the split no longer covered the
  new check. `_emit_dump_value` now takes keyword-only arguments, which satisfies
  the check honestly rather than widening the suppression — `expr`, `out` and
  `indent` are all strings, so a transposition at a call site would silently emit
  wrong C++.
- **The pybind11 round-trip tests failed to load their extension.** The harness
  compiled a shared object into the system temp directory and `dlopen`ed it from
  there; `/tmp` is mounted `noexec` on the target, so the mapping was refused and
  every test failed with `failed to map segment from shared object` after building
  perfectly. The extension is now built under the project's own build tree, which
  also satisfies the rule that a build produces nothing outside the project
  directory. The load failure now explains itself, naming `noexec` and SELinux,
  rather than repeating dlopen's cryptic wording.

### Changed

- **The printf family is banned project-wide in favour of fmt**, with the rule in
  `coding-rules-for-ai-chatbots.txt` and enforcement as `check_standards.py`
  check 26. All 84 remaining call sites converted. A printf format string is
  unchecked against its arguments, so a mismatch is undefined behaviour rather
  than a diagnostic. The rule names which member of the family to use —
  `format_to_n` for a caller's buffer, `format_to` to append, `print` for a
  stream, and `format` only where a `std::string` was wanted anyway, since that
  one allocates — and carves out signal handlers, where neither fmt nor printf is
  legal and `write(2)` is the only correct answer.
- **`pylint` and `pybind11` are now pinned in the Python dev extras.** Neither was.
  pybind11 was not even declared, despite three test modules being unable to run
  without it, which is why it was simply absent on the RHEL8 host and why two
  machines ended up four major versions apart.

### Added

- **`release_check.py`** — a pre-release gate, deliberately not part of a normal
  build. It checks the working tree is committed, that the version agrees across
  `CMakeLists.txt`, both README references and the CHANGELOG, runs the coding
  standards and a full build with nothing skipped, and **builds under RHEL8's
  gcc 8.5 in the Rocky container**. It states plainly what it cannot cover: the
  container shares RHEL8's gcc but not its Python, so it would have caught the
  first of the three failures above and neither of the others.

### Known limitations

Unchanged from 0.2.0: the order gateway remains a single point of failure,
in-flight execution reports do not survive a client reconnect, hot-path cores are
allocated but not quiet, and `ResendRequest` / `SequenceReset-GapFill` remain
untested under load. See the 0.2.0 entry for detail.

## [0.2.0] - 2026-07-30

Second tagged release. The headline is a **second order-entry gateway**, which makes the
venue multi-gateway for the first time, and a **declared CPU core layout** that replaces
greedy, start-order-dependent core claiming. FIX message construction is now derived
entirely from a FIX data dictionary rather than hand-written, and both web UIs have lost
their CSS framework.

### Added

- **Binary order gateway** (`applications/binary_gateway`) — a second front door onto the
  same venue, speaking internal DSL-generated PDUs with no FIX layer, authenticated with
  SCRAM-SHA-256. Ships with a load generator, and the fix-test-client gained a gateway
  selector so a session can be pointed at either. Multi-gateway execution-report routing
  works via `origin_gateway_id` on the `WalRecord` envelope, covering WAL replay, the
  matching engine book, and `BookUpdate`.
- **Declared CPU core layout** — a per-machine manifest in the environment TOML declares
  which components run on each host, and a machine-invariant `hot_path_rank` declares the
  order in which entitlement to a good core is surrendered, with whole rank groups admitted
  atomically. `deploy.py` runs on the target host, reads the real topology and resolves rank
  to core ids, so one declaration works on a 32-core workstation, a 20-core machine or a
  small VM. Every process now starts masked to a background pool and the reactor explicitly
  *promotes* the threads that earn hot-path cores, so forgetting a thread is harmless
  instead of silently costly. `CpuRegistry` is reduced from an allocator to a record and
  cross-installation collision detector.
- **`cpu_audit.py`** — checks every running thread's real affinity mask from `/proc`
  against the layout, and classifies what else can run on the hot-path cores by what can be
  done about it (per-CPU kernel threads, steerable IRQs, ordinary userspace needing
  `isolcpus`). `--strict` exits non-zero, so a measurement run can be gated on a quiet
  machine.
- **FIX PDU generation from the data dictionary** — `dd_to_dsl` generates full-message DSL
  from `applications/fix_orders.dd.xml` at build time, and the pipeline now runs on
  100%-DD-derived PDUs. FIX repeating groups (`NoUnderlyings`, `NoPartyIDs`) are carried
  end to end, echoed onto the execution report by the matching engine.
- **`WalRecord` as the pipeline envelope** — routing metadata (`gateway_session_conn_id`,
  `origin_gateway_id`) travels on one envelope on the wire and on disk, rather than being
  re-derived per hop.
- **Repeating-group-aware FIX validation** in `fix_codec`, and a reusable repeating-group
  walker that de-duplicates group descent.
- **`callgrind_run.py`** performance tooling; `topic_probe` now subscribes to a list of
  topics including all.
- **fix-test-client order entry** — a full New Order Single form with repeating groups, a
  blotter showing session and admin messages with a heartbeat toggle, and a message-type
  column.

### Changed

- **Both web UIs dropped Pico.css.** The fix-test-client needed no replacement — its own
  `style.css` already was a dense native-desktop look, and Pico had been layered on top of
  it with a third stylesheet to undo the damage. The admin service, which genuinely
  depended on Pico's classless styling, gained a hand-written `static/desktop.css` styling
  the same markup; no templates changed beyond the stylesheet link. Operator branding via
  `brand.css-file` continues to work through documented custom properties that replace the
  `--pico-*` variables it used to target.
- **Gateway logging made symmetric** across the FIX and binary gateways, so a comparison
  between them measures protocol rather than log volume.
- **Timers identified by integer `TimerID`**, dropping timer names from the hot path.
- **Service names interned to a `ServiceID`** off the reactor command hot path.
- **Generated code moved out of the source tree** — FIX PDU codegen renamed off "equity"
  and emitted into the build directory, so a clean build fully regenerates.
- **Doxygen runs without `dot` on the RHEL8 family**, for much faster documentation builds.
- **TLS status corrected** in `docs/design/secure_comms.md`: TLS is implemented with
  OpenSSL and in use — the order gateway exposes an encrypted FIX listener via `[fix_tls]`
  and the authentication service listener is TLS-secured with optional mutual TLS —
  superseding the earlier "ready to wire up" note. The README gained a Security (TLS and
  SCRAM) section, version/licence/C++ badges and a current-version line.

### Fixed

- **Order gateway dropped echoed repeating groups** from the client-facing execution
  report, and used a fixed-size encode buffer for it. Now growable.
- **A `FixField` was used after the underlying cursor advanced** during group extraction.
- **Deployed paths could escape the install tree**, and CPU pinning could fail silently.
- **Two unresolvable anchor links and three further gaps** were making the Doxygen build
  red; `doxygen` now runs clean in the Rocky image under `WARN_AS_ERROR`.
- **Coverage reporting overstated function coverage.**
- **Admin service: New User and Edit role returned HTTP 500.** `users/form.ftl` used a
  `?:` ternary, which Freemarker does not have, so the template never parsed. Neither page
  had ever rendered.
- **fix-test-client blotter row colours** had been painted over for a month by an opaque
  table-cell background.

### Removed

- **Fixed-size encode buffers**, everywhere they remained, replaced by measure-then-fit or
  grow-and-retry against a reusable buffer. A `check_standards` guard now fails the build
  on new ones.

### Documentation

- **[Gateway High Availability](docs/design/gateway_ha.md)** — a new design document
  recording the agreed direction for gateway redundancy: sessions **pinned to a primary and
  backup instance** rather than pooled any-of-N, and in-flight execution reports surviving
  a reconnect. **This is direction, not code — it is planned for 0.3.0 and none of it is
  built in this release.** The gateway remains a single point of failure.
- The **"N-way pooled redundancy"** claim in `wal_and_ha.md` is **withdrawn**. It was never
  implemented, and it stopped matching the code when execution-report routing moved to the
  gateway-local `gateway_session_conn_id`.
- New design documents for CPU pinning and its anti-affinity problem; the roadmap's
  near-term list pruned of items already done.

### Known limitations

- **The order gateway is a single point of failure.** One instance is run, and the code
  cannot yet express a second: `origin_gateway_id` names a protocol rather than an
  instance, and the sequencer's gateway endpoints are scalars. See the design document
  above.
- **In-flight execution reports do not survive a client reconnect**, even to the same
  gateway. There is no outbound message store; every `ResendRequest` is answered with a
  blanket `SequenceReset-GapFill`. Cancel-on-disconnect *is* implemented, so orders do not
  linger on the book behind a dead session.
- **Hot-path cores are allocated but not quiet.** An affinity mask reserves a core *for* a
  thread; it does not reserve it *from* anything else. On a workstation without `isolcpus`,
  interrupt handlers and ordinary userspace threads still share those cores. Run
  `cpu_audit.py --strict` before trusting any latency measurement.
- **`ResendRequest` / `SequenceReset-GapFill`** remain untested under load.

## [0.1.0] - 2026-07-23

First tagged release. `pubsub_itc_fw` is a low-latency, multi-threaded, event-driven
C++17 application framework built around the reactor pattern.

### Added

- **Reactor core** — an epoll-driven event loop with CPU-pinned application threads,
  inter-thread communication over lock-free MPSC queues, and `timerfd`-based timers.
  No heap allocation on the hot path: pool, bump, and slab allocators throughout, with
  zero-copy inbound and outbound PDU paths.
- **Write-ahead log (WAL)** — a segmented, mmap-backed, single-writer append-only log
  with a scan-from-zero / stop-on-damage replay model and a cursor abstraction. Shared
  as a primitive by the sequencer and the topic publisher.
- **Topic-based publish/subscribe** — fan-out with replay, backed by the WAL and paced
  by TCP (no per-record acks; the WAL is the backlog). Page batching, explicit and
  recoverable slow-consumer handling, a separate control channel, and leader-only
  publishing. Topic identity is generated from the DSL. The Matching Engine Publisher
  (MEP) is the reference publisher; `topic_probe` and the `TopicSubscriberThread` base
  are the reference subscribers.
- **High availability** — component-specific composition of shared primitives:
  arbiter-elected leader/follower with lease + epoch fencing, point-to-point WAL
  replication with two-tier commit, a PSA + witness arbiter pool, cancel-on-failover for
  the matching engine, an N-way gateway pool, and an active/active authentication service.
- **Binary serialisation DSL** — a Python code generator emitting self-contained C++17
  (and Java) encode/decode headers with zero-copy decode on little-endian hardware,
  deterministic wire sizes, and allocator-friendly decoding; sub-100 ns round-trip on
  typical messages. Includes generated structured `to_string` / `toString` dumps.
- **`fix_codec` library** — an hffix-style zero-copy FIX reader, writer, and
  dictionary-driven validator (SessionRejectReason rules, per-message permitted-tag
  bitset, perfect-hash tag lookup), generated from FIX XML data dictionaries.
- **Secure communications** — TLS via OpenSSL memory BIOs and SCRAM-SHA-256 authentication.
- **Sample applications** forming a minimal exchange-system skeleton: order gateway
  (FIX 5.0 SP2 connectivity), sequencer, matching engine, matching engine publisher,
  authentication service, admin service, arbiter, witness, `topic_probe`, and a Java
  fix-test-client.
- **Documentation** — a design/reference documentation set under `docs/`, including the
  pub/sub and WAL designs, and a Doxygen API reference.
- **RHEL8 build support** — verified to build and test on the RHEL8 target (Rocky 8,
  gcc-8.5, JDK17, Maven 3.5.4) via a Docker image: 731 C++ tests, 246 DSL tests, and
  both Java artifacts build cleanly.
