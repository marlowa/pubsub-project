# Voided commands, and halting when no matching engine leads {#voided_commands}

This is the second design of phase 8 of the [recovery plan](../recovery_plan.md), agreed as decision 5 of
[matching_engine_lockstep.md](matching_engine_lockstep.md). It covers:

- [BUG-0193](../bug_list.md#bug_0193), part 2: a command that crashes both matching engines costs that command, and
  not the venue.
- [BUG-0197](../bug_list.md#bug_0197), parts 1, 3 and 4: the venue holds members' orders only for as long as a normal
  change of matching engine leader takes; past that it halts, every order it was holding is given a definite
  rejection, and trading resumes when an operator lifts the halt.
- Where [BUG-0065](../bug_list.md#bug_0065), the operator's means of declaring and lifting a halt, fits in the plan.

Both bugs need the same thing that nothing in the venue does yet: a command in the WAL that changes what an earlier
command in the WAL does. Section 3 designs it, under the name this project already uses for it in BUG-0193: **voiding**
a command. A voided command is not applied. Its member is sent a rejection instead.

Section 2 says how the venue behaves now. Sections 3 to 6 are the design. Section 7 says what goes and what changes.
Section 8 is the steps and how each is known to be right. Section 9 holds its decisions, which Andrew took on 31 October 2026.

## 1. Why a later command cannot simply be applied in its place

A matching engine applies the WAL in order. A command at position M that says "do not apply the command at position
N", with N before M, reaches every engine after N. So:

- **A command that crashes the engine** crashes it again on every restart, at N, before the engine reaches M. The void
  would never be read.
- **An order held while no engine leads** is applied when an engine reaches it, and may rest on the book or trade,
  before the engine reaches the command that rejects it.

So every engine has to be told about each void before it reaches the command the void names. That is the centre of
this design (section 3.3). The second problem is an engine that has already applied a command by the time the command
is voided. It cannot undo what it did. Section 3.4 says what it does instead, and sections 4 and 5 say why that is
made rare.

## 2. How the venue behaves now

**A command that fails.** A C++ exception thrown while the matching engine applies one new order or one cancel costs
that command. If the command had not yet changed the book or been answered, it is rejected to its member, and the
engine goes on to the next command. Both engines do the same, because both are deterministic
([deterministic_matching_engine.md](deterministic_matching_engine.md), section 6; `MatchingEngineStateMachine::after_failure`).
A failure that is not an exception, such as a segmentation fault or an abort, kills the process. Its supervisor
restarts it once and gives up at the second interruption (`launch.py --max-interruptions 2`, principle V14 of
[design_principles.md](../design_principles.md)). The restarted engine reads its open-order region, asks the leading
sequencer to catch it up from the region's published position, and so applies the same command again.

**Orders while no engine leads.** The leading sequencer writes each order to the WAL as usual. With no engine to send
it to, it counts the order as deferred and moves its released position past it (`note_order_deferred`,
`released_through` in `SequencerThread`). Both kinds of engine are then sent it:

- an engine that follows the WAL is sent every committed record (`send_committed_records_to_follower_engines`), so it
  applies a deferred order as soon as it is committed;
- an engine that asks to be caught up in order to lead is sent everything up to the released position, so it applies
  deferred orders in its catch-up, and their reports are marked as possible repeats (R-0122, `ha_test.py` scenario 53).

The sequencer stops accepting new orders, and tells the gateways so with `OrderAcceptance`, once it has deferred for
45 seconds or holds 250,000 deferred orders (`order_deferral_refusal_age` and `order_deferral_refusal_count` in
`SequencerThread.hpp`).

**The halt for a long recovery.** Once no engine could be trading within `[recovery] automatic_recovery_limit_seconds`,
10 seconds in every environment, the leading sequencer writes a `TechnicalEvent` that halts trading, with the reason
`RecoveryTooLong`, and the gateways refuse new orders and cancels, saying the venue is halted while it recovers
([far_back_catch_up.md](far_back_catch_up.md), section 5; scenarios 85 and 86). When an engine leads again, the
sequencer tells the gateways to pass orders on again (`check_engine_absence`), and the engine refuses every new order
because it applied the halt. The orders the sequencer was holding before the halt are applied in the engine's
catch-up like any other.

**Nothing lifts a halt.** The matching engine lifts a halt when it applies a `TechnicalEvent` that is not halted
(`TechnicalEventReason::LiftedByOperator` exists, and a unit test applies one), but nothing in the venue writes one. A
halt therefore lasts until the end of the trading day. There is no means for an operator to declare a halt either.

**The cancel after a long absence.** Separately, once no engine could trade for longer than `[absence]
absence_limit_seconds`, 300 seconds in every environment, the leading sequencer writes a `CancelBook` command that
cancels every open order and halts trading if it cancelled any (R-0117, `write_absence_command`).

## 3. Voiding a command

### 3.1 The command

A new command in the WAL, `VoidCommands`, written only by the leading sequencer, like `CancelBook` and
`TechnicalEvent`. It carries:

| Field | What it says |
|---|---|
| `first_seq_no`, `last_seq_no` | the positions of the commands it voids, both included; the same number for one command |
| `new_orders_only` | true: only the new orders in the range are voided, and every other command in it is applied as usual |
| `reason` | why: the engines died applying the command (section 4), or the order was held when trading halted (section 5) |

A void always names commands before its own position, because it is written after them.

### 3.2 What an engine does with a voided command

Where it would apply a voided command, it applies none of it, and answers it instead:

- **a new order** is rejected, with a reason text that says why (the engine died applying it, or the venue halted while
  it was waiting). The report is made as any rejection is: its identifiers come from the position of the voided order
  in the WAL, so every engine makes the same report;
- **a cancel** is rejected with an order cancel reject, giving the status of the order it names as the book holds it,
  so the member learns that the order is still open, or that it is not;
- **a command the sequencer wrote** is not voided. Section 4.3 says why.

The rejection answers the voided command: its WAL record's `answers_seq_no` is the voided command's position. So the
rules that tell an engine which reports the WAL already holds work unchanged
([far_back_catch_up.md](far_back_catch_up.md), section 2; [matching_engine_lockstep.md](matching_engine_lockstep.md),
section 3.2).

When the engine reaches the `VoidCommands` record itself, it applies nothing and sends nothing: it has already acted on
it at each command it names. It checks that it did, which is section 3.4.

### 3.3 Every engine is told about each void before it reaches the command it names

The leading sequencer keeps the list of the day's voids: the range, `new_orders_only` and the reason of each. It is
kept from the WAL, not from anything the sequencer was told, in three ways:

- **at start-up**, from the read of the whole WAL the replicated WAL already makes to build its table of epochs
  (`ReplicatedWal::open`), so that the list is complete before the sequencer serves any engine;
- **while it leads**, by noting each void as it writes it;
- **while it follows**, by noting each void as it writes its leader's record of it, so that the list is complete
  whenever it takes the lead.

It sends the whole list to an engine in a new message, `VoidedCommands`:

- **at the start of every catch-up**, from its own thread before it hands the request to the reading thread, so that
  the list reaches the engine before `WalFollowStarted` and before the first record. This covers every engine that
  starts, restarts, rebuilds its book, asks again after a break, or is promoted;
- **to every engine connected to it, once the record of a new void is committed**, before anything committed with it is
  released to an engine, so that an engine already following the WAL, or in the middle of a catch-up, learns of it
  before it is sent any command the void names.

The list it sends holds only the voids whose records are committed. A record that is not committed may yet be lost with
the leader that wrote it, and an engine told of a void that the WAL then does not hold would reject commands that every
other engine applies.

A day has few voids, so the list is small, and the engine holds it in memory. It is sent the list again on every
catch-up, so it does not record the list itself, only the position of the latest void it has been told of, in its
open-order region beside the published position. An engine that restarts from its region needs that position to tell
a void an earlier process with the region was told of, and acted on before reaching the commands it names, from one
that is new to it (section 3.4). A sequencer that does not lead sends nothing, as for every other answer to an engine.

Programs that apply the WAL themselves read every `VoidCommands` record first, and then apply the WAL: `wal_replay_check`,
and the run of both engines on a copy of each scenario's WAL in `ha_test.py` (`run_engines_on_the_wal`), which is sent
the list by the sequencer the test starts. The matching engine publisher's topics carry members' orders and cancels and
the execution reports, and none of the commands the sequencer writes itself, so a subscriber sees a voided order on the
orders topic and its rejection on the execution reports topic, as for any order the venue rejects.

### 3.4 An engine that applied a command before it was voided

An engine can have applied a command before the void naming it was written. It learns this when a `VoidedCommands`
list arrives with a void new to it whose first command is at or below the highest it has applied, or when it reaches a
`VoidCommands` record it was never told of. A void is new to the engine unless it is already in the engine's list, or
its record is at or below the position of the latest void recorded in the engine's region: an earlier process with the
region was told of that void, and either acted on it before reaching its commands or, told too late, marked the region
unusable, so that no engine starts from it.

It cannot undo the command. Its book now differs from the book every engine that knew of the void will build, and its
reports differ from theirs. So it does what a follower does when its reports differ from the leader's (decision 3 of
[matching_engine_lockstep.md](matching_engine_lockstep.md)): it says so at Error and by the test event
`FollowerCannotLead`, stops asking for a lease, and marks its open-order region unusable, so that, restarted, it rebuilds
its book from the WAL, this time told of the void first.

Sections 4.3 and 5.1 are written so that the engine that leads is never in this position: a command the leading engine
has gone past is answered by reports in the WAL, which stops the sequencer voiding it, and no engine is sent a held
order before the leading engine is. Steps 2 and 3 test those conditions.

## 4. A command that crashes both engines (BUG-0193, part 2)

### 4.1 The engine records the command it is applying

The open-order region's header gains one number: the position of the command being applied. The engine writes it
before it applies each command, live or in a catch-up, and writes zero once it has finished the command and sent its
reports. Each is one store to memory the engine has already mapped, in the same header as the published position; the
cost is measured in step 2.

The region survives the process dying, because it is a file mapped into memory and the operating system keeps what was
written to it. So an engine that starts and finds a number other than zero in its region knows it died applying that
command. An engine that starts with a region that cannot be used, and rebuilds its book from the WAL, has no such
number, and asks for no void.

### 4.2 It asks for the command to be voided, if no other engine has applied it

An engine that finds it died applying command N:

1. **Asks its peer**, over the link between the two engines (`[peer]`), for the highest command the peer has applied,
   and waits for the answer for at most a lease period. If the peer has applied N or later, N did not cause the crash,
   because the two engines are deterministic and the peer survived it: the engine asks for nothing, catches up as
   usual and applies N.
2. **Otherwise asks the leading sequencer** to void N, with a new message, `VoidRequest`, sent to each sequencer it has
   an order connection to, as `HaltRequest` is. It asks again every second until a `VoidedCommands` list naming N
   arrives, or a sequencer declines.
3. **Then asks to be caught up** as usual. The `VoidedCommands` list that starts the catch-up names N, so the engine
   rejects N where it would have applied it.

Both engines usually crash on the same command, so both usually ask. The first request writes the void; the second
finds N already voided and is answered with the list.

### 4.3 The sequencer writes the void only where it is safe

The leading sequencer writes `VoidCommands` for N only if all of these hold:

- **N is a member's new order or cancel.** A command the sequencer wrote itself, such as a `CancelBook` or a
  `TechnicalEvent`, is not voided. Voiding one would leave open orders the venue meant to cancel, or trading open that
  the venue meant to halt, and no member could be told why. A crash on one of those is left to a person: the engines
  crash again, the supervisor stops restarting them, and the venue halts by the limit on automatic recovery.
- **The WAL holds no report answering N or any later command.** Such a report means an engine applied N and went on,
  and perhaps that a member has been told what N did. The sequencer keeps the highest command any report in its WAL
  answers, as each report record is written, so this is one comparison.
- **N is not voided already.**

Otherwise it declines, with a new message `VoidDeclined` that says which condition failed, and the engine applies N.
If N crashes it again, that is the engine's second interruption, its supervisor stops restarting it (V14), and the
venue halts by the limit on automatic recovery for a person to deal with. A command that crashes both engines then
costs the venue the rest of the day, as it does now, but only where voiding it could be wrong.

A crash during a command for some reason other than the command, such as the operating system killing the process, is
treated the same way if the other engine is not running either: the command is voided, and its member is sent a
rejection. A rejection is a definite answer, so the cost is one order that could have been accepted.

### 4.4 What it costs the venue

At production size, an engine takes 8 to 17 seconds to read its open-order region on starting
([BUG-0028](../bug_list.md#bug_0028)), longer than the 10-second limit on automatic recovery. So a command that crashes
both engines usually halts trading while they restart, by section 5, and an operator lifts the halt once an engine
leads (section 6). The void is what lets the engines come back at all: without it they crash again, and the supervisor
gives up on both.

A command that makes the engine's thread stop without crashing is not covered here. That is
[BUG-0194](../bug_list.md#bug_0194).

## 5. Orders while no engine leads (BUG-0197, parts 1 and 3)

### 5.1 No engine is sent an order before the leading engine is sent it

While no engine leads, the leading sequencer keeps every order it writes **held**, as it already holds each order
until its follower sequencer has acknowledged it (`hold_until_acknowledged`), and does not move its released position
past it. Orders beyond the storage for held orders wait in the WAL, as they already do (`first_unheld_seq_`).

Every engine is then sent records only up to the released position:

- an engine that follows the WAL is sent each record once it is released, instead of once it is committed;
- an engine that is caught up, whether to lead or to follow, is served up to the released position, as an engine
  caught up to lead is now.

When an engine takes the lead, it is caught up to the released position, and the held orders are then released to it
live, in order, as orders held for the follower sequencer are released now.

So **no engine applies an order the leading engine has not been sent.** That gives section 5.3 a set of orders that no
engine has applied, which can therefore be voided without any engine meeting section 3.4. It also changes what a
member is sent for an order held across an ordinary change of engine leader: its report is the first report of it, and
is not marked as a possible repeat, because no engine applied the order before. Every report a catch-up sends is still
marked as a possible repeat, as R-0122 requires.

In ordinary running nothing changes: an order is released when its follower sequencer acknowledges it, which is when
it is committed, and the follower engine is sent it then, as now.

### 5.2 Orders are held for no longer than a normal change of leader takes

When the leading engine dies, the other can take the lead once the dead engine's lease has run out. The dead engine
may have renewed its lease just before it died, so the gap is at most one renewal interval, plus the lease period, plus
the drift allowance: 4.25 seconds with the lease timings every environment uses (1,000, 3,000 and 250 milliseconds),
plus the time the new leader's catch-up to lead takes, which is a few records. This is worked out from the timings, not
measured; step 3 measures it.

Holding orders across that gap is right, because it happens on every failover, its length is known, and it can be
stated to members. So the sequencer holds orders for that long and no longer: once no engine has led for
`[recovery] orders_held_at_most_seconds`, the sequencer stops accepting new orders, and tells the gateways so with
`OrderAcceptance` and the reason `NoMatchingEngine`, which they already act on, so they refuse each new order at once.
That replaces the 45-second limit. The limit of 250,000 deferred orders is removed: at the highest rate measured on this
venue, about 34,500 orders a second, five seconds of orders is about 172,500, so the count could never speak first.
The value is decision 4.

Refusing is not halting ([BUG-0065](../bug_list.md#bug_0065)): it is automatic, and it ends by itself when an engine
leads. The orders already held stay held, and are released to the engine that leads, which applies them.

### 5.3 The halt voids the orders held

If no engine can be trading within the limit on automatic recovery, the leading sequencer halts trading, as it does now.
It writes, in this order:

1. **`VoidCommands`** naming every held order, from the first held to the last, with `new_orders_only` set and the
   reason that the venue halted while the order was waiting. Every held command was written to the WAL and has been sent
   to no engine (section 5.1), so the void is safe. The cancels among them are not voided: a cancel applied late still
   does what its member asked.
2. **A `CancelBook`** that cancels every open order, if decision 6 is B.
3. **The `TechnicalEvent`** that halts trading, with the reason `RecoveryTooLong`, as now.

These three commands are held like the orders before them, and released with them, in order.

When an engine next leads, it is sent the held commands in order. It rejects each held new order, because the
`VoidedCommands` list that started its catch-up names them; it carries out each held cancel; and it applies the cancel
of the book and the halt. Each held order's member is sent a rejection, marked as the first report of that order.

### 5.4 What a member knows, and when

At the moment of the halt, every gateway refuses new orders, saying the venue is halted while it recovers, as it does
now. The rejections of the held orders, and the cancels of the resting orders if decision 6 is B, are sent when an
engine next leads, because only an engine makes reports. That is decision 5.

What makes the outcome definite at the moment of the halt, before those reports arrive, is the rule itself: an order
the venue had not answered when it halted for recovery is rejected. That holds in every case, and a member can be told
it in advance. The reports confirm it. Section 6 makes sure that every one of them has been sent before trading
resumes.

## 6. Lifting the halt (BUG-0197, part 4, and the part of BUG-0065 it needs)

### 6.1 The sequencer keeps whether trading is halted

The leading sequencer keeps whether trading is halted from the `TechnicalEvent` records in its WAL, in the same three
ways it keeps the voids (section 3.3). While trading is halted it tells the gateways so with `OrderAcceptance`, whether
or not an engine leads, with a new reason, `Halted`, beside `RecoveryTooLong`, which stays for the time no engine leads.
With `Halted`, a gateway refuses new orders and passes cancels on, because the engine carries out cancels while halted.
The sequencer no longer tells the gateways to pass orders on when an engine leads again during a halt
(`recovery_limit_passed_` in `check_engine_absence` goes).

### 6.2 An operator declares and lifts a halt

An operator sends the leading sequencer a request to halt or to lift a halt, with a new message, `OperatorHaltRequest`,
carrying whether trading is to be halted, and the operator's name for the Quill log. How the request reaches the
sequencer is decision 7.

- **To halt**, the sequencer writes a `TechnicalEvent` that halts trading, with a new reason, `DeclaredByOperator`. It
  cancels nothing: an operator's halt keeps the book, and the operator's reasons for halting are not the venue's to
  guess.
- **To lift**, the sequencer writes a `TechnicalEvent` that is not halted, with the reason `LiftedByOperator`, only if
  an engine leads and the WAL holds a report of every command up to the halt: that is, the leading engine has answered
  every order the venue was holding, and every member has been sent its rejections and its cancels. Otherwise it
  declines, and says why, so that the operator can try again. This is what BUG-0197 means by the operator confirming that
  the engine leads with a sound book: the venue checks it, rather than leaving the operator to.

The sequencer answers every request, saying what it wrote or why it wrote nothing, and logs it at Warning with the
operator's name. A follower sequencer declines, as for `HaltRequest`.

### 6.3 What is left of BUG-0065

This design builds what BUG-0197 needs: an operator can declare and lift a halt, and the gateways refuse orders while
trading is halted. It does not build:

- telling a member that is connected and not trading, or that logs on during a halt, that the venue is halted and when
  it resumes: FIX `TradingSessionStatus` (35=h) and its equivalent on the binary gateway, which R-0023 requires;
- halting one instrument rather than the venue;
- the table of what each trading phase allows ([trading_phases.md](../venue/trading_phases.md), step 4).

Decision 8 proposes that these stay in BUG-0065, to be designed when the plan returns to the bug list (phase 12), and
that R-0023 is marked partly met until then.

## 7. What goes, and what changes

**Goes:**

- `order_deferral_refusal_age` (45 seconds) and `order_deferral_refusal_count` (250,000) in `SequencerThread.hpp`,
  replaced by `[recovery] orders_held_at_most_seconds` (section 5.2);
- counting an order as deferred and releasing it while no engine leads (`note_order_deferred` moving the released
  position): the order stays held (section 5.1);
- if decision 6 is B, the cancel after a long absence: `[absence] absence_limit_seconds`, `write_absence_command` and
  `CancelBookReason::UnableToMatchTooLong`, because the halt cancels the book first, ten seconds in rather than three
  hundred. If decision 6 is A, the same goes, because the venue is halted already and a person decides; the orders stay
  on the book through the halt. R-0117 is rewritten to match either way;
- the sequencer telling the gateways to pass orders on when an engine leads again during a halt (section 6.1).

**Changes:**

- an engine that follows the WAL is sent records once they are released, not once they are committed (section 5.1);
- `ha_test.py` scenario 53, whose deferred orders are reported as possible repeats from a catch-up, requires them to be
  reported live, as first reports, by the engine that takes the lead;
- scenarios 85 and 86, of the halt for a long recovery, require the orders held before the halt to be rejected, and,
  with decision 6 B, the open orders to be cancelled;
- the open-order region's header holds four values of the engine's own instead of one: whether trading is halted, the
  position of the latest void the engine has been told of (section 3.3), and, from step 2, the command being applied
  (section 4.1). The region's format number, `store_version` in `MappedSlotStore.cpp`, is raised, so a region written by
  an earlier build is refused and the book rebuilt from the WAL, as for any change of format;
- `trading_phases.md`, which says nothing writes a `TechnicalEvent` and the gateways do not act on one, is brought up to
  date.

## 8. Steps, and how each is known to be right

Each step ends with both full suites from fresh WALs, with the primary engine leading first and with the secondary
leading first.

1. **The void (section 3).** `VoidCommands`, `VoidedCommands`, the sequencer's list kept from the WAL, the engine's
   rejection of a voided command, section 3.4, and the programs that apply the WAL reading the voids first. Unit tests of
   the state machine: a voided new order is rejected and a voided cancel answered with the order's status, each made to
   fail by applying the command instead; an engine that applied a command before learning of its void refuses to lead
   and marks its region unusable, made to fail by leaving the check out. A unit test that the sequencer's list rebuilt
   from a WAL holding voids equals the list it kept while writing them. Nothing writes a void yet except the tests.

   **Built.** `VoidCommands`, `VoidedCommand` and `VoidedCommands` are in `leader_follower.dsl` (156 to 158), and the list
   is `fix_common::VoidedCommandList`. The sequencer keeps it as section 3.3 says: the replicated WAL shows the
   application every record as it reads the whole WAL on opening (`ReplicatedWalEventsInterface::read_at_open`), and
   the sequencer finds the voids in those records, in the records it writes as leader and in those it writes as
   follower, with one function (`void_from_wal`, `void_from_command`). It sends the voids whose records are committed
   to an engine before handing a catch-up to the reading thread, and to both engines when a new void's record is
   committed, before anything committed with it is released. The engine rejects a voided new order or cancel in
   `MatchingEngineStateMachine`, keeps the position of the latest void it has been told of in its region
   (`MappedSlotStore` now keeps four values of the owner's), and refuses to lead, with `FollowerCannotLead` and the
   reason `AppliedVoidedCommand`, if it learns of a void too late. `wal_replay_check` reads the voids first. Unit tests:
   five of the list, four of the sequencer's decoding, seven of the state machine, three of the region's values and one
   of the replicated WAL's reading at opening; each was made to fail by taking out what it tests. Nothing writes a void
   yet. Both full suites passed on 31 October 2026: 89 of 89 with the primary engine leading first and 31 of 31 with the
   secondary first.
2. **Voiding a command that crashed both engines (section 4).** A way to crash an engine on a chosen command, which
   exists only for the tests: the engine aborts when it applies a new order whose ClOrdID a test names in the engine's
   `[test_events]` section, which only a test writes. A new scenario names an order to both engines, places orders before
   and after it, and requires: both engines restart once, the named order is rejected to its member with the reason that
   the engines died applying it, every other order is answered as usual, and both engines then give the reports a replay
   gives. With the request for a void left out, the engines crash again and the supervisor gives up, so the scenario
   fails. A second scenario names the order to the leading engine only, and requires the follower to take the lead
   having applied it, and no void to be written. The cost of the two stores per command is measured with a trading-day
   run, as in step 5 of [matching_engine_lockstep.md](matching_engine_lockstep.md).
3. **Holding orders while no engine leads (sections 5.1 and 5.2).** Orders stay held, every engine is sent records only
   up to the released position, and new orders are refused once no engine has led for `orders_held_at_most_seconds`.
   The scenarios that change the engine leader measure how long the venue held orders in each, so that the value of
   decision 4 is checked against measured changes of leader before it is fixed. A new scenario kills both engines,
   places orders, and requires those placed within the limit to be held and answered once an engine leads, each as a
   first report, and those placed after it to be refused by the gateway. Made to fail by restoring the release of
   orders while no engine leads: the follower engine then applies the held orders before any engine leads.
4. **Voiding the held orders at the halt (section 5.3).** Scenarios 85 and 86 require the held new orders to be rejected,
   the held cancels to be carried out, and, with decision 6 B, every open order to be cancelled with a report; made to
   fail by leaving out the `VoidCommands`, when the held orders rest on the book instead. The absence rule goes.
5. **The operator's halt and lift (section 6).** The sequencer's halted state, the reason `Halted`, the operator's
   request and how it reaches the sequencer. A scenario halts the venue by the limit on automatic recovery, has an
   operator try to lift it before any engine leads and requires the request to be declined, starts an engine, lifts the
   halt, and requires orders to be accepted again; a second declares a halt by the operator and requires the book to be
   kept and cancels to be carried out during it. Made to fail by writing the lift without the check of section 6.2.

## 9. Decisions

Decisions 1 to 4 and 8 are separate yes-or-no questions. Decisions 5, 6 and 7 each ask you to choose one of the options
given.

### Decision 1. Is a command voided by a later command in the WAL, with every engine told of each void before it reaches the command it names? (Yes or no.)

Section 3. Engines learn of voids from a list the leading sequencer sends at the start of every catch-up and whenever it
writes a void. An engine that has applied a command before learning that it is voided does not lead, and rebuilds its
book from the WAL when restarted.

The alternative to a list is for the sequencer to mark each voided record as it serves it. Every path that sends a
command to an engine would then have to know the voids, including the reading thread, which runs on a thread of its
own; with a list, only the sequencer's own thread and the engine do.

**Recommendation: yes.**

**Decided** on 31 October 2026: yes.

### Decision 2. Does an engine that died applying a command ask for it to be voided, under the conditions of section 4? (Yes or no.)

Section 4. The engine records the command it is applying in its region. On restarting, it asks for the command to be
voided only if its peer has not applied it; the sequencer voids it only if it is a member's order or cancel and no
report in the WAL answers it or any later command. A crash on a command the sequencer wrote is left to a person.

**Recommendation: yes.**

**Decided** on 31 October 2026: yes.

### Decision 3. While no engine leads, are orders kept held, so that no engine is sent an order before the leading engine is sent it? (Yes or no.)

Section 5.1. It is what makes the held orders safe to void at a halt. Without it, an engine that follows the WAL applies
a held order as soon as it is committed, and a void at the halt would make it unable to lead, under section 3.4,
exactly when the venue has no engine leading. It also means a member whose order was held across a change of engine
leader is sent a first report, not a possible repeat.

**Recommendation: yes.**

**Decided** on 31 October 2026: yes.

### Decision 4. Does the sequencer stop accepting new orders once no engine has led for 5 seconds, in place of 45 seconds and 250,000 orders? (Yes or no.)

Section 5.2. Five seconds is the longest normal change of engine leader worked out from the lease timings, 4.25
seconds, with a margin for the new leader's catch-up. Step 3 measures the real changes of leader before the value is
fixed. The setting is `[recovery] orders_held_at_most_seconds`.

**Recommendation: yes.**

**Decided** on 31 October 2026: yes.

### Decision 5. Who sends the rejections of the orders held when trading halts? (Choose one.)

Section 5.4.

- **A. The engine that next leads**, when it applies them. The member learns that the venue has halted at once, from
  the gateway's refusal, and the rejection of its held order when an engine next leads, which is always before trading
  resumes (section 6.2). The rule, that an order unanswered when the venue halts for recovery is rejected, tells the
  member the outcome in the meantime.
- **B. The leading sequencer, at the moment of the halt.** It writes a rejection of each held new order into the WAL as
  a report, and forwards it, so the member has it at once. The sequencer would need the engines' identifier secret to
  make the report's identifiers, and to make reports, which is the engine's work; every engine would then have to
  produce nothing for those orders, and the follower's comparison of reports would have to allow for reports no engine
  made. It cannot do the same for the cancels of decision 6, because it does not hold the book.

**Recommendation: A.**

**Decided** on 31 October 2026: A.

### Decision 6. What does the halt for a long recovery do to the orders resting on the book? (Choose one.)

Section 5.3. BUG-0197 leaves this open as a business decision.

- **A. They are kept**, and are live again when the halt is lifted. While the halt lasts, a member can cancel one only
  once an engine leads.
- **B. They are cancelled**, by a `CancelBook` written with the halt, and each member is sent a cancel for each, when an
  engine next leads.

For B: [BUG-0065](../bug_list.md#bug_0065) records that on 28 August 2026 you agreed a halt cancels every order except
long orders, and long orders are not implemented, so that is every order. It is also what the venue does now after 300
seconds without an engine (R-0117), for the reason that members were locked out of their orders while the market moved;
during a halt for recovery they are locked out in the same way, because the gateways refuse cancels while no engine
leads. An operator's halt (section 6.2) keeps the book whichever is chosen.

**Recommendation: B.**

**Decided** on 31 October 2026: B.

### Decision 7. How does an operator's request to halt or lift reach the sequencer? (Choose one.)

Section 6.2.

- **A. A command-line tool**, run by the operator, which sends `OperatorHaltRequest` to both sequencers over a new
  listener on each that accepts only TLS with a client certificate, as the authentication service's admin listener can
  (`tls_require_client_certificate`). It depends on nothing but the sequencers.
- **B. A page in the admin service**, open only to its `ADMIN` role, which sends the request over the same kind of
  listener. The admin service already has logins, roles and a TLS client. The venue could then not lift a halt while the
  admin service was not running.

**Recommendation: A**, because a halt the venue cannot lift without an optional service would make the venue depend on
it, which principle V13 rules out. A page in the admin service can be added later, sending the same request.

**Decided** on 31 October 2026: A.

### Decision 8. Do the parts of BUG-0065 that section 6.3 lists stay in BUG-0065, to be designed in phase 12, with R-0023 marked partly met until then? (Yes or no.)

Section 6.3. They are telling members who are not trading, halting one instrument, and the table of what each phase
allows. None is needed for BUG-0193 or BUG-0197.

**Recommendation: yes.**

**Decided** on 31 October 2026: yes.
