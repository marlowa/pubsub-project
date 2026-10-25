# A deterministic matching engine {#deterministic_matching_engine}

> **Status: decided, 25 October 2026; steps 1, 2, 3 and 5 of section 8 built.** This is the design for phase 7 of the
> [recovery plan](../recovery_plan.md). The state machine of section 7.1, the remedies of section 5, `wal_replay_check`,
> the identifiers of section 3 and the handling of a command the engine cannot apply, with its fuzzing, of section 6,
> are built; steps 4 and 6 are not. Step 4 waits on decision 6 of section 9, which is not
> yet taken; decisions 7 and 8 were taken on 25 October 2026. Andrew answered decisions 1 to 5 of section 9 on
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

These are the places, found by reading `MatchingEngineThread.cpp` and `OrderBook.hpp`, where the same commands could
lead to a different book or different reports, and which this design removes. Steps 2 and 3 of section 8 have removed
causes 1 and 3 to 6; cause 2 is removed by step 4.

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

6. **An order applied during a catch-up took a different path from one applied live.** The catch-up path did not keep
   the order's TimeInForce or ExpireTime on the book, its acknowledgement left them out and did not echo the order's
   NoUnderlyings and NoPartyIDs groups, it ignored a new order whose ClOrdID the session already had open where the
   live path refused it with a report, it sent no report for a cancel of an order the book did not hold, and it took
   its ExecIDs at a different point. So an engine that caught up and one that served live held different books and
   sent different reports for the same commands ([BUG-0204](../bug_list.md#bug_0204)).

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

The cipher is FF1, the NIST standard for format-preserving encryption (decision 1 in section 9), over AES-256 with
OpenSSL's AES. Making one identifier takes 354 nanoseconds at the median and 375 at the 99.9th percentile, measured on
25 October 2026 with `report_identifiers_bench` over 10 million ExecIDs, pinned to one core with the machine set up for
measuring, in three runs that agreed to within a few nanoseconds; the times include two readings of the clock. An
accepted order needs two, its OrderID and its ExecID, so about 0.7 microseconds of a round trip of about 98. The
slowest one in a million took about 29 microseconds in every run, which is the size of an interruption by the
operating system rather than of the cipher.

The result is written as 16 hexadecimal digits after a letter that says what it names: `O` for an `OrderID`, `E` for
an `ExecID`, for example `E3F9A0C21D4B7E605`. That is 17 characters, inside the 32 the gateways hold for an
`OrderID` (`max_supported_order_id_length`) (decision 3).

### 3.3 The day's key

Each kind of identifier has its own key, so that an `OrderID` and an `ExecID` made from the same two numbers differ.
Both are derived from one **day key** by a standard key derivation function, HMAC-SHA256 with the kind of identifier
as its label.

The day key must be the same on both engines, different every trading day, and known to support staff, who need it to
turn an identifier back into its sequence number. It is derived from one long-lived secret, held on both engine
machines and by support staff as the venue's TLS keys are, and the trading date, with HMAC-SHA256: the day key is
HMAC-SHA256 of the date written yyyy-mm-dd, keyed with the secret, and the key for each kind of identifier is
HMAC-SHA256 of its name, `OrderID` or `ExecID`, keyed with the day key (decision 2).

The trading date is the UTC date of the first record in the day's WAL, so the engines agree on it even if the day runs
past midnight or one of them starts after it. An engine does not read the WAL itself, and one that restarts catches up
from its own position, so it never sees the first record. The leading sequencer therefore writes the date on every new
order and cancel it puts in the WAL, as `trading_date` on the `WalRecord` envelope, and the engines and
`wal_replay_check` take it from the command. The sequencer finds the date in one of three ways:

- a follower sequencer notes the date on each command its leader sends it, so on taking the lead it writes the date
  its leader wrote;
- a sequencer that leads before it has been sent a command reads the wall time of the first record in its WAL once,
  with `Wal::first_wall_time_ns`, which reads the start of the earliest segment and nothing else;
- a sequencer whose WAL holds nothing takes the date of the command it is writing, which then becomes the first record.

A command that carries no trading date is a defect in the sequencer, and the engine refuses it, as it refuses a
command that carries no time (section 5).

The engines never work the date out from their own clocks, which [trading_phases.md](../venue/trading_phases.md)
forbids: they take the date the sequencer writes. The sequencer takes it from the first record of the day's WAL because
nothing yet declares the trading day. Once the technical event of trading_phases.md exists, carrying the trading day
from the start of day ([BUG-0137](../bug_list.md#bug_0137)), the sequencer writes the day it declares, and the engines
are unchanged.

The secret is a file of 64 hexadecimal digits, named by `identifiers.secret_file` in each engine's configuration.
`deploy.py` writes one, readable by its owner alone, where none exists, and never replaces one that does, because the
identifiers members were given could then no longer be turned back. Both engines of a pair must hold the same secret,
so for a pair on two machines the one file is copied to both. The engines check that they do: each sends its peer a
fingerprint of its secret with its other settings (section 5), and an engine whose peer's secret differs is barred as
one whose region differs is.

### 3.4 Turning an identifier back

A small tool, `identifier_to_position`, takes the secret file, the trading date and one or more identifiers, and prints
for each the sequence number and, for an ExecID, the place. Support staff use it to find the command behind a report a
member quotes. It reads the secret from the file the engines read, so it needs the same access:

```
identifier_to_position --secret-file etc/matching_engine/identifier_secret.key --date 2026-10-25 E3F9A0C21D4B7E605
```

The implementation is `applications/matching_engine/ReportIdentifiers.{hpp,cpp}` for the identifiers and the keys, and
`Ff1Cipher.{hpp,cpp}` for FF1, checked against the nine sample vectors NIST publishes for it.

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
  acceptable, measured on 25 October 2026 with `order_book_size_bench`. Sorting pairs of a sequence number and a record
  number, from a random order, took 0.58 s for 10 million, 3.1 s for 50 million and 6.6 s for 100 million, holding
  16 bytes for each open order, 1.5 GB at 100 million, while it lasts. Visiting every order of a book recovered from
  its region, in the order placed, took 0.18 s for 2 million orders and 1.0 s for 10 million. A region of 100 million
  records is 24.8 GB, more than this machine can hold in memory beside everything else, so the whole visit was not
  measured at that size; by the 10 million figure it is about 10 seconds. A cancel of every order still takes its
  time from the engine's clock: what triggers it is a decision one engine takes alone, which step 4 makes a command
  in the WAL, with a time of its own.
- **One path for a command, live or from a catch-up.** A new order and a cancel are applied by the same code whichever
  way they arrive. The source makes these differences and no others: a catch-up does not check the order of sequence
  numbers, whose records are below the position by construction, nor the halt, which is a decision one engine takes
  alone until step 4 makes it a command; its reports are marked as possible repeats; and an order it puts on the book
  is not replicated to the follower engine or counted as processed. A catch-up does not apply a command at or below
  the highest sequence number the engine has already acted on, which a catch-up asked for again can deliver; every
  command is applied once, in the order of its sequence number. The book is published after every command during
  a catch-up, and, live, only after a command that changed it, so that a restarted engine applies a live refusal again;
  where the published position stands is not part of what the engine reports.
- **Time only from the WAL.** The fallback to the engine's own clock goes. A command without a time is a defect in the
  sequencer; the engine refuses it, reports it at Error, and treats it as section 6 treats any command it cannot
  apply. The refusal's TransactTime is zero, which is what the command carried, and so the same on every engine and
  every replay; its text says the command carried no time. Andrew decided this on 25 October 2026.
- **Settings the pair must share.** The settings that change what the engine does with a command, the region's
  capacity and the identifier secret (as a fingerprint, section 3.3), must be the same on both engines. Each engine reports them to its peer when the two connect.
  A difference is the same seen from either side, so it is the engine that leads that keeps leading: on learning that
  its peer's settings differ from its own, it says, in the statements the lease rules already carry, that its peer
  may not lead, and reports the difference at Error. The venue goes on trading on the leader, having lost its
  resilience rather than its service. If neither engine leads when they connect, whichever wins the election leads and
  bars the other. A barred engine does not take over if the leader dies, which is then a loss of service, but one
  reported when the settings were first compared rather than discovered as two engines that disagree. Andrew decided
  this on 25 October 2026.

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

These are built. `MatchingEngineStateMachine::apply_new_order` and `apply_cancel` catch a C++ exception thrown while
one command is applied. The state machine counts what the command has done: where it has not yet changed the book or
sent a report, it is rejected with the text "Venue could not process the order", or "Venue could not process the
cancel. The order is unchanged", under the command's own ExecID; where it has, it stands as far as it went, the book is
published to it, and nothing more is sent, because a rejection would contradict what the book or the member already
has. Either way the failure is logged at Error with the sequence number. `OrderBook::add` releases the record again if
the map cannot take the order, so that a failure there changes nothing. `OrderBook::consistency_problem` checks that the
region and the map agree and that every order is found by its key.

The fuzzing test is `applications/matching_engine/tests/MatchingEngineStateMachineFuzzTest.cpp`. It applies 200,000
commands as an ordinary test, from three sessions over 48 ClOrdIDs, with a region of 32 records, and now and then a
ClOrdID too long to hold or a command with no time or no trading date. After each command it checks one report with an
ExecID never seen before, the book against a model of the rules, the book's consistency, and that a second state
machine given the same commands gave the same report. `MATCHING_ENGINE_FUZZ_COMMANDS` and `MATCHING_ENGINE_FUZZ_SEED`
run it longer by hand. Its first run found [BUG-0205](../bug_list.md#bug_0205): the order book's map lost an order
while it migrated.

A crash that no catch can stop, such as a segmentation fault, still crashes both engines at the same command. Voiding
such a command, so that it costs that command and not the venue, needs both engines and is designed with phase 8.

## 7. Showing that it is deterministic

### 7.1 The engine's state machine, apart from its connections

The engine's handling of commands is a class of its own, the engine's **state machine**,
`MatchingEngineStateMachine`: given a command, with its sequence number, its time and the session that sent it, it
changes the book and produces the reports, and it does nothing else. It reads no clock, opens no connection and keeps
no timer. `MatchingEngineThread` gives it the commands, live or from a catch-up, each with the time and the trading
date the sequencer recorded for it, and holds the maker of identifiers, `ReportIdentifiers`, which it gives the state
machine. The state machine hands what it produces to an interface, `MatchingEngineOutputsInterface`:
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
difference is a failure, and it names the command. Each report's WAL record carries the sequence number of the
command it answers (`answers_seq_no` on `WalRecord`), which the sequencer writes when it receives the report, so that
the pairing of reports with commands is read from the WAL rather than inferred.

`wal_replay_check` is built (`applications/matching_engine/tools/WalReplayCheckMain.cpp`). It is given the region
capacity and the identifier secret of the engines that led, and pairs a replayed report with the WAL's copy by ExecID,
which names a report by the position of its command whichever engine sent it. `ha_test.py` runs it after every
scenario that passes, including those in which an engine restarts or engines change roles, except where an engine
cancelled every order by its own decision, sending at least one cancel report, or halted by its own decision (test
events BookCancelled and Halted), and where the engines that led were configured differently. Those decisions are not
commands in the WAL until step 4, so the WAL holds the reports they produced with no command a replay could apply.
Step 4 makes the engine's decisions commands in the WAL, and step 6 runs the check on every scenario.

A scenario can stop the venue while the engine's answers to its last commands are in flight, so that the WAL stops
before them. The engine answers commands in the order of their sequence numbers, so `wal_replay_check` counts the
commands after the last one the WAL holds an answer to, and does not compare them; a command before it with no answer
is a failure.

Until step 4, a cancel of every order has no sequence number of its own, so its ExecIDs are made from the highest
sequence number the engine has acted on, from place 1, since that command's own report holds place 0, and with the
keys of the last trading date the engine was given.

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

Decisions 1 to 5 were all decided on 25 October 2026. Decisions 1, 2 and 3 each chose one of the ways listed;
decisions 4 and 5 were answered yes or no. Decisions 6, 7 and 8 are needed before step 4 is built, each a choice of
one of the ways listed. Decisions 7 and 8 were taken on 25 October 2026; decision 6 is not yet taken.

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

### Decision 6. What do the commands to halt and to resume trading look like? (Choose one.)

**Background.** Section 4.1 makes halting and resuming trading commands in the WAL, each with a reason.
[trading_phases.md](../venue/trading_phases.md) designs one message, the technical event, which announces every change
of the venue's phase and says whether trading is halted, carries the trading day, and is sequenced in the WAL with the
orders. It is not built, and [BUG-0197](../bug_list.md#bug_0197) and [BUG-0137](../bug_list.md#bug_0137) need it. A
halt in trading_phases.md is a flag the venue has while in a phase, not a phase of its own. The venue has no phases
yet: it is always, in effect, open.

- **A. The technical event of trading_phases.md, built now.** One WAL record carrying the phase, whether trading is
  halted, the trading day and a reason. Until the start and end of day exist, the phase is always open. The engine
  applies it; the gateways and the publishers do not yet act on it, which BUG-0197 adds in phase 8. There is one
  message for a halt, now and later.
- **B. Two commands for the engine alone now**, a halt with its reason and a resume, which the technical event
  replaces when trading_phases.md is built. Smaller now, and replaced later, so that a WAL written before the
  replacement holds the old commands.

**Recommendation: A.** It is the message the venue's design already says a halt is, and building it now means no
second kind of halt record exists to be replaced, read and explained later.

### Decision 7. When cancelling on promotion is a command, which orders does it cancel? (Choose one.)

**Background.** Today a promoted engine cancels the orders it holds once it has caught up and before it serves, so
only the orders it inherited are cancelled. As a command, the cancel is written by the leading sequencer when the
promoted engine asks for it (section 4.2). Orders that reach the sequencer between the engine catching up and the
command being written are put in the WAL before the command. They were sent to no engine, because none was serving.

- **A. The command names the position the engine caught up to, and cancels the orders placed at or before it.** Orders
  that came after it stay on the book and are acknowledged, as they are today. Every engine applying the command
  cancels the same orders, because the position is in the command.
- **B. The command cancels every order on the book at its own position**, including those that came after the engine
  caught up. Simpler, but a member whose order arrived in that interval is told it was accepted and, at once, that it
  was cancelled, which does not happen today.

**Recommendation: A.** It keeps what members see on a promotion as it is, and costs one number in the command.

**Decided 25 October 2026: A, the command names the position the engine caught up to.**

### Decision 8. What does an engine do when its own region cannot be used? (Choose one.)

**Background.** Section 4 does not cover this case. R-0102 and R-0123: when the region holding an engine's open orders
cannot be read at a start, the engine puts it aside and starts a new one. If the sequencer's WAL reaches back to the
day's first record, the engine rebuilds its book from it, then cancels every order, tells each member and halts trading
(`RecordCouldNotBeUsed`); if it does not, the engine halts without cancelling (`CannotAccountForBook`). Both are
decisions one engine takes about its own file, so neither is a command in the WAL: a replay cannot reproduce them, and
the other engine does not take them. Once the engine is deterministic, a book rebuilt from the WAL from its first
record is exactly the book the venue had.

- **A. Rebuild without cancelling where the WAL reaches the first record; otherwise ask the sequencer for a halt.** The
  rebuilt book is the venue's book, so the engine carries on, and reports at Error that its region was rebuilt. Where
  the WAL does not reach the first record, the engine asks the leading sequencer to write a halt, as section 4 has it
  ask for a cancel on promotion. This changes R-0123, which says the engine cancels what it rebuilt.
- **B. Keep both as decisions of one engine, outside the WAL.** R-0123 stays as it is. This is the one place where the
  engine does something no command says, and the replay check does not run on a scenario where it happens.
- **C. Make both commands, as they are today.** The engine asks the leading sequencer to write a cancel of the book and
  a halt, or a halt alone. R-0123 stays as it is, and the WAL records the decisions, at the cost of cancelling a book
  that could be kept.

**Recommendation: A.** It is what a deterministic engine makes possible: a damaged file on one machine costs nothing
when the WAL holds the day, and the venue halts only when it truly cannot say what it held. It needs R-0123 changed,
which is why it is a decision.

**Decided 25 October 2026: A, rebuild without cancelling where the WAL reaches the first record, and otherwise ask the
sequencer for a halt.** R-0123 is to be reworded to match, with the new wording agreed before it is built.
