# Dividing the sequencer and the matching engine into parts

This is the design for phase 10 of the [recovery plan](../recovery_plan.md): dividing what is left of
`SequencerThread` and `MatchingEngineThread` into parts, each with one job and its own state, all still run by the
same single thread. Behaviour does not change. Section 10 holds its decisions, which Andrew took on 3 November 2026, each as recommended.

## 1. Why

`SequencerThread` is 4,559 lines of code and 1,205 of declarations, and `MatchingEngineThread` is 2,412 and 781. Each
holds, as members of one class, the state of several separate jobs. The sequencer, for example, holds which gateway
connection each member's session is bound to, the orders waiting to be sent to the matching engine, the reports
waiting to be sent to the members, whether trading is halted, which commands are voided, and which matching engine
leads. Any member function can read or change any of it.

That causes three problems.

- **A reader cannot tell what a change touches.** To know whether a change to how reports are held can affect how
  orders are released, a reader has to search 4,559 lines for every use of every member involved.
- **The same fact is kept in several places, in slightly different ways.** For example, whether trading is halted,
  which commands are voided, the trading date, and the highest command a report answers are all facts read from the
  WAL. The sequencer notes them in four places: when the WAL is opened (`read_at_open`), when a record is written as
  follower (`written_as_follower`), when this instance writes a record as leader (`append_envelope_to_wal`), and when
  it writes a command of its own (`write_own_command`). Each place calls a different subset of the five functions
  that note them. Some of the differences are intended (a follower learns the trading date from its leader's commands,
  and a leader reads it from the first record in its WAL). Whether every difference is intended cannot be seen
  without reading all four places side by side.
- **A situation is spread across several true-or-false members.** In the matching engine, what the instance is doing
  is held in `ha_role_state_`, `catch_up_purpose_`, `follows_the_wal_`, `has_reconciled_`,
  `role_when_following_again_`, `refused_to_lead_` and `peer_leader_heard_while_reconciling_`. Not every combination
  of their values can happen, and the code does not say which ones can. Several defects in this project (BUG-0064,
  BUG-0200 and BUG-0218 among them) were a combination nobody had considered.

## 2. What a part is

A part is a class that:

- **owns the state of one job**, held in private members that nothing else can read or change;
- **is told what happens** by the thread, through its member functions: a message that arrived, a connection made or
  lost, a timer, a record written or committed;
- **asks for what it cannot do itself** through an interface the thread implements, such as sending a message on a
  connection, starting a timer, or raising a test event. It does not hold the reactor, the connections or the other
  parts;
- **writes its own Quill log lines**, through the logger it is given, as `UnansweredCommands` and
  `MatchingEngineStateMachine` already do;
- **holds one explicit state where its situation is more than one true-or-false value**: an enumeration of the
  situations that can happen, with each move between them made in one place, and the moves written down in the
  class's comment;
- **can be tested without a reactor**, by a unit test that calls its member functions and records what it asks the
  thread to do.

This is the pattern the project already uses: `MatchingEngineStateMachine` with `MatchingEngineOutputsInterface`,
`pubsub_itc_fw::UnansweredCommands` with `UnansweredCommandLinksInterface`, and the replicated WAL with its links and
events interfaces. The sequencer also already has small parts of this kind: `EngineOrderRouting`, `EngineAbsenceRule`,
`RecoveryLimitRule`, `TradingHaltState`, `KeptReportStore` and `ReportsHeldForUnknownSessions`. This phase gives the
rest of the two programs the same shape.

Every part runs on the thread that owns it. So there are no new threads, no locks, and no change to which thread
owns which state (principle F1). A part allocates its storage when it is created, as the members it takes over do
now, so nothing new allocates on the order path (F5). The parts are the venue's own business, so they stay in the
application directories (`applications/sequencer`, `applications/matching_engine`), not the library.

What remains in each thread class is the connections, the timers, and the handing of each event to the part whose job
it is. The thread class holds no state of its own apart from connection and timer identifiers.

## 3. What does not change

- The messages on every connection, and the records in the WAL.
- The configuration files.
- The test events, and what each scenario of `ha_test.py` checks.
- The Quill log lines, apart from the name at the start of a line that moves into a part, where it now names the
  part.
- The order in which things are done in response to one event. Where the thread now does several jobs' work for one
  event, for example when a record is committed, it calls the parts in the order the work is done now, and the
  comment at that place says why the order matters.

## 4. The parts of the sequencer

Each entry names the part, says what its job is, and lists the members it takes over from `SequencerThread`.

1. **`VenueStateFromTheWal`**: the facts read from the WAL, which every instance keeps whether it leads or follows.
   It holds whether trading is halted and the last member's command before each halt (`trading_halt_`), the voided
   commands (`voids_`, `void_awaiting_commit_`), the trading date (`trading_date_`), and the highest command a report
   answers (`highest_command_answered_`). It is told of each record in one way, `note_record(seq_no, record, how)`,
   where `how` says whether the record was read when the WAL was opened, written as follower, or written by this
   instance as leader. The differences between those three cases are made in that one function, each with a comment
   saying why.
2. **`MemberSessions`**: which gateway connection each member's session is bound to, and what the venue remembers
   about each session between connections. It holds `session_destinations_`, `session_sequence_state_`,
   `seq_no_to_session_`, `sessions_with_reports_dropped_` and `reports_dropped_unbound_`. It handles `SessionBound`,
   `SessionUnbound`, `SessionSequenceUpdate`, the session replay request, and the request for reports a member was not
   delivered.
3. **`ReportForwarding`**: getting each report from the matching engine to its member's gateway. It holds the reports
   waiting for their record to be committed (`pending_er_`), the reports held for sessions not yet announced
   (`held_reports_` and its counts), the reports held for a gateway with no connection for reports
   (`reports_held_for_gateways_` and its counts), which gateways have announced their sessions
   (`gateway_announcements_`), the gateway connections (`gateway_conn_ids_`), and how far every report has been
   forwarded (`latest_report_seen_`, `reports_forwarded_through()`).
4. **`KeptReportStore`**, extended: the reports kept while this instance does not lead, and the discarding of those the
   leader has forwarded. It takes over the samples of the leader's forwarded position (`forwarded_samples_` and the
   members beside it) and `last_granted_to_leader_`.
5. **`OrderRelease`**: holding each order until it may be sent to the leading matching engine, and sending it. It
   holds `held_orders_`, `first_unheld_seq_`, the pausing of reading from the gateways (`order_reading_paused_`,
   `paused_for_hand_over_`), and the wait after taking the lead for the engine to say the highest order it has acted
   on (`awaiting_engine_position_`, `held_at_takeover_` and the members beside them). The two reasons for pausing the
   reading become one state with the reasons it can be paused for.
6. **`EnginePresence`**: which matching engine leads and whether one takes orders, and what follows from that. It holds
   `engine_routing_`, `engine_servings_`, `leading_engine_instance_`, the time without an engine (`no_engine_since_`,
   `an_engine_took_orders_`, `orders_held_without_an_engine_`), the limit on automatic recovery (`engine_absence_`,
   `recovery_limit_`, `engine_catch_up_progress_`), and what the gateways are told about taking orders
   (`order_acceptance_`). Whether an engine takes orders, whether one ever has since this instance began leading, and
   what the gateways are told become one state.
7. **`CommandChecks`**: the check of each command against those already in the WAL, and what follows from a match:
   refusing a reused `ClOrdID`, or answering a command sent again from the WAL
   ([one_check_against_sequencing_twice.md](../availability/one_check_against_sequencing_twice.md)). It holds the
   counts and the summary of checks.
8. **`HaltAndVoidRequests`**: the requests to halt trading or void commands, from an operator, from a matching engine
   that cannot account for the book, and from a matching engine that died applying a command, and the halt the leader
   writes when no engine could be trading within the limit on automatic recovery. It decides; the commands it writes
   go into the WAL through the thread, as now.
9. **`ServingWalFollowers`**: what this instance does for those that follow its WAL: the follower sequencer's
   catch-up, the matching engine publishers and the matching engines that follow the WAL, once handed over, and the
   damaged records that stop one being caught up. It holds `wal_following_`, `handed_over_publishers_`,
   `handed_over_engines_`, `wal_follower_service_`, and `damaged_records_reported_` with the members beside it.

Replay mode (`--replay`) is the subject of decision 3.

## 5. The parts of the matching engine

1. **`EngineReportSending`**: sending each report to both sequencers. It holds the numbering of reports
   (`report_number_`, `report_number_epoch_`), the reports held until this instance knows whether it may serve, or until
   there is a connection to send them on (`held_reports_`, `held_reports_overflowed_`,
   `waiting_for_report_connection_`), and the two connections for reports.
2. **`FollowerReportComparison`**: the reports of the commands this instance applies while following the WAL, kept
   until the WAL holds the leader's reports of the same commands, compared with them, and sent on taking the lead if
   the WAL lacks them ([both_engines_apply_the_wal.md](../availability/both_engines_apply_the_wal.md)). It holds
   `kept_reports_` and the members beside it, and `refused_to_lead_` and `report_differences_`.
3. **`EngineCatchUp`**: asking the leading sequencer to be caught up, and checking the answer was complete (R-0101). It
   holds `follow_request_number_`, `catch_up_tally_`, `catch_up_connection_`, the retrying
   (`reconciliation_timer_id_` and the counts beside it), `reports_in_wal_through_`, `has_position_`,
   `follow_request_sent_`, and the decision about an unusable region (`region_was_unusable_`, `halt_awaited_`).
4. **`EngineRole`**: what this instance is doing: starting, catching up, leading, or following, and why. It holds the
   lease agent and the promise recorder, the epoch and its store, and the role, with `catch_up_purpose_`,
   `follows_the_wal_`, `has_reconciled_`, `role_when_following_again_` and `peer_leader_heard_while_reconciling_`
   folded into one state.
5. **`DiedApplyingCheck`**: the question of whether to ask for the command the previous process died applying to be
   voided (`died_applying_question_` and the members beside it), which is already one state.
6. **`PeerSettingsCheck`**: comparing the settings the two engines of a pair must share, and barring a peer whose
   settings differ (`peer_settings_known_` and the members beside it).

## 6. The states written down

The finish condition asks for each part's states to be written down. This is done first, as step 1, for both
programs as the code is now, before anything moves. It is written in this document, as a table for each part: the
situations, and for each move between them, what causes it and what is done.

Writing them down before moving the code has two purposes. It is the check that the moved code behaves as the old
code did: each step compares the part's moves with the table. And it is where a combination of values that should
not happen, or a move nobody intended, is found. Following the plan, each defect found is recorded in the bug list and
not fixed within this phase, unless it can lose or duplicate an order, in which case it is raised with Andrew first.

### 6.1 The matching engine's role (`EngineRole`)

**Where it is held now.** Seven members of `MatchingEngineThread` together say what the instance is doing:

- `ha_role_state_`: Unknown, Reconciling (catching up), Leader or Follower;
- `catch_up_purpose_`: what the catch-up under way is for: BecomeCurrent, Lead or FollowAgain;
- `follows_the_wal_`: whether the latest catch-up request asked to follow the WAL rather than to lead;
- `has_reconciled_`: whether a catch-up has completed;
- `role_when_following_again_`: for a catch-up to follow again, the role to go back to when it ends;
- `peer_leader_heard_while_reconciling_`, with `peer_leader_instance_id_` and `peer_leader_epoch_`: that this
  instance agreed the peer leads while it was catching up, to be acted on when the catch-up ends;
- `refused_to_lead_`: that this instance has found its reports differ from the leader's, and will not lead until it
  is restarted.

Three functions read them together. `following_the_wal()` is true when high availability is on, `follows_the_wal_`
is set, and the role is neither Reconciling nor Leader. `keeps_its_reports()` is true when high availability is on,
`follows_the_wal_` is set, and the role is not Leader. `holding_reports_until_entitled()` is true when high
availability is on, the role is Reconciling, and the purpose is not Lead.

**The situations.** With high availability on, these are the combinations that occur. Each is named by what the
instance is doing.

| Situation | Members now | Commands it is sent | Its reports | Announces |
|-----------|-------------|---------------------|-------------|-----------|
| **Starting** | Unknown; no catch-up has completed | discarded | none produced | nothing |
| **Catching up to become current** | Reconciling; purpose BecomeCurrent; asked to follow | applied | kept, and compared with the leader's, because the request asked to follow | nothing |
| **Current, following the WAL** | Unknown; a catch-up has completed; asked to follow | applied, as a follower | kept, and compared with the leader's | nothing |
| **Catching up to lead** | Reconciling; purpose Lead; asked to lead | applied | sent | nothing |
| **Leading** | Leader | applied | sent | Leader |
| **Catching up to follow again** | Reconciling; purpose FollowAgain; asked to follow; the role to go back to is Unknown or Follower | applied | kept | nothing |
| **Following** | Follower; asked to follow | applied, as a follower | kept, and compared with the leader's | Follower |
| **Following, without following the WAL** | Follower; the last request asked to lead | discarded | none produced | Follower |

The last situation is a defect: see "Found" below.

Holding a report until the instance knows whether it serves (section 6.2) happens when high availability is on and
the instance is catching up for a purpose other than leading. With high availability on, such a catch-up's request
always asks to follow, and a report of a command applied after a request to follow is kept rather than held. So the
holding is reached only before any request has been sent in a catch-up, when no record can arrive; with the code as
it is, a report is not held for this reason in practice.

`refused_to_lead_` is separate from the situation: an instance that has refused to lead goes on doing what it was
doing, and no longer asks for a lease. It is held by `FollowerReportComparison` (section 6.3), through which every
cause of refusing goes.

With high availability off, the single instance is in none of these. It is Unknown, applies every command it is
sent, and catches up once, when the first sequencer connects, asking as one that is to lead.

**The moves.** "The lease agent says" means the change `PairLeaseAgent` returns to `act_on`: BecameLeader,
StoppedLeading, or AgreedPeerLeads. AgreedPeerLeads is returned for every request from the peer that this instance
grants, so a leading peer causes it once every renewal interval, not only once.

| From | Event | To | Done as it moves |
|------|-------|----|------------------|
| Starting | the first connection from a sequencer for orders | Catching up to become current | `begin_reconciliation`: the catch-up is asked for |
| Starting | a command arrives | Starting | discarded |
| Starting | the lease agent says AgreedPeerLeads | Following, without following the WAL | `follow_peer_claiming_leadership`: takes the peer's epoch, unless it is older than this instance's; announces Follower. The first sequencer connection then asks for no catch-up, because the role is Follower |
| Catching up to become current | the catch-up completes | Current, following the WAL | reports held stay held; the next record expected is set; if the peer was agreed to lead meanwhile, and trading is not halted, Following, at the peer's epoch, if that is not older than this instance's |
| Catching up to become current | the lease agent says AgreedPeerLeads | the same | noted, to be acted on when the catch-up completes |
| Current, following the WAL | the lease agent says AgreedPeerLeads | Following | `follow_peer_claiming_leadership`: takes the peer's epoch, unless it is older than this instance's; reports held are discarded; announces Follower |
| Starting, Current, Following, or catching up to become current or to follow again | the lease agent says BecameLeader | Catching up to lead | `become_leader_when_current`; a catch-up under way is replaced, and its answer not waited for |
| Catching up to lead | the catch-up completes | Leading | announces Leader; reports held are sent |
| Catching up to lead | the lease agent says StoppedLeading | Following, without following the WAL | the purpose is set back to BecomeCurrent; a halt awaited is no longer asked for; reports held are discarded; announces Follower |
| Leading | the lease agent says StoppedLeading | Catching up to follow again, to go back to Following | announces Follower, then asks to follow again |
| Current, following the WAL, or Following | a record arrives out of turn; a sequencer connects; or a sequencer says it now leads | Catching up to follow again, to go back to the situation it was in | `follow_the_wal_again` |
| Catching up to follow again | the catch-up completes | the situation it is to go back to | the next record expected is set |
| Catching up to follow again | the lease agent says AgreedPeerLeads | the same | noted, but not acted on when this catch-up completes; an instance going back to Current, following the WAL, moves to Following at the next grant |
| any situation that is catching up | the catch-up is incomplete (R-0101) | the same | asked for again on the next tick of the retry timer |

**Found.** *Following, without following the WAL* is reached in two ways, and is a defect, recorded as
[BUG-0225](../bug_list.md#bug_0225):

- **An instance restarted beside a leading peer** grants its peer a lease while it is still Starting, before any
  sequencer has connected, and becomes a Follower without having asked to follow the WAL. When the sequencers connect,
  it asks for no catch-up, because the role is Follower. This is the ordinary way a standby comes back. It was seen in
  scenario 24 on 3 November 2026, with the restarted engine's Quill log copied aside while the scenario ran.
- **The lease runs out while the instance is catching up to lead**, for example when it cannot reach its peer or the
  arbiter during a catch-up that takes seconds. `enter_follower_state` asks to follow the WAL only when the role it
  is leaving is Leader; here it is Reconciling. Found by reading the code; not reproduced.

From then on the instance announces that it follows, but applies no command, keeps no report, and its book falls
behind. No event makes it ask to follow: each of the three that do (a record out of turn, a sequencer connecting, a
sequencer saying it now leads) is acted on only by an instance that follows the WAL or is catching up. It catches up
again only if it is later granted the lead. Nothing is lost, because that catch-up brings every command it lacks, but
the venue has a standby that cannot take over promptly, and nothing says so, which principle V11 requires. Following
the plan, it is recorded and not fixed within this phase: it loses and duplicates no order.

### 6.2 Sending reports (`EngineReportSending`)

**Where it is held now.** `held_reports_`, `held_reports_overflowed_`, `waiting_for_report_connection_`, the two
connections for reports (`sequencer_er_conn_id_`, `sequencer_er_secondary_conn_id_`), and the numbering
(`report_number_`, `report_number_epoch_`). Each held report records whether it has been numbered, and whether it has
been sent to each sequencer.

**What decides where a report goes.** `EngineOutputs::send_report` first passes over a report of a command applied
while following, or a report marked as a possible repeat, whose command the WAL already holds every report of
(`reports_in_wal_through_`). A report of a command applied while following is then kept (section 6.3), not sent. Any
other report goes to `send_er_to_sequencer`, which holds it or sends it.

| Situation | What makes it so | A new report is |
|-----------|------------------|-----------------|
| **Sending** | not catching up to become current or to follow again, and a connection for reports is up | numbered and sent on every connection for reports that is up |
| **Holding until it knows whether it serves** | high availability on, and catching up for a purpose other than to lead (`holding_reports_until_entitled`) | held, up to 50,000; past that, sent as in the next two rows, and the overflow said once at Warning |
| **Holding for want of a connection** | neither connection for reports is up | held, up to 50,000; past that, lost, and the loss said once at Error |

**The moves.**

| Event | What is done with the reports held |
|-------|-------------------------------------|
| the instance becomes Leading (`adopt_leader_role`) | `release_held_reports`: each is numbered, if it was not, and sent on each connection for reports that is up and it has not been sent on; one sent on every connection there is is dropped |
| the instance becomes Following (`enter_follower_state`) | `discard_held_reports`: all dropped, and the number said at Info |
| a connection for reports is established | `release_reports_held_for_a_connection`: sent as above, unless the instance is holding until it knows whether it serves |
| the instance takes the lead and its catch-up starts (`send_kept_reports_after`) | the reports kept while following, of commands after the last the WAL holds every report of, are added to those held, and sent as above |

One member, `held_reports_overflowed_`, records the overflow of both kinds of holding, though the two overflows mean
different things: in the first, later reports are sent; in the second, they are lost. In `EngineReportSending` they
become two.

### 6.3 Comparing the reports of commands applied while following (`FollowerReportComparison`)

**Where it is held now.** `kept_reports_`, `kept_reports_overflowed_`, `keeping_from_`,
`last_report_record_accounted_`, `highest_command_compared_`, `report_differences_`, and `refused_to_lead_` with the
counter beside it.

**What it does.** While the instance keeps its reports (`keeps_its_reports`), each report of a command it applies is
kept, in the order of the commands. When a report record of the leader's arrives, answering command N:

- each kept report of a command before N that was never compared is a difference (ReportNotSent), and every kept
  report of a command before N is dropped;
- the first kept report of N not yet compared is compared byte for byte with the leader's; if they differ, that is a
  difference (ReportDiffers);
- if none of N is kept, and N is after the last command the WAL held every report of when the catch-up started, at or
  after the first command whose reports are kept, and the store has not overflowed, that is a difference
  (ReportNotProduced);
- a report record of a command before the highest already compared is passed over: a sequencer that took the lead
  writes such reports again.

The first difference makes the instance refuse to lead (`refuse_to_lead`): it stops asking for a lease and marks its
region unusable. The situation is one member, `refused_to_lead_`, which never goes back. On taking the lead, the kept
reports of commands after the last the WAL holds every report of are sent (section 6.2), and the rest dropped.

When 100,000 reports are kept, later ones are not kept: the first time, that is said at Error and the instance
refuses to lead (KeptReportsFull), and from then on a missing report is no longer counted as a difference. A void
learned after applying a command it names also makes the instance refuse to lead (AppliedVoidedCommand).

### 6.4 Catching up (`EngineCatchUp`)

**Where it is held now.** `follow_request_number_`, `follow_request_sent_`, `catch_up_connection_`,
`catch_up_tally_`, `reconciliation_timer_id_`, `reconciliation_attempts_`, `reconciliation_records_seen_`,
`reconciliation_records_at_last_tick_`, `reconciliation_reports_sent_`, `reconciliation_reports_left_out_`,
`reports_in_wal_through_`, `records_from_earlier_answers_`, `has_position_`, `last_replicated_seq_no_`,
`next_followed_seq_no_`, and, for an unusable region, `region_was_unusable_` and `halt_awaited_` with its timer.

**The situations of one catch-up.**

| Situation | What makes it so | A record that arrives is |
|-----------|------------------|--------------------------|
| **Not asked yet** | catching up, and the request not sent: no sequencer is connected for orders, or the question of a command the previous process died applying is not settled | ignored, as from an earlier answer |
| **Asked, no answer started** | the request sent, on every sequencer connection for orders, with a new number | ignored, as from an earlier answer |
| **Answer under way** | `WalFollowStarted` with the latest number has arrived; its connection is noted, and the tally set from where it starts | applied, and counted in the tally |
| **Following** | the catch-up has completed, and the instance follows the WAL | applied only if it is the next record expected; otherwise nothing more is applied and the instance asks to be caught up again |

**The moves.**

| Event | Done |
|-------|------|
| a catch-up begins (`begin_reconciliation`) | the counts are set to zero, the retry timer started, and the request sent if a sequencer is connected for orders |
| a sequencer connects for orders, while catching up | if the request has not been sent, the catch-up begins again; if it has, the request is sent on that connection alone, with the same number, so that an answer under way is left to finish (BUG-0200) |
| the retry timer, while catching up | nothing while the died-applying question is open; a warning if no sequencer is connected for orders; nothing if records have arrived since the last tick; otherwise the request is sent again, with a new number |
| `WalLeaderChanged`, while catching up | the request is sent again, with a new number |
| `WalFollowCaughtUp` with the latest number | if the tally accounts for every record the sequencer sent, the catch-up is complete: the timer is stopped, and the role moves as section 6.1 says; if not, it is said at Error, and the timer asks again |
| `WalFollowCaughtUp` or `WalFollowStarted` with another number, or when not catching up | ignored |
| the catch-up completes, with the region found unusable | if the WAL reaches the first command of the day, the region counts as usable from then on; otherwise, if the instance is catching up to lead, or high availability is off, it asks the sequencers every second to halt trading, until trading is halted or it stops leading |

Both of the first two situations ignore every record: `from_an_earlier_answer` ignores a record that arrives while
catching up on any connection other than the one the latest answer started on, and before an answer has started
there is no such connection. A record from an earlier answer can still be arriving when a new request is sent,
because the leading sequencer stops answering an earlier request before it answers a later one, but what it had
already sent is still on its way.

### 6.5 A command the previous process died applying (`DiedApplyingCheck`)

**Where it is held now.** `died_applying_question_`, already one state (none, asking the peer, asking the sequencers),
with `died_applying_`, `died_applying_outcome_`, `peer_asked_whether_applied_`, and two timers.

| From | Event | To | Done |
|------|-------|----|------|
| none | the application is ready, and the region recorded a command being applied above the position it published | asking the peer (high availability on), or asking the sequencers (off) | the peer is asked, once, on whichever connection to it is up, and a timer of one lease period started |
| asking the peer | the peer says it has applied the command | none | settled: PeerAppliedIt |
| asking the peer | the peer says it has not, or the timer runs out | asking the sequencers | `VoidRequest` on every sequencer connection for orders, and again every second |
| asking the sequencers | `VoidedCommands` names the command | none | settled: Voided |
| asking the sequencers | `VoidDeclined` names the command | none | settled: Declined |

Settling it stops both timers, tells the test, and sends the catch-up request if a catch-up is waiting for it. While
the question is open, no catch-up is asked for, because the catch-up would apply the command again.

### 6.6 The settings the two engines must share (`PeerSettingsCheck`)

**Where it is held now.** `peer_settings_known_`, `peer_region_capacity_`, `peer_secret_fingerprint_` and
`peer_settings_differ_`.

The situations are: the peer's settings not yet known; known and the same; known and different. They are sent when
this instance's connection to the peer's listener is established, and kept when that connection is lost. While this
instance leads, it says through the lease agent that its peer may not lead whenever the settings differ, and that it
may lead again when they no longer differ (`bar_peer_if_settings_differ`), which is checked when the peer's settings
arrive and when this instance takes the lead.

### 6.7 The sequencer's role, and what each part does when it changes

The sequencer's role is the replicated WAL's, in the library: unknown when the process starts, then leader or follower.
The replicated WAL moves it to leader when the lease agent says BecameLeader, and to follower when it says
StoppedLeading or AgreedPeerLeads. It never moves it back to unknown. With high availability off, it is leader from
the start. `SequencerThread::on_role_changed` is told of each change and does each part's work for it, in this order:

| Part | On taking the lead | On stopping leading |
|------|--------------------|---------------------|
| `ServingWalFollowers` | told that this instance leads, at its epoch | every follower served is stopped, the engines being served are forgotten, and a pause of reading for a hand-over ends; the publishers and engines handed over are forgotten |
| `ReportForwarding` | nothing | every report held for sessions not yet announced, and for gateways with no connection for reports, is discarded |
| `KeptReportStore` | those the leader had forwarded are discarded; the rest are forwarded, marked as possible repeats | nothing |
| `EnginePresence` | the gateways are told whether the venue takes orders; the absence of an engine is checked | the absence is told that another instance leads; what the gateways are told goes back to Accepting, and the time without an engine is forgotten |
| `OrderRelease` | with high availability on, the leading engine is asked for the highest order it has acted on | the wait for that answer ends; every held order is discarded, the record of orders that waited in the WAL is cleared, and reading from the gateways is resumed if it can be |
| `CommandChecks` | nothing | the summary of checks is logged, if any check was made since the last |

The work on stopping leading is split across the two branches of the function: the first group is done whenever the
instance stops leading, the second whenever it becomes a follower. Since the replicated WAL only ever moves a leader
to follower, the two always happen together, but a reader has to check the replicated WAL to know that.

### 6.8 Holding and releasing orders (`OrderRelease`)

**Where it is held now.** `held_orders_`, `first_unheld_seq_`, `order_reading_paused_`, `paused_for_hand_over_`,
`order_connection_ids_`, and, for the wait after taking the lead, `awaiting_engine_position_`, `held_at_takeover_`,
`engine_position_request_id_`, `engine_position_asks_` and `engine_position_asked_at_`.

**An order.** The leader writes each order into the WAL and holds it in `held_orders_`, keeping the inbound message's
buffer. When the storage is full, the order is not held; `first_unheld_seq_` records the first such order, and no
later order is held either, so that orders are sent in order. Held orders are sent to the leading engine, oldest
first, once their records are committed (`committed_through`), unless the leader is waiting for the engine's position
or no engine takes orders (section 6.9). Orders that waited in the WAL are sent by the reading thread. The last order
released (`released_through`) is the lower of the last record committed and the record before the oldest order still
waiting.

**Reading from the gateways.** Two members hold three situations:

| Situation | Members now | Entered when | Left when |
|-----------|-------------|--------------|-----------|
| **Reading** | neither set | | |
| **Paused for held orders** | `order_reading_paused_` | leading, waiting for the follower's confirmation or for the engine's position, and three quarters of the storage is used or an order waited in the WAL | the storage falls to a quarter and no order waits in the WAL, or the leader no longer waits for either |
| **Paused for a hand-over** | both set | a follower's hand-over cannot finish because records are released faster than one round between the two threads | the hand-over finishes, or this instance stops leading; reading then resumes unless held orders keep it paused |

**The wait after taking the lead** (`awaiting_engine_position_`), with high availability on:

| From | Event | To | Done |
|------|-------|----|------|
| not waiting | this instance takes the lead, and an engine is routed to | waiting | the highest record in the WAL is noted, and the engine asked for the highest order it has acted on |
| not waiting | this instance takes the lead, and no engine is routed to | not waiting | nothing: whichever engine acts next catches up from the WAL |
| waiting | a second passes without an answer (on the acknowledgement watch timer) | waiting | asked again, with a new request number; the fifth ask is logged at Warning |
| waiting | the answer, with the latest request number | not waiting | if the engine lacks orders the WAL holds, and takes orders, the reading thread sends them from the WAL; then the held orders are released |
| waiting | asked again with no engine routed to | not waiting | the held orders are released |
| waiting | this instance stops leading | not waiting | |

### 6.9 Which matching engine takes orders (`EnginePresence`)

**Where it is held now.** `engine_routing_`, `engine_servings_`, `leading_engine_instance_` and
`leading_engine_announced_on_`, `no_engine_since_`, `an_engine_took_orders_`, `orders_held_without_an_engine_`,
`last_no_engine_warning_`, `order_acceptance_`, and, for the limit on automatic recovery, `engine_absence_`,
`engine_routed_stamp_`, `recovery_limit_`, `engine_catch_up_progress_` and `engine_catch_up_connection_`.

**Whether an engine takes orders** (`an_engine_takes_orders`): the order connection goes to an engine known to lead,
which does not follow the WAL and is not being caught up in answer to its own request.

**The time without an engine**, while this instance leads:

| Situation | Members now | Entered when | Done on entering |
|-----------|-------------|--------------|------------------|
| **An engine takes orders** | `no_engine_since_` empty, `an_engine_took_orders_` set | an engine takes orders | if there was a time without one, it is logged with its length and the orders held, `EngineTakesOrdersAgain` raised, and what the gateways are told worked out again |
| **No engine yet** | `no_engine_since_` set, `an_engine_took_orders_` not set | this instance leads and no engine has taken orders since it began | the start is noted, and logged at Warning |
| **No engine since one did** | `no_engine_since_` set, `an_engine_took_orders_` set | the engine that took orders stops doing so | as above |

While there is no engine, a Warning is repeated every 5 seconds, and once no engine has taken orders for
`orders_held_at_most_seconds`, the gateways are told to refuse new orders.

**What the gateways are told** (`order_acceptance_`), worked out by `order_acceptance_now` while leading, and always
Accepting otherwise:

| Halted? | No engine for the limit? | Told |
|---------|--------------------------|------|
| no | no | Accepting |
| no | yes | NoMatchingEngine |
| yes, for a recovery that took too long, and no engine takes orders | either | RecoveryTooLong |
| yes, otherwise | no | Halted |
| yes, otherwise | yes | NoMatchingEngine |

It is worked out again when an engine starts or stops taking orders, on the engine absence timer, about once a
second, and when a record that changes whether trading is halted is written.

**The limit on automatic recovery** measures a different absence: `engine_absence_` is told whether the order
connection goes to any engine (`engine_routing_.active()` is valid), whether or not that engine is known to lead,
follows the WAL, or is being caught up; and the stamp beside the WAL records when an engine was last routed to. So the
venue keeps two clocks of the time without an engine, by two definitions. The design keeps both, in one part, each
with a comment saying which question it answers; whether the two definitions should be one is a question for the
design of that part, in step 12, not a change made by moving the code.

### 6.10 The facts read from the WAL (`VenueStateFromTheWal`)

**Where it is held now.** `trading_halt_`, `voids_`, `void_awaiting_commit_`, `trading_date_`,
`highest_command_answered_`, and the samples of the leader's forwarded position (section 6.16).

**Where each is noted from.**

| Fact | When the WAL is opened (`read_at_open`) | As follower (`written_as_follower`) | As leader |
|------|-----------------------------------------|-------------------------------------|-----------|
| whether trading is halted | yes | yes | yes, for every record written (`append_envelope_to_wal`) |
| the voided commands | yes, and a record that does not decode is noted as one that cannot be read | yes | yes, for each command of its own (`write_own_command`) |
| the highest command a report answers | yes | yes | yes, for each report written (`forward_report_from_engine`) |
| the trading date | no | yes, from each command that carries one, following the leader if they differ | read once, from the first record in the WAL, when the first command needs it (`trading_date_for`) |
| the leader's forwarded position | no | yes | no |

Each difference has a reason: the trading date is the date of the first record of the day, which a leader reads and a
follower is told; and only a follower reads the position its leader has forwarded reports through. When records are
discarded from the WAL (`discarded_after`), the voids and halts after the point are discarded too, and a change in
whether trading is halted is logged at Warning. The highest command answered is not lowered: a figure too high only
declines a void that could have been written.

### 6.11 Forwarding reports (`ReportForwarding`)

**Where it is held now.** `pending_er_`, `latest_report_seen_`, `held_reports_` with `reports_held_since_sessions_known_`,
`held_reports_dropped_reported_`, `held_reports_dropping_`, `held_reports_oldest_position_` and
`held_report_encoding_`; `reports_held_for_gateways_` with `gateways_holding_reports_`,
`gateway_held_reports_dropped_reported_` and `held_envelope_encoding_`; `gateway_announcements_`; `gateway_conn_ids_`
and `unknown_gateways_warned_`.

**A report from the leading matching engine** is given its own record in the WAL, and then goes one of four ways:

| Where it goes | When |
|---------------|------|
| **sent at once** to its member's gateway | the leader does not wait for its follower, or the record it depends on is committed: the command's record, or, for a report marked as a possible repeat or answering no command, its own record |
| **waits** in `pending_er_`, keyed by that record | otherwise; sent when the record is committed (`forward_pending_er_through`) |
| **held for an unannounced session** | when it is to be sent, its session is connected nowhere, this instance leads, and some configured gateway instance has not announced its sessions on its present link; sent when the session binds, or dropped once every gateway has announced |
| **held for its gateway** | when it is to be sent, this instance leads, has no connection for reports to that gateway instance, and the instance is in the configuration; sent when the connection is made |

A report whose session is connected nowhere, once every gateway has announced, is dropped and counted
(`note_report_dropped_unbound`). Every report held of either kind is discarded when this instance stops leading.

**How far reports have been forwarded** (`reports_forwarded_through`), written on every record the leader writes: just
before the oldest report waiting in `pending_er_` or held for an unannounced session, or else the latest report seen.
A report held for its gateway does not hold it back, which is a defect, recorded as
[BUG-0226](../bug_list.md#bug_0226): a follower discards its kept copy of such a report, and the report is lost if
the leader dies before the connection is made.

**Gateways announcing their sessions** (`gateway_announcements_`): for each configured gateway instance, whether it
has announced its sessions on its present link, and that link. While any has not, reports for sessions connected
nowhere are held; when the last announces, those still held are dropped. A link lost makes its gateway unannounced
again.

The counts of reports dropped for lack of room in each store are said once when dropping starts and again with the
total when reports next leave the store. Three pairs of members hold that, one for each store, as
`held_reports_dropped_reported_` and `held_reports_dropping_` do for the first.

### 6.12 Member sessions (`MemberSessions`)

**Where it is held now.** `session_destinations_`, `session_sequence_state_`, `seq_no_to_session_`,
`sessions_with_reports_dropped_`, and `reports_dropped_unbound_` with its gauge.

**A session**, named by its comp id and protocol, is **bound** to a gateway instance and connection, or **connected
nowhere**. It is bound by `SessionBound` and unbound by `SessionUnbound`. Every instance keeps the bindings, leading
or not; only the leader answers a `SessionBound` with `SessionBoundAck`.

| Event | Done |
|-------|------|
| `SessionBound`, the session still connected (a gateway announcing again after a change of leader) | the destination is recorded; reports held for it are sent; `SessionAnnouncedAgain` raised |
| `SessionBound`, a new logon | the destination is recorded; reports held for it are discarded for the FIX gateway, which sends them itself from the WAL when the member asks, and sent for the binary gateway; with `reset_seq_nums`, the sequence state is forgotten; the leader answers with the numbers to resume from, adding the reports forwarded since the gateway last reported and an allowance of 64 when the previous session's gateway died without unbinding it |
| `SessionUnbound` | the destination is forgotten; the numbers the gateway reports are kept |
| `SessionSequenceUpdate` | the numbers the gateway reports are kept, never lowered |
| a report forwarded to the session | counted against the numbers last reported (`note_report_forwarded`) |
| a report dropped because the session is connected nowhere | the first since it was last bound is logged at Warning, the rest at Debug |

**The sequence state of a session** (`SessionSequenceState`) holds the outbound and inbound numbers last reported,
which outbound numbers held reports, the last report delivered, the last record in the WAL when the session bound
again (until the reports missed meanwhile have been sent), and whether the delivered figure may be behind.

**Which session placed each order** (`seq_no_to_session_`) is kept by its record's number from when the order is
written until a report ends it (filled, cancelled, rejected, expired, done for the day, or replaced).

### 6.13 Checking each command against the WAL (`CommandChecks`)

**Where it is held now.** The counts (`sent_again_already_logged_`, `sent_again_sequenced_`,
`commands_refused_reused_identifier_`, `commands_answered_from_wal_`, `commands_unchecked_without_identity_`),
`commands_checked_since_summary_`, `last_command_checked_at_` and `command_identity_buffer_`.

Each command the leader receives goes one of five ways
([one_check_against_sequencing_twice.md](../availability/one_check_against_sequencing_twice.md)):

| The command | Done |
|-------------|------|
| carries no comp id or no `ClOrdID` | sequenced, unchecked, and counted |
| is not in the WAL | sequenced; if it was marked as sent again, counted |
| is in the WAL, not marked as sent again | refused with `CommandRefused` on the connection it came by, nothing written, and counted |
| is in the WAL, marked as sent again, and the WAL holds a report answering it | that report forwarded to the member's session, marked as a possible repeat, and counted |
| is in the WAL, marked as sent again, and the WAL holds no report of it | passed over: the engine answers it by the ordinary path; counted |

A summary of the counts is logged a minute after the last command checked, or when this instance stops leading.

### 6.14 Requests to halt trading or void commands (`HaltAndVoidRequests`)

**Where it is held now.** Nothing of its own beyond what section 6.10 holds; it decides from those facts, from whether
an engine takes orders, and from what the WAL holds, and writes commands into the WAL through the thread.

| Request | Who acts | Done |
|---------|----------|------|
| an operator asks to halt (`OperatorHaltRequest`, halt) | the leader | a `TechnicalEvent` that halts, reason DeclaredByOperator, cancelling nothing |
| an operator asks to lift the halt | the leader | a `TechnicalEvent` that does not halt, reason LiftedByOperator, only if an engine takes orders and the WAL holds a report answering the last member's command before the halt; otherwise declined, saying why |
| a matching engine that cannot account for the book asks to halt (`HaltRequest`) | the leader, and only for the engine orders are routed to | a `TechnicalEvent` that halts, with the reason the engine gave |
| a matching engine that died applying a command asks for it to be voided (`VoidRequest`) | the leader | if it is voided already, the voids are sent; otherwise `VoidDeclined`, saying why, if another engine has announced that it leads, if a report in the WAL answers it or a later command, if the WAL does not hold it, or if it is not a member's new order or cancel; otherwise a `VoidCommands` naming it |
| no engine could be trading within the limit on automatic recovery (section 6.9) | the leader, on the engine absence timer | a `VoidCommands` naming every order held, a `CancelBook`, and a `TechnicalEvent` that halts, in that order; then the gateways are told |

Every operator request is answered and logged at Warning with the operator's name.

### 6.15 Serving those that follow the WAL (`ServingWalFollowers`)

**Where it is held now.** `wal_following_`, `wal_follower_service_`, `handed_over_publishers_`,
`handed_over_engines_`, `engine_servings_`, `damaged_records_reported_`, `damaged_record_standing_` with its gauge, and
`paused_for_hand_over_` (shared with section 6.8).

**A matching engine being served** (`EngineServing`) is one of three kinds, held now in three true-or-false members of
which the third is always the opposite of the second when the first is set:

| Kind | `answers_catch_up_request` | `asking_to_lead` | `follows_the_wal` | When handed over |
|------|---------------------------|------------------|-------------------|------------------|
| **sent the orders it lacks**, after this instance took the lead and asked its position | no | no | no | nothing more |
| **caught up to lead** | yes | yes | no | `WalFollowCaughtUp` sent; orders routed to it |
| **caught up to follow** | yes | no | yes | `WalFollowCaughtUp` sent; sent every record as it is committed from then on |

While an engine is being served, this instance sends it nothing itself: the reading thread sends it everything up to
the last order released. A request from an engine while this instance does not lead is declined, and the order
connection is noted as one whose engine follows. The follower sequencer's catch-up, and each publisher's, are served
the same way by the library's `WalFollowerService`.

A damaged record that stops a follower being caught up is said at Error the first time it stops each kind of follower,
and at Info when the same follower asks again, and is shown by a gauge holding its number until a follower is handed
over past it.

### 6.16 Reports kept while not leading (`KeptReportStore`, extended)

**Where it is held now.** `kept_reports_` (already a class), `last_granted_to_leader_`, `kept_reports_lost_reported_`
with its gauge, `kept_reports_discarded_at_`, `kept_reports_discarded_as_forwarded_`, and the samples of the leader's
forwarded position (`forwarded_samples_`, `forwarded_samples_head_`, `forwarded_samples_count_`,
`latest_record_written_ns_`).

While this instance does not lead, each report from the matching engine is kept. A kept report is no longer needed
when either:

- it arrived more than a lease period plus the drift allowance before the last time this instance granted its leader
  a lease (`kept_reports_needed_from`), because the leader forwarded everything it received in that time; or
- a record the leader wrote at least 100 milliseconds before the latest record this instance holds says the leader
  had forwarded it (`discard_kept_reports_the_leader_forwarded`, once a second on the acknowledgement watch timer).

On taking the lead, those the leader had forwarded are discarded, and the rest forwarded to their members, marked as
possible repeats. A report lost because the store was full is counted, and said when the loss happens, not for each
report after it.

## 7. How it is tested

- **After every step**, the whole test suite passes: `scripts/devsetup.sh --skip-db`, and then both full suites of
  `ha_test.py`, with the primary engine first and with `--secondary-engine-first`, each from cleared WALs.
- **Each part has unit tests** of the decisions it makes (decision 4), as `UnansweredCommandsTest` does: each move
  between its states, and what it asks the thread to do. Each test is made to fail on purpose before it is trusted.
- **The order's round trip is measured** before the first step that touches the order path and after the last, by the
  method in [latency_findings.md](../operations/latency_findings.md), as the plan requires of any phase that touches
  the order path. This needs the machine in measuring mode, which Andrew switches.

## 8. What it is expected to cost

- **On the order path**, a few more function calls for each order, some of them through an interface, which is a
  virtual call: a few nanoseconds against a round trip of about 125 microseconds. The measurement says whether that is
  so.
- **Memory**: none. The parts take over the members that hold the storage now.
- **Time**: sixteen steps (section 9), each followed by the developer loop and both full suites, about an hour and a
  half of running each.

## 9. Steps

Each step moves one part, adds its unit tests, and leaves the whole suite passing. The order shown is the matching
engine first (decision 2, option A).

1. **The states written down**, for both programs, as the code is now (section 6). Documents only.

   **Done** on 3 November 2026: sections 6.1 to 6.16. It found two defects, recorded and not fixed within this phase:
   [BUG-0225](../bug_list.md#bug_0225), a matching engine that becomes a follower before it has asked to follow the
   WAL, seen in scenario 24; and [BUG-0226](../bug_list.md#bug_0226), a report held for a gateway with no connection
   for reports counting as forwarded, found by reading the code. It also found two things for the design of the parts
   to settle: the two definitions of the time without an engine (section 6.9), and the work on stopping leading split
   across two branches that always happen together (section 6.7).
2. `EngineReportSending`.

   **As built.** `EngineReportSending` (`applications/matching_engine/EngineReportSending.hpp`) holds the two
   connections for reports, the reports held and the numbering, and makes the decisions of section 6.2. The thread
   encodes each report, hands it over with whether the instance is holding until it knows whether it serves, and times
   the acknowledgement of a new order when the part says it was sent. It implements
   `EngineReportSendingLinksInterface`, which sends a report on a connection and raises the test events `ReportsHeld`,
   `ReportsReleased` and `ReportsDiscarded`. The held report is `HeldEngineReport`, in a header of its own, because the
   reports kept while following (step 3) are the same. The one overflow flag became two, one for each kind of holding,
   so that reaching both limits says both. The part's Quill log lines begin with its name. `EngineReportSendingTest`
   has twelve tests, and six faults put in on purpose were each caught. Both full suites pass: 99 of 99 with the
   primary engine first, and 38 of 38 with the secondary engine first.
3. `FollowerReportComparison`.

   **As built.** `FollowerReportComparison` (`applications/matching_engine/FollowerReportComparison.hpp`) holds the
   reports kept while following, the comparison with the leader's, the count of differences, and whether the instance
   has refused to lead; every cause of refusing goes through it, including a void learned too late, which the thread
   reports. The thread encodes each report to keep, unless there is no room, and implements
   `FollowerReportComparisonLinksInterface`, which stops the instance asking for a lease, marks its region unusable,
   counts a difference, and raises the test events `FollowerCannotLead` and `KeptReportsSent`. On taking the lead,
   the kept reports the WAL lacks are handed to `EngineReportSending`. The part's Quill log lines begin with its name.
   `FollowerReportComparisonTest` has eleven tests, and seven faults put in on purpose were each caught. Both full
   suites pass: 99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
4. `EngineCatchUp`.

   **As built.** `EngineCatchUp` (`applications/matching_engine/EngineCatchUp.hpp`) holds the request numbers, the
   connection the latest answer started on and its tally, the counts of a catch-up, the engine's position, the record
   expected next while following, and the decision about an unusable region with its request to halt. Whether the
   engine is catching up, and for what purpose, stays in the role (step 5): the thread passes it in, with whether a
   catch-up may be asked for and the last command applied (`RequestContext`). The thread keeps the two timers and the
   sequencer connections for orders, which the part reads, and implements `EngineCatchUpLinksInterface`, which sends a
   request, starts and stops the timers, asks for a halt, and raises the test event `RegionRebuilt`. The part's Quill
   log lines begin with its name. `EngineCatchUpTest` has seventeen tests, and nine faults put in on purpose were each
   caught. Both full suites pass: 99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
5. `EngineRole`, with its one state.

   **As built.** `EngineRole` (`applications/matching_engine/EngineRole.hpp`) holds what the instance is doing as one
   state, `EngineRole::Situation`: starting, catching up to become current, current, catching up to lead, leading,
   catching up to follow again, or following. The role as the Quill log and the test name it, `MeRole`, is worked out
   from it. Four of the seven members of section 6.1 are folded into it: the role, the purpose of the catch-up, the
   situation to go back to after following again, and the peer noted as leading. Two stay beside it as facts, because
   each is acted on in more than one situation: whether the latest catch-up request asked to follow, which decides
   whether a catch-up's reports are kept and whether a follower follows the WAL at all; and whether any catch-up has
   completed, for the test's report of where the instance stands. The seventh, the refusal to lead, is held by
   `FollowerReportComparison` (step 3). The situation of BUG-0225, a follower that never asked to follow the WAL, is
   kept as it is and named (`follows_without_following_the_wal`). The lease agent, the epoch and its store stay in the
   thread for now: their handling is short and mostly the library's `PairLeaseAgent`, and step 6 decides where they
   belong. `EngineRoleTest` has eleven tests, and six faults put in on purpose were each caught; one of them only by a
   test of a combination no sequence of events reaches, which pins the check that a leader never keeps its reports.
   Both full suites pass: 99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
6. `DiedApplyingCheck` and `PeerSettingsCheck`, and what is left of `MatchingEngineThread` reduced to its connections,
   timers and the handing on of events.

   **As built.** `DiedApplyingCheck` (`applications/matching_engine/DiedApplyingCheck.hpp`) holds the question of
   section 6.5, already one state, with the command and the outcome; the thread implements
   `DiedApplyingCheckLinksInterface`, which asks the peer, starts and stops the two timers, sends `VoidRequest`, and,
   when the question is settled, raises `DiedApplying` and asks for a catch-up that was waiting. `PeerSettingsCheck`
   (`applications/matching_engine/PeerSettingsCheck.hpp`) holds what the peer last said it is configured with, compares
   it, and bars a peer whose settings differ; the thread implements `PeerSettingsCheckLinksInterface`, which reaches
   the lease agent and raises `SettingsDifferFromPeer`. It is made at the end of the constructor, once the open-order
   region's capacity is known. `DiedApplyingCheckTest` has ten tests and `PeerSettingsCheckTest` seven, and ten faults
   put in on purpose were each caught.

   **The lease agent and the epoch stay in the thread.** `PairLeaseAgent` is already the library's own part for
   deciding which instance leads; the thread only connects it to the peer and the arbiters, keeps its epoch on disk,
   and passes on what it decides to `EngineRole`. Moving it into `EngineRole` would move that connecting, not a job.

   **What is left in `MatchingEngineThread`**: its connections and timers; the metric handles and the test event
   channel; the lease agent, the epoch and their stores; the parts, each with the links class that connects it; and
   a few small things. Two of those last only while one command is handled: the gateway's time for the order being
   applied, and whether it came as a follower of the WAL. One is a count for the Quill log. Three are kept only so
   that a test that connects late is told where the instance stands: why the region was unusable, why trading is
   halted, and the position the instance resumed leading from. And two serve test hooks. The thread's source and
   header together went from 3,193 lines before this phase to 2,467. Both full suites pass: 99 of 99 with the primary
   engine first, and 38 of 38 with the secondary engine first.

   **The round trip.** The engine after step 6 is measured against the engine built from the code before step 2, the
   first step to touch the order path, in one deployment that differs only in the engine binary. Steps 2 to 6 cost
   nothing measurable: the median was 106.7 to 107.1 µs and the 90th percentile 113.7 to 114.5 µs with either engine
   ([latency_findings.md](../operations/latency_findings.md), "Dividing the matching engine into parts costs nothing
   measurable"). The sequencer's steps are measured the same way, against the code after step 6.
7. `VenueStateFromTheWal`.

   **As built.** `VenueStateFromTheWal` (`applications/sequencer/VenueStateFromTheWal.hpp`) holds whether trading is
   halted, the voids, the void written as leader and not yet sent to the engines, the trading date, and the highest
   command a report answers. Every record is noted in one way, `note_record`, told how the instance came to hold it:
   read when the WAL was opened, written as follower, or written by this instance as leader. The differences of
   section 6.10 are made in that one function, each with its reason. As leader, every record is noted as
   `append_envelope_to_wal` writes it, so the report's highest command answered and the void of a command of the
   sequencer's own are noted there, as the halt already was, rather than by the callers after it; finding a void in a
   member's order costs one comparison of its message type. The leader reads the first record's time from the WAL
   only while the trading date is not known. The part's Quill log lines begin with its name.
   `VenueStateFromTheWalTest` has eleven tests, and eight faults put in on purpose were each caught. Both full suites
   pass: 99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
8. `MemberSessions`.

   **As built.** `MemberSessions` (`applications/sequencer/MemberSessions.hpp`) holds where each member's session is
   bound, what the venue remembers of each session's numbering between connections, which session placed each order
   still open, and the reports dropped because a session was connected nowhere. It makes the decisions of section 6.12:
   recording a binding, working out the numbers a logon resumes from, keeping what a gateway reports at unbind and in
   updates, never lowering them, and saying once which reports a member missed while away. The thread decodes each
   message, holds and sends reports (section 6.11, step 9), sends `SessionBoundAck`, and reads the WAL for the reports a
   member missed; it implements `MemberSessionsLinksInterface`, which reads the test's settings, sets the gauge of
   reports dropped, and raises `ReportDroppedSessionNotConnected`, `SessionAnnouncedAgain` and `SessionUnboundRecorded`.
   A logon is two calls, `bind` and then `resume`, with the reports held for the session dealt with between them, so
   that a gateway is sent them before its `SessionBoundAck`, as before. The part's Quill log lines begin with its name.
   `MemberSessionsTest` has thirteen tests, and nine faults put in on purpose were each caught. Both full suites pass:
   99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
9. `ReportForwarding`.

   **As built.** `ReportForwarding` (`applications/sequencer/ReportForwarding.hpp`) holds the reports waiting for their
   record to be committed, the reports held for unannounced sessions and for gateways with no connection for reports,
   which gateways have announced their sessions, the gateway connections, and how far reports have been forwarded. It
   makes the decisions of section 6.11: sending a report to its gateway, holding it, waiting for its record, sending
   the waiting reports once their records are committed, and discarding what is held when this instance stops leading.
   The first half of `forward_report_from_engine` stays in the thread, because it writes the report's own record into
   the WAL: it works out whose report it is, from `MemberSessions`, writes the record, and then hands the report to the
   part, to send now or to wait. The thread implements `ReportForwardingLinksInterface`, which sends a report, times the
   acknowledgement of a new order, reaches `MemberSessions`, and raises the test events of holding and releasing.
   BUG-0226 is kept as it is, and a test pins it. The part's Quill log lines begin with its name.
   `ReportForwardingTest` has fourteen tests, and nine faults put in on purpose were each caught. Both full suites pass:
   99 of 99 with the primary engine first, and 38 of 38 with the secondary engine first.
10. `KeptReportStore`, extended.
11. `OrderRelease`.
12. `EnginePresence`.
13. `CommandChecks`.
14. `HaltAndVoidRequests`.
15. `ServingWalFollowers`, and what is left of `SequencerThread` reduced to its connections, timers and the handing on
    of events.
16. Replay mode, as decision 3 says; the round trip measured again; the sequencer and matching engine documents brought
    up to date.

## 10. Decisions

Decisions 1 and 4 are separate yes-or-no questions. Decisions 2 and 3 each ask you to choose one of the options given.

### Decision 1. Is each part made as section 2 describes: a class owning one job's state, told of events by the thread, asking for what it cannot do itself through an interface the thread implements, and run on the same thread? (Yes or no.)

This is the pattern of `MatchingEngineStateMachine` and `UnansweredCommands`. The alternative, splitting each thread
class's member functions across several source files without moving the state, makes the files shorter but leaves
every function able to change every member, so it solves none of the three problems in section 1.

**Recommendation: yes.**

**Decided** on 3 November 2026: yes.

### Decision 2. Which program is divided first? (Choose one.)

- **A. The matching engine first.** It is the smaller, so the pattern is settled on less code. And its role, held in
  seven members, is the clearest case of a situation spread across several true-or-false values, which is where
  defects have come from.
- **B. The sequencer first.** It is the larger, and holds the venue's own business, so more is gained sooner.

**Recommendation: A.**

**Decided** on 3 November 2026: A.

### Decision 3. What happens to the sequencer's replay mode? (Choose one.)

`sequencer <logfile> <config.toml> --replay` reads the whole WAL and sends the matching engine every order and cancel
in it, with their original times, so that the engine produces the same reports again
([sequencer.md](sequencer.md), "Replay mode"). Nothing tests it: no script and no scenario starts a sequencer with
`--replay`. A matching engine already rebuilds its book from the WAL by being caught up by the leading sequencer.

- **A. Remove it.** A mode that nothing tests, carried through sixteen steps of restructuring, would be broken without
  anyone seeing it. Removing it removes `replay_buffer_`, the two flags beside it, and the special cases for it in
  starting up.
- **B. Keep it, as its own part, with a scenario that runs it**, so that the restructuring is checked against it.

**Recommendation: A**, unless you use it. It is a removal of a feature, which is why it is a question.

**Decided** on 3 November 2026: A.

### Decision 4. Does each part have unit tests of its own, besides the whole suite passing? (Yes or no.)

The whole suite shows that behaviour has not changed. Unit tests of a part show that each of its moves does what its
table says, including moves the scenarios reach rarely or never, and they are what make a part cheap to change later.
They add to each step the time to write them.

**Recommendation: yes.**

**Decided** on 3 November 2026: yes.
