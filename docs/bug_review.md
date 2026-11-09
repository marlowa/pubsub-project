# Phase 12: the open bugs judged against the principles {#bug_review}

This is the working record of phase 12 of the [recovery plan](recovery_plan.md). Every entry in the
[bug list](bug_list.md) that is open is judged here against the design principles in
[design_principles.md](design_principles.md) and against the code as it is now, after phases 3 to 11.
Nothing is fixed while the review is made: what it produces is a judgement for each entry, with the
evidence for it, and then an order of work agreed with Andrew for the entries that remain.

## Summary

All 81 open entries have been judged. The figures:

| Judgement | Entries |
|---|---|
| Breach of a named principle | 16, and 3 more if their causes prove to be in the venue: BUG-0142, BUG-0207, BUG-0130, BUG-0113 (its remainder), BUG-0210, BUG-0137, BUG-0225, BUG-0211, BUG-0226, BUG-0131, BUG-0090, BUG-0072, BUG-0129, BUG-0127, BUG-0128, BUG-0119; and BUG-0183, BUG-0175 and BUG-0184 if their causes are in the venue; with BUG-0095's undefined behaviour and BUG-0181's test that can pass over lost orders treated as equally urgent |
| Fixed, or no longer applies: closed on 8 November 2026 with Andrew's agreement | 13: BUG-0066, BUG-0193, BUG-0086, BUG-0141, BUG-0092, BUG-0096, BUG-0169, BUG-0144, BUG-0040, BUG-0030, BUG-0083, BUG-0145 (into BUG-0185), and BUG-0167 if its remaining risk is accepted |
| Decision needed from Andrew | BUG-0167, BUG-0151, BUG-0229, BUG-0073 |
| Partly done | BUG-0121, BUG-0134, BUG-0028, BUG-0065, BUG-0069, BUG-0041 |
| The rest | new requirements, investigations, small defects, and tests and tools, each placed in its area's recommended order |

**The decisions that unblock the most.**

1. **How long a member's execution reports are kept (R-0008).** Decided on 8 November 2026: the recovery
   service keeps a fixed count of recent messages for each comp id, the same count for every comp id, and
   answers a resend request beyond them with a FIX gap fill, as the London Stock Exchange and Turquoise do.
   The count is 100,000 messages. It decides BUG-0048 (reclaiming the WAL), BUG-0113's remainder
   (the recovery service), BUG-0086, and through them BUG-0121's start-up time and BUG-0209's scenario.
2. **The start-of-day and end-of-day procedures with the trading phases.** BUG-0137 (V12), BUG-0132,
   BUG-0065's remainder, and BUG-0028's check that the reserved memory is there at the start of the day
   belong in one design.
3. **Which sequencer the gateways use with high availability off** (BUG-0061, then BUG-0062). Decided on 8
   November 2026: none but the primary, and a secondary instance refuses to start with high availability off
   (R-0163). Built and closed on 9 November 2026.

**The order of work**, agreed with Andrew on 8 November 2026:

1. Close the thirteen entries above that are fixed or no longer apply. Done, 8 November 2026.
2. The small breaches, each with a remedy already agreed or obvious: BUG-0129 (a data race; fixed and closed on
   8 November 2026), BUG-0225 (fixed and closed on 8 November 2026, with BUG-0233; BUG-0234 and BUG-0235 found),
   BUG-0226, BUG-0210, BUG-0095 and BUG-0189, and the tests BUG-0181 and BUG-0185, each fixed and closed on 8 November
   2026. Done.
3. The breaches that need a short design first: BUG-0142 (statements written in the background; fixed and closed on 9
   November 2026),
   BUG-0130 (segments kept ready; fixed and closed on 9 November 2026), BUG-0207 (with its scenario), BUG-0211, BUG-0090 with BUG-0091,
   BUG-0131, and BUG-0072. The designs for BUG-0207, BUG-0211, BUG-0090 with BUG-0091, and BUG-0131 were agreed with
   Andrew on 9 November 2026, as was the design for BUG-0237; BUG-0131's decision sets the venue's figure for open orders
   at 2,000,000, which is the figure BUG-0072 needs.
4. BUG-0087's tool for counting heap allocations, then the framework's locks, BUG-0127 and BUG-0128,
   and BUG-0119.
5. The three decisions above, and the designs they lead to: retention and the recovery service; the
   trading day; high availability off.
6. Then the new requirements, the investigations and the remaining small defects, in the order Andrew
   chooses.

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
6. The framework: reactor, timers, allocators and threads (section 6)
7. Tests and tooling (section 7)
8. Operations, deployment and metrics (section 8)

## 1. Leadership, leases and the arbiters

Eleven entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0142](bug_list.md#bug_0142): the arbiter writes each changed statement to disk on its own thread | **Fixed on 9 November 2026** and closed | The arbiter writes statements on a thread of its own and confirms each only once it is on disk; scenario 103 shows the primary sequencer keeping its lease while the arbiter's statement writes take 5 seconds, and losing it when the arbiter is made to wait for the write | Nothing |
| [BUG-0207](bug_list.md#bug_0207): a sequencer that was down past the recovery limit can, after the whole venue restarts, cancel the book for an absence that did not happen | **Breach** of V11: a loss of resilience is not a loss of service | `EnginePresence` measures the absence at start-up from this instance's own `EngineRoutedStamp` and the first record of its WAL, and nothing else. The exchange the entry proposed for carrying the peer's stamp, `StatusQuery` and `StatusResponse`, is no longer sent by anything | Not reproduced yet: a scenario first. Then the leading sequencer measures from the later of its own stamp and its peer's; with the status messages gone, the peer's stamp has to travel another way, such as in the WAL's records |
| [BUG-0121](bug_list.md#bug_0121): a restarted sequencer takes longer to start than its peer's lease | **Partly done** | The record of command identifiers is built in the background (`LoggedCommandIdentifiersBuilder`). The whole WAL is still read at start-up: `ReplicatedWal::open` says so, because the table of epochs and the check for a gap need every record. So a supervised restart late in a busy day will not be back within the lease, and rule 6's promise that a restarted leader keeps the lead does not hold then | The decided change, not yet designed: keep the table of epochs in a small file written when the epoch changes, and check for gaps only after the last snapshot |
| [BUG-0134](bug_list.md#bug_0134): a venue running without a standby says so once | **Partly done** | The gauge `sequencer_running_alone` now reads 1 whenever the leader does not wait for its follower, including when no follower is connected, and starting to do so is logged at Warning for those two cases (`ReplicatedWal`, the reasons for committing without the follower) | The repeated Error while it lasts, with how long and how many commands exist only on the leader's machine; the dashboard's alert |
| [BUG-0167](bug_list.md#bug_0167): a lease promise record can take longer to write than a lease period | **Decision needed**: close, with the risk accepted, or keep open | The promise record is written in the background and refreshed five seconds before it is needed (`PairLeaseAgent`). BUG-0228, closed, measured the cause (a slow disk under load from other writers), moved the WALs and the small synced files to a disk of their own, made an instance write its epoch before asking to lead (rule 13), and added metrics for how long each record takes and how much lease is left. What remains is that a disk slower than a lease period still costs the lead, and is now reported | Andrew to decide whether the residual risk is accepted and the entry closed |
| [BUG-0029](bug_list.md#bug_0029): a process death on the same host takes the machine-death path | **Closed** on 8 November 2026 by decision | Andrew chose to replace the 50-millisecond target with "the lead never moves": a process restarted by its supervisor leads again within its lease period. Measured twice: 2.50 and 2.11 seconds from death to leading, most of it the sequencer's 2-second wait before connecting to the engine again | [BUG-0237](bug_list.md#bug_0237), the sequencer's wait, recorded the same day |
| [BUG-0135](bug_list.md#bug_0135): while the leader trades alone, a power failure can lose the last seconds of the record | **New requirement** | Nothing syncs the leader's WAL while it runs without its follower; there is no such interval in `ReplicatedWal` or `Wal` | Build it as the entry describes, on a thread of its own (F4), if Andrew agrees |
| [BUG-0222](bug_list.md#bug_0222): no operator procedure lets the follower lead after the old leader's machine is lost while it ran alone | **New requirement** (R-0161, R-0162) | Not built; [a_follower_behind_does_not_lead.md](availability/a_follower_behind_does_not_lead.md), section 4.4, says so | The operator's tool and the procedure for telling members, designed together |
| [BUG-0136](bug_list.md#bug_0136): whether a copy of the log elsewhere could recover what the leader's machine took with it | **Investigation** | Unchanged. The order activity recorder is not built | Find out what each follower of the log keeps; then decide whether recovering from it is worth designing. Best done with BUG-0222 |
| [BUG-0061](bug_list.md#bug_0061): with high availability off, a secondary sequencer started alone leads, but no gateway can reach it | **Fixed on 9 November 2026** and closed | Every secondary refuses to start with high availability off (R-0163), and scenarios 46 and 47 check each refuses and says why | Nothing |
| [BUG-0062](bug_list.md#bug_0062): two instances that led with high availability off, and nothing notices when they are reunited | **Closed** on 9 November 2026, with Andrew's agreement, because it no longer arises as described | With every secondary refusing to start with high availability off, two instances cannot both lead in that configuration; a primary later joined by its secondary is a follower behind, already handled | None |

**Recommended order in this area.** BUG-0142 and BUG-0207 first, because each breaks a principle, and
BUG-0207 needs a scenario before it is fixed. Then BUG-0121, because without it the rule that a restarted
leader keeps the lead fails late in a busy day. BUG-0134's remaining reporting is small. BUG-0237, the sequencer's
2-second wait before connecting to a restarted engine again, is small and comes after them. BUG-0167
waits on Andrew's decision; BUG-0135 and BUG-0222 with BUG-0136 are new requirements to be placed in the order with
the rest. BUG-0061 and BUG-0062 are closed.

## 2. The write-ahead log, its retention, the disk, and the trading day

Ten entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0130](bug_list.md#bug_0130): the WAL's helper keeps only one segment ready | **Fixed on 9 November 2026** and closed | Four segments are kept ready, each filled under a temporary name and given its final name only when full, and the writing thread never waits for the helper; a short segment that crashed the next start is completed at opening | The trading-day run the design asks for |
| [BUG-0113](bug_list.md#bug_0113): a resend request reads the log on the thread that sequences orders | **Partly done**; what is left is a **Breach** of F4, accepted for now and listed by name | A matching engine's catch-up is read on the reading thread (phase 5). A resend reads backwards from the newest segment and stops once it has what it needs, so a resend after a reconnect reads a few segments; one that asks for reports from far back still reads far back on the sequencer's thread. `scripts/check_standards.py` permits this read by name, as a resend or a request for undelivered reports, "BUG-0113, which moves them to a recovery service" | The recovery service, chosen on 4 October 2026, keeping a fixed count of messages for each comp id of 100,000 (R-0008, decided on 8 November 2026) |
| [BUG-0210](bug_list.md#bug_0210): a sequencer whose WAL has lost its first segment dies on an exception | **Breach** of V11 | Unchanged. `Wal::truncate_after` opens the segment that holds the record to keep, and throws if it cannot be opened; nothing catches it | **Fixed on 8 November 2026** and closed: a follower that has lost its first segment file discards every record and is sent them all again, and an instance running without high availability refuses a WAL with a gap, naming each missing segment file. Scenario 102 deletes a follower's first segment file; it failed with the old code |
| [BUG-0183](bug_list.md#bug_0183): a sequencer dies with SIGBUS appending to a WAL left by earlier runs | **Investigation**; a **Breach** of V2 if the cause is in the venue | The cause is not found. Every run of the tests starts from fresh WALs, which avoids it. BUG-0137's refusal of a file from another trading day would stop the venue starting on such a WAL at all | Find which segment was short and why; whatever the cause, a write that cannot be backed refuses with a message rather than killing the process |
| [BUG-0175](bug_list.md#bug_0175): while the disk is slow, the leading sequencer's own thread waits in the file system's journal | **Investigation**; a **Breach** of F4 if confirmed | Not reproduced since the WALs moved to a disk of their own with `lazytime` (BUG-0228). Finding the call needs the scheduler's events with kernel stacks, which `perf` now records without root when attached per thread (the method recorded under BUG-0228) | Reproduce under a slow disk with that method, then take the call off the thread or show F4 permits it |
| [BUG-0137](bug_list.md#bug_0137): no start-of-day or end-of-day procedure, and a file from an earlier day is taken as a restart | **Breach** of V12, which says each trading day starts fresh and is met only by a procedure that does not exist | Unchanged: no list of the venue's files, no procedure, and no component checks which trading day a file belongs to | The four parts agreed on 11 October 2026, designed with the trading phases |
| [BUG-0132](bug_list.md#bug_0132): orders open at the end of the trading day are never cancelled | **New requirement**, needed for V2 (every order is a day order) | Unchanged; the trading phases `closed` and `eod` are designed and not built ([trading_phases.md](venue/trading_phases.md)) | Cancel every open order through the sequencer when trading closes, designed with the trading phases and BUG-0137 |
| [BUG-0048](bug_list.md#bug_0048): the log retains all of its history | **Decided** on 8 November 2026: the recovery service keeps the most recent 100,000 messages for each comp id (R-0008); what is left is work | Unchanged: `Wal::truncate_below` is called by nothing, and nothing reclaims the log during the day. Of the three positions retention depends on, the matching engine's position is still not reported to the sequencer, and the third is replaced by the recovery service's own store | The recovery service built; the engine's position reported, and retention anchored to the lower of the follower's acknowledgement and the engine's checkpoint |
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
| [BUG-0225](bug_list.md#bug_0225): an engine that becomes a follower before asking to follow the WAL applies nothing | **Breach** of V11: a standby that cannot take over promptly is not reported | Unchanged when judged: `MatchingEngineThread::follow_peer_claiming_leadership` made the instance a follower with `enter_follower_state`, and nothing then asked to follow the WAL; scenario 24 passed, because it checked only the role | **Fixed on 8 November 2026** and closed: an engine that becomes a follower without having asked to follow the WAL asks at once, and the leading sequencer takes a request to follow as the engine saying it does not lead. Scenario 24 checks that the restarted engine applies commands, and scenario 100 makes it take the defect's way every time, with a setting used only by tests that Andrew agreed ([BUG-0233](bug_list.md#bug_0233), closed). The full suite found [BUG-0234](bug_list.md#bug_0234), reports lost when an engine's catch-up outruns the sequencer, and a run of scenario 80 alone found [BUG-0235](bug_list.md#bug_0235), a test whose window is too narrow |
| [BUG-0211](bug_list.md#bug_0211): changing the region's size during a day can rebuild a different book | **Breach** of V8 | Unchanged. `OrderBook::open` opens the region with the configured capacity, and a region of a different capacity is refused as unusable and rebuilt from the WAL, as a damaged one is | Tell a region refused only for its capacity from a damaged one, from its intact header, and refuse to rebuild with a smaller capacity |
| [BUG-0194](bug_list.md#bug_0194): an engine that is connected but has stopped working looks healthy | **New requirement**, a design, for V6 and V8 | Unchanged. The reactor's watchdog catches a thread stuck in one callback; nothing catches an engine that answers its lease and applies nothing | The design the entry asks for: what evidence shows an engine is working, who notices, and what is done |
| [BUG-0028](bug_list.md#bug_0028): growing the order book needs more memory than the machine has | **Partly done** | The map is reserved for the configured number of open orders at start-up, prod sets 100 million, and the book is no longer copied to the follower (phase 8), so the doubling the title describes no longer happens. The entry's own list of what is still open stands: the reserved map's pages are not touched at start-up, so the memory is not proven available; clearing erased slots copies the map; a restart costs time in proportion to the region's capacity, 8 to 17 seconds at 100 million; preprod and test-1 allow one million | Those four points. The title no longer describes the problem, and should be changed when the entry is next worked on |
| [BUG-0066](bug_list.md#bug_0066): a flapping matching engine resets the deferral clock | **Fixed** in effect, by V14 and by phase 8 | What the entry measured was orders stranded on every cycle and the venue never refusing. Orders are no longer stranded: they are held while no engine leads and released to the engine that next leads (BUG-0064, fixed). An engine interrupted twice is not restarted again (`launch.py --max-interruptions 2`, V14), so a flap becomes one absence, which reaches the refusal after five seconds and the halt after ten. What remains true is that the venue's own clock still restarts each time an engine leads; it is the supervisor that ends the flap | Close it, if Andrew agrees; [order_acceptance.md](availability/order_acceptance.md) then drops it from "What this does not solve" |
| [BUG-0193](bug_list.md#bug_0193): a command that crashes one engine crashes both | **Fixed** | All three parts are built: a fault caught while applying a command rejects that command (phase 7, [deterministic_matching_engine.md](availability/deterministic_matching_engine.md), section 6, with fuzzing); an engine that died applying a command asks for it to be voided unless its peer applied it ([voided_commands.md](availability/voided_commands.md), section 4); the gateways' strict checks remain | Close it, if Andrew agrees |
| [BUG-0151](bug_list.md#bug_0151): the round trip is about 20 microseconds slower when the secondary engine leads | **Investigation**, then a **Decision needed** | Unchanged. `environments/dev.toml` still gives the primary engine `reactor_spin_before_block = "50ms"` and the secondary `"0s"`, a setting that follows the instance's name rather than its role | Confirm the cause by measurement; then give both the same setting, or spin only while leading |
| [BUG-0209](bug_list.md#bug_0209): no scenario reaches the halt when an engine cannot account for the venue's orders | **Test or tool** | Unchanged; the case cannot be reached while the WAL keeps the whole day | A scenario, possible once BUG-0048 is done. Fixing BUG-0210 does not make it possible: a follower that loses its first segment file is sent every record again, and an instance running alone refuses to start |
| [BUG-0216](bug_list.md#bug_0216): sending every record to the follower engine adds about 2 microseconds at the median | **Investigation** | Unchanged: one run of each build, and the spread between runs of one build not measured | Measure that spread first; only then decide whether to reduce it |

**Recommended order in this area.** BUG-0225 and BUG-0211 first, as breaches; BUG-0225 is small. Then the
design for BUG-0194. BUG-0028's remaining points belong with the start-of-day work of BUG-0137, since
proving the memory available is a start-of-day check. BUG-0066 and BUG-0193 can be closed if Andrew agrees.
BUG-0151 and BUG-0216 are measurements; BUG-0209 waits on BUG-0048.

## 4. The sequencer's handling of reports

Three entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0226](bug_list.md#bug_0226): a report held for a gateway with no connection counts as forwarded | **Breach** of V8 | Unchanged after phase 10 moved the code into `ReportForwarding`. `ReportForwarding::forwarded_through` holds the position back for reports waiting on the follower (`pending_`) and for reports held for an unannounced session (`held_reports_oldest_position_`), but not for reports held in `reports_held_for_gateways_` | **Fixed on 8 November 2026** and closed: the leader takes the oldest report held for a gateway into the position it says it has forwarded through, and the follower discards a kept report by its age only if the report has no position. Scenario 101 kills the leader while it holds a gateway's reports and requires every one to reach the member; it failed with either half put back |
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
| [BUG-0095](bug_list.md#bug_0095): checking a FIX price or quantity overflows a signed integer | **Defect**, undefined behaviour in the C++ | Unchanged: `FixField::as_decimal` still builds `digits = digits * 10 + ...` in an `int64_t` with no check. The binary gateway's checks, added since (`BinaryCommandChecks`), call the same rule, so the binary gateway now has the same fault | **Fixed on 8 November 2026** and closed: `FixField::is_decimal` checks the text's form character by character and builds no number, for both gateways. Under the undefined behaviour sanitizer the old check overflowed on a 40-digit value and the new one does not |
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

## 6. The framework: reactor, timers, allocators and threads

Eleven entries. None was touched by phases 3 to 11, which moved the venue's machinery into the library
but did not change these parts of it.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0129](bug_list.md#bug_0129): the stuck-thread check takes a lock it does not need, and reads the handler times without synchronisation | **Breach** of F2 (the lock), and a data race, undefined behaviour in C++ | Unchanged when judged: `Reactor.cpp` took `thread_registry_mutex_` (twelve uses), and `ApplicationThread::time_event_started_` and `time_event_finished_` were plain `HighResolutionClock::time_point` members written by one thread and read by another | **Fixed on 8 November 2026**, as the entry describes, and closed. The lock is now taken only at start-up and shutdown, and the two handler times are atomic. A unit test run under ThreadSanitizer reports the race with plain members and nothing with atomic ones |
| [BUG-0127](bug_list.md#bug_0127): a pool that runs out takes a mutex and maps a new pool while holding it | **Breach** of F2 and F5 | Unchanged: `ExpandablePoolAllocator` still has `expansion_mutex_` on its slow path | No lock on the allocation path, as the entry describes; with BUG-0072, sizing so the slow path is not reached in a normal day |
| [BUG-0128](bug_list.md#bug_0128): every timer start creates a timer and every cancel destroys one, with a lock and system calls on the reactor thread | **Breach** of F2 and F3 | Unchanged: `Reactor` still has `timer_registry_mutex_`, and there is still no way to create a timer apart from starting it | Timers created once and armed and disarmed afterwards; the design the entry asks for |
| [BUG-0119](bug_list.md#bug_0119): a thread still in a handler when its reactor shuts down uses the destroyed reactor | **Breach** of F8 (shutdown is deterministic) | Unchanged in the shutdown path | Decide whether a reactor may ever be destroyed while one of its threads may still run, and make shutdown wait or refuse accordingly |
| [BUG-0184](bug_list.md#bug_0184): a paused connection sometimes went on delivering | **Investigation**, a **Breach** if the fault is in the reactor | Still failing about one run in four of the coverage build, as the entry records | Find whether pausing fails or the test's timing does, under load |
| [BUG-0104](bug_list.md#bug_0104): a connection whose reads were paused can stay stalled after they resume | **Investigation** | Unchanged; the kernel rule is not established | As the entry describes; related to BUG-0184 and best done with it |
| [BUG-0087](bug_list.md#bug_0087): whether the hot path allocates cannot be established on demand | **New requirement**, a tool | Not built: no counting interposer in `scripts/` | The tool; then BUG-0131, BUG-0127 and BUG-0072 can each be shown fixed by a count rather than by reading |
| [BUG-0166](bug_list.md#bug_0166): the reactor writes a Warning for every send to a connection that has gone | **Defect** | Unchanged: `Reactor::process_control_commands` logs "unknown connection id ... for SendPdu" for each send | One Warning per connection, with a count |
| [BUG-0161](bug_list.md#bug_0161): the framework names the venue's ideas in how it classifies a send | **Defect**, of naming | Unchanged: `MemberIsWaitingFlag.hpp` is still in the framework | The neutral names the entry suggests |
| [BUG-0177](bug_list.md#bug_0177): the C++ generated from the DSL does not compile for a list of enums | **Defect**, in the DSL compiler | Unchanged in `python/dsl/generator_cpp.py` | As the entry describes |
| [BUG-0179](bug_list.md#bug_0179): the Java generated from the DSL decodes an unlisted enum value as null | **Defect**, in the DSL compiler | Unchanged: `generator_java.py` still decodes through `fromValue`, which returns `null` for a value it does not know | As the entry describes |

**Recommended order in this area.** BUG-0129 first, for its data race. Then BUG-0127 and BUG-0128,
the two locks on paths an order can take; each needs a design agreed before code, and BUG-0128 the larger
one. BUG-0087's tool is worth building before those, since it is how their fixes, and BUG-0131's, would
be shown to work. BUG-0119 with the shutdown sequence. BUG-0184 and BUG-0104 together. The four defects
are small and can be done when their files are next touched.

## 7. Tests and tooling

Fifteen entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0181](bug_list.md#bug_0181): with orders in flight, Phase 5 confirms that a thousand further orders were accepted, not the recovery orders themselves | **Test or tool**, and the most serious here: a scenario can pass when the orders sent after a failure are lost | Unchanged: Phase 5 waits with `await_new_orders_since` for any 1,000 further orders | **Fixed on 8 November 2026** and closed: Phase 5 names the recovery orders by their ClOrdIDs, and waits while reports keep arriving; made to look for orders never sent, it failed |
| [BUG-0185](bug_list.md#bug_0185) and [BUG-0145](bug_list.md#bug_0145): scenarios 59 and 76 stop the leader the moment `inject_order` has sent | **Test or tool**; the two entries describe one fault | Unchanged: both scenarios send `SIGSTOP` straight after reading `inject_order`'s "sent" line. Scenario 62, which BUG-0145 also named, now waits until the leader's WAL holds the orders | **Fixed on 8 November 2026** and closed: scenarios 59 and 76 stop the leader only once its WAL holds the orders, through the function scenario 62 now uses too |
| [BUG-0149](bug_list.md#bug_0149): scenario 62 can crash reading a WAL segment the sequencer deletes | **Test or tool** | Unchanged: `wal_record_ids`, `wal_records_after` and `wal_entries` list the directory and then open each file, with nothing to allow for a file deleted in between | Pass over a segment that has gone, or read only once the sequencer has finished changing the WAL |
| [BUG-0215](bug_list.md#bug_0215): `perf_run.py` fails every run of the trading-day profile | **Test or tool** | Unchanged: the profile leaves about 11.7 million orders resting, and the development region holds 2 million | A higher cancel ratio in the profile, or the refusals for a full book counted as answered |
| [BUG-0214](bug_list.md#bug_0214): `fix_load_client.py` reports answered messages as unreported | **Test or tool** | Unchanged | As the entry describes |
| [BUG-0014](bug_list.md#bug_0014): Python style warnings in the scripts, and a lint gate that ignores them | **Test or tool**, under way | Andrew's commit 2d28b39 dealt with some of the warnings; the gate does not yet fail on style messages | Finish the warnings, then make the gate fail on them |
| [BUG-0068](bug_list.md#bug_0068): the specification states behaviour that almost nothing tests | **Investigation**, a coverage record | `scripts/check_book_requirements.py` reports the figures on every build | Kept open as the record of coverage; worked with BUG-0229 |
| [BUG-0229](bug_list.md#bug_0229): the functional specification has not been checked against the code | **Decision needed**: Andrew has left it for now | Unchanged; recorded so as not to be forgotten | When to audit the book, and to specify the ordinary order lifecycle |
| [BUG-0173](bug_list.md#bug_0173): no scenario withholds a record during a real catch-up | **Test or tool**, a missing scenario | Unchanged | As the entry describes |
| [BUG-0083](bug_list.md#bug_0083): scenario 26 failed once inside the suite and has not been reproduced | **No longer applies**, as far as can be told | It was one failure in five runs, in August, before phases 3 to 10 replaced the code it ran through; full runs since, such as the 99 of 99 on 7 November 2026, have passed scenario 26 | Close it, if Andrew agrees; a recurrence would be a new entry |
| [BUG-0144](bug_list.md#bug_0144): `ha_test.py` searches each read of a Quill log separately | **No longer applies** | Phase 6 removed every wait on and count of a Quill log: `poll_log_for` no longer exists, and nothing calls `count_lines_with_all` | Close it, if Andrew agrees |
| [BUG-0169](bug_list.md#bug_0169): the high availability tests depend on the wording of Quill log messages | **Fixed** by phase 6 | No check reads a Quill log of the venue ([test_events_inventory.md](availability/test_events_inventory.md)). Three `TEST CONTRACT` messages remain, in the arbiter, the witness and the sequencer, each an operator message a scenario checks deliberately, because what it checks is that an operator is told why | Close it, if Andrew agrees |
| [BUG-0040](bug_list.md#bug_0040): the order-accounting check reports lost orders when it could not count them | **Fixed** | `perf_run.py` now says "NOT VERIFIED -- the run completed, but its order count could not be read" and that this is not a failure of the venue | Close it, if Andrew agrees |
| [BUG-0030](bug_list.md#bug_0030): restart coverage | **Fixed**: the matrix is complete | All eighteen cells are covered, as the entry records. Row R6, "restart with no arbiter reachable, degraded", describes a degraded path that majority leases removed; those scenarios now check that no instance leads without a majority | Close it, if Andrew agrees |

**Recommended order in this area.** BUG-0181 first, because it lets a scenario pass over lost orders,
which is the failure the tests exist to catch. Then BUG-0185 (with BUG-0145 closed into it) and BUG-0149,
which make the suite fail at random and so make its verdict harder to trust. BUG-0215 and BUG-0214 make
the load tools' verdicts trustworthy. Six entries can be closed if Andrew agrees: BUG-0083, BUG-0144,
BUG-0169, BUG-0040, BUG-0030, and BUG-0145 into BUG-0185.

## 8. Operations, deployment and metrics

Seven entries.

| Entry | Judgement | Evidence | What is left |
|---|---|---|---|
| [BUG-0189](bug_list.md#bug_0189): anyone who reaches first-run setup can create the first administrator | **Defect**, of security (R-0153) | Unchanged: nothing in `java/admin-service` asks for a one-time secret on first-run setup | **Fixed on 8 November 2026** and closed: the editor writes a one-time secret, readable only by its owner, while no administrator exists; setup refuses a wrong or missing one with HTTP 403, and creating the first administrator deletes it. Tried against the deployed editor, which found [BUG-0236](bug_list.md#bug_0236): the administrators' file is readable by every account |
| [BUG-0069](bug_list.md#bug_0069): the sequencer, arbiters and witness report no metrics | **Partly done** | The sequencer now registers seventeen metrics (`SequencerThread.cpp`), among them `sequencer_running_alone` and the measures of a slow disk from BUG-0228. The arbiter, the witness and the authentication service register none of their own | Metrics for the arbiters (which is active, leases granted and refused, statements held) and the witness |
| [BUG-0041](bug_list.md#bug_0041): five ways the venue will not start on a RHEL8 target host | **Partly done** | The first of the five is fixed: `db/create_db.py` now finds Liquibase with `shutil.which`. The other four were not checked in this review | Check the other four against the scripts, and the Rocky 8 Docker build, which is blocked on network access in the cloud environment (`doc-audit-progress.txt`) |
| [BUG-0073](bug_list.md#bug_0073): the placeholder environments carry settings nobody chose | **Decision needed** | Unchanged; preprod, prod and test-1 still disable three of the four gateways, and still list `binary_order_gateway_a` among the components started | What each environment is to run, decided before any of them is deployed to |
| [BUG-0060](bug_list.md#bug_0060): microbursts are not measured | **New requirement** | Unchanged | Place in the order of work; the latency work is otherwise complete |
| [BUG-0006](bug_list.md#bug_0006): ResendRequest under load | **Test or tool**, a missing measurement | Unchanged; a resend from far back is also BUG-0113's remainder | Exercise resends in a trading-day run; best done after the recovery service of BUG-0113 |
| [BUG-0047](bug_list.md#bug_0047): disaster recovery is not modelled | **New requirement**, deliberately deferred | Unchanged | Nothing until Andrew says |

**Recommended order in this area.** BUG-0189 first: it is small and a security hole. BUG-0069's
remainder next. BUG-0041's other four points when the venue is next brought up on another host.
