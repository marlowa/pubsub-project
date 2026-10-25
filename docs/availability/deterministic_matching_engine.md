# A deterministic matching engine {#deterministic_matching_engine}

> **Status: decided, 25 October 2026; step 1 of section 8 built.** This is the design for phase 7 of the
> [recovery plan](../recovery_plan.md). The state machine of section 7.1 is built; steps 2 to 6 are not. Andrew answered the five decisions of section 9 on
> 25 October 2026: identifiers are scrambled with FF1, the day key is derived from a secret and the trading date, an
> identifier is a letter and 16 hexadecimal digits, the leading sequencer measures the absence rule, and cancelling
> on promotion becomes a command in phase 7.

## 1. What this is for

Phase 7 makes the matching engine **deterministic**: given the same commands in the same order, it always
produces the same book and the same execution reports, byte for byte, whichever machine it runs on, however often
it has restarted, and whether it led or followed when it applied them.

That is what phase 8 needs. In phase 8 both engines of the pair apply every committed command from the WAL, and the
follower's book is then the leader's book because the two applied the same commands, not because one was copied to
the other. If the two engines could reach different books from the same commands, phase 8 would replace one way of
losing orders with another.

Phase 7 changes only the engine and the commands it is given. The follower engine still receives the leader's book
as `BookUpdate` messages until phase 8 removes them.

**Finished when** both engines, run on the same WAL, produce identical reports, and the high availability scenarios
pass. Section 7 says how that is checked.

## 2. Why the engine is not deterministic now

These are the places, found by reading `MatchingEngineThread.cpp` and `OrderBook.hpp`, where the same commands can
lead to a different book or different reports.

1. **Identifiers come from counters kept in memory.** `OrderID` is `ME-ORD-` and a counter, `order_id_counter_`, and
   `ExecID` is `ME-EXEC-` and another, `exec_id_counter_`. After a restart the order counter is set above the highest
   order number in the book the engine recovered, and the report counter starts again at zero
   ([BUG-0139](../bug_list.md#bug_0139)). So two engines number the same report differently, and one engine numbers
   two different reports the same after a restart.
2. **Three decisions are taken by one engine alone**, from its own clock or circumstances, and change its book
   without any command in the WAL:
   - cancelling every order and halting when it measures, from the time stamp in its own region, that it was away
     too long (R-0117). It measures its own absence, not the venue's
     ([BUG-0146](../bug_list.md#bug_0146));
   - cancelling every inherited order on promotion, under `open_orders_on_promotion = "cancel"`. The cancels are
     execution reports, which a catch-up never sends an engine, so an engine that restarts later still holds the
     orders ([BUG-0199](../bug_list.md#bug_0199));
   - refusing every order while halted. The halt is a flag in one engine's memory, set by the first two.
3. **The order of the reports of a cancel-all depends on how the book is stored.** `cancel_all_orders_on_failover`
   walks the book with `OrderBook::for_each`, which visits the entries in the order of the hash map's slots. That
   order depends on the order in which the keys were inserted and on how far an incremental rehash has got. An engine
   that rebuilt its map from its region at a restart inserted the keys in a different order from one that accepted
   them live, so the two can send the same cancels in a different order.
4. **A report's time falls back to the engine's own clock.** Every report's `TransactTime` is the time the sequencer
   recorded for the command, carried in the WAL, except where that time is zero, when the engine uses its own clock
   (`config_.wall_clock->now_ns()`). Every command the sequencer writes carries its time, so the fallback should never
   be taken; if it is, the two engines' reports differ.
5. **Settings that change what the engine does can differ between the two engines.** The most direct is the size of
   the book's region: an engine whose region is full refuses an order that the other engine, with a larger region,
   accepts. Each engine reads its own configuration, and nothing checks that the pair agree.

Everything else the engine does with a command depends only on the command, the book, and the commands before it:
whether a new order's ClOrdID is already live for its session, whether its fields are too long to hold, whether a
command arrived out of sequence, and the cancel of an order a member names.

## 3. Identifiers from the position in the WAL

**Agreed** in question 3 of [ha_library_boundary.md](ha_library_boundary.md): identifiers are made from the position
in the WAL and scrambled, so that members are not shown the venue's sequence numbers.

### 3.1 What the identifier is made from

Every report is caused by one command, and one command can cause many reports: a new order causes one, and a cancel
of every order before a halt causes one for each order. So a report is named by two numbers:

- the **sequence number** of the command that caused it, which the WAL gives and which no other command of the day
  has; and
- the report's **place** among that command's reports: 0 for the first, 1 for the second, and so on.

An order is named by the sequence number of the new order that placed it, and place 0. So the `OrderID` of an order
and the `ExecID` of the report that accepted it are made from the same two numbers, and they are told apart by being
scrambled with different keys (section 3.3).

The two numbers are packed into 64 bits: 36 bits for the sequence number and 28 bits for the place. 36 bits holds
about 68,700 million sequence numbers, and 28 bits about 268 million reports for one command. The day's largest
figures are well inside both: about 100 million orders open at once, which bounds a cancel of every order, and a few
hundred million records in the WAL. An engine refuses to issue an identifier for a number outside these ranges and
halts, rather than issue one that could repeat.

### 3.2 How it is scrambled

The 64 bits are passed through a **block cipher with a 64-bit block**: a keyed function that maps every 64-bit value
to a different 64-bit value, and can be reversed only with the key. Because it is a one-to-one mapping, two different
inputs can never give the same identifier, so identifiers stay unique for the day. Because the key is the same on
both engines, both give the same identifier for the same report. Without the key, consecutive identifiers look
unrelated, so the difference between two of them says nothing about how many commands came between.

The cipher is FF1, the NIST standard for format-preserving encryption (decision 1 in section 9).

The result is written as 16 hexadecimal digits after a letter that says what it names: `O` for an `OrderID`, `E` for
an `ExecID`, for example `E3F9A0C21D4B7E605`. That is 17 characters, inside the 32 the gateways hold for an
`OrderID` (`max_supported_order_id_length`) (decision 3).

### 3.3 The day's key

Each kind of identifier has its own key, so that an `OrderID` and an `ExecID` made from the same two numbers differ.
Both are derived from one **day key** by a standard key derivation function, HMAC-SHA256 with the kind of identifier
as its label.

The day key must be the same on both engines, different every trading day, and known to support staff, who need it to
turn an identifier back into its sequence number. It is derived from one long-lived secret, held on both engine
machines and by support staff as the venue's TLS keys are, and the trading date, with HMAC-SHA256. The trading date is
the date of the first record in the day's WAL, which both engines read, so they agree even if one starts after
midnight (decision 2).

### 3.4 Turning an identifier back

A small tool, `identifier_to_position`, takes the day key and an identifier and prints the sequence number and the
place. Support staff use it to find the command behind a report a member quotes. It reads the key the same way the
engines do, so it needs the same access.

### 3.5 What changes for members

The identifiers change form, from `ME-ORD-1234` and `ME-EXEC-5678` to the 17-character form above. FIX treats both as
opaque strings, so a conforming member needs no change. Anything that reads the current form must change: the high
availability scenarios do not, since phase 6 moved them onto test events, but some unit tests and tools compare
identifiers, and step 3 of section 8 finds and changes them.

## 4. Decisions as commands in the WAL

**Agreed** in question 2 of [ha_library_boundary.md](ha_library_boundary.md): every decision that changes the
matching engine's book enters the WAL as a command, so that every engine applies it at the same position.

### 4.1 The commands

Three kinds of command join the new order and the cancel as commands the engine applies:

- **Cancel the book.** The engine cancels every order on its book, sending each member a cancel report, in the order
  in which the orders were placed (section 5). It carries a reason, which the reports' text gives.
- **Halt trading.** The engine refuses every new order from then on, until trading resumes. A cancel is still
  carried out, so that a member can always take its orders off the book. It carries a reason.
- **Resume trading.** The engine accepts orders again. Only a person lifts a halt (R-0023), through the operator's
  means of doing so, and the resume command is how that reaches the engines.

Each is a WAL record like an order, with a sequence number and the time the sequencer recorded, and a catch-up sends
it to an engine like an order. So an engine that restarts and catches up meets each decision at the position where
it was taken, and nowhere else.

### 4.2 Who issues them

**The leading sequencer**, because it is the leader of the WAL and the only process that puts records in it. No engine
cancels its book or halts on its own initiative any more. The decisions that are taken now are issued as follows.

- **The absence rule (R-0117).** The rule is about the venue, not one engine: when no engine could trade for longer
  than `absence_limit_seconds`, the open orders are cancelled, the members told, and trading halts. The leading
  sequencer knows when an engine leads, because it routes orders only to the engine that leads
  (`EngineOrderRouting`). It measures how long it has had no engine to route to, and when that exceeds the limit it
  writes a cancel of the book and a halt. When the whole venue was down, sequencers included, the leading sequencer
  measures the absence when it starts, from the time of the last record in its WAL to the time it leads; if that
  exceeds the limit, it writes the same two commands before any order. An engine never measures its own absence, so
  a follower that was down for an hour cancels nothing ([BUG-0146](../bug_list.md#bug_0146)). The rule is kept in
  one class of its own inside the sequencer, so that it can move into the library if a second rule of the kind
  appears (decision 4).
- **Cancelling on promotion.** While `open_orders_on_promotion = "cancel"` is set, a promoted engine asks the leading
  sequencer to write a cancel of the book before it serves, and applies it from the WAL like any other command.
  Every engine that catches up afterwards meets it too, so a restarted engine no longer keeps orders its peer
  cancelled ([BUG-0199](../bug_list.md#bug_0199)). Phase 8 removes cancelling on promotion altogether; it is made a
  command now all the same, so that the venue stops holding orders its members were told are cancelled from phase 7
  on (decision 5).
- **The halt that BUG-0197 needs.** [BUG-0197](../bug_list.md#bug_0197) is designed with phase 8, but the commands
  here are the ones it uses: after a normal failover's length with no engine leading, the leading sequencer writes a
  halt, and the gateways refuse orders as it says.

### 4.3 What the engine stops doing

- It no longer stamps its region with the time while it runs, or measures its own absence when it starts; the
  `cancel_and_halt_pending_` path goes.
- It no longer cancels its book on promotion by itself; `cancel_all_orders_on_failover` is called only by the cancel
  command.
- It no longer keeps a halt of its own. Whether it is halted is part of its state, set and cleared by commands, and
  recovered by catching up.

## 5. The other causes, put right

- **The order of a cancel of the book.** The engine cancels in the order of the orders' positions in the WAL, which
  every engine agrees on, not in the order of the hash map. The book does not keep that position now: an order's
  record in the region (`OrderEntry`) holds the counter its `OrderID` was made from, `order_id_num`. It is replaced by
  the sequence number of the new order that placed it, which section 3 needs as well, and the region's record format
  changes with it, so a region written before the change is not read after it. The cancel is then a sort of the live
  orders by that number. At 100 million orders the sort is a few seconds' work at the moment of a halt, which is
  acceptable, but it is measured in step 2 before it is relied on.
- **Time only from the WAL.** The fallback to the engine's own clock goes. A command without a time is a defect in the
  sequencer; the engine refuses it, reports it at Error, and treats it as section 6 treats any command it cannot
  apply.
- **Settings the pair must share.** The settings that change what the engine does with a command, starting with the
  region's capacity, must be the same on both engines. Each engine reports them to its peer when the two connect,
  and an engine whose settings differ from its peer's does not lead, and says why at Error. A deployment cannot then
  run a pair that would diverge.

## 6. A command the engine cannot apply

Once both engines apply every command, a command that reaches a defect in the engine reaches it in both, at the same
command ([BUG-0193](../bug_list.md#bug_0193)). Phase 7 does the part of BUG-0193's answer that needs only one engine.

- **Applying a command either completes or changes nothing.** The engine checks everything a command needs before it
  changes the book, and changes the book only once every check has passed. Where a change has several parts, such as
  removing an order from the region and from the map, it is ordered so that a failure between them can be put right
  from the region at the next start.
- **A failure while applying one command rejects that command.** A C++ exception or a failed check of the engine's own
  state, caught where one command is applied, rejects the command to its member with an execution report saying the
  venue could not process it, is reported at Error with the command's sequence number, and the engine goes on to the
  next command. Both engines, being deterministic, reject it alike.
- **Fuzzing.** A test program applies random, well-formed commands to the engine's state machine (section 7.1),
  millions at a time, and after each checks that the book is consistent: every order in the map is in the region and
  the other way round, every live ClOrdID is unique for its session, and the counts agree. It runs as an ordinary
  test with a fixed seed, so that a failure can be reproduced, and for longer by hand.

A crash that no catch can stop, such as a segmentation fault, still crashes both engines at the same command. Voiding
such a command, so that it costs that command and not the venue, needs both engines and is designed with phase 8.

## 7. Showing that it is deterministic

### 7.1 The engine's state machine, apart from its connections

The engine's handling of commands is a class of its own, the engine's **state machine**,
`MatchingEngineStateMachine`: given a command, with its sequence number, its time and the session that sent it, it
changes the book and produces the reports, and it does nothing else. It reads no clock, opens no connection and keeps
no timer. `MatchingEngineThread` gives it the commands, live or from a catch-up, and works out each command's time,
which is the time the sequencer recorded for it or, where that is zero, the engine's own clock (cause 4 of section 2,
which step 2 removes). The state machine hands what it produces to an interface, `MatchingEngineOutputsInterface`:
the reports, which the thread sends to the sequencers; the changes to the book, which it sends to the follower engine;
and what became of each order, which it sends to a test that asked. It also cancels every order on the book when
the thread asks it to.

The thread keeps everything else: which commands to discard, by role; the counting that shows a catch-up complete;
holding reports while the engine may not serve or has no connection; the open-order region's opening, recovery and
liveness mark, which the state machine is given a reference to; and applying the leader's `BookUpdate` messages on
the follower, which phase 8 removes. The state machine's unit tests are
`applications/matching_engine/tests/MatchingEngineStateMachineTest.cpp`.

That makes the property testable directly, and it is what the fuzzing of section 6 and the replay of section 7.2
drive.

### 7.2 Replaying a WAL

The sequencer's WAL holds every command, and also every report the engine that led sent it
(`SequencerThread::forward_report_from_engine` appends each one), including the reports an engine sends again while
it catches up. So a WAL from a run is its own record of what the engine did. A program, `wal_replay_check`, reads a
WAL, applies its commands to a fresh state machine, and compares the reports it produces for each command with the
first copy of each in the WAL, field by field, ignoring only the mark that says a report is a possible repeat. Any
difference is a failure, and it names the command.

The finish condition is checked in two ways:

- `wal_replay_check` passes on the WAL of every high availability scenario, including those in which the engine
  restarts, is promoted, or both engines change roles more than once. A scenario's WAL holds reports from more than
  one engine, so this checks that every engine that led produced what a fresh one does.
- Both engines are run on the same WAL from the start of the day, one after the other, each catching up from an empty
  book, and their reports are compared with each other and with the WAL.

## 8. Steps

Each step is built, tested and committed on its own, and every high availability scenario passes after each.

1. **The state machine.** Move the handling of commands out of `MatchingEngineThread` into the state machine of
   section 7.1, without changing what it does. Unit tests for the state machine on its own.
2. **The causes in section 5.** A cancel of the book in the order of the WAL, measured at 100 million orders; time
   only from the WAL; the pair's shared settings checked when the engines connect. With these, `wal_replay_check` is
   written and passes on scenarios that neither restart nor promote an engine.
3. **Identifiers** (section 3): FF1, the day key from the secret and the trading date, and `identifier_to_position`.
   [BUG-0139](../bug_list.md#bug_0139) is fixed by this step.
4. **Decisions as commands** (section 4): the three commands, the absence rule measured by the leading sequencer, and
   cancelling on promotion as a command. [BUG-0146](../bug_list.md#bug_0146) and
   [BUG-0199](../bug_list.md#bug_0199) are fixed by this step, and scenario 79 passes.
5. **A command the engine cannot apply** (section 6), and the fuzzing test.
6. **The finish condition** (section 7.2) on every scenario's WAL, and on both engines run one after the other.

## 9. Decisions needed

There are five, all decided on 25 October 2026. Decisions 1, 2 and 3 each chose one of the ways listed; decisions 4
and 5 were answered yes or no.

### Decision 1. Which cipher scrambles the identifiers? (Choose one.)

**Background.** Section 3.2: the 64 bits made from a sequence number and a report's place are passed through a cipher
with a 64-bit block, keyed for the day. It must be one-to-one, so that identifiers stay unique; reversible with the
key, for support staff; and hard to reverse without it. It runs once for every report the engine sends, on the
engine's thread, so its cost is part of every order's round trip, which is about 98 microseconds today. None of the
costs below has been measured on this machine; step 3 measures the chosen one before it is relied on.

- **A. FF1, the NIST standard for this purpose** (format-preserving encryption, SP 800-38G), built on AES with a
  128-bit key, using OpenSSL's AES, which the venue already links. It is the standard way to encrypt a value into a
  value of the same size, so it needs no defence beyond naming the standard. It runs ten rounds, each of one or two AES
  operations, which with the processor's AES instructions is expected to be well under a microsecond a report.
- **B. A Feistel network with SipHash as its round function.** The construction is the textbook way to build a block
  cipher from a keyed hash, and it is small to write and expected to be several times cheaper than A. But the textbook
  proof of its security, for four rounds, holds only up to about 2 to the power 16 values for a 64-bit block, which is
  far fewer than the identifiers of one day; more rounds give more, but how many are enough for so small a block is the
  question FF1's design and its analysis answer, and this would have to answer it again. It is not a named standard,
  so its use has to be explained to anyone who asks.
- **C. Speck64/128**, a published lightweight block cipher with exactly a 64-bit block. Cheap and simple, but it was
  designed by the NSA and withdrawn from ISO standardisation in 2018 after objections, which some reviewers will
  raise.

**Recommendation: A.** A financial venue is better placed defending a NIST standard than its own construction, the
small block is exactly the case FF1 is designed for, and the cost is expected to be small against a 98-microsecond
round trip. Step 3 measures it before it is relied on.

**Decided 25 October 2026: A, FF1.**

### Decision 2. Where does the day key come from? (Choose one.)

**Background.** Section 3.3: both engines need the same key, different each trading day, and support staff need it to
turn an identifier back into a sequence number. There is no start-of-day procedure yet
([BUG-0137](../bug_list.md#bug_0137)).

- **A. Derived from a secret and the trading date.** The deployment holds one long-lived secret, given to both engine
  machines and to support staff as the venue's other secrets are, such as its TLS keys. Each engine computes the day
  key from that secret and the trading date with HMAC-SHA256. The trading date is the date of the first record in the
  day's WAL, which both engines read, so they agree even if one starts after midnight. Nothing has to be created or
  distributed each day. Against: whoever holds the secret can derive every day's key, so the secret must be guarded
  as closely as a TLS private key, and changing it is a deployment change.
- **B. Created each day by a start-of-day procedure** and written to a file on both engine machines and in a place
  support staff can read. Each day's key stands alone. Against: it needs the start-of-day procedure of BUG-0137,
  which does not exist, and the venue cannot trade on a day whose key did not reach both machines.
- **C. Created by the leading sequencer at the start of the day and written into the WAL** as the day's first record.
  Both engines read it as they read any record. Against: the WAL is copied to the publishers and from them to systems
  outside the venue, so the key would leave the venue unless every copy filtered it out, and support staff would need
  to read it from the WAL.

**Recommendation: A.** It needs nothing that does not exist, and it is how keys for many systems are derived. The
secret is held like the TLS keys already are.

**Decided 25 October 2026: A, derived from a secret and the trading date.**

### Decision 3. What does an identifier look like? (Choose one.)

**Background.** Section 3.2. FIX treats `OrderID` and `ExecID` as strings, and the gateways hold up to 32 characters.

- **A. A letter for the kind, then 16 hexadecimal digits**, for example `O3F9A0C21D4B7E605` and `E81C04F5A92D3B670`.
  17 characters; easy to read aloud and to type into the support tool.
- **B. A letter for the kind, then 13 characters of base 32** (the digits and the letters without I, L, O and U), for
  example `O7YQ2KD9M4XATR`. 14 characters, but an alphabet support staff have to learn.
- **C. 20 decimal digits, with no letter**, for example `04573902178834512397`. Looks like the identifiers some
  members' systems expect, but says nothing about its kind, and is the longest to read.

**Recommendation: A.**

**Decided 25 October 2026: A, a letter for the kind and 16 hexadecimal digits.**

### Decision 4. Should the leading sequencer measure the absence rule? (Yes or no.)

**Background.** Section 4.2. R-0117 cancels the open orders and halts when no engine could trade for longer than
`absence_limit_seconds`. Some process must measure that and write the two commands. The leading sequencer is the
natural one: it is the leader of the WAL, it is running whenever anything can be traded, and it knows whether it has
an engine to route orders to. But it ties the sequencer to a rule of the venue's, where it now knows only how to
sequence and route. The alternative is a small part of the library's replicated WAL that lets an application register
a rule of its own, run on the WAL's leader, which keeps the venue's rule out of the sequencer's code but is more to
build.

**Recommendation: yes,** with the rule kept in one class of its own inside the sequencer, so that it can move into the
library later if a second rule of this kind appears.

**Decided 25 October 2026: yes.**

### Decision 5. Should cancelling on promotion become a command now, in phase 7? (Yes or no.)

**Background.** Section 4.2. Every environment sets `open_orders_on_promotion = "cancel"`, so a promoted engine
cancels its book. Today it does so by itself, and an engine restarted later still holds the cancelled orders, because
a catch-up sends no reports ([BUG-0199](../bug_list.md#bug_0199), scenario 79). Phase 8 removes cancelling on promotion
altogether, because a follower that applied every command has the leader's book. Making it a command now fixes
BUG-0199 in phase 7, at the cost of building something phase 8 removes; leaving it means BUG-0199 stays until phase 8
is finished.

**Recommendation: yes.** It is a small change once the cancel command of section 4.1 exists, and it stops the venue
holding orders its members were told are cancelled from phase 7 on.

**Decided 25 October 2026: yes.**
