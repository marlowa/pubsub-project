# Running the matching engine pair in lockstep {#matching_engine_lockstep}

This is the design for phase 8 of the [recovery plan](../recovery_plan.md): both matching engines apply every committed
command from the WAL themselves, whichever one leads, and only the leading engine's execution reports are sent on to
the members. It is way B of [ha_library_boundary.md](ha_library_boundary.md), section 10, agreed on 12 October 2026.

It fixes:

- [BUG-0187](../bug_list.md#bug_0187): the follower's copy of the book loses every change made while the link between
  the engines is down, and nothing notices. `ha_test.py` scenario 87 shows it.
- [BUG-0188](../bug_list.md#bug_0188): the book is copied only from the engine configured as primary.

And it removes the reason for `open_orders_on_promotion = "cancel"`: a promoted engine's book is already the one the
members were told of, so it keeps every resting order.

Sections 2 to 6 are the design. Section 7 is how it is built and how each step is known to be right, and section 8
holds the decisions it needs from Andrew. Section 9 says what the phase also has to design, and proposes where.

## 1. How the pair works now

Each matching engine connects to both sequencers. The leading sequencer sends every committed command, as it is
released, only to the engine it routes orders to, which is the engine that has announced that it leads
(`EngineOrderRouting`, `SequencerThread::send_held_order_to_matching_engine`). The engine that follows is sent
nothing live. Instead, the leading engine sends it a `BookUpdate` for each order it adds to or removes from its book,
over the link between the two engines, and the follower's book is built from those. The follower writes them into its
open-order region and notes the number of the WAL record behind the last one it applied (`last_replicated_seq_no_`).

When the follower is to lead, it asks the leading sequencer to catch it up from the larger of that number and the
last command it has applied, applies what it is sent, and then, because every environment sets
`open_orders_on_promotion = "cancel"`, asks for a `CancelBook` command that cancels every order it inherited. Every
member is sent a cancel for every resting order.

The link between the engines carries more than book updates. Each engine is one of the three voters on the other's
lease, so the lease requests and grants between the pair travel on it (`EngineLeaseLinks::send_request_to_peer`);
each engine announces its role on it; and each sends its settings on it (`EngineSettings`), so that an engine whose
peer has settings that would make their books differ refuses to lead after it.

## 2. A follower engine follows the WAL

### 2.1 It is sent every committed record

A follower engine asks the leading sequencer to catch it up as it does now, from the last command it has applied, and
to go on sending it new records. Its `WalFollowRequest` says which it is asking for in a field of its own, `to_lead`:
an engine that is to lead asks with it set and is then sent commands by order routing, as now, and a follower asks
with it clear. The field is needed because an engine sets `wants_new_records` whichever it asks for, as a publisher
does, since both want new records once caught up; the sequencer reads `to_lead` where it now reads
`wants_new_records` to tell a promotion from a start. The library already serves a request for new records for the
matching engine publishers: the reading thread sends every record after the position named, up to the last record committed,
the sequencer then sends `WalFollowCaughtUp`, and from then on the sequencer's own thread sends the follower each
record as it is committed, reading it back from the WAL (`WalHandedOverFollowers`,
[following_the_wal.md](following_the_wal.md), section 4.3). The hand-over is made at an exact position, so nothing is
missed or sent twice.

The follower is sent every record, not only commands: the report records the sequencer writes for the leading
engine's reports as well. That is what lets it check that nothing is missing (section 2.2) and know which reports the
WAL holds (section 3.2). The reading thread passes each record through a converter chosen for the kind of follower,
and the one for a matching engine passes over every record that is not a command for the engine
(`engine_record_from_wal`), while the one for a publisher sends every record. A follower engine is served with a
converter that sends every record, and once caught up is handed over to a set of followers of its own on the
sequencer's thread, as the publishers are (`handed_over_publishers_`).

The leading engine is sent commands as it is now, by the sequencer's own thread as each is released, from the bytes it
holds in memory. Its path, which carries every member's order, is unchanged.

### 2.2 It notices a missing record

The WAL's records are numbered one after another with no gaps, and a follower that is sent every record is sent them
in that order on one connection. So each record must be numbered one more than the last. A record numbered otherwise
means one was missed: the follower applies nothing more on that connection, says so at Error, and asks to be caught
up again from the last command it applied, which is the request it makes after any break (section 3 of
[far_back_catch_up.md](far_back_catch_up.md)). Nothing in the venue is expected to cause this: the check is there so
that if something does, it is noticed at the record where it happened, rather than at a promotion.

When its connection to the leading sequencer breaks, or another sequencer takes the lead (`WalLeaderChanged`), the
follower asks again from the last command it applied, as every follower of the WAL does.

### 2.3 What it does with each record

A command is applied by the same code that applies it on the leader (`MatchingEngineStateMachine`), so it changes
the book, and the open-order region that holds it, as it does on the leader. The engine's decisions are commands in
the WAL and its identifiers come from positions in the WAL (phase 7), so the follower's book is the leader's after
the same record.

A report record is used only to know what the WAL holds (section 3.2) and, if decision 3 is yes, to compare with the
follower's own report (section 4).

### 2.4 A follower too slow to keep up costs resilience, not service

The sequencer's sends to the follower wait on the connection while the follower is slow to read them. If they pass
the limit the framework allows to wait on one connection (`[reactor] connection_waiting_sends_maximum`), the framework
closes the connection, as it does for any peer that is not reading what it is sent. The follower then reconnects and
asks to be caught up, and the reading thread serves it, paced by the connection. The leading engine and the members'
orders are not held up by any of this, and the follower says at Warning that it is catching up again, so that an
operator can see the venue has, for that time, no standby that is current (V11).

## 3. Reports

### 3.1 A follower sends no report

A follower applying a command produces the same reports the leader produces. It does not send them: the leader's
reports are the ones the members are sent. The leading sequencer still passes on a report from any engine, because an
engine taking the lead sends reports during its catch-up to lead (section 3.3), before the sequencer routes orders to
it; a sequencer that passed on only the routed engine's reports would drop exactly those.

### 3.2 It keeps them until the WAL holds the leader's

A follower cannot simply throw its reports away. If the leading engine dies after applying a command and before its
reports reach the WAL, the member is never sent them, unless the engine that takes over sends them. So the follower
keeps each report it produces until it knows the WAL holds the leader's reports of the same command.

It knows from the report records it is sent. Each report record says which command it answers (`answers_seq_no`), and
the leading engine sends its reports in the order of the commands they answer. So once a report record answering
command N has arrived, the WAL holds every report of every command before N: the follower drops its kept reports of
those commands. This is the same rule the leading sequencer uses to tell a catching-up engine which reports the WAL
already holds (`fully_reported_through`, section 2 of [far_back_catch_up.md](far_back_catch_up.md)).

The kept reports are therefore those of the commands applied since the last report record arrived, which in ordinary
running is the few commands the leader is still answering. They are held in memory, up to a limit of
100,000 reports (`max_kept_reports_` in `MatchingEngineThread`). Passing it means the leading engine has not had a report written
for that many commands, and section 4 says what the follower does then.

### 3.3 On taking the lead, it sends what the WAL lacks

When a follower is to lead, it asks to be caught up to lead, as now. The leading sequencer's `WalFollowStarted` names
the last command the WAL holds every report of. The engine sends its kept reports of every later command, each marked
as a possible repeat, because the old leader may have sent the same report to the member before it died; then it
applies what the catch-up sends it, sending those reports as a catch-up does; then it leads. No member is left without
a report, and none is sent a report the WAL already holds.

## 4. Noticing that the two engines differ

The plan leaves open whether the follower compares its output with the leader's while the venue runs. With section 3,
the comparison costs almost nothing: when a report record arrives, the follower holds its own report of the same
command, and compares the two before dropping its own. Two reports are the same if they agree in every field that
`wal_replay_check` compares when `ha_test.py` runs both engines on a WAL and checks their reports against a replay;
a report the follower kept is never marked as a possible repeat, and the leader's live report is not either.

A difference means the two engines did not apply the same command alike: a defect in the engine, a difference in their
settings that the settings check missed, or damage to one engine's book. Under V11 the leader carries on trading
whatever the follower finds. What the follower does is decision 3; the recommendation is that it says so at Error and
by a test event, sends a metric, and does not lead until it has been restarted and has rebuilt its book from the WAL,
because a book that gives different answers from the ones the members were given is not one it should give them
answers from. The same applies when its kept reports pass their limit: it can no longer promise to send the reports
the WAL lacks.

The leading sequencer measures how long no engine leads, so if the leader is then lost, the venue halts by the limit
on automatic recovery ([far_back_catch_up.md](far_back_catch_up.md), section 5) rather than leading with a book it has
reason to doubt.

## 5. Promotion and start-up

Both engines already start in an unknown role, catch up when the first sequencer connects, and say what role they have
only once a lease settles it ([BUG-0195](../bug_list.md#bug_0195)). With this design a follower stays current after
that, so a promotion is the catch-up to lead of section 3.3, over the few records still on their way, and nothing
else. There is no copy to reconcile, so the decision between "starting" and "promoted" that `book_holds_peer_copy_`
makes now goes, and with it the case of [BUG-0143](../bug_list.md#bug_0143), in which an engine whose lease ran out
while starting is taken to be promoted and cancels every order it recovered: nothing is cancelled on promotion.

## 6. What is removed, and what stays

**Removed:**

- the `BookUpdate` message, its sending (`send_book_update`) and its applying (`apply_book_update`,
  `held_book_updates_`, `settle_held_book_updates`), and `book_holds_peer_copy_`. `last_replicated_seq_no_` stays,
  because it also holds the position the open-order region records;
- the setting `open_orders_on_promotion`, the `CancelBook` an engine asks for on promotion, and the checks in
  `ha_test.py` that expect it;
- the code by which a follower discards a command the sequencer sends it on its order connection
  (`MatchingEngineThread.cpp`, the passive follower), since a follower applies every command it is sent as a follower
  of the WAL.

**Stays:** the link between the engines, `[book_replication]` in their configuration, for the lease requests and
grants between the pair, the role announcements and the settings check (decision 4). Its name no longer says what it
is for, and renaming it, to `[peer]`, is part of step 3.

## 7. Steps, and how each is known to be right

1. **The follower follows the WAL (section 2).** The follower asks with `to_lead` clear and applies every command
   it is sent; the leading engine stops sending `BookUpdate`. Unit tests of the follower's check that records run on
   one after another, made to fail by sending one out of order. Scenario 87 must then pass: its `expected_failure` is
   removed. Every scenario that promotes an engine passes, with `open_orders_on_promotion` still deployed as
   `"cancel"`, since that step does not yet remove it.
2. **Reports (section 3).** The follower keeps its reports and sends those the WAL lacks when it takes the lead; the
   leading sequencer passes on only the routed engine's reports. Unit tests of the kept reports: dropped when a later
   command's report record arrives, kept otherwise, and the right ones sent on taking the lead. A new scenario blocks
   the leading engine's sends of reports to the sequencers with `libblock_sends_to_ports.so`, places orders, kills
   the leading engine, and requires the member to receive a report of every order, from the promoted engine. It fails
   with step 1 alone.
3. **Removing the copy (section 6).** The code and setting of section 6 go, the environments drop
   `open_orders_on_promotion`, and the scenarios that promote an engine require the orders resting before the
   promotion to be resting after it. This meets the first and second points of the phase's finish condition.
4. **Comparing (section 4).** If decision 3 is yes. Made to fail on purpose by a scenario that gives the two engines
   different identifier secrets, so that their order identifiers differ, and requires the follower to report the
   difference and the leader to go on trading. The settings check refuses that pair today, so the scenario turns it
   off for the test.
5. **The cost on the sequencer's thread.** Sending every record to the follower engine adds a read back from the WAL
   and a send for every record, on the sequencer's own thread. It is measured with the order path's latency
   instruments on a trading-day run before and after, as the other changes to that thread were, and stated here.

**Steps 1 and 2 are built.** Scenario 87 passes with either engine leading first. Scenario 88 blocks the leading
engine's reports, kills it after a thousand orders, and requires a New report of each from the engine that takes the
lead; with the kept reports not sent it fails. Both full suites passed on 28 October 2026: 88 of 88 with the primary
engine leading first and 30 of 30 with the secondary first.

Each step ends with both full suites from fresh WALs. Steps 1 and 2 are built and committed together: a follower that
applies commands and sends none of their reports leaves a member with no report of a command whose leader died before
its reports reached the WAL, until step 2 has the follower keep and send them.

With step 1, an engine that has followed the WAL holds the venue's book, so taking the lead from it is a promotion
whichever engine it is, where before it was a promotion only for an engine that had been sent book updates. So until
step 3, the deployed `"cancel"` cancels the orders an engine accepted while it waited to lead, as scenario 79 shows; the
member is told of each.

## 8. Decisions

### Decision 1. Is the follower engine sent every committed record as a follower of the WAL, rather than only commands? (Yes or no.)

Section 2.1. Sent every record, through the library's existing path for followers that want new records, the
follower can check that the numbers run on with none missing, and learns from the report records which of its reports
the WAL holds, with no new message. Sent only commands, as the leader is, it would need the sequencer to tell it which
reports the WAL holds, by a new message, and could not tell a missed command from the ordinary gaps the report
records leave between commands.

**Recommendation: yes.** The cost is section 7, step 5, to be measured.

**Decided** on 28 October 2026: yes.

### Decision 2. Does the follower keep its reports until the WAL holds the leader's, and send those the WAL lacks when it takes the lead? (Yes or no.)

Section 3. The alternative is for both engines to send every report and the sequencer to pass on the first copy of
each, which is way C of [ha_library_boundary.md](ha_library_boundary.md); it doubles the reports the sequencer is
sent, and was left for later.

**Recommendation: yes.**

**Decided** on 28 October 2026: yes.

### Decision 3. What does a follower do when its report differs from the leader's, or its kept reports pass their limit? (Choose one.)

Section 4.

- **A. It says so, and may still lead.**
- **B. It says so, and does not lead until it has been restarted and has rebuilt its book from the WAL.** If the
  leader is then lost, the venue halts by the limit on automatic recovery.
- **C. It does not compare.** Section 4 is not built; a difference is found only when a promoted engine's answers
  differ from what the members were told.

**Recommendation: B.**

**Decided** on 28 October 2026: B.

### Decision 4. Does the link between the engines stay, for the lease, the role announcements and the settings check? (Yes or no.)

Section 6. The plan's list for phase 8 removes "the connection between the two engines that carries" the book
updates. That connection also carries the lease requests and grants between the pair, because each engine is a voter
on the other's lease. Removing it would leave each engine's lease with two voters, itself and the arbiter, of which
it needs both.

**Recommendation: yes**, with only the book updates removed from it, and the plan's list corrected to say so.

**Decided** on 28 October 2026: yes.

### Decision 5. Are voiding a command that crashed an engine, and rejecting the orders held before a halt, designed in a second document within phase 8, after steps 1 to 5? (Yes or no.)

Section 9.

**Recommendation: yes.**

**Decided** on 28 October 2026: yes.

## 9. What else phase 8 designs

The plan puts two more designs in this phase: voiding a command that crashed both engines
([BUG-0193](../bug_list.md#bug_0193)), and giving every order held while no engine leads a definite rejection through
the WAL ([BUG-0197](../bug_list.md#bug_0197), parts 1, 3 and 4). Both need the same thing that nothing in the venue
does yet: a command in the WAL that changes what an earlier command does. An engine applies the WAL in order, so it
reaches the earlier command before the later one that rejects it; a crashing command crashes the engine again on every
catch-up before the void is reached, and a held order is applied, and may rest on the book, before the rejection is
reached. So the engine has to be told of such commands before it reaches the commands they name: for instance, by the
reading thread sending them at the start of a catch-up, as it sends `WalFollowStarted`. That is a design of its own,
with decisions of its own, and it rests on this one: once both engines follow the WAL, the rejection reaches both
alike. It also needs a way for an operator to declare and lift a halt ([BUG-0065](../bug_list.md#bug_0065)), which
this design does not provide. Decision 5 proposes designing them in a second document, once steps 1 to 5 are built.
