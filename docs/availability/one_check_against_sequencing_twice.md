# One check against sequencing a command twice {#one_check_against_sequencing_twice}

This is the design of phase 9 of the [recovery plan](../recovery_plan.md). It covers:

- [BUG-0122](../bug_list.md#bug_0122): a command is protected from being sequenced twice only on particular paths, and
  each new way for a command to arrive twice has needed its own mechanism.
- [BUG-0114](../bug_list.md#bug_0114): an order identifier used earlier in the day is accepted again once its first
  order has ended, which requirement R-0119 forbids.
- The second half of the phase: moving the gateways' store of unanswered commands, and their sending of those commands
  again, onto the library's client side ([ha_library_boundary.md](ha_library_boundary.md), section 7).

The rule the phase brings the code up to is principle V7 of [design_principles.md](../design_principles.md), agreed on
11 October 2026: the leading sequencer places a command in its WAL at most once, judged by the command's identity, and
checks every command, in one check, whatever path it arrived by.

Section 1 says how the venue behaves now. Sections 2 to 6 are the design. Section 7 says what goes. Section 8 is the
steps and how each is known to be right. Section 9 holds the decisions.

## 1. What happens now

**A command's identity** is the member's comp id, the protocol it arrived by (FIX or binary), and its `ClOrdID`. The
gateway copies all three onto the envelope it sends the sequencers, and the leader writes them into the WAL with the
command.

**The record of identifiers.** The sequencer keeps a record of the identity of every command its WAL holds,
`LoggedCommandIdentifiers`. Each identity is held as a 64-bit number made from the three values, in a hash set reserved
at startup for 200 million identities: 2^28 slots of 16 bytes, 4 GiB. A number is not the identity itself, since two
different identities can, very rarely, give the same number. So the record answers only "the WAL certainly does not
hold this command" or "the WAL may hold it". The record is built from the WAL alone: the sequencer adds each command's
number as it writes the command, whether it leads or follows, and at startup a thread of its own
(`LoggedCommandIdentifiersBuilder`) reads the WAL that is already there.

**When the record says "may hold"**, the sequencer finds out for certain with `LogTailIndex`. It reads the WAL
backwards from its end, segment by segment, to the time the gateway first received the command, which the envelope
carries, and compares the exact identities. This read is on the thread that sequences orders. It is cheap only because
every command it checks today is recent.

**Which commands are checked.** Only these:

1. A command its gateway marked `sent_again`. A gateway marks every command it sends again: after a new leader
   announces itself, and on a connection to a sequencer that has been established again
   ([commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md), section 3.2).
2. While one gateway instance has two connections open to the leader, every command from that gateway, marked or not.
   Commands written on the old connection may still be unread at the leader, and could be read after their marked
   copies on the new one. Since they are not marked, nothing else would check them. The leader tracks how many
   connections each gateway instance has open for this alone (`note_command_connection`).

Every other command is sequenced without a check. That is safe only as long as an unmarked command can never arrive
after a marked copy of itself, which is why each way of sending a command again has needed its own argument, and the
second rule above has needed its own mechanism.

**A command found in the WAL** is not sequenced again. It is answered by one of the three routes of section 3.6 of
[commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md): the report the follower kept and
forwards on taking the lead, the command sent from the WAL to an engine that never received it, or the engine's report
of it after the change of leader.

**A member reusing an identifier.** The only check is the matching engine's, against the orders on its book
(`MatchingEngineThread::handle_new_order_single`, `order_book_.contains`). An identifier whose order has been filled
or cancelled is no longer on the book, and is accepted again (BUG-0114). The same holds for a member that, after its
gateway restarted, resubmits under the original `ClOrdID` an order the venue had already taken, as requirement R-0003
allows: if that order has ended, the resubmission becomes a second order, which R-0006 and R-0010 forbid.

**The gateway's side.** Each gateway keeps a copy of every command until the first report for it arrives
(`fix_common::UnansweredCommandStore`), and matches a report to its copy by comp id and `ClOrdID`. A command whose comp
id and `ClOrdID` the store already holds unanswered, a member resubmitting before it has had an answer, is not kept a
second time, but it is still sent to the sequencers.

## 2. The design in one paragraph

The leading sequencer checks every command, before giving it a number, against the record of identifiers. The record
holds, beside each identity's number, the position in the WAL of the command that has it, so a possible match is
proved or disproved by reading that one record, however long ago it was written. A command whose identity the WAL
does not hold is sequenced as now. One whose identity the WAL holds is not sequenced. If its gateway marked it as sent
again, it is answered with what became of the original, from the WAL where the WAL holds the answer. If not, it is a
member using an identifier again, and the leader refuses it: it sends the gateway a refusal, which the gateway turns
into a rejection for the member. The refusal is not written into the WAL, because nothing in the WAL changes. The rule
about two connections, the tracking of connections it needs, and `LogTailIndex` all go.

## 3. The check

### 3.1 One check, on every command, in the leader

The leader checks every new order and every cancel, whatever path it came by. Only the leader checks: a follower writes
its WAL only from its leader's records, and an instance that does not yet know its role discards the gateway's copies,
both as now.

The check is made on the thread that sequences orders, just before the command is given its number, in the place where
the two narrower checks are made today (`SequencerThread`, the handling of an order envelope).

A command that does not say whose it is, without a comp id or a `ClOrdID`, cannot be checked. The gateways never send
one: both gateways drop a command without a `ClOrdID`, since a refusal has to name the command by it, and both put the
comp id on every envelope. The leader sequences such
a command without a check, as now, and in addition counts them and logs the first at Error, since one means a gateway
is not doing what this design relies on.

### 3.2 The record holds each command's position

Each entry of the record becomes a pair: the identity's 64-bit number, and the position in the WAL of the command that
has it, the segment and the offset within it (`WalPosition`), packed into one 64-bit number. The set becomes a map,
still a `tsl::robin_map`, still reserved for its full size at startup and never grown, so no insert during the day
rehashes the table or waits for the kernel to supply a page.

**What it costs in memory.** A slot of the map is 24 bytes, against 16 for the set, measured with `sizeof` on the
`tsl` bucket type. At the 2^28 slots reserved for 200 million identities, the record grows from 4 GiB to 6 GiB.

**Where the positions come from.** The sequencer writes each command into the WAL itself, while leading and while
following, and the WAL's writer knows the position of every record it writes (`WalWriter::current_position`). At
startup, `LoggedCommandIdentifiersBuilder` reads the WAL with `WalReader::replay_segment`. The reader knows the offset
of each record it reads but does not pass it on: its callback is given the record's number and bytes only. It gains
the offset, a small change to the library that its other callers can ignore.

**Proving a match.** When the map holds a command's number, the leader reads the one record at the position beside
it, decodes its comp id, protocol and `ClOrdID`, and compares them with the command's. If they are equal, the WAL holds
the command. If not, the two identities merely gave the same number; the WAL does not hold the command, and the command
is sequenced as a new one. That case is expected to arise about once in the lifetime of the venue (section 3.4), and
the leader logs it at Warning when it does, with both identities.

**What the read costs.** One read of one record, on the thread that sequences orders. A command sent again is recent,
so its record is almost always still in memory, in the page cache. A member reusing an identifier from hours earlier
may need a read from disk, of the order of 0.1 to 10 milliseconds, while orders behind it wait. Reusing an identifier
is a member's mistake, rare, and costs the venue that one read, not a read of every record written since, as
`LogTailIndex` would. Section 8, step 5, measures it.

**When the record is full.** As now, the record accepts no more entries once it holds as many as it was reserved for,
and says so once at Error. A command whose number is not in a full record may still be in the WAL. The leader then has
no position to read, and must search the WAL for it; with a full record it does that with the same backwards read as
today. The record's size is set so that this does not happen on a normal day (50 million orders, and the test of 100
million), and the Error asks for `commands.identifiers_reserved` to be raised.

**While the record is still being built**, in the second or so after the sequencer starts, a command the part not yet
read may hold is checked the same way, by searching the WAL, as now.

### 3.3 What is sent back for a command already in the WAL

**Marked as sent again.** Its gateway sent it again because it had no answer. The original's answer is one of:

1. **A report the WAL already holds.** The engine applied the original, the report reached this sequencer and is in
   its WAL, but the gateway does not have it: the report was lost with a connection, or was on its way when the leader
   changed. The leader reads forward from the original's position, which the record now gives it, to the first report
   that answers it, and forwards that report again, marked as a possible repeat, by the ordinary path for reports. The
   read is short: an order is normally answered within about 100 microseconds, a few records later. **This is new**
   (decision 4). Today nothing answers this case except the follower's kept reports, which cover only the last 4.25
   seconds.
2. **No report yet.** The engine has not yet applied the original, or applies it after the change of leader; its
   report arrives by the ordinary path, or the original is sent from the WAL to an engine that never received it, as
   now (routes 2 and 3 of section 3.6 of [commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md)).

How far forward the leader reads before deciding there is no report is bounded. It stops at the end of what is written,
and does not read further than a fixed number of records, 4,096; a command not answered within that many records is
waiting for its engine, which answers it by route 2.

**Not marked.** The member sent a command with an identity it has used today. The leader refuses it (section 4). That
is R-0119 for an identifier whose first order has ended; it is R-0006 and R-0010 for an order resubmitted after a
gateway restart, which the venue had already taken; and it is the same answer for a member that sends a command again
under the same `ClOrdID` before it has had an answer to the first.

### 3.4 How often two identities give the same number

Two identities give the same number with a chance of one in 2^64 for each pair. On a day of 50 million commands, the
chance that some new command's number equals that of a different command already in the record is about 50 million
squared divided by 2^65, about 7 in 100,000. At 250 trading days a year, that is about once in 60 years. Because a
possible match is always proved by reading the record (section 3.2), such a coincidence costs one read and a Warning,
and never a wrong answer.

## 4. The refusal

### 4.1 From the sequencer to the gateway

The leader sends the refusal to the gateway the command came from, on the connection it came by, as a new message from
the sequencer to the gateways, `CommandRefused`. It carries the command's comp id, protocol and `ClOrdID`, whether it
was a new order or a cancel, the reason, `IdentifierAlreadyUsed`, and the position in the WAL of the command that used
the identifier first.

**It is not written into the WAL.** Nothing in the WAL, the book, or the matching engines changes, and the command
itself is never written: that is what V7 requires. The refusal is the leader's answer to one message, like the answer
to an operator's request in [voided_commands.md](voided_commands.md), section 6.2.

**If it is lost**, because the connection or the leader fails before the gateway has it, the gateway still holds the
command unanswered, and sends it again, marked, to the next leader. That leader finds the identity in its WAL, sees the
mark, and answers as for any command sent again (section 3.3): with the report of the original command, marked as a
possible repeat. The member then has a repeat of the first order's report under that `ClOrdID`, rather than a refusal.
It is told nothing false, since that report is what became of the identifier, and its gateway's copy is answered. This
happens only when a member reuses an identifier within moments of a change of leader.

### 4.2 The gateway turns it into a rejection

On `CommandRefused`, the gateway answers the member as it answers a command it refuses itself:

- a new order: an execution report with order status Rejected and `OrdRejReason` 6, duplicate order, and the text
  "ClOrdID already used today";
- a cancel: an order cancel reject with `CxlRejReason` 6, duplicate ClOrdID received, and the same text;
- the binary gateway sends its own forms of both, with the same reasons.

It then marks its copy of the command answered.

### 4.3 The gateway refuses what it already holds unanswered

A gateway that is asked to send a command whose comp id and `ClOrdID` it already holds unanswered refuses it itself, at
once, with the same rejection, and does not send it. This is the case the store already recognises (`Kept::already_held`)
and today sends on regardless.

This is needed for the refusal to be read correctly. The gateway matches every answer to its copy by comp id and
`ClOrdID`. If it sent the second command on, the leader's refusal of it would carry the same comp id and `ClOrdID` as
the first command, which is still waiting for its answer, and the gateway would mark the first command's copy answered.
With the gateway refusing the second command itself, every `CommandRefused` a gateway receives is for a copy it holds,
and for no other.

## 5. The matching engine's own check

The engine refuses a new order whose `ClOrdID` is on its book. Once the leader checks every command, no command whose
identity the WAL holds reaches the engine, so the engine's check never refuses anything. It stays (decision 5): it
costs one lookup the engine makes anyway, both engines make it alike so it does not disturb their agreement, and it
protects the book if a command ever reaches the engine by a path the sequencer did not check, such as a WAL written by
hand for a test.

## 6. Where the check lives

[ha_library_boundary.md](ha_library_boundary.md), section 6, places the check in the library's replicated WAL: the
library keeps the record and makes the check, and the application supplies how to read a command's identity and what
to send back. The plan's phase 9 says the same.

Decision 6 asks whether to build it there now, or in the sequencer first and move it in phase 10, when what remains of
the sequencer is divided. Building it in the library means:

- `LoggedCommandIdentifiers` and `LoggedCommandIdentifiersBuilder` move into the library, unchanged apart from the
  positions, with their unit tests.
- `ReplicatedWal` gains a check the application turns on, with two functions the application gives it: one that reads
  a command's identity from the bytes the application asks the WAL to append, and one that the WAL calls when a
  command's identity is already held, with the position of the first, so the application decides what to send back
  (section 3.3 and section 4). The library knows nothing of comp ids, protocols, `ClOrdID`s or reports.
- The record's size, `commands.identifiers_reserved` today, becomes a setting of the replicated WAL.

## 7. What goes and what changes

**Goes:**

- the rule that every command from a gateway with two connections open is checked, `note_command_connection`,
  `forget_command_connection`, `command_connections_` and their logs;
- `LogTailIndex` and its unit tests, except for the backwards search a full or unfinished record still needs
  (section 3.2), which becomes a function of the record's owner;
- the sequencer's counts of commands sent again that were already in the WAL, which become counts kept by the check.

**Changes:**

- the record of identifiers holds positions, and grows from 4 GiB to 6 GiB at its present size;
- a gateway refuses a command whose identity it holds unanswered, instead of sending it on;
- a member reusing an identifier, or resubmitting an order the venue took, is refused with a reason, whatever became of
  the first order;
- design_principles.md, V7: "Today" becomes "holds";
- the functional specification: R-0119 is covered by a new scenario and its gap is removed; R-0006 and R-0010 are
  covered if the scenario for a gateway restart is written (section 8, step 3).

**Stays:** the mark `sent_again`, which now only chooses the answer; the gateways sending again everything they hold
whenever a new leader announces itself or a connection to a sequencer is established again; the routes by which a
command sent again is answered.

## 8. Steps

Each step is built, made to fail on purpose, and run through both full suites of `ha_test.py`, with the primary engine
first and with `--secondary-engine-first`, before the next starts.

1. **The record holds positions, in the library** (with decision 6 A; otherwise in the sequencer). The record and its
   builder move into the library and hold each command's position; the replicated WAL proves a possible match by
   reading the record at that position. Behaviour is unchanged: the sequencer still checks only the commands it checks
   today. Unit tests: a match is proved by reading the record; two identities with the same number are told apart (a
   test sets the number so that they collide); a full record and an unfinished one fall back to the search. Made to
   fail: with the comparison of identities removed, the collision test fails.
2. **The refusal.** `CommandRefused`, and both gateways turning it into a rejection; both gateways refusing a command
   whose identity they hold unanswered (section 4.3). Unit tests in each gateway. Nothing sends `CommandRefused` yet,
   so the scenarios are unchanged.
3. **Every command is checked.** The leader checks every command, refuses an unmarked one already held, and answers a
   marked one as section 3.3 says. The rule about two connections, its tracking, and `LogTailIndex` go. New scenarios:
   - a member reuses the `ClOrdID` of an order it has cancelled, and of an order that has been filled, and each is
     refused with reason 6 while the first order's record is unchanged (R-0119);
   - a member sends a second order under a `ClOrdID` before the first has been answered, and the gateway refuses it
     itself;
   - a gateway is restarted while a member's order rests, the member logs on again and resubmits that order under its
     `ClOrdID`, and it is refused and no second order rests (R-0006, R-0010). This is the scenario the specification
     says is missing; it needs a way to restart one gateway in `ha_test.py`.
   The existing scenarios for commands sent again (66, 67, 68, 71) must pass unchanged. Made to fail: with the check
   left only for marked commands, the reuse scenarios fail; with the gateway's own refusal of a held identity removed,
   the second scenario fails, with the first order's copy marked answered by the refusal of the second.
4. **The answer from the WAL** for a marked command whose original's report the WAL holds (section 3.3, item 1;
   decision 4). A scenario drops the report on its way to the gateway, by a test fault, and changes the leader; the
   gateway's copy must be answered by the report from the WAL, marked as a possible repeat. Made to fail: without the
   read forward, the copy stays unanswered.
5. **The cost.** The time the check adds to the sequencing of an order, measured on the order path by the method of
   [latency_findings.md](../operations/latency_findings.md), as part of the sequencer's time to send an order on; and
   the time of one read of a record that is not in the page cache. BUG-0122 estimates the lookup at about 100
   nanoseconds; this step replaces the estimate with a measurement.
6. **The gateways onto the library's client side** (decision 7). `UnansweredCommandStore` and the sending of commands
   again move into the library, as [ha_library_boundary.md](ha_library_boundary.md), section 7, describes: the library
   keeps the copies and sends them again on a change of leader or a connection established again, and refuses new
   commands when the store is full; each gateway supplies how to recognise an answer and what to tell its member when
   the store is full. Behaviour is unchanged; every scenario for commands sent again is the check.

BUG-0114 and BUG-0122 close at the end of step 4.

## 9. Decisions

Decisions 1, 3, 4, 5 and 7 are separate yes-or-no questions. Decisions 2 and 6 each ask you to choose one of the
options given.

### Decision 1. Does the leader check every command, in one check, refusing an unmarked command already in the WAL and answering a marked one with what became of the original, so that the rule about two connections goes? (Yes or no.)

Sections 3.1 and 3.3. This is V7 as agreed on 11 October 2026; the question is whether this is the shape it takes.

**Recommendation: yes.**

### Decision 2. How is a possible match proved? (Choose one.)

Section 3.2. The record says only that the WAL may hold a command, so a match must be proved before a command is
refused or answered as already held.

- **A. The record holds each command's position, and the leader reads that one record.** Exact, and one read however
  old the first command is. The record grows from 4 GiB to 6 GiB. `LogTailIndex` goes, apart from the search a full or
  unfinished record needs. It is also what makes decision 4 possible.
- **B. The record stays as it is, and an unmarked match is refused on the number alone**, without reading the WAL; a
  marked one is still proved by reading backwards, as now, which is cheap because it is recent. No more memory and no
  read for a reused identifier. A new command would be wrongly refused about once in 60 years (section 3.4), with the
  reason "ClOrdID already used today", which the member would find inexplicable; it would lose nothing, since it would
  be told. Decision 4 is not possible.
- **C. The record stays as it is, and every match is proved by reading backwards**, as now. A reused identifier from
  the morning would make the leader read hours of the WAL on the thread that sequences orders, while every order
  waits. Not recommended.

**Recommendation: A.** It is exact, and the 2 GiB buys the removal of a mechanism and the answer of decision 4.

### Decision 3. Is a reused identifier refused by the leader sending the gateway a refusal that is not written into the WAL, with the gateway refusing itself a command whose identity it holds unanswered? (Yes or no.)

Section 4. The refusal changes nothing in the WAL, the book or the engines. The gateway's own refusal is what lets it
read the leader's refusal correctly.

**Recommendation: yes.**

### Decision 4. When a command marked as sent again is already in the WAL, and the WAL holds the report answering it, does the leader forward that report again, marked as a possible repeat? (Yes or no.)

Section 3.3, item 1. Today a report lost between the leader and the gateway is recovered only if it was in the last
4.25 seconds, from the follower's kept reports, or by the member asking for a FIX resend. This needs decision 2 A.

**Recommendation: yes.**

### Decision 5. Does the matching engine keep its own check against the orders on its book? (Yes or no.)

Section 5. It never refuses anything once the leader checks every command, and costs nothing extra.

**Recommendation: yes.**

### Decision 6. Where is the check built? (Choose one.)

Section 6.

- **A. In the library's replicated WAL now**, as [ha_library_boundary.md](ha_library_boundary.md), section 6, and the
  plan place it. More work in step 1, and no second move later.
- **B. In the sequencer now, moved into the library in phase 10**, when what remains of the sequencer is divided. Less
  work now, and the same code moved twice.

**Recommendation: A.**

### Decision 7. Are the gateways moved onto the library's client side in this phase, as step 6? (Yes or no.)

Section 8, step 6. The plan puts it in phase 9. It changes no behaviour, so it could wait without harm; doing it here
keeps the store of unanswered commands and the check that makes sending again safe in one phase.

**Recommendation: yes.**
