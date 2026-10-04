# Commands sent while the sequencers change leader {#commands_during_a_change_of_leader}

## 1. What this document covers

This is the detailed design of part 4.3 of
[change_of_sequencer_leader.md](change_of_sequencer_leader.md): how every command a gateway accepts
from a member is answered when the leading sequencer dies, including the commands sent in the
seconds before the follower takes the lead. That document chose the approach, option A: the gateway
keeps each command until it is answered, and sends the unanswered ones again to the new leader. This
document says how each part of that works, what it costs, and which questions need a decision before
it is built. Section 7 lists those questions.

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
  identifiers. On taking the lead it reads the rest before it handles any command sent again, so the
  record is complete before it is used. That read is at most about a second of records.
- At startup, the sequencer already reads its whole log, to build the table of epochs and to find a
  gap. The identifiers are added during the same read.

### 3.5 A command the new leader holds and the engine never received

On taking the lead, the new leader asks every matching engine to catch up from it. The leading
engine sends `MePositionRequest`, giving the last record it applied, and the new leader sends it every
command after that from its log, by the existing catch-up path, before any new command. The engine
marks the reports from a catch-up as possible repeats, as it already does. The new leader holds new
commands until the catch-up is complete, using the deferral it already has for a matching engine that
is not ready.

This adds a small new message, from the sequencer to the engines, asking them to catch up. An engine
already performs the catch-up when it starts or is promoted; this lets a new sequencer leader ask for
it too.

### 3.6 How a command the new leader already holds is answered

A command sent again that is already in the new leader's log is answered by one of three routes, and
nothing new is needed for any of them:

1. The engine applied it and its report reached the follower, which kept it and forwards it on taking
   the lead (part 4.4).
2. The engine never received it, and the catch-up of section 3.5 sends it.
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
5. It sends `OrderAcceptance`, carrying its epoch, and asks the engines to catch up.
6. The leading engine catches up, applying any command in the log it never received, and reports.
7. Each gateway sends again every command it still holds, marked `sent_again`. The new leader
   sequences those its log does not hold, and discards those it does.
8. Every command is answered once, or answered and repeated with the repeat marked.

## 5. What it costs

| Part | On the ordinary path | Other |
|---|---|---|
| Gateway store | A copy of each command's envelope, about 200 bytes, and an index entry; removed when its report arrives | Memory set in configuration |
| `OrderAcceptance` epoch, `sent_again` | One field each | None |
| Record of identifiers | A hash and an insert per command in the sequencer, on the sequencer thread | Memory: see section 7, decision 1. A read once a second while following |
| Catch-up on a change of sequencer leader | None | A round trip between sequencer and engine at each change of leader, before new commands are sent |

The cost on the ordinary path is to be measured by the method in
`docs/operations/latency_findings.md` once built, as part 4.2's was.

## 6. Tests

Each test is shown to fail before the change it tests is made.

| Test | What it requires |
|---|---|
| Scenario 1, strengthened | Every one of the orders sent while the leader is killed is answered, accepted or refused, and none is placed twice. Today none of 20,000 is answered |
| New: a command in the log the engine never received | The follower's acknowledgements to the leader are blocked with `libblock_sends_to_ports.so`, so the follower writes and acknowledges orders that the leader never sends to the engine. The leader is killed. Every order is answered. This shows the gap of section 3.5 before it is fixed |
| New: a command sent again that the new leader holds | The member is answered once, and the matching engine holds one order for it, not two |
| New: the gateway's store is full | A command beyond the store's capacity is refused with a reply, and the venue says why |
| Unit tests | The gateway store, its index, and the record of identifiers, including a model test like the one for `KeptReportStore` |

## 7. Decisions needed

These are separate questions, each needing its own answer.

**Decision 1. The form of the record of identifiers.** The venue plans for 50 million orders a day
and is tested with 100 million (section 7 of the parent document).

- *Recommended: a set of 64-bit hashes, of fixed size set in configuration, allocated at start.* Each
  identifier is a 64-bit hash of the session's comp id, its gateway protocol and the `ClOrdID`. The
  table is kept at most half full, so 50 million identifiers need 2^27 slots of 8 bytes, 1 GiB; the
  100 million test needs 2 GiB. A fixed size means the table never grows during the day and never
  pauses to grow. Two different commands with the same hash would make the new leader treat a command
  sent again as one it holds, and not sequence it. With 50 million identifiers and a thousand commands
  sent again, the chance of that at one change of leader is about one in 370 million.
- *Alternative: 128-bit hashes,* which make a collision impossible in practice, at twice the memory:
  2 GiB at 50 million, 4 GiB at 100 million.
- *Alternative: the venue's growing hash table, `IncrementalRehashMap`,* which needs no size in
  configuration but holds a value with each key and keeps two tables during growth: about 2.3 GiB at
  50 million and more while growing.
- *When a fixed-size table is full,* the sequencer cannot record more identifiers. It would log an
  Error and, for a command sent again that it cannot check, refuse it with a reply rather than risk
  placing it twice. The table would be sized so that this does not happen in a day.

**Decision 2. Which commands the record checks.**

- *Recommended: only commands marked `sent_again`, in this part.* That is what G1 needs, and it costs
  nothing on the ordinary path beyond the insert.
- *Alternative: every command,* which would also meet R-0119 (an identifier used today is refused for
  the rest of the day), a gap the specification records. It costs a lookup per command on the order
  path, typically a cache miss in a table of a gigabyte, and the sequencer would have to produce the
  refusal report itself, which today only the matching engine does. It is a separate change and is
  better made on its own.

**Decision 3. How much of the log the record covers.** R-0119 bounds the record by the trading day,
but the sequencer does not yet hold the trading day: the technical events that carry it are designed
in [trading_phases.md](../venue/trading_phases.md) and not built.

- *Recommended: every command in the log, until the trading day exists.* The log is not reclaimed
  today ([BUG-0048](../bug_list.md#bug_0048)), so a log that spans several days gives a record that
  spans them too, and the table must be sized for that, or the venue started with a fresh log each day
  as it is now. The record is cut back to the trading day when the technical events are built.
- How long the startup read takes at 50 million records is to be measured against the guide of about
  five seconds for startup.

**Decision 4. The size of each gateway's store.** Recommended: 65,536 commands and 16 MiB, set in the
gateway's configuration. At the highest rate measured, about 34,000 orders a second across the venue,
that holds about two seconds of every order the venue takes, through one gateway. A change of leader
takes about three seconds, so a gateway taking the venue's whole flow would refuse some commands
during it, with a reply, rather than hold them. A larger store costs only memory.

**Decision 5. A session that disconnects while its commands are held.** Recommended: keep them, send
them again at a change of leader like any other, and drop them when the session's grace period ends.
The session's identity outlives the connection, so a report for one of them is routed to the member
when it reconnects, as it is now.

## 8. The order of the work

1. The scenario for section 3.5, shown to fail; then the catch-up on a change of sequencer leader.
2. The record of identifiers in the sequencer, with its unit tests, and the startup read measured.
3. `sent_again` on the envelope and the check in the sequencer.
4. The epoch on `OrderAcceptance`, and the reordering of section 3.3.
5. The gateway store and sending again, in the FIX gateway and then the binary gateway.
6. Scenario 1 strengthened, and the remaining scenarios.

Related: [change_of_sequencer_leader.md](change_of_sequencer_leader.md),
[majority_leases.md](majority_leases.md), [order_acceptance.md](order_acceptance.md), and the
requirements R-0002, R-0003, R-0006, R-0010, R-0119 and R-0122 in the functional specification.
