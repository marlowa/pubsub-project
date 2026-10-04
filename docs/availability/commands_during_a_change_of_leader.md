# Commands sent while the sequencers change leader {#commands_during_a_change_of_leader}

## 1. What this document covers

This is the detailed design of part 4.3 of
[change_of_sequencer_leader.md](change_of_sequencer_leader.md): how every command a gateway accepts
from a member is answered when the leading sequencer dies, including the commands sent in the
seconds before the follower takes the lead. That document chose the approach, option A: the gateway
keeps each command until it is answered, and sends the unanswered ones again to the new leader. This
document says how each part of that works, what it costs, and what has been decided about it.
Section 7 records the decisions.

A "command" here is a new order (`NewOrderSingle`) or a request to cancel (`OrderCancelRequest`). The
guarantee is G1 of the parent document: every command a gateway accepted is answered, placed and
reported or refused with a reply, never left unanswered and never placed twice.

## 2. What happens today

- A gateway sends each command to both sequencers, wrapped in a `WalRecord` envelope that names the
  member's session, and keeps no copy (`FixOrderGatewayThread::forward_order_in_envelope`,
  `BinaryOrderGatewayThread::forward_envelope_to_sequencers`).
- The leader writes the command to its log, sends the record to the follower, and sends the command
  to the matching engine when the follower acknowledges the record (part 4.2).
- The follower discards the copy it receives from the gateway, because it writes its log only from
  its leader's records.
- Between the leader's death and the follower taking the lead, every command a gateway sends reaches
  only the follower, which discards it. Nothing else holds it. `ha_test.py` scenario 1 measures this:
  20,000 orders sent while the leader was killed, none of them answered
  ([BUG-0103](../bug_list.md#bug_0103)).

Reading the code shows a second gap, not yet shown by a test. **A command can be in the new leader's
log without the matching engine ever receiving it.** The follower writes a record and acknowledges it;
if the leader dies after the follower wrote the record and before the acknowledgement reached the
leader, the leader never sent the command to the engine. The new leader holds it, and nothing sends it
to the engine: an engine asks a sequencer to catch it up only when the engine itself starts or takes
the lead (`MePositionRequest`), not when the sequencers change leader. The member is never answered.
Section 3.5 covers this, and section 6 describes the scenario that must show it first.

## 3. The design

### 3.1 The gateway keeps each command until it is answered

Each gateway keeps a copy of every command it sends to the sequencers: the encoded `WalRecord`
envelope, exactly as sent, so that sending it again sends the same bytes.

- **The store** is a block of bytes and a list of where each copy lies, both of fixed size, set in
  the gateway's configuration and allocated when the gateway starts, in the form of
  `KeptReportStore` in the sequencer. Nothing on the order path allocates memory.
- **What answers a command.** A command is answered by the first report for it that reaches the
  gateway: a report on the same session whose `ClOrdID` is the command's. For a new order that is the
  acceptance or the rejection. For a request to cancel it is the cancel's report or the refusal of the
  cancel, both of which carry the cancel's own `ClOrdID`. A refusal the gateway sends itself, such as
  a missing field, never enters the store, because the command was never sent.
- **Finding the copy** when a report arrives needs an index from the session and `ClOrdID` to the
  copy: a fixed-size table, allocated at start like the store.
- **Copies leave the store out of order,** because commands are answered out of order. An answered
  copy is marked and its space is reclaimed when every copy older than it has gone too, as the oldest
  copies leave the block in order. One command that stays unanswered for a long time therefore holds
  the space behind it. That is the right behaviour: a venue that cannot answer commands should stop
  taking more.
- **When the store is full,** the gateway refuses the command with a reply, exactly as it refuses a
  command while the venue says it is not accepting orders, and counts it. A full store means commands
  are not being answered, which is a loss of service, so it is logged as a Warning when it starts and
  again when it ends, and published as a metric.

How it is built:

- The store is `fix_common::UnansweredCommandStore`, shared by both gateways, and exists only with high
  availability on, because without it there is no change of leader. A copy is kept just before the
  command is sent, so a command that cannot be kept is not sent.
- A report is matched to its command by the member's comp id and the `ClOrdID`, both of which the
  sequencer puts on the report's envelope. The binary gateway relays a report without decoding it, so
  the sequencer copies the `ClOrdID` from the report it decodes anyway.
- A member's command refused because the store is full is answered as the gateway answers any command
  it refuses itself: a rejected report for an order, an order cancel reject for a cancel, each saying
  the venue is busy. A cancel the gateway generates itself when a member disconnects is sent even if it
  cannot be kept, because no member is waiting to have it refused.
- A unit test counts heap allocations while commands are kept and answered, and requires none.

The normal number of commands in flight is small, a handful per session, because a command is
answered in about 100 microseconds. The store is sized for the commands that wait while the venue
cannot answer, such as during a change of leader or while no matching engine is running.

### 3.2 The gateway learns that a new instance leads, and sends again what it holds

Every new leader already sends `OrderAcceptance` to every gateway on taking the lead. It gains a
field: the leader's epoch.

A gateway remembers the highest epoch it has seen. When an `OrderAcceptance` arrives with a higher
one, it sends every command it is still holding to both sequencers, in the order they were first sent,
each with a new field on the envelope, `sent_again`, set to true. The first `OrderAcceptance` a
gateway receives after it starts sets the epoch without sending anything again, because a gateway
that has just started holds nothing.

Both gateways, FIX and binary, do this the same way.

### 3.3 The new leader forwards kept reports before it says it leads

On taking the lead, the new leader forwards the reports it kept (part 4.4) **before** it sends
`OrderAcceptance`. Both go to each gateway on the same connection, in the order sent, so a gateway
receives the answers to commands it is holding before it is told to send them again, and sends again
only commands that are still unanswered. Today `adopt_role` sends `OrderAcceptance` first; the order
is swapped. Sending a command again that has in fact been answered is harmless, because section 3.4
recognises it, so this is to avoid needless work, not to make the design correct.

### 3.4 The new leader recognises a command it already holds

A command sent again is either in the new leader's log or not, and the gateway cannot tell which.

- **Not in the log:** the old leader never wrote it, or wrote it and died before the follower
  received it, so nothing ever acted on it. The new leader sequences it as a new command, by the
  ordinary path.
- **In the log:** it must not be sequenced again. The new leader discards it, and logs that at Info.
  Its answer comes by one of the routes in section 3.6.

To tell the two apart, the sequencer keeps a **record of the identifiers in its log**: for each
command its log holds, the session's identity and the command's `ClOrdID`. This is the record
R-0119 and R-0010 describe. Section 7 sets out the decisions about its form, how much of the log it
covers, and whether it checks every command or only those sent again.

**Which thread keeps it.** The record is a hash table, and the venue's hash table is confined to one
thread, which it checks. The sequencer thread keeps it, and it is built **only from the log**, never
from the copies a follower receives from the gateways, because a copy a follower received may never
have been written to any log, and treating it as held would lose it.

- While leading, the sequencer thread writes every record itself, and adds each command's identifier
  as it writes it.
- While following, most records are written by the reactor thread, as they arrive from the leader.
  The sequencer thread reads the records written since it last looked, once a second, and adds their
  identifiers. It reads only up to `highest_replicated_seq_no_`, which the writing thread raises only
  after a record is completely written. Whether the log's reader may safely read a segment while the
  reactor thread appends to it, and while a repair cuts the log back, is to be confirmed in the code
  before this is built. If it may not, the reactor thread instead passes each record's sequence number
  and identifier to the sequencer thread through a fixed-size queue with one writer and one reader, and
  the table is still touched by one thread only. On taking the lead it reads the rest before it handles any command sent again, so the
  record is complete before it is used. That read is at most about a second of records.
- At startup, the sequencer already reads its whole log, to build the table of epochs and to find a
  gap. The identifiers are added during the same read.
- Each command's `ClOrdID` is copied onto its envelope by the gateway, and the leader logs it, so that
  the record can be built and kept up to date from the log without decoding each command. The leader
  also logs the member's gateway protocol and instance from the envelope, which it did not before, so
  that a command is identified by the whole of its session.

### 3.5 A command the new leader holds and the engine never received

On taking the lead, the new leader asks the leading matching engine for the highest sequence number of
an order or cancel it has acted on (`EnginePositionQuery`, message 118). The engine answers with that
number (`EnginePosition`, message 119). The new leader then sends it, from its log, every order and
cancel numbered above that and up to the highest record it held when it took the lead. The engine
has never received these, so it acts on them as ordinary orders, and their reports are not marked
as repeats.

- **New orders wait meanwhile.** Until the answer arrives, the new leader writes and replicates each
  new order but does not send it to the engine: it waits with the orders that wait for the
  follower's acknowledgement or for a voter's confirmation, so that the engine never acts on a later
  order before an earlier one. When the answer has been acted on, those orders go the way they would
  have gone.
- **Only an engine that acts on orders answers:** with HA off, the single engine; with HA on, the
  leading one. An engine that is still catching up as part of a promotion does not answer; it is
  catching up from this sequencer, which sends it everything anyway. The new leader asks again once a
  second until it is answered, with a Warning the first time it asks again.
- **If no engine is connected, or the asked engine disconnects,** the new leader stops waiting. The
  engine that acts next catches up from the log before it acts, which sends it everything.
- **Why the engine's answer can be trusted.** The engine is asked on a different connection from the
  one the previous leader used, so in principle an order that leader sent could still be on its way.
  It is not in practice: the new leader leads only once the previous leader's lease has run out,
  seconds after it last sent anything, and the engine has long since read it.
- **Orders sent from the log carry the member's gateway protocol.** The engine files an order under
  the session's comp id and its gateway protocol, so the protocol is copied onto each order sent from
  the log. The same function sends the catch-up an engine performs when it starts or is promoted, so
  this also corrects the catch-up for members of the binary gateway, which were filed under the
  default protocol.

`ha_test.py` scenario 66 checks this ([BUG-0115](../bug_list.md#bug_0115)).

### 3.6 How a command the new leader already holds is answered

A command sent again that is already in the new leader's log is answered by one of three routes, and
nothing new is needed for any of them:

1. The engine applied it and its report reached the follower, which kept it and forwards it on taking
   the lead (part 4.4).
2. The engine never received it, and the new leader sends it from its log (section 3.5).
3. The engine applies it, or reports on it, after the change of leader, and the new leader forwards
   that report by the ordinary path.

One case remains: the engine applied it and its report reached neither sequencer, for example because
the engine's connection to the follower was down while the old leader died. That command stays
unanswered at the gateway. The remedy is an order status enquiry, R-0002, which the venue does not
answer today ([BUG-0089](../bug_list.md#bug_0089)). It is outside this design.

## 4. A change of leader under this design

1. The leader dies. Commands are in flight: some in both logs, some in the old leader's log only, some
   in neither. The gateways hold every one that is unanswered.
2. The gateways go on sending new commands to both sequencers and keeping them. The follower discards
   them and keeps the engine's reports.
3. The follower takes the lead and numbers above every record it holds (part 4.1). It completes its
   record of identifiers from its log.
4. It forwards the kept reports (part 4.4). The gateways release the commands those answer.
5. It sends `OrderAcceptance`, carrying its epoch, and asks the leading engine for its position.
6. It sends the engine any order in its log the engine never received, and the engine applies and
   reports them.
7. Each gateway sends again every command it still holds, marked `sent_again`. The new leader
   sequences those its log does not hold, and discards those it does.
8. Every command is answered once, or answered and repeated with the repeat marked.

## 5. What it costs

| Part | On the ordinary path | Other |
|---|---|---|
| Gateway store | A copy of each command's envelope, about 200 bytes, and an index entry; removed when its report arrives | Memory set in configuration |
| `OrderAcceptance` epoch, `sent_again` | One field each | None |
| Record of identifiers | A hash and an insert per command in the sequencer, on the sequencer thread | Memory: see section 7, decision 1. A read once a second while following |
| Engine position on a change of sequencer leader | None | A round trip between sequencer and engine at each change of leader, before new commands are sent |

The cost on the ordinary path is to be measured by the method in
`docs/operations/latency_findings.md` once built, as part 4.2's was.

## 6. Tests

Each test was shown to fail before the change it tests was made.

| Test | What it requires |
|---|---|
| Scenario 66 | The follower's acknowledgements to the leader are blocked with `libblock_sends_to_ports.so`, so the follower writes and acknowledges orders the leader never sends to the engine; the leader is stopped and killed. Every order is applied once and answered. Section 3.5, [BUG-0115](../bug_list.md#bug_0115) |
| Scenario 67 | An order, then the same order marked as sent again, then an order marked as sent again that was never sent before. The first is applied once and never refused as a duplicate; the last is applied once. Section 3.4 |
| Scenario 68 | A member sends orders at 100 a second from a second before the leader is killed until two seconds after the follower takes the lead. Every order is answered, and none is applied twice. Before the gateways kept commands, 266 of 598 orders were never answered |
| Unit tests | `UnansweredCommandStore`, including a test against a simple model and a test that counts heap allocations; `LoggedCommandIdentifiers`; `LogTailIndex`, against a real log |

Scenario 1 sends its 20,000 orders while the leader is killed, but they now all reach the venue before
the kill, so it no longer exercises orders sent during the change of leader; scenario 68 does.

## 7. Decisions

**Decision 1, decided: the record is a `tsl::robin_set<uint64_t>` reserved for 200 million
identifiers.** The venue plans for 50 million orders a day and tests with 100 million; a MiFID test
doubles that. Each identifier is a 64-bit number derived from the session's comp id, its gateway
protocol and the `ClOrdID`.

- *Memory.* The table is reserved at startup and never grows during the day, so it never pauses to
  rehash. Its size is a power of two and each slot is 16 bytes, the number and the library's
  bookkeeping, so 200 million identifiers at a maximum fill of 0.9 take 2^28 slots, 4 GiB. Reserving it
  writes to all of it at startup, so inserting during the day never waits for the kernel to supply a
  page. Before each insert the sequencer compares the number held with the reserved size and treats
  reaching it as full, rather than letting the table grow. The table uses the standard allocator, so it
  is given `GrowthReportingAllocator`, as other containers whose storage is not the venue's own are.
- *Why a match is checked.* An identifier is a string of up to dozens of characters, and a 64-bit
  number cannot give each one a different value, so two different identifiers can, rarely, give the
  same number. A number that is not in the table means the command is certainly not in the log. A
  number that is in it means only that the command may be, so the new leader checks: it searches its
  own log, backwards from the end, for a record with exactly the same comp id, protocol and `ClOrdID`.
  It searches no further back than the time the gateway first received the command, which the envelope
  carries (`gateway_ingress_ns`), less five seconds for any difference between the machines' clocks:
  normally a few seconds of records. Found, the command is held and is not sequenced again; not found,
  the numbers merely matched, and the command is sequenced as new. A match therefore never causes a
  command to be lost. Right after a change of leader many commands can need this check, so the search
  is not repeated for each: the new leader builds an exact index of the end of its log
  (`LogTailIndex`) on the first one, reading each record once, forwards for records written since and
  backwards only as far as a command's time requires, and discards it a minute after the last command
  sent again. A command that carries no receipt time, such as a cancel a gateway generated itself when a
  member disconnected, is looked for in the whole log.
- *The same check covers records a repair discards.* When a rejoining follower discards records its
  leader does not hold, their identifiers stay in the table. A command sent again that matches one is
  not found in the log, and is sequenced as new. Nothing needs removing from the table.
- *When the table is full,* the sequencer logs an Error once. Commands logged after that are not in the
  table, so a number that is not in it no longer proves anything, and every command sent again is then
  checked against the log by the same index. Nothing is refused: refusing a command that may be live
  would tell a member it was refused when it was not.

**Decision 2, decided: only commands marked `sent_again` are checked, in this part.** Checking every
command, which would meet R-0119, is a separate change, recorded as [BUG-0114](../bug_list.md#bug_0114).

**Decision 3, decided: the record covers the whole log, which is the whole trading day.** Every trading
day starts with no log: after end of day the processes are stopped and the logs moved away, and the
next day starts with none. So a log holds exactly one day, and a `ClOrdID` used on an earlier day is
never in it. A sequencer that starts at the start of the day has an empty log and builds an empty
record. Only a sequencer restarted during the day reads records at startup, and it already reads its
whole log then, to build the table of epochs and to find a gap; the record adds a hash per record to
that read. How long that read takes late in a busy day is part of a wider question, recorded under
[BUG-0113](../bug_list.md#bug_0113): the size of the log, which the move of resends to a separate
recovery service is to bound.

**Decision 4, decided: room for 262,144 commands and 64 MiB in each gateway's store.** The gateway keeps a copy of each command until
its answer arrives, normally a tenth of a millisecond later, so the store normally holds only a
handful. It fills only while answers stop arriving, chiefly during a change of sequencer leader,
about three seconds, when every command members send through that gateway is added and none removed.
It must hold all of them, or the gateway refuses the rest with a reply saying the venue is busy. Each
copy is about 200 bytes. At the highest rate measured, about 34,000 orders a second for the whole
venue, room for 65,536 commands (16 MiB) lasts about two seconds if one gateway carries all of it, less
than a change of leader takes. So the store has room for 262,144 commands, 64 MiB, about eight seconds
of the venue's whole peak through one gateway. Both are set in the gateway's configuration.

**Decision 5, decided: a session that disconnects keeps its commands.** They are sent again at a change
of leader like any other, and dropped when the session's grace period ends. The session's identity
outlives the connection, so a report for one of them is routed to the member when it reconnects, as it
is now.

## 8. The order of the work

All six steps are built.

1. The scenario for section 3.5, shown to fail; then the position query on a change of sequencer leader.
2. The record of identifiers in the sequencer, with its unit tests. Reserving it for 200 million
   identifiers takes about 0.9 seconds at startup, measured, well within the guide of about five
   seconds.
3. `sent_again` on the envelope and the check in the sequencer.
4. The epoch on `OrderAcceptance`, and the reordering of section 3.3.
5. The gateway store and sending again, in the FIX gateway and the binary gateway.
6. Scenario 68 in place of strengthening scenario 1, for the reason in section 6.

One consequence found while testing is recorded as [BUG-0116](../bug_list.md#bug_0116): the reports a
new leader forwards from part 4.4 include almost every report of the last few seconds, which the old
leader had already forwarded, so a member can receive tens of thousands of repeats at once.

Related: [change_of_sequencer_leader.md](change_of_sequencer_leader.md),
[majority_leases.md](majority_leases.md), [order_acceptance.md](order_acceptance.md), and the
requirements R-0002, R-0003, R-0006, R-0010, R-0119 and R-0122 in the functional specification.
