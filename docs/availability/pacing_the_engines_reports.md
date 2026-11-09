# Pacing the matching engine's reports {#pacing_the_engines_reports}

**Status: designed and agreed, not built.** This is the design for [BUG-0234](../bug_list.md#bug_0234).
Section 6 records the three decisions it needed, each agreed with Andrew on 9 November 2026.

## 1. The problem

A matching engine sends an execution report to the sequencers for every command it applies, on its
connections for reports. The framework limits how much may wait to be written on one connection:
65,536 sends, `ReactorConfiguration::connection_waiting_sends_maximum`. A connection that reaches the
limit is taken to belong to a peer that has stopped reading, so the framework closes it and connects
again two seconds later.

In normal running that limit is never approached, because the engine applies commands only as fast as
the sequencer sends them, and the sequencer reads the reports as fast as it sends the commands. A
catch-up is different. The sequencer's reading thread sends the engine every command it lacks as fast
as the connection takes them, and the engine applies them far faster than one command at a time from
members. In the check that runs an engine on a copy of scenario 80's WAL, an engine applied 200,000
orders in 0.44 seconds, the queue on its connection for reports reached 65,536 sends, and the framework
closed the connection. The engine carried on sending on the closed connection, and the reactor dropped
each of those reports. 112,629 of the 200,000 reports never reached the WAL, and nothing said which.

## 2. What already exists

Each of the three pieces this design needs is already in the framework and already in use.

- **`ApplicationThread::pause_reading` and `resume_reading`.** These stop the reactor reading from a
  connection, and start it again. While reading is paused, the kernel's receive buffer fills and TCP
  stops the peer sending, so nothing is lost or refused. The sequencer uses them in `OrderRelease`, and
  the matching engine uses them to hold a sequencer's connection in a test setting.
- **`ApplicationThread::request_writable_notification`.** This asks for one call of
  `on_connection_writable` when the connection has nothing waiting to be written: no send part-written
  and nothing in its queue of waiting sends. So the call means that every report sent so far has been
  handed to the kernel. The gateways use it to send their unanswered commands again 256 at a time
  (`PacedResend`).
- **The sequencer's reading thread already sends at the pace the connection takes.** It sends 256
  records, asks to be told when the connection can take more, and sends the next 256 only then
  (`WalReadingThread`). So if the engine stops reading its connection from the sequencer, the
  reading thread stops sending without anything more being built on the sequencer's side.

## 3. The design

### 3.1 The rule

The engine counts the reports it has sent on its connection for reports to the leading sequencer and
not yet seen written. When the count reaches a limit, 4,096 reports, the engine:

1. pauses reading on its connections from the sequencers, on which commands arrive;
2. asks to be notified when the connection for reports has nothing waiting to be written.

When the notification arrives, every report sent has been handed to the kernel. The engine sets the
count to zero and resumes reading. So the queue of waiting sends on the connection for reports never
holds much more than 4,096 reports, against a limit of 65,536, and the catch-up runs at the speed the
sequencer reads the reports.

### 3.2 How the wait reaches the sequencer

While reading is paused, commands the sequencer sends stay in the engine's kernel receive buffer.
When that buffer is full, TCP stops the sequencer's reading thread sending, and its next request to
be told the connection can take more is not answered until the engine reads again. Nothing is
queued in the engine's own memory, and nothing new is built in the sequencer.

**Commands already read when the pause takes effect.** A pause is a request to the reactor, and the
reactor may already have read some commands into the engine's queue by the time it acts on it. The
framework's own description of `pause_reading` warns of this. The engine applies those commands
normally, and their reports are sent. They are what the gap between 4,096 and 65,536 is for: the
reactor reads from a socket in pieces of a bounded size, so it cannot pass the engine anywhere near
61,000 commands between the request and the pause. The tests in section 5 check the largest number
actually sent while paused.

### 3.3 When it applies

The rule applies at all times, not only during a catch-up. In normal running the count never reaches
4,096, because the engine's output follows the rate at which members send orders, so the rule never
pauses anything and costs one counter on each report sent. Applying it at all times means there is one
rule, and it also covers any other burst of reports, not only the ones a catch-up causes. This is
decision 2.

### 3.4 Which connection for reports is paced

The engine sends every report to both sequencers: the leading sequencer forwards it to the member and
the other keeps it in case it takes the lead. The design paces on the connection to the leading
sequencer only, so that a slow follower sequencer cannot slow the venue. That follows principle V11:
a loss of resilience is not a loss of service. The cost is that the follower's connection can still
fill and be closed during a long catch-up, and the follower sequencer then lacks some of the reports
it keeps for a change of leader. Section 4 makes that visible. This is decision 1.

**How the engine knows which connection goes to the leading sequencer.** The order connections the
sequencers open to the engine say nothing about which sequencer opened them, so each sequencer announces
its role and epoch with `RoleAnnouncement` on the engine's connection for reports to it: when the
connection is established, and again on every change of role. The engine paces on the connection whose
sequencer says it leads, under the higher epoch if both say so (agreed with Andrew, 9 November 2026).

**What made the pause too late, and the change that fixes it.** Measured on 9 November 2026, the pause
took effect only after tens of thousands of commands already in the engine's queue had been applied:
up to 34,121 reports were outstanding. The reactor reads a connection carrying messages into the
thread's queue without limit. The framework therefore limits the messages from one connection waiting
in a thread's queue ([bounding_messages_waiting.md](../framework/bounding_messages_waiting.md)), and the
engine limits its order connections from the sequencers to 1,024.

### 3.5 The limit

4,096 reports, a constant beside the code that uses it. It must stay well below
`connection_waiting_sends_maximum`. The engine checks this when it reads its configuration and refuses
to start if the waiting-sends limit is less than four times the pacing limit, as the gateways already
do for their paced resend.

## 4. A connection for reports lost while reports are owed

Separate from pacing, and agreed as decision 3. Today, when a connection for
reports is closed, the engine carries on sending on it and the reactor drops every report, logging
"unknown connection id" for each. In the failing run that was 47,091 lines, and no line said which
reports were lost.

The proposal: when a connection for reports is lost, the engine stops sending on it, and logs one
Error naming the sequencer and the range of commands whose reports were sent on it and may not have
been written. Pacing means a catch-up should never cause this. It still covers a connection lost for
any other reason, including the follower sequencer's connection under section 3.4. It meets the
principle in BUG-0234: a report the venue owes reaches the member, or the venue knows and says which
did not.

## 5. Steps, and how each is known to be right

1. **A class that keeps the count and decides when to pause and resume**, with no connections of its
   own, so that it can be unit-tested with a fake. The tests cover: no pause below the limit; a pause
   at the limit; a resume on the notification; reports sent while paused counted; and a connection
   lost while paused, which must resume reading and not wait for a notification that will never come.
   Each test is made to fail on purpose first.
2. **The engine uses it**, pausing its order connections from the sequencers and asking for the
   notification on the connection to the leading sequencer.
3. **The failure reproduced on demand, before the fix is trusted.** Scenario 80's check passes or fails
   depending on the relative speed of the two processes. A test setting that turns pacing off, used with
   a smaller waiting-sends limit, makes the loss happen every time. With pacing off, the copy of the WAL
   must be short of reports; with pacing on, it must hold all 600,000 records. The check is run several
   times each way.
4. **The time a catch-up takes**, measured with pacing on and off, so that the cost is known. It should
   be close to the time the sequencer takes to read the reports, since that was always the real limit.
5. **Section 4**, with a unit test of the Error and a run in which the connection for reports
   is closed during a catch-up on purpose.

## 6. Decisions

### Decision 1. Is pacing on the leading sequencer's connection only? (Yes or no.)

**Decided: yes.** Pacing on both connections would let a slow follower sequencer slow the venue, against
principle V11. The cost is that the follower can still lose reports it keeps for a change of leader
during a long catch-up, and section 4 says so when it happens.

### Decision 2. Does the rule apply at all times, not only during a catch-up? (Yes or no.)

**Decided: yes.** In normal running it never pauses anything, it costs one counter for each report, and
one rule is simpler than a rule that switches on and off. It also covers bursts that have nothing to do
with a catch-up.

### Decision 3. When a connection for reports is lost, does the engine stop sending on it and log one Error naming the commands whose reports may be lost? (Yes or no.)

**Decided: yes.** Today the loss is silent apart from one line per dropped report, and none of those
lines says which member's report it was.
