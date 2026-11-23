# Phase 12: the open bugs judged against the principles {#bug_review}

This is the working record of phase 12 of the [recovery plan](recovery_plan.md). Every entry in the
[bug list](bug_list.md) that is open is judged here against the design principles in
[design_principles.md](design_principles.md) and against the code as it is now. The review fixes nothing
itself: what it gives is a judgement for each open entry, with the evidence for it, and an order of work
agreed with Andrew. An entry that is closed is no longer listed here; its record is in the bug list.

## Summary

There are 47 open entries, and each is judged below. The figures:

| Judgement | Entries |
|---|---|
| Breach of a named principle | 7: BUG-0113 (what is left of it), BUG-0285, BUG-0131, BUG-0273, BUG-0127, BUG-0128, BUG-0119; and 2 more if their causes prove to be in the venue: BUG-0175 and BUG-0184 |
| Partly done | 5: BUG-0121, BUG-0134, BUG-0279, BUG-0283, BUG-0069 |
| Decision needed from Andrew | 1: BUG-0229 |
| Decided, and waiting to be built | 1: BUG-0048, with the recovery service of BUG-0113 |
| New requirement | 13 |
| Investigation | 11 |
| Test or tool | 8 |
| Defect that breaks no principle | 1: BUG-0259 |

**The decisions that would unblock the most.**

1. **What the recovery service does with reports a member never received** (BUG-0113). How long a member's
   execution reports are kept for resends is decided (R-0008, 8 November 2026): the recovery service keeps the
   most recent 100,000 messages for each comp id, and answers a resend request for anything older with a FIX gap
   fill. That settles resends, which ask again for messages already sent. It does not settle reports produced
   while a member had no connection, which were never sent and cannot be answered with a gap fill. **Decided on 22
   November 2026: the recovery service keeps every report not yet delivered to a session, however many**, its room
   sized from the open-order capacity. This decides the recovery service's design, and through it BUG-0048 (reclaiming the WAL during the day), BUG-0121's
   start-up time, BUG-0209's scenario and BUG-0006's measurement.
2. **What happens to members' orders when the venue's own gateways are down** (BUG-0281). **Decided on 22 November
   2026: the cancel-on-disconnect grace period runs on whatever the cause,** as it does now.
3. **How long the leading sequencer holds a report for a session connected nowhere** while a configured gateway
   instance has not announced its sessions (BUG-0258). **Decided on 22 November 2026: only a session still open is
   held;** an ended session's reports go to the WAL and reach the member when it logs on again. Built on 23 November,
   and BUG-0258 is closed.
4. **Which of two remedies for repeated reports from a restarting engine** (BUG-0250). Decided on 22 November 2026,
   and found unnecessary on 23 November: the flood does not happen, and BUG-0250 is closed.

**The order of work**, agreed with Andrew on 22 November 2026:

1. The small breaches, each with an obvious remedy: BUG-0272, BUG-0249 and BUG-0286 are closed.
2. The partly done entries with little left: BUG-0278 and BUG-0065 are closed, halting one instrument having moved
   to BUG-0242; BUG-0262 is closed; BUG-0279's scenario is written, and Andrew decided on 22 November 2026 that the
   FIX gateway is bounded as the binary one is, by its thread stopping taking orders from a member's bytes while the
   limit on unanswered commands is reached.
3. The four decisions above, each put to Andrew with its background and options. All four decided on 22 November 2026.
4. The breaches that need a short design first: BUG-0127 and BUG-0128 (the framework's two locks), BUG-0119
   (shutdown), BUG-0273 (the matching engine's held reports) and BUG-0131's remainder.
5. BUG-0121 (start-up time), then the recovery service with BUG-0048 and BUG-0113, the largest piece of work.
6. Then the new requirements, the investigations and the tests and tools, in the order Andrew chooses.

## How each entry is judged

Each entry is given one of these judgements. The evidence column says what was checked, in the code or the
documents, to reach it.

| Judgement | Meaning |
|---|---|
| **Fixed** | The defect is gone, or the task is done. The entry can be closed once Andrew agrees |
| **Partly done** | Some of what the entry asks for is built; what is left is stated |
| **Breach** | The code breaks a named principle. These come first in the order of work |
| **New requirement** | The code breaks no principle, but the venue lacks something it should have. Whether and when to build it is a decision |
| **Decision needed** | The entry cannot go further until a question is answered, which is stated |
| **Investigation** | The entry asks for something to be found out, not built |
| **Test or tool** | The defect is in a test, a script or a tool, not in the venue |
| **No longer applies** | What the entry describes no longer exists in the code |
| **Defect** | A defect that breaks no principle; small, and placed in the order of work with the rest |

The entries are grouped by design area, not by number. Within an area they are listed in the order the
review recommends working on them.

## The areas

1. Leadership, leases and the arbiters (section 1)
2. The write-ahead log, its retention, the disk, and the trading day (section 2)
3. The matching engine (section 3)
4. The sequencer's handling of reports (section 4)
5. The gateways and what a member sees (section 5)
6. The framework: reactor, timers, allocators and threads (section 6)
7. Tests and tooling (section 7)
8. Operations, deployment and metrics (section 8)

## 1. Leadership, leases and the arbiters

Five entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0121](bug_list.md#bug_0121): a restarted sequencer takes longer to start than its peer's lease | **Partly done** | The record of command identifiers is built in the background (`LoggedCommandIdentifiersBuilder`). `ReplicatedWal::open` still reads the whole WAL, because the table of epochs needs every record's epoch and the check for a gap needs every record's number. So a supervised restart late in a busy day is not back within the lease, and rule 6's promise that a restarted leader keeps the lead does not hold then | The decided change, not yet designed: keep the table of epochs in a small file written when the epoch changes, and check for gaps only after the last snapshot |
| [BUG-0134](bug_list.md#bug_0134): a venue running without a standby says so too quietly | **Partly done** | The gauge `sequencer_running_alone` covers both ways of running without a standby; starting to do so is logged at Error, and the Error is repeated once a minute with how long it has lasted and how many records exist only on the leader (`ReplicatedWal.cpp`) | The dashboard's alert |
| [BUG-0135](bug_list.md#bug_0135): while the leader trades alone, a power failure can lose the last seconds of the record | **New requirement** | Nothing syncs the leader's WAL at an interval while it runs without its follower; there is no such interval in `ReplicatedWal` or `Wal` | Build it as the entry describes, on a thread of its own (principle F4), if Andrew agrees |
| [BUG-0222](bug_list.md#bug_0222): no operator procedure lets the follower lead after the old leader's machine is lost while it ran alone | **New requirement** (R-0161, R-0162) | Not built; [a_follower_behind_does_not_lead.md](availability/a_follower_behind_does_not_lead.md), section 4.4, says so | The operator's tool and the procedure for telling members, designed together |
| [BUG-0136](bug_list.md#bug_0136): whether a copy of the WAL elsewhere could recover what the leader's machine took with it | **Investigation** | The order activity recorder is not built, and nothing else keeps a copy of the WAL away from the two sequencers' machines | Find out what each follower of the WAL keeps; then decide whether recovering from it is worth designing. Best done with BUG-0222 |

**Recommended order in this area.** BUG-0121 first, because without it the rule that a restarted leader keeps the
lead fails late in a busy day. BUG-0134's alert is small. BUG-0135, and BUG-0222 with BUG-0136, are new
requirements to be placed in the order with the rest.

## 2. The write-ahead log, its retention, the disk, and the trading day

Four entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0113](bug_list.md#bug_0113): a resend or a request for undelivered reports can read far back in the WAL on the thread that sequences orders | **Breach** of F4, accepted for now and permitted by name in `scripts/check_standards.py` | Two reads remain on the sequencer's thread: `handle_session_replay_request`, a member's resend, and `handle_undelivered_reports_request`, the reports produced while a member had no connection. Each reads segments from the newest backwards and stops when it has what it needs, so a resend after a reconnect reads a few segments; one reaching far back, or a member away most of the day, reads much of the day's WAL. A matching engine's catch-up is read on the thread that follows the WAL and is not part of this entry | The recovery service, with the 100,000 messages for each comp id decided under R-0008, and every undelivered report kept, as decided on 22 November 2026 |
| [BUG-0048](bug_list.md#bug_0048): the WAL keeps the whole day | **Decided**, and waiting to be built | `Wal::truncate_below` is called by nothing, and nothing reclaims the WAL during the day. Of the positions retention depends on, the matching engine's position is not reported to the sequencer, and members' resends are to be served by the recovery service's own store | The recovery service built; the engine's position reported; and retention anchored to the lower of the follower's acknowledgement and the engine's checkpoint |
| [BUG-0175](bug_list.md#bug_0175): while the disk is slow, the leading sequencer's own thread waits in the file system's journal | **Investigation**; a **Breach** of F4 if confirmed | Not reproduced since the WALs moved to a disk of their own mounted with `lazytime`. Finding the call needs the scheduler's events with kernel stacks, which `perf` records without root when attached to one thread (the method recorded under [BUG-0228](bug_list.md#bug_0228)) | Reproduce under a slow disk with that method, then take the call off the thread or show that F4 permits it |
| [BUG-0264](bug_list.md#bug_0264): under overload the follower sequencer still falls behind its leader | **Investigation** | In an overload of 68,626 orders a second the leader judged its follower too far behind nine times, because its storage for orders waiting on the follower was full, and ran without it for 3,963 ms of a 26.2-second run. The leader's reactor is idle about half the time, so what holds the follower back is not known | Profile the follower in the same overload to find the stage that limits it; then keep it in step at the rate the leader sustains, or state that rate as the venue's ceiling |

**Recommended order in this area.** BUG-0113 and BUG-0048 wait on the decision about undelivered reports and then on the recovery service.
BUG-0175 and BUG-0264 are investigations to schedule.

## 3. The matching engine

Four entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0287](bug_list.md#bug_0287): a matching engine that asks to lead while it is still catching up has been seen never to finish catching up or to follow | **Investigation**; a **Breach** of V11 if confirmed | In two runs of scenario 129 the following engine, restarted with its region damaged and its lease promises forgotten, began replaying the day, asked to lead after its lease period, gave up when its peer held the votes, and then logged nothing more about its catch-up and never followed in the 11 seconds left; in two other runs it finished within its lease period and followed | Reproduce with a longer wait, find the cause from its catch-up and lease state together, and a scenario that makes it ask to lead during its catch-up every time |
| [BUG-0273](bug_list.md#bug_0273): a matching engine holding reports allocates from the heap for each | **Breach** of F5 | `EngineReportSending` keeps held reports in a `std::vector<HeldEngineReport>`, and each `HeldEngineReport` holds its bytes in a `std::vector<uint8_t>`; `discard` gives the memory back with `shrink_to_fit`. Reports are held only outside ordinary running: with no connection to either sequencer, or while the engine catches up before it knows whether it serves | Storage of a fixed size allocated at start, as for the reports kept while following ([BUG-0271](bug_list.md#bug_0271)); the design must say what happens to a report sent on one connection and waiting for the other, since held reports are not always released oldest first |
| [BUG-0209](bug_list.md#bug_0209): no scenario reaches the halt when an engine cannot account for the venue's orders | **Test or tool** | The case cannot be reached while the WAL keeps the whole day: a follower that loses its first segment file is sent every record again, and an instance running alone refuses a WAL with a gap | A scenario, possible once BUG-0048 is done |
| [BUG-0216](bug_list.md#bug_0216): sending every record to the follower engine adds about 2 microseconds at the median | **Investigation** | One run of each build was compared; the spread between runs of one build is not measured | Measure that spread first; only then decide whether to reduce it |

**Recommended order in this area.** BUG-0287 first: a standby that may never become able to take over matters more than
the rest. Then BUG-0273's design, with the framework's breaches in section 6. BUG-0209
waits on BUG-0048. BUG-0216 is a measurement.

## 4. The sequencer's handling of reports

Two entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0285](bug_list.md#bug_0285): while the leader stops waiting for its follower, it sends execution reports whose records are not yet committed | **Breach** of V2 | `forward_report_from_engine` sends a report at once whenever `waits_for_follower()` is false, but `ReplicatedWal::advance_commit` commits nothing while the leader runs without its follower until a voter confirms that the follower may not lead; in that time, about a second, the follower could still be elected without the record | A report released only once its record is committed, as a change of phase is (BUG-0249), with the delay to members' reports at those moments stated and agreed; a scenario that slows a voter's statement write so the time is seconds long |
| [BUG-0131](bug_list.md#bug_0131): the leading sequencer allocates from the heap for every order, and copies waiting reports | **Breach** of F5 and F6 | The waiting reports are in a ring allocated once, each report in the slab chunk it arrived in, and reports are routed by the session they carry, so no map of orders grows through the day | Lock the record of command identifiers into memory at start ([the_sequencer_holds_its_memory_from_the_start.md](venue/the_sequencer_holds_its_memory_from_the_start.md)), and compare a latency run with the last |

**Recommended order in this area.** BUG-0285 first, reusing the rule and the scenario that closed BUG-0249. Then
BUG-0131's remainder through its agreed design.

## 5. The gateways and what a member sees

Eleven entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0279](bug_list.md#bug_0279): a gateway's limit on unanswered commands does not bound the commands it has waiting to be sent | **Partly done** | The binary gateway limits the messages waiting from each member's connection to 64; both gateways refuse a logon beyond `[members] most_connected`; every environment sizes the binary gateway's command queue from those settings, and in scenario 116 the queue no longer grows | The FIX gateway's thread stops taking orders from a member's bytes while the limit on unanswered commands is reached, as decided on 22 November 2026; its command queue sized from settings; a scenario that overloads it. Scenario 127 shows a member beyond the limit refused by each gateway; under the most the fix8 clients can offer, short of an overload, the FIX gateway's command queue held at most 10,529 commands |
| [BUG-0259](bug_list.md#bug_0259): under a steady flood, the warning that a gateway's store of unanswered commands is full is logged once for every command refused | **Defect** | `UnansweredCommands::keep` logs a Warning when the store becomes full and an Info when it has room again; under a steady flood one answer makes room for one command, so the two alternate once for each command | The warning said once when commands begin to be refused, and once when an interval passes with none refused, with how many were refused, as `KeptReports` does |
| [BUG-0203](bug_list.md#bug_0203): the FIX gateway sometimes refuses a logon as an impostor when two are under way together | **Investigation** | Not reproduced since it was recorded; the cause is unknown | Reproduce with many simultaneous logons, as at the start of the day, and find the shared state |
| [BUG-0281](bug_list.md#bug_0281): what happens to members' orders when their gateways are down for longer than the grace period is not decided | **New requirement**, the main decision made | The sequencer starts each session's cancel-on-disconnect grace period when its gateway is lost, whatever the cause, and cancels the session's day orders when it runs out ([gateway_ha.md](availability/gateway_ha.md)) | The grace period runs whatever the cause (decided 22 November 2026). Left: how the venue detects that no gateway has come back, whether it halts or alerts an operator, and scenarios. It depends on BUG-0046 |
| [BUG-0089](bug_list.md#bug_0089): a member cannot ask the venue what it is holding | **New requirement** (R-0002) | There are no order status or mass status messages in the dictionary or the code | Order status and mass status requests, answered from the book |
| [BUG-0046](bug_list.md#bug_0046): the binary gateway has no recovery of reports in flight | **New requirement** | The binary protocol has no resend | A recovery for the binary protocol, designed with the recovery service of BUG-0113 |
| [BUG-0045](bug_list.md#bug_0045): a member has no defined way to discover that its primary gateway is down | **New requirement**, an open design question | Nothing in the member documentation says what a member should do | A statement of what a member should do, in the member documentation |
| [BUG-0059](bug_list.md#bug_0059): no defence against a member reconnecting in a loop | **New requirement** | The entry sets out the graduated answer; nothing of it is built | Place in the order of work |
| [BUG-0058](bug_list.md#bug_0058): a member halted by a sequence gap is invisible to monitoring | **New requirement** | No gauge or counter records a session halted by a gap | The gauge the entry describes |
| [BUG-0242](bug_list.md#bug_0242): the venue has no list of the instruments it trades | **New requirement**, needing its own design | Any Symbol up to `fix_order_limits::max_symbol_length`, 35 characters, is accepted; nothing reads SecurityID (48) or SecurityIDSource (22) | A design, discussed with Andrew first: where the list comes from, what identifies an instrument on an order, how the list changes during a day, what options on futures need, and whether one instrument can be halted |
| [BUG-0265](bug_list.md#bug_0265): the Java test client's blotter does not show the trading-session status its member is told | **New requirement**, in a tool | The blotter in `java/fix-test-client` does not handle 35=h | Ask for the status at logon, act on every 35=h, and show the state prominently |

**Recommended order in this area.** BUG-0279's change to the FIX gateway, as decided. BUG-0259 is small and can be done whenever `UnansweredCommands` is next touched. BUG-0281 waits on
Andrew's decision. BUG-0203 needs a stress test of many simultaneous logons. The new requirements are placed
with the rest.

## 6. The framework: reactor, timers, allocators and threads

Seven entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0283](bug_list.md#bug_0283): parts of the framework library that the venue runs every day are not reached by any test | **Partly done** | Groups 1 to 3 have tests, each made to fail by breaking the code it tests; the coverage baseline records 11,292 of 12,811 lines run | Group 1's reading of `/sys`, which needs a made-up `/sys` tree; then groups 4 and 5, the failures that change what the venue does |
| [BUG-0127](bug_list.md#bug_0127): a pool that runs out takes a mutex and maps a new pool while holding it | **Breach** of F2 and F5 | `ExpandablePoolAllocator` takes `expansion_mutex_` on its slow path | No lock on the allocation path, as the entry describes, and pools sized so that the slow path is not reached in a normal day |
| [BUG-0128](bug_list.md#bug_0128): every timer start creates a timer and every cancel destroys one, with a lock and system calls on the reactor thread | **Breach** of F2 and F3 | `Reactor` takes `timer_registry_mutex_`, and there is no way to create a timer apart from starting it | Timers created once and armed and disarmed afterwards; the design the entry asks for |
| [BUG-0119](bug_list.md#bug_0119): a thread still in a handler when its reactor shuts down uses the destroyed reactor | **Breach** of F8, shutdown is deterministic | Shutdown does not wait for a thread still in a handler, nor refuse to destroy the reactor while one may still run | Decide whether a reactor may ever be destroyed while one of its threads may still run, and make shutdown wait or refuse accordingly |
| [BUG-0184](bug_list.md#bug_0184): a paused connection sometimes went on delivering | **Investigation**, a **Breach** if the fault is in the reactor | The test fails about one run in four of the coverage build, as the entry records | Find whether pausing fails or the test's timing does, under load |
| [BUG-0104](bug_list.md#bug_0104): a connection whose reads were paused can stay stalled after they resume | **Investigation** | The kernel's rule for this case is not established | As the entry describes; best done with BUG-0184 |
| [BUG-0280](bug_list.md#bug_0280): huge pages are not used for the large tables the order path reads at random, and the allocators' huge-page path has never run on the development machine | **Investigation** | The development machine has no huge pages reserved, so `FixedSizeMemoryPool`'s `MAP_HUGETLB` path always falls back to ordinary pages there; no measurement shows misses in the cache of address translations to be a real share of the order path's time | Measure those misses under `perf stat` through a load run first; only if they matter, huge pages for the large tables; and the huge-page path exercised by a test on a machine with huge pages reserved |

**Recommended order in this area.** BUG-0127 and BUG-0128 first, the two locks on paths an order can
take, each needing a design agreed before code, BUG-0128 the larger. BUG-0119 with the shutdown sequence.
BUG-0184 and BUG-0104 together. BUG-0283's remaining groups by its own rule, testing the failures that change
what the venue does rather than every log line. BUG-0280 is a measurement.

## 7. Tests and tooling

Eight entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0223](bug_list.md#bug_0223): the fix8 test client sometimes logs on and exits without sending its orders | **Test or tool** | Seen in scenario 22 and scenario 12; the harness notices the exit at once | Keep the client's exit status and standard error when it happens, to find the cause |
| [BUG-0261](bug_list.md#bug_0261): the check that runs the matching engines on a copy of the WAL starts a sequencer that can write new commands into that copy | **Test or tool**, partly done | `run_engines_on_the_wal` in `ha_test.py` gives the copy's sequencer settings that keep it from writing the end of day, and scenario 116 waits for its sessions' cancels before it ends. Nothing keeps the copy's sequencer from writing the cancels of a session whose grace period passes during the check | The copy's sequencer writes nothing into the copy, so that every engine run sees the same records |
| [BUG-0253](bug_list.md#bug_0253): `binary_load_client --help` prints its help and then tries to connect to the venue | **Test or tool** | `parse_options` in `BinaryLoadClientMain.cpp` returns `true` for `--help`, the value it returns for a valid command line, so `main` goes on to connect | `--help` prints the usage and exits with status 0, doing nothing else |
| [BUG-0173](bug_list.md#bug_0173): no scenario withholds a record during a real catch-up | **Test or tool**, a missing scenario | No scenario does it | As the entry describes |
| [BUG-0014](bug_list.md#bug_0014): Python style warnings in the scripts, and a lint gate that ignores them | **Test or tool**, under way | Some of the warnings are dealt with; the gate does not fail on style messages | Finish the warnings, then make the gate fail on them |
| [BUG-0267](bug_list.md#bug_0267): `ha_test.py` is one script of 15,000 lines | **Test or tool** | The script has 15,943 lines, and its checks are in one function | A package, `scripts/ha_test/`, with the machinery every scenario shares in one module and each group of scenarios in its own, with the same command line and the same results for every scenario |
| [BUG-0068](bug_list.md#bug_0068): the specification states behaviour that almost nothing tests | **Investigation**, a record of coverage | `scripts/check_book_requirements.py` reports the figures on every build | Kept open as the record of coverage; worked with BUG-0229 |
| [BUG-0229](bug_list.md#bug_0229): the functional specification has not been checked against the code | **Decision needed**: Andrew has left it for now | Recorded so as not to be forgotten | When to audit the specification, and to specify the ordinary order lifecycle |

**Recommended order in this area.** BUG-0223 first, because a client that exits without sending its orders
makes a scenario's verdict harder to trust. BUG-0261 next, for the same reason in the check that runs the
engines on the WAL. BUG-0253 is small. BUG-0267 is a large change with no effect on what is tested, best done
when the suite is not otherwise being changed.

## 8. Operations, deployment and metrics

Six entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0069](bug_list.md#bug_0069): the sequencer, arbiters and witness report no metrics | **Partly done** | The sequencer registers its metrics, among them `sequencer_running_alone` and the measures of a slow disk. The arbiter, the witness and the authentication service register none of their own | Metrics for the arbiters (which is active, leases granted and refused, statements held) and the witness |
| [BUG-0266](bug_list.md#bug_0266): the administration service's desktop does not show the venue's trading phase | **New requirement** | The administration service subscribes to the `technical_events` topic (`TechnicalEventsSubscriber`), which carries every phase, but uses it only for the end of day, and no page shows the phase | The desktop shows the latest phase and the trading day |
| [BUG-0060](bug_list.md#bug_0060): microbursts are not measured | **New requirement** | Nothing measures them | Place in the order of work |
| [BUG-0006](bug_list.md#bug_0006): ResendRequest under load | **Test or tool**, a missing measurement | No trading-day run sends resends | Exercise resends in a trading-day run; best done after the recovery service of BUG-0113 |
| [BUG-0245](bug_list.md#bug_0245): in one trading-day run the 99th percentile of the first two phases was higher than in the runs before it | **Investigation** | One run only: 326 and 384 microseconds against 238 to 250 in three earlier runs | A second run, to see whether it repeats |
| [BUG-0047](bug_list.md#bug_0047): disaster recovery is not modelled | **New requirement**, deliberately deferred | Not designed | Nothing until Andrew says |

**Recommended order in this area.** BUG-0069's remainder first. BUG-0266 is small now that the subscription
exists. BUG-0245 needs one run. BUG-0006 waits for the recovery service.
