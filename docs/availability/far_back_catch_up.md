# A matching engine catching up from far back {#far_back_catch_up}

This design covers what a matching engine catching up from far back in the WAL must do so that the venue keeps
working while it does, and the limit on how long the venue recovers on its own:

- It sends no report the WAL already holds, so a catch-up does not flood the leading sequencer with repeated reports
  ([BUG-0176](../bug_list.md#bug_0176), section 2).
- An engine that asks again to be caught up applies nothing twice ([BUG-0212](../bug_list.md#bug_0212), section 3).
- Every catch-up of an engine or a publisher is read on the sequencer's reading thread and paced by the connection,
  with high availability on or off ([BUG-0172](../bug_list.md#bug_0172), section 4).
- Past a stated time, the venue does not recover on its own while no engine can trade, but halts and leaves the
  decision to a person (section 5).

## 1. Why a catch-up leaves reports out

A matching engine that starts, or that is about to lead, asks the leading sequencer for every command after the last
one its book reflects (`WalFollowRequest`). The leading sequencer's reading thread sends them, as fast as the engine's
connection takes them, and then the sequencer sends `WalFollowCaughtUp`, saying how many it sent and the last one
([following_the_wal.md](following_the_wal.md)). The engine applies each command as it arrives.

Applying a command produces execution reports. A report the engine sends goes to the sequencers marked as a possible
repeat, because an earlier engine may already have sent the same report to the member (R-0122), and the leading
sequencer gives it a record of its own in the WAL, replicates it to the follower sequencer and forwards it to the
member's gateway, all on its own thread. An engine that sent every report of a catch-up from the start of the day
would send the leading sequencer about one report for every command of the day so far. Measured on 18 October 2026,
a catch-up of 1.37 million records sent that way kept the leading sequencer's thread busy without a pause for over 18
seconds, its lease ran out without its noticing, and the member's connection was closed because 13 MB of repeated
reports were waiting for it (BUG-0176).

Almost all of those reports are already in the WAL. The engine that led before sent them, and the sequencers wrote
them, replicated them and delivered them. Sending them again tells the member nothing it has not been told, so the
engine leaves them out (section 2).

## 2. Reports the WAL already holds

### 2.1 The rule

**When the leading sequencer starts serving an engine's catch-up, it tells the engine the last command up to which the
WAL holds every report. The engine sends no report for that command or any before it. It still applies every one of
them to its book, exactly as now.** Reports for the commands after that point are sent as now, each marked as a
possible repeat.

The point is called below "the last fully reported command".

### 2.2 How the sequencer knows the last fully reported command

Every report record the sequencer writes carries the number of the command it answers (`answers_seq_no` on the
`WalRecord` envelope), or none for a report no command produced. Let A be the highest command number that any
committed report record in the WAL answers. Then the WAL holds every report of every command before A, so the last
fully reported command is A - 1. Command A itself may have more reports still to come: an order that trades produces
a report for itself and one for each order it trades against, and the engine may have stopped part way through them.

Why the WAL holds every report of every command before A:

1. **One engine sends its reports in the order of the commands.** The engine applies commands one at a time, in the
   order of their numbers, on one thread, and sends every report of a command before it applies the next. It sends
   them on one connection to each sequencer, and a connection delivers in order. So when the report for A arrives
   from an engine, every report that engine produced for an earlier command arrived before it.
2. **The leading sequencer writes the reports in the order they arrive**, on its own thread, each as a record of its
   own. Records are committed in the order of their numbers, so when the record of the report for A is committed,
   every report record before it is committed too.
3. **A change of sequencer leader keeps the order.** A follower sequencer keeps a copy of every report the engine
   sends it (`KeptReportStore`), and on taking the lead it writes the ones its predecessor had not forwarded before it
   handles any new report. It discards only the copies that the records it received say the leader had already
   forwarded, and those records come after the reports in the WAL, so those reports are in its WAL already.
4. **A different engine does not break it.** Two engines' reports can both be in the WAL: the reports a catching-up
   engine sends, and the leading engine's, or an engine that died and the one that took over. The argument in 1 and 2
   is about each engine separately: the report for A came from one engine, and that engine's reports of every earlier
   command are in the WAL before it. An engine applies every command in order from the position it starts from, and
   with this design it sends reports only for commands after the last fully reported command, so its reports for the
   commands before A that it did not send are in the WAL by the same rule applied earlier.

A lower value than A - 1 is always safe: it only makes the engine send more reports again, each marked as a possible
repeat. So whenever the sequencer cannot be sure, it sends a lower value, and 0 means "send every report", which is
what every engine does now.

**Committed only.** A record the leader has written but the follower has not acknowledged may be lost in a change of
leader, so only committed report records count.

**Where it is worked out.** The reading thread works it out when it starts to answer an engine's request, before it
sends `WalFollowStarted`. It reads the newest segment of the WAL, up to the last committed record, and takes the
highest command that any report record in it answers; if that segment holds no report record, it reads the segment
before, and so on, up to 64 segments (256 MB at the configured segment size of 4 MB), after which it gives 0. A
segment is read in a few milliseconds, on the reading thread, not on the sequencer's thread. Working it out from the
WAL as it stands, rather than keeping it as records are written, means it is right on a sequencer that has just
started, on one that has just taken the lead, and on one whose WAL a repair has just cut back, with nothing to keep in
step. A report in an older segment could answer a higher command than any in the newest only if every report in the
newer segment were a kept report written again on a change of leader, and even then the lower value is safe.

The library reads the WAL; the sequencer supplies a function, run on the reading thread for each record read, that
says which command a report record answers and passes over every other record, as it supplies the functions that turn
records into followers' messages (section 3.5 of [following_the_wal.md](following_the_wal.md)).

### 2.3 How the engine is told

A new message in the library's own message file, sent by the leading sequencer on the engine's connection before the
first record of a catch-up:

```
message WalFollowStarted (id=155, version=1)
    i64 request_number      # the request this answers, echoed from WalFollowRequest
    i64 after_seq_no        # the position the request named, echoed
    i64 serving_through_seq_no # the last record this instance may send the follower now; more may follow
    i64 application_value   # a value of the application's own, which the library carries and never reads
end
```

The library sends it, because it is the library that starts serving a follower; the application supplies the value
when it asks the library to serve a follower, as it already supplies the position up to which each follower is served
(section 3.3 of [following_the_wal.md](following_the_wal.md)). For the matching engine the sequencer supplies the last
fully reported command. A publisher is given 0, and ignores it.

`WalFollowRequest` gains `request_number`, which the follower counts up from 1 with each request it sends, and
`WalFollowCaughtUp` gains it too, echoed. Section 3 says why.

The reading thread sends it, as the first thing it does when told to serve the follower, because it is the thread that
sends the records. It acts on what it is told in the order it is told, so every record of an earlier answer on the same
connection is queued before `WalFollowStarted`, and every record of this answer after it. Sent by the sequencer's own
thread instead, it could overtake records of an earlier answer that the reading thread had not yet stopped sending. A
follower that holds nothing, and asks to be placed at the newest record, is answered through the reading thread too.

### 2.4 What the engine does

The engine keeps the last fully reported command from the `WalFollowStarted` that answers its latest request. While
it catches up, for a command at or before it, it applies the command and sends no report. For a command after it, it
sends every report, marked as a possible repeat, as now. This is the same whether it is catching up in order to lead
or not: an engine cannot tell whether the other engine leads, and at a start of the whole venue neither does, while
the WAL can hold orders taken while no engine was running, which nothing has reported. An engine that left those out
because it was not catching up in order to lead, and then took the lead, would find nothing more to catch up, and
their members would never be told.

Everything else is as now: the book, the open-order region, the follower's copy, the halt state and the count of
records checked against `WalFollowCaughtUp` (R-0101). Only the reports are left out. `CatchUpEvent` to a test gains
the number of reports left out.

### 2.5 Why every member still receives every report

Principle V8 says every report the matching engine produces reaches the member. A report left out by this rule is one
already in the WAL, committed, and the sequencers deliver what the WAL holds by four routes, none of which needs the
engine to send it again:

- the leader forwarded it when it was written, or it will once the record of the command it answers is acknowledged;
- a new sequencer leader forwards every report it kept that its predecessor had not said it forwarded (section 2.2,
  point 3);
- a session that was connected nowhere when the report was written is sent it when it binds again
  (`UndeliveredReportsRequest`);
- a member that asks for a resend is sent it from the WAL (`SessionReplayRequest`).

What the left-out reports would have said is what the WAL's reports say: the engine is deterministic, and phase 7
checks it on every scenario by running both engines on the scenario's WAL and comparing their reports with those it
holds (`run_engines_on_the_wal` in `ha_test.py`). The book the engine builds is the same either way.

### 2.6 The check that runs both engines on a WAL

That check runs each engine from an empty book on a copy of a scenario's WAL, with one sequencer and high availability
off, and requires the reports each engine sends to be the ones a replay produces. Under this rule the engines would
send none, because the copy already holds every report. So the sequencer gains a setting, `[catch_up]
send_every_report`, which makes it give 0 as the last fully reported command. It is false in every environment and in
every deployed configuration, and the check sets it true in the copy of the configuration it starts the sequencer
with. A scenario checks that with the setting false an engine catching up from the first record sends no report for
a command the WAL already holds a report for.

## 3. Asking again (BUG-0212)

An engine whose catch-up was cut short, by a lost connection or by a count that did not match, asks again. It must
then never apply a command twice, and must never take the records of an old request as live commands.

- **The engine asks from the last command it has applied**, however it applied it, not from where its last complete
  catch-up ended. A connection delivers in order, so what arrived on a connection that was lost is a run of records
  with nothing missing in the middle, and the records after the last one applied are exactly what is still needed.
  The count check (R-0101) is unchanged: it counts the records of the latest request only.
- **A command at or before the last command applied is passed over without a report during a catch-up.** The state
  machine does this for every command applied, live or by a catch-up, complete or not (`applied_before`). A command
  that arrives live with a number at or below the last applied is still refused as out of sequence, with a report to
  the member (`sequence_goes_backwards`): live, it means the engine's book and the leading sequencer's WAL disagree,
  which the member must be told of.
- **The records of an old request are ignored.** Each request carries a number, which `WalFollowStarted` and
  `WalFollowCaughtUp` echo. The engine sends the same number on every sequencer connection. While it catches up, it
  applies only records that arrive on the connection on which the `WalFollowStarted` for its latest request arrived,
  and after it; any other record answers an earlier request, and is ignored without a report. It ignores a
  `WalFollowCaughtUp` for an earlier request. A connection carries one answer at a time: the library stops sending
  the earlier answer on a connection before it sends the start of a later one (section 2.3). The library's follower
  side, `WalFollowerClient`, which the publishers use, does the same.
- The comment in `MatchingEngineThread::handle_caught_up` that says applying the same records twice "is absorbed" is
  replaced by one that says what is done.

## 4. A reading thread with high availability off (BUG-0172)

With high availability off the sequencer has no reading thread, and catches up an engine or a publisher on its own
thread, reading and sending every record in one go (`engine_catch_up_from_wal`, `WalHandedOverFollowers::send_to`,
`SequencerThread::send_logged_orders`). It gets a reading thread in every configuration, and every catch-up goes
through the library, as it does with high availability on. With high availability off the reading thread serves an
engine up to the last order released and a publisher up to the last record committed, which with no follower is every
record as it is written. `engine_catch_up_from_wal` goes, and so does the path that caught a publisher up on the
sequencer's thread; `send_logged_orders` stays only to count the orders deferred while no engine is connected, which
sends nothing. The build's check that no new code reads the
WAL on the sequencer's thread (step 5 of phase 5) covers them.

## 5. The limit on automatic recovery

### 5.1 What is wanted

Agreed on 20 October 2026, and recorded in BUG-0176: while no matching engine can trade, the venue cannot answer
members, and a catch-up from far back takes seconds or more, so members find their orders unanswered and telephone to
ask whether the venue is working. The venue should state the longest recovery it does by itself, as it states the
longest absence of an engine (R-0117). When an engine that is the only one able to trade could not be current within
that time, the venue halts trading, every order is refused with the reason, and a person decides when trading resumes.
A follower catching up while the other engine leads is not limited, since members do not see it, and with section 2
it sends the leading sequencer only the reports of commands the WAL does not yet hold every report of: those taken
while it was catching up.

Phases 7 and 8 keep the follower engine current, so a catch-up from far back happens only when both engines have lost
their state, and never from further back than the start of the trading day.

### 5.2 The design

**The leading sequencer measures it**, alongside the absence rule (`EngineAbsenceRule`), once a second, for the same
reason: the rule is about the venue, and the leading sequencer is the process that knows whether any engine can trade,
because it routes orders only to an engine that leads (`EngineOrderRouting`). It also knows how far the catch-up has
got, because its reading thread is sending it: about every 100 milliseconds while it serves a follower, the reading
thread tells the sequencer's thread the last record it has sent and the last it is serving up to (a `Progress`
report, passed to `WalFollowerEventsInterface::follower_progress`). Sending is paced by what the engine's connection
takes, so the rate at which that position moves is the rate at which the engine applies records, to within what the
connection's buffers hold.

**What it compares.** While no engine leads and an engine is catching up in order to lead, once the catch-up has run
for one second the sequencer works out how long the rest will take: the records still to send, up to the last record
released, divided by the rate over the last second. If the time no engine has led so far, plus that, exceeds
`[recovery] automatic_recovery_limit_seconds`, the venue will not be trading within the limit. It also acts if the
time no engine has led exceeds the limit with no estimate, for instance because no engine has asked.

**What it does**, once for each absence:

1. It tells every gateway the venue is not accepting orders (`OrderAcceptance` with `accepting` false), which the
   gateways already act on: they refuse new orders and cancels at once, with a reason. `OrderAcceptance` carries a
   reason, `OrderAcceptanceReason`, so that the member is told the venue is recovering and halted, not only that no
   matching engine is available. The FIX and binary gateways refuse a new order with "Venue is halted: no matching
   engine could be trading within the time the venue recovers by itself", and a cancel with "Venue is halted while it
   recovers and cannot process cancels now. The order is unchanged".
2. It writes a `TechnicalEvent` that halts trading, with a new reason `RecoveryTooLong`, as the sequencer already does
   for `CannotAccountForBook`. Every engine applies it at its position, so when the engine has caught up it does not
   resume trading by itself, and an engine that replays the WAL later meets the halt at the same place.
3. Nothing is cancelled. The absence rule still cancels every open order if no engine can trade for longer than its
   own limit (300 seconds in production); the recovery limit is shorter, and halts only.

**This is built.** `RecoveryLimitRule` decides and `SequencerThread::check_engine_absence` acts, beside the absence
rule. The rule estimates the time still to come from how far the reading thread moved over at least the last second,
and acts once the time so far and that estimate together pass the limit, or the time so far alone passes it. The
gateways are told the venue is accepting again once an engine is routed to: the engine applied the halt in its
catch-up, so it refuses new orders as halted, and each refusal comes from it.

Orders that reached the sequencer before the halt are before it in the WAL, and are applied when the engine reaches
them, so each is answered then. Trading resumes when a person lifts the halt, through the operator's means of doing
so ([BUG-0065](../bug_list.md#bug_0065)), which is not built; until it is, a halt lasts until the end of the day, as
the `CannotAccountForBook` halt does now.

**It is the same rule as [BUG-0197](../bug_list.md#bug_0197).** That entry, agreed on 24 October 2026, asks that
orders be held only for as long as a normal failover takes, and that past that, if no engine leads, the venue halts
and says so at once. A far-back catch-up is one way for no engine to lead. So BUG-0197's halt and this limit are one
measurement, one limit and one action, and are built once; BUG-0197 adds the rejection of the orders held before the
halt, which needs rejecting a command through the WAL and is designed in phase 8. The limit's value then decides how
long a normal failover may take, and should be set from a measurement of one.

### 5.3 What the limit is set to

The longest a normal failover takes, with a margin: the lease period, 3 seconds, plus the catch-up of an engine that
was current, which is milliseconds. The limit is 10 seconds.

A catch-up from far back, with the reports the WAL holds left out, was measured on 27 October 2026 with
`engine_catchup.sh`: after a fill of 300,000 orders and 270,145 cancels, both engines were stopped and one was started
alone with its region at record 0. It applied 605,923 commands, the 1,209,752 records of the WAL, in 2.50 seconds,
which is about 240,000 commands a second, and sent 3,419 reports again, nearly all for orders sent while no engine was
running, whose reports the WAL did not yet hold. The leading sequencer led throughout. At that rate a 10-second limit,
less the 3 seconds of a lease, leaves room for a catch-up of about 1.7 million commands. A busy day of 50 million orders and as many cancels
over eight and a half hours is about 200,000 commands a minute, so that is a catch-up from about eight minutes back;
a catch-up from the start of such a day, at about 100 million commands, takes about 7 minutes and is halted. The limit stays at 10 seconds until the business states another figure (decision 3).

## 6. Steps, and how each is known to be right

1. **Asking again (section 3).** A unit test of the state machine applies a run of commands, then the same run again
   as a catch-up, and requires no report the second time and the same book; it fails today. Scenario 80's check that
   runs the engines on its WAL passes once step 2 is also done.
2. **The reading thread with high availability off (section 4).** Scenario 80's check that runs the engines on its
   WAL catches up an engine from the first of 550,000 records with one sequencer and high availability off. It fails
   today, and must pass, with the copy holding exactly the expected records. A check is added that the sequencer's
   connection to the engine was not closed during it.
3. **Reports the WAL already holds (section 2).** A unit test of the class that keeps the last fully reported command,
   covering a report with no command, reports from two engines interleaved, a record not yet committed, and the
   value read at start-up. A scenario catches up an engine from the first record while the venue has no other engine,
   after a fill of 200,000 orders, and requires the leading sequencer to keep its lease throughout, the engine to send
   no report for a command the WAL already holds every report of, and every report the WAL holds to have reached the
   member. Made to fail by running it on the engine before the change, where it fails on the reports and, at that
   size, on the lease. The catch-up rate is measured, with the reports left out, at about the 1.37 million records
   of 18 October 2026, and stated in section 5.3.

   **Built.** Scenario 80 catches up an engine from the first record of its WAL and requires it to leave out more
   than a hundred times as many reports as it sends, and the leading sequencer to lead throughout. On 27 October
   2026 the engine left out 199,999 reports and sent 1, and the catch-up took 0.81 seconds, against 2.86 seconds
   when every report is sent. With `send_every_report` set it fails, because the engine leaves out no report. Both full suites
   passed on 27 October 2026: 84 of 84 with the primary engine leading first and 27 of 27 with the secondary first.
4. **The limit (section 5).** A unit test of the rule, as for `EngineAbsenceRule`. A scenario stops both engines,
   restarts one with its region at record 0 after enough orders that its catch-up is longer than a limit set short for
   the test, and requires the gateways to refuse orders with the reason, a halt in the WAL with reason
   `RecoveryTooLong`, and the engine, once caught up, to refuse new orders as halted. A second scenario with a catch-up
   shorter than the limit requires no halt.

   **Built.** `RecoveryLimitRuleTest` has eight tests; the one that requires a catch-up that will not finish in time
   to be halted a second in fails when the estimate is ignored. Scenario 85 keeps the engine down for 8 seconds with a
   limit of 2, and requires the FIX gateway to report that it was told the venue is halted for recovery, the
   restarted engine to halt with reason `RecoveryTooLong` and cancel nothing, and an order sent afterwards to be
   refused; with a limit of 30 it fails, because the gateway is never told. Scenario 86 kills the leading engine with
   the deployed limit of 10 seconds, and requires the failover to finish with no halt; with a limit of 1 second it
   fails, because the venue halts and the orders sent after the failover are refused. Both full suites passed on 27
   October 2026: 86 of 86 with the primary engine leading first and 28 of 28 with the secondary first.

Each step ends with both full suites from fresh WALs, as phase 7's steps have.

## 7. Decisions

### Decision 1. Does the leading sequencer measure the limit, rather than the engine? (Yes or no.)

The note of 20 October 2026 says the engine compares how far behind it is with how fast it applies records. Section 5.2
puts the measurement in the leading sequencer instead, beside the absence rule, because the rule is about the venue,
the sequencer is the process that knows whether any engine leads, and it can see the catch-up's progress through its
own reading thread without any new message from the engine. An engine measuring it would have to ask the sequencer
for the halt in any case, since only the leader writes commands.

**Decided** on 27 October 2026: yes.

### Decision 2. Are the limit and BUG-0197's halt built as one rule? (Yes or no.)

Section 5.2: both say that when no engine has led for longer than a stated time, the venue halts and says so at once.
Built once, there is one measurement and one limit. Built separately, the venue would have two limits on the same
situation, and which fired first would depend on their values.

**Decided** on 27 October 2026: yes. The halt and the refusal through `OrderAcceptance` are built in phase 7 with this work; the rejection
of the orders held before the halt stays in phase 8 with the rest of BUG-0197.

### Decision 3. What is the limit? (Choose one, or give a figure.)

- **A. 10 seconds.** Three lease periods: a normal failover always fits, and a member waits at most 10 seconds to be
  told the venue is halted.
- **B. 30 seconds.** Room for a catch-up of a few hundred thousand records to finish by itself, if step 3 measures
  them that fast.
- **C. Set after step 3's measurement**, and built with a value of 10 seconds until then.

**Decided** on 27 October 2026: C. The figure is the business's, and how much of a day's catch-up fits inside it is not yet
measured. The limit is built with a value of 10 seconds, and set once step 3 has measured the catch-up rate.
