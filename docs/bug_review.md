# Phase 12: the open bugs judged against the principles {#bug_review}

This is the working record of phase 12 of the [recovery plan](recovery_plan.md). Every entry in the
[bug list](bug_list.md) that is open is judged here against the design principles in
[design_principles.md](design_principles.md) and against the code as it is now, after phases 3 to 11.
Nothing is fixed while the review is made: what it produces is a judgement for each entry, with the
evidence for it, and then an order of work agreed with Andrew for the entries that remain.

## How each entry is judged

Each entry is given one of these judgements. The evidence column says what was actually checked, in the
code or the documents, to reach it.

| Judgement | Meaning |
|---|---|
| **Fixed** | The phases already removed the defect, or did what the task asked. The entry can be closed once Andrew agrees |
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
6. The framework: reactor, timers, allocators and threads
7. Tests and tooling
8. Operations, deployment and metrics

## 1. Leadership, leases and the arbiters

Eleven entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0142](bug_list.md#bug_0142): the arbiter writes each changed statement to disk on its own thread | **Breach**, of the rule that code which renews leases never waits for the disk ([majority_leases.md](availability/majority_leases.md), rule 6: promise records are written on a thread of their own for exactly this reason) | `ArbiterThread::record_component_statement`, called for each lease request that carries a statement, calls `save_component_statements()`, which writes, syncs and renames the file through `ComponentStatementStore` on the arbiter's application thread, before it answers. BUG-0230 added the passive arbiter's records and `StatementHeld`, and kept the write where it was. In the development environment the file is beside `var/arbiter_*_lease_promise.state`, under the install on `/mnt/sda1`, the disk without the `lazytime` option, not on `/nvme` where the WALs moved | Write the statements on the arbiter's background recorder, as its own promises are, and send `StatementHeld` only once that write has finished. Move the arbiters' small synced files to the WAL disk, as the sequencers' and engines' were moved |
| [BUG-0207](bug_list.md#bug_0207): a sequencer that was down past the recovery limit can, after the whole venue restarts, cancel the book for an absence that did not happen | **Breach** of V11: a loss of resilience is not a loss of service | `EnginePresence` measures the absence at start-up from this instance's own `EngineRoutedStamp` and the first record of its WAL, and nothing else. The exchange the entry proposed for carrying the peer's stamp, `StatusQuery` and `StatusResponse`, is no longer sent by anything | Not reproduced yet: a scenario first. Then the leading sequencer measures from the later of its own stamp and its peer's; with the status messages gone, the peer's stamp has to travel another way, such as in the WAL's records |
| [BUG-0121](bug_list.md#bug_0121): a restarted sequencer takes longer to start than its peer's lease | **Partly done** | The record of command identifiers is built in the background (`LoggedCommandIdentifiersBuilder`). The whole WAL is still read at start-up: `ReplicatedWal::open` says so, because the table of epochs and the check for a gap need every record. So a supervised restart late in a busy day will not be back within the lease, and rule 6's promise that a restarted leader keeps the lead does not hold then | The decided change, not yet designed: keep the table of epochs in a small file written when the epoch changes, and check for gaps only after the last snapshot |
| [BUG-0134](bug_list.md#bug_0134): a venue running without a standby says so once | **Partly done** | The gauge `sequencer_running_alone` now reads 1 whenever the leader does not wait for its follower, including when no follower is connected, and starting to do so is logged at Warning for those two cases (`ReplicatedWal`, the reasons for committing without the follower) | The repeated Error while it lasts, with how long and how many commands exist only on the leader's machine; the dashboard's alert |
| [BUG-0167](bug_list.md#bug_0167): a lease promise record can take longer to write than a lease period | **Decision needed**: close, with the risk accepted, or keep open | The promise record is written in the background and refreshed five seconds before it is needed (`PairLeaseAgent`). BUG-0228, closed, measured the cause (a slow disk under load from other writers), moved the WALs and the small synced files to a disk of their own, made an instance write its epoch before asking to lead (rule 13), and added metrics for how long each record takes and how much lease is left. What remains is that a disk slower than a lease period still costs the lead, and is now reported | Andrew to decide whether the residual risk is accepted and the entry closed |
| [BUG-0029](bug_list.md#bug_0029): a process death on the same host takes the machine-death path | **Partly done**, and **Decision needed** on the target | A leader restarted by its supervisor within the lease period keeps the lead (rule 6), so a process death no longer moves the lead when a supervisor is running. It costs about 3.5 seconds of interruption for a matching engine, measured ([process_death.md](availability/process_death.md)), against a design target of 50 milliseconds that nothing now aims at | Whether the 50-millisecond target stands, and if so the design that would meet it (the book attached again in place, not rebuilt). BUG-0121 must be fixed for the sequencer's restart to stay within the lease late in the day |
| [BUG-0135](bug_list.md#bug_0135): while the leader trades alone, a power failure can lose the last seconds of the record | **New requirement** | Nothing syncs the leader's WAL while it runs without its follower; there is no such interval in `ReplicatedWal` or `Wal` | Build it as the entry describes, on a thread of its own (F4), if Andrew agrees |
| [BUG-0222](bug_list.md#bug_0222): no operator procedure lets the follower lead after the old leader's machine is lost while it ran alone | **New requirement** (R-0161, R-0162) | Not built; [a_follower_behind_does_not_lead.md](availability/a_follower_behind_does_not_lead.md), section 4.4, says so | The operator's tool and the procedure for telling members, designed together |
| [BUG-0136](bug_list.md#bug_0136): whether a copy of the log elsewhere could recover what the leader's machine took with it | **Investigation** | Unchanged. The order activity recorder is not built | Find out what each follower of the log keeps; then decide whether recovering from it is worth designing. Best done with BUG-0222 |
| [BUG-0061](bug_list.md#bug_0061): with high availability off, a secondary sequencer started alone leads, but no gateway can reach it | **Decision needed** | Unchanged: with high availability off, both gateways connect only to the primary sequencer (`connect_to_service("sequencer_primary")`, the secondary only when `ha_enabled`) | Which sequencer the gateways use with high availability off: a setting naming it, or whichever says it leads |
| [BUG-0062](bug_list.md#bug_0062): two instances that led with high availability off, and nothing notices when they are reunited | **New requirement**, reachable only once BUG-0061 is fixed | Unchanged | Detecting, when the instances come together with high availability on, that both have led, and halting rather than choosing one. Design it with BUG-0061 |

**Recommended order in this area.** BUG-0142 and BUG-0207 first, because each breaks a principle, and
BUG-0207 needs a scenario before it is fixed. Then BUG-0121, because without it the rule that a restarted
leader keeps the lead fails late in a busy day. BUG-0134's remaining reporting is small. BUG-0167 and
BUG-0029 wait on Andrew's decisions; BUG-0135, BUG-0222 with BUG-0136, and BUG-0061 with BUG-0062 are
new requirements to be placed in the order with the rest.

## 2. The write-ahead log, its retention, the disk, and the trading day

Ten entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0130](bug_list.md#bug_0130): the WAL's helper keeps only one segment ready | **Breach** of F2 (no order-handling thread waits for another thread) and F4 (none waits for the disk) | Unchanged. `WalWriter::ensure_capacity` still calls `await_helper()` when the helper is filling the next segment, and fills one itself when the helper is not ready; the helper prepares one segment at a time. A segment filled by the writing thread is counted (`segments_filled_inline_`) and not logged: `WalWriter.cpp` logs nothing | What was agreed on 11 October 2026: a configurable number of segments kept ready, four by default, and a Warning when the writer fills one itself |
| [BUG-0113](bug_list.md#bug_0113): a resend request reads the log on the thread that sequences orders | **Partly done**; what is left is a **Breach** of F4, accepted for now and listed by name | A matching engine's catch-up is read on the reading thread (phase 5). A resend reads backwards from the newest segment and stops once it has what it needs, so a resend after a reconnect reads a few segments; one that asks for reports from far back still reads far back on the sequencer's thread. `scripts/check_standards.py` permits this read by name, as a resend or a request for undelivered reports, "BUG-0113, which moves them to a recovery service" | The recovery service, chosen on 4 October 2026, which needs the period for which a member's reports are kept (R-0008) to be stated first |
| [BUG-0210](bug_list.md#bug_0210): a sequencer whose WAL has lost its first segment dies on an exception | **Breach** of V11 | Unchanged. `Wal::truncate_after` opens the segment that holds the record to keep, and throws if it cannot be opened; nothing catches it | A missing segment repaired as a gap is, by discarding what follows and being sent it again; on a leader, a refusal naming the missing file |
| [BUG-0183](bug_list.md#bug_0183): a sequencer dies with SIGBUS appending to a WAL left by earlier runs | **Investigation**; a **Breach** of V2 if the cause is in the venue | The cause is not found. Every run of the tests starts from fresh WALs, which avoids it. BUG-0137's refusal of a file from another trading day would stop the venue starting on such a WAL at all | Find which segment was short and why; whatever the cause, a write that cannot be backed refuses with a message rather than killing the process |
| [BUG-0175](bug_list.md#bug_0175): while the disk is slow, the leading sequencer's own thread waits in the file system's journal | **Investigation**; a **Breach** of F4 if confirmed | Not reproduced since the WALs moved to a disk of their own with `lazytime` (BUG-0228). Finding the call needs the scheduler's events with kernel stacks, which `perf` now records without root when attached per thread (the method recorded under BUG-0228) | Reproduce under a slow disk with that method, then take the call off the thread or show F4 permits it |
| [BUG-0137](bug_list.md#bug_0137): no start-of-day or end-of-day procedure, and a file from an earlier day is taken as a restart | **Breach** of V12, which says each trading day starts fresh and is met only by a procedure that does not exist | Unchanged: no list of the venue's files, no procedure, and no component checks which trading day a file belongs to | The four parts agreed on 11 October 2026, designed with the trading phases |
| [BUG-0132](bug_list.md#bug_0132): orders open at the end of the trading day are never cancelled | **New requirement**, needed for V2 (every order is a day order) | Unchanged; the trading phases `closed` and `eod` are designed and not built ([trading_phases.md](venue/trading_phases.md)) | Cancel every open order through the sequencer when trading closes, designed with the trading phases and BUG-0137 |
| [BUG-0048](bug_list.md#bug_0048): the log retains all of its history | **Decision needed**: the period for which a member's reports are kept (R-0008) | Unchanged: `Wal::truncate_below` is called by nothing, and nothing reclaims the log during the day. Of the three positions retention depends on, the matching engine's position is still not reported to the sequencer, and the third needs the period above | The period stated; then the engine's position reported, and retention anchored to the lowest of the three |
| [BUG-0086](bug_list.md#bug_0086): the engine's fallback recovery replays a whole day | **Fixed** as far as this entry goes; what is left is BUG-0048 | The document is corrected ([open_order_checkpoint.md](durability/open_order_checkpoint.md) says the record is not truncated and what the replay costs). The entry itself says it closes with BUG-0048 | Close it into BUG-0048, if Andrew agrees |
| [BUG-0141](bug_list.md#bug_0141): a follower discarding records while its identifier record is built stops | **Fixed**, by a different route from the one the entry asked for | `LoggedCommandIdentifiersBuilder` now passes over a segment it cannot read, counts it, and `ReplicatedWal` logs a Warning naming the cause, so the process no longer stops. An identifier it records from a record later discarded is harmless: a match is checked against the WAL itself and searched for when it differs (phase 9, [one_check_against_sequencing_twice.md](availability/one_check_against_sequencing_twice.md)). The builder and the repair still use the files without synchronising, which the entry asked to end | Close it, if Andrew accepts that a builder skipping a segment the repair deleted is correct rather than a race to be removed |

**Recommended order in this area.** BUG-0130 and BUG-0210 first: each is a breach with an agreed or
obvious remedy. BUG-0137 next, because V12 rests on it, designed with the trading phases and BUG-0132.
BUG-0183 and BUG-0175 are investigations to schedule. BUG-0048 and BUG-0113's remainder wait on the
decision about how long a member's reports are kept; BUG-0086 and BUG-0141 can be closed if Andrew agrees.

## 3. The matching engine

Nine entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0225](bug_list.md#bug_0225): an engine that becomes a follower before asking to follow the WAL applies nothing | **Breach** of V11: a standby that cannot take over promptly is not reported | Unchanged. `MatchingEngineThread::follow_peer_claiming_leadership` makes the instance a follower with `enter_follower_state`, and nothing then asks to follow the WAL; scenario 24 shows it and passes, because it checks only the role | Ask to follow the WAL on every way into the follower role; a scenario that checks the restarted engine applies commands |
| [BUG-0211](bug_list.md#bug_0211): changing the region's size during a day can rebuild a different book | **Breach** of V8 | Unchanged. `OrderBook::open` opens the region with the configured capacity, and a region of a different capacity is refused as unusable and rebuilt from the WAL, as a damaged one is | Tell a region refused only for its capacity from a damaged one, from its intact header, and refuse to rebuild with a smaller capacity |
| [BUG-0194](bug_list.md#bug_0194): an engine that is connected but has stopped working looks healthy | **New requirement**, a design, for V6 and V8 | Unchanged. The reactor's watchdog catches a thread stuck in one callback; nothing catches an engine that answers its lease and applies nothing | The design the entry asks for: what evidence shows an engine is working, who notices, and what is done |
| [BUG-0028](bug_list.md#bug_0028): growing the order book needs more memory than the machine has | **Partly done** | The map is reserved for the configured number of open orders at start-up, prod sets 100 million, and the book is no longer copied to the follower (phase 8), so the doubling the title describes no longer happens. The entry's own list of what is still open stands: the reserved map's pages are not touched at start-up, so the memory is not proven available; clearing erased slots copies the map; a restart costs time in proportion to the region's capacity, 8 to 17 seconds at 100 million; preprod and test-1 allow one million | Those four points. The title no longer describes the problem, and should be changed when the entry is next worked on |
| [BUG-0066](bug_list.md#bug_0066): a flapping matching engine resets the deferral clock | **Fixed** in effect, by V14 and by phase 8 | What the entry measured was orders stranded on every cycle and the venue never refusing. Orders are no longer stranded: they are held while no engine leads and released to the engine that next leads (BUG-0064, fixed). An engine interrupted twice is not restarted again (`launch.py --max-interruptions 2`, V14), so a flap becomes one absence, which reaches the refusal after five seconds and the halt after ten. What remains true is that the venue's own clock still restarts each time an engine leads; it is the supervisor that ends the flap | Close it, if Andrew agrees; [order_acceptance.md](availability/order_acceptance.md) then drops it from "What this does not solve" |
| [BUG-0193](bug_list.md#bug_0193): a command that crashes one engine crashes both | **Fixed** | All three parts are built: a fault caught while applying a command rejects that command (phase 7, [deterministic_matching_engine.md](availability/deterministic_matching_engine.md), section 6, with fuzzing); an engine that died applying a command asks for it to be voided unless its peer applied it ([voided_commands.md](availability/voided_commands.md), section 4); the gateways' strict checks remain | Close it, if Andrew agrees |
| [BUG-0151](bug_list.md#bug_0151): the round trip is about 20 microseconds slower when the secondary engine leads | **Investigation**, then a **Decision needed** | Unchanged. `environments/dev.toml` still gives the primary engine `reactor_spin_before_block = "50ms"` and the secondary `"0s"`, a setting that follows the instance's name rather than its role | Confirm the cause by measurement; then give both the same setting, or spin only while leading |
| [BUG-0209](bug_list.md#bug_0209): no scenario reaches the halt when an engine cannot account for the venue's orders | **Test or tool** | Unchanged; the case cannot be reached while the WAL keeps the whole day | A scenario, possible once BUG-0210 or BUG-0048 is done |
| [BUG-0216](bug_list.md#bug_0216): sending every record to the follower engine adds about 2 microseconds at the median | **Investigation** | Unchanged: one run of each build, and the spread between runs of one build not measured | Measure that spread first; only then decide whether to reduce it |

**Recommended order in this area.** BUG-0225 and BUG-0211 first, as breaches; BUG-0225 is small. Then the
design for BUG-0194. BUG-0028's remaining points belong with the start-of-day work of BUG-0137, since
proving the memory available is a start-of-day check. BUG-0066 and BUG-0193 can be closed if Andrew agrees.
BUG-0151 and BUG-0216 are measurements; BUG-0209 waits on BUG-0210.

## 4. The sequencer's handling of reports

Three entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0226](bug_list.md#bug_0226): a report held for a gateway with no connection counts as forwarded | **Breach** of V8 | Unchanged after phase 10 moved the code into `ReportForwarding`. `ReportForwarding::forwarded_through` holds the position back for reports waiting on the follower (`pending_`) and for reports held for an unannounced session (`held_reports_oldest_position_`), but not for reports held in `reports_held_for_gateways_` | Take the oldest report held for a gateway into the position; a scenario, as the entry describes, made to fail on purpose first |
| [BUG-0131](bug_list.md#bug_0131): the leading sequencer allocates from the heap for every order, and copies waiting reports | **Breach** of F5 and F6 | Unchanged after phase 10. `MemberSessions::origins_` is a `std::unordered_map<int64_t, OriginSession>`, gaining a node for every order; `ReportForwarding::pending_` is a `std::multimap` whose entries hold the report in a `std::vector<uint8_t>` and the ClOrdID in a `std::string` | The five steps agreed on 11 October 2026, with the waiting reports' storage and its limits written up and agreed before any code |
| [BUG-0227](bug_list.md#bug_0227): an operator whose lift cannot be written is told reports are outstanding | **Defect** | Unchanged in `HaltAndVoidRequests::operator_request`; cannot happen with the buffer the sequencer sizes for a `TechnicalEvent` | An outcome of its own for a command that could not be encoded |

**Recommended order in this area.** BUG-0226 first: it is small, and a report a member never receives is
the failure V8 exists to prevent. BUG-0131 next, through its agreed design. BUG-0227 whenever the
`OperatorHaltAnswer` outcomes are next touched.

## 5. The gateways and what a member sees

Fifteen entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0090](bug_list.md#bug_0090): a restarted gateway silently stops honouring cancel-on-disconnect | **Breach** of V6 in spirit: a member's instruction the venue accepted is not carried out, and nothing says so | Unchanged. `FixSession::open_orders` is filled only as a live execution report arrives (`open_orders::track_open_order`), and the cleanup of a session returns at once, logging nothing, when the set is empty (`FixOrderGatewayThread.cpp`, `if (session.open_orders.empty())`) | The gateway learns a session's open orders when it takes the session on, or the cancel on disconnect is done where the orders are known; and the empty case is logged. Designed with BUG-0091 |
| [BUG-0095](bug_list.md#bug_0095): checking a FIX price or quantity overflows a signed integer | **Defect**, undefined behaviour in the C++ | Unchanged: `FixField::as_decimal` still builds `digits = digits * 10 + ...` in an `int64_t` with no check. The binary gateway's checks, added since (`BinaryCommandChecks`), call the same rule, so the binary gateway now has the same fault | Check the text's form without building the number, which nothing uses |
| [BUG-0091](bug_list.md#bug_0091): a member's standing instructions die with the gateway that received them | **New requirement** (R-0103), its mechanism decided | Unchanged: `SessionBound` and `SessionBoundAck` carry no cancel-on-disconnect fields | Add them, as [session_binding.md](availability/session_binding.md) decides, with BUG-0090 |
| [BUG-0072](bug_list.md#bug_0072): the gateway's open-order pool is sized by nothing in particular | **Breach** of F5 by configuration: routine growth of the pool is an allocation on the order path | Unchanged: `environments/prod.toml` gives the binary gateway 4,096 slots in one pool | Size the pool from the number of open orders the venue plans for, as the matching engine's region is |
| [BUG-0203](bug_list.md#bug_0203): the FIX gateway sometimes refuses a logon as an impostor when two are under way together | **Investigation** | Not reproduced since; cause unknown | Reproduce with many simultaneous logons, as at the start of the day, and find the shared state |
| [BUG-0056](bug_list.md#bug_0056): the FIX gateway stopped completing logons while still running | **Investigation** | Not reproduced; the evidence is gone | Keep until a reproduction or a stress test of abandoned connections shows it or rules it out |
| [BUG-0089](bug_list.md#bug_0089): a member cannot ask the venue what it is holding | **New requirement** (R-0002) | Unchanged: no order status messages in the dictionary or the code | Order status and mass status requests, answered from the book |
| [BUG-0046](bug_list.md#bug_0046): the binary gateway has no in-flight report recovery | **New requirement** | Unchanged | A recovery for the binary protocol, designed with the sequencer's replay contract |
| [BUG-0045](bug_list.md#bug_0045): a member has no defined way to discover its primary gateway is down | **New requirement**, an open design question | Unchanged | A statement of what a member should do, in the member documentation |
| [BUG-0059](bug_list.md#bug_0059): no defence against a member reconnecting in a loop | **New requirement** | Unchanged; the entry already sets out the graduated answer | Place in the order of work |
| [BUG-0058](bug_list.md#bug_0058): a member halted by a sequence gap is invisible to monitoring | **New requirement** | Unchanged | The gauge the entry describes |
| [BUG-0109](bug_list.md#bug_0109): the FIX gateway's health line counts cancel reports as answered orders | **Defect** | Unchanged: `accounted` is every report sent, dropped or refused, and `awaiting` is new orders less that | Count only reports that answer new orders on the order side |
| [BUG-0065](bug_list.md#bug_0065): the venue tells a member of a halt only in the reply to an order | **Partly done** | Halting and lifting are built; the entry itself lists what remains | `TradingSessionStatus` and its binary equivalent, halting one instrument, and the table of what each trading phase allows, designed with the trading phases |
| [BUG-0092](bug_list.md#bug_0092): a refused cancel is answered with an execution report | **Fixed** | The FIX gateway answers its own refusal of a cancel with an `OrderCancelReject` (35=9), and `FixErEncoder` encodes the matching engine's refusal of a cancel as one too (`FixErEncoder.hpp`, "Encodes a matching engine report that refuses a request to cancel as a FIX OrderCancelReject"), under R-0151 and BUG-0099 | Close it, if Andrew agrees |
| [BUG-0096](bug_list.md#bug_0096): the binary gateway passes on prices and quantities without checking them | **Fixed** | `BinaryCommandChecks` checks every quantity and price as a decimal number by the FIX codec's rule (BUG-0100) | Close it, if Andrew agrees; the overflow it inherits is BUG-0095 |

**Recommended order in this area.** BUG-0090 and BUG-0091 together first: a member's instruction silently
not carried out is the most serious thing here. BUG-0095 next, being small and undefined behaviour in
both gateways. BUG-0072 is a configuration change. The investigations, BUG-0203 and BUG-0056, need a
stress test of many logons and abandoned connections, which could serve both. The new requirements are
placed with the rest; BUG-0065's remainder goes with the trading phases. BUG-0092 and BUG-0096 can close.
