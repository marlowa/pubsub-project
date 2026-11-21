# A gateway limits the commands it has sent and not yet had answered {#a_gateway_limits_its_unanswered_commands}

**Status: built.** The design for [BUG-0251](../bug_list.md#bug_0251). Section 7 records its four decisions, each
agreed with Andrew on 19 November 2026; section 8 what was measured.

## 1. The problem

Members can offer orders faster than the venue can process them. The development venue processes about 110,000 orders
a second; a load client sending 1.8 million orders on four sessions as fast as the sockets accept them offers more than
that. Nothing in the order path then holds the excess back at its source. The gateway reads every order its members
send, sends each to both sequencers, and keeps a copy until it is answered. The orders queue wherever the next stage is
slower, and the queueing shows as latency: up to 14 seconds in the runs recorded in BUG-0251.

The queues cause three faults, each measured on 19 November 2026 (BUG-0251, "Measured on 19 November 2026"):

1. **The follower matching engine stops being a standby.** It applies each command as soon as the WAL commits it, and
   keeps its own reports of every command until the WAL holds the leading engine's reports of the same commands, to
   compare them. Its store holds 100,000 reports and 64 MiB of them (`FollowerReportComparison::max_kept_reports` and
   `max_kept_report_bytes`). When more than that many commands, or that many bytes of their reports, are between being
   committed and having the leading engine's reports written, the store fills, and the
   follower refuses to lead until it is restarted. This happened in both runs.
2. **A gateway can close its own connection to the sequencer.** A connection on which 65,536 sends are waiting to be
   written is closed (`ReactorConfiguration::connection_waiting_sends_maximum`). The leading sequencer stops reading
   the gateways while 16,384 orders wait for a voter to confirm that the follower sequencer may not lead. At 110,000
   orders a second a gateway reaches 65,536 waiting sends in about 0.6 seconds; the pauses measured lasted between 0.35
   and 0.43 seconds. When the sequencer's reading of the gateways was limited in an experiment, the gateway did close
   the connection, connected again, and sent every unanswered command again.
3. **A leader can lose its lease.** In that experiment, after the gateway sent its commands again, the leading
   sequencer logged nothing for 7 seconds and its lease ran out, 38 times in one run. What it was doing is not yet known.
   A message that renews a lease waits in the sequencer's queue behind every message that arrived before it, so the
   longer that queue, the nearer the lease comes to running out.

## 2. What already exists

- **The store of unanswered commands** (`pubsub_itc_fw::UnansweredCommands`, used by both gateways;
  [one_check_against_sequencing_twice.md](one_check_against_sequencing_twice.md)). A gateway keeps a copy of every
  command it sends to the sequencers until it receives the answer, an execution report or a refusal, and sends every
  copy it holds again after a change of leader or when a connection to a sequencer is made again. It therefore already
  knows, at every moment, how many of its commands the venue has not yet answered. When the store is full, about
  156,000 commands of the size the load client sends (64 MiB), the gateway refuses each new command.
- **Pausing a connection** (`ApplicationThread::pause_reading` and `resume_reading`). The reactor stops reading a
  connection, its socket's receive buffer fills, and TCP stops the peer sending. Nothing is lost.
- **Throttles** in both gateways ([gateway throttles](../venue/gateway_throttles.md)) limit how fast each member may send,
  so that one member cannot take the whole of the venue's capacity.

## 3. The design

### 3.1 The rule

A gateway stops reading every member's connection while it has too many commands unanswered, and reads them again once
the venue has answered enough of them:

- **Stop** when the number of unanswered commands reaches the **limit**.
- **Read again** when it falls to half the limit. Half, rather than one below, so that reading is not stopped and
  started for every command when members offer orders at almost exactly the venue's speed.

Orders the gateway has not read stay in the members' sockets, and TCP slows each member down. A member sees its orders
acknowledged later, as it does today, but the waiting happens in its own socket instead of inside the venue.

### 3.2 Why this bounds every queue in the order path

A command is unanswered from the moment the gateway sends it until the gateway receives its answer. Every queue between
the two holds only unanswered commands, so none of them can hold more than the sum of the gateways' limits:

| Queue | Bounded by |
|---|---|
| sends waiting on a gateway's connection to each sequencer | the gateway's limit |
| orders in the sequencer's queue, ahead of any message that renews a lease | the sum of the gateways' limits |
| commands committed whose leading engine's reports are not yet in the WAL, which the follower engine must keep | the sum of the gateways' limits |
| commands a gateway sends again after a change of leader or a connection made again | the gateway's limit |

With four gateways and a limit of 16,384, the largest of these is 65,536. That is below the follower engine's 100,000,
so fault 1 cannot happen. No single gateway has more than 16,384 sends waiting on a connection, a quarter of the
65,536 at which the connection is closed, so fault 2 cannot happen. And at the 110,000 orders a second measured, 65,536
orders ahead of a lease renewal take about 0.6 seconds, a fifth of the 3-second lease, which answers fault 3 for the
queue's length. Section 6 says what remains to be found for fault 3.

**The bound does not yet hold as stated** ([BUG-0279](../bug_list.md#bug_0279)). A command counts as unanswered from
the moment the gateway sends it, but the gateway stops only its reading: the orders the reactor has already read from the
members' connections when the limit is reached are still taken and sent. In `ha_test.py` scenario 116, on 20 November
2026, with four members offering orders as fast as their sockets accept them, the binary order gateway had 29,992 sends
waiting on its connection to each sequencer, 13,608 more than its limit. Faults 1 and 2 still did not happen there, but
with more members the excess grows, and nothing bounds it. The fix, a limit on the messages waiting from each member's
connection and on the members a gateway serves, is in
[bounding_what_a_gateway_has_read.md](bounding_what_a_gateway_has_read.md); once it is built, each figure above is the
gateway's limit plus what its members' connections may have waiting.

Commands that do not come from a gateway, such as the cancel of all of an ended session's orders, are outside this
bound. The matching engine already carries out such a cancel in pieces
([pacing_the_engines_reports.md](pacing_the_engines_reports.md), section 7).

### 3.3 Where it is built

The count and the rule go in `UnansweredCommands`, so that both gateways share one implementation, as they share the
store. It tells the gateway to stop and to start reading through two new calls on `UnansweredCommandLinksInterface`,
one for each. The gateway pauses or resumes each member connection it has, and pauses a member connection made while
reading is stopped.

A command a member sends while reading is stopped stays in that member's socket. A FIX member's TestRequest and
Heartbeat wait there too. A pause lasts as long as it takes the venue to answer half the limit, a fraction of a second
at the rates measured. That is far shorter than a FIX session's heartbeat interval, which the member's logon states and
the FIX gateway takes as 30 seconds when it does not.

### 3.4 An answer that never comes

If a command were never answered, its copy would count against the limit for as long as the gateway holds it. Enough
such commands would stop the gateway reading its members for good, which is worse than the refusal of new commands
that happens today when the store is full. Today a copy leaves the store when its answer arrives, or when its session
ends for good (`drop_session`). A command is also unanswered, for a time, while the venue holds orders because no matching engine leads: for up to
`[recovery] orders_held_at_most_seconds`, 5 seconds, after which the venue refuses them, and the refusals are answers
([order_acceptance.md](order_acceptance.md)). A gateway that reaches its limit meanwhile stops reading its members until
the refusals or the new engine's reports arrive. That is intended: nothing it read in that time could be acted on
sooner.

Every way a command the gateway kept can fail to be answered, and what removes its copy:

| What happens to the command | What removes the copy |
|---|---|
| No sequencer leads, or the one it reached does not lead and discards it | sent again when an instance announces that it leads, and answered then |
| The leading sequencer refuses it: the phase accepts no command, or its ClOrdID was used before | the `CommandRefused` the sequencer sends on the connection it came by |
| The leading sequencer holds it because no matching engine leads | the new engine's report, or the refusal once `orders_held_at_most_seconds` has passed |
| It was sent again and the WAL already holds it | the report the WAL holds, forwarded from the WAL; or, with none there, the engine's report by the ordinary path |
| The member's session ends, or moves to another gateway, before the answer arrives | the session ends for good on this gateway when its grace period, 30 seconds, passes after it disconnected (`drop_session`) |
| The leading sequencer cannot decode the envelope | nothing until a change of leader sends it again; this happens only through a defect, and the sequencer logs a Warning |
| Its report was held by the leading sequencer for this gateway while the gateway had no connection for reports, and dropped because the room for such reports was full | **nothing until the session ends**, which may be the end of the day |

The last row is the one that matters. While a gateway has no connection for reports from the leading sequencer, the
sequencer holds the reports for it, in a room of limited size, and sends them when the connection is made; when the
room is full it drops the oldest, and logs an Error ("report(s) held for gateway ... were dropped to make room while it
had no connection for reports"). The commands those reports answered stay in the gateway's store. With the limit,
enough of them would stop the gateway reading its members for the rest of the day.

So, when a gateway's connection for reports from the leading sequencer is made, it sends again every command it still
holds (decision 4). The sequencer finds each in its WAL and forwards the first report the WAL holds of it, or, if the
WAL holds none, leaves it to the engine's ordinary report. A copy whose report was dropped is then answered. The number
sent again is the store's count, which the limit keeps to 16,384 or a little over.

### 3.5 The limit

A setting of each gateway, `[unanswered_commands] limit`, 16,384 by default. It must be no more than the store's
capacity in commands. A smaller limit keeps the queues shorter but can lower the rate a gateway reaches; the steps in
section 5 measure the rate the venue reaches at 4,096, 8,192 and 16,384 before the default is settled.

## 4. The sequencer's reading of the gateways

The leading sequencer could also limit how many orders from each gateway may wait in its queue, with
`limit_messages_waiting`, as it does for the engines' reports. With the gateways limiting themselves, such a limit
cannot be reached, and it could only do harm: when it was tried without the gateways' limit, the gateway closed its
connection (fault 2). So it is not added (decision 2).

## 5. Steps, and how each is known to be right

1. **List every way a command can go unanswered**, and what removes its copy (section 3.4). Record any that removes it
   never.
2. **The rule in `UnansweredCommands`**, with unit tests: reading stops at the limit and starts again at half; a
   command answered or dropped counts at once; commands sent again after a change of leader do not change the
   number unanswered. Each test is made to fail on purpose before it is trusted.
3. **Both gateways pause and resume their member connections**, and a member connection made while reading is stopped
   is paused. A test event in each gateway says when reading stops and starts, for the scenario.
4. **A scenario in `ha_test.py`** that offers 1.8 million orders on four sessions as fast as the sockets accept them,
   and decides from test events alone: no `LeaseChanged` saying a lease ran out, no `RoleChanged` from leader, no
   connection from a gateway to a sequencer lost (`ConnectionChanged`), and no `FollowerCannotLead` from the follower
   matching engine; and every order acknowledged. Before the change it must fail, on the follower engine's
   `FollowerCannotLead`, which both runs measured.
5. **The rate against the limit**: the scenario's load run at limits of 4,096, 8,192 and 16,384, with the rate and the
   largest latency of each recorded in [latency_findings.md](../operations/latency_findings.md).

## 6. What remains to be found

- **What the sequencer did for 7 seconds** after the gateway sent its commands again. Answering a command sent again
  means finding its first report in the WAL (`CommandChecksLinksInterface::answer_from_wal`), which may read many
  records for each command. With the limit, a gateway sends at most 16,384 commands again, but the cost of each is
  measured all the same, and if it is large it is recorded as its own defect.
- **Whether the follower engine's store fills for another reason** as well. The scenario in step 4 shows it: with the
  limit in place, a `FollowerCannotLead` would mean a cause section 3.2 does not account for.

## 7. Decisions

### Decision 1. Does a gateway stop reading all its members' connections while it has 16,384 unanswered commands, until the number falls to half, with 16,384 as a provisional default to be measured in step 5? Agreed: yes.

The alternative is to stop reading only the connections of the members with the most commands unanswered. That is
fairer to a member sending slowly, but it needs a count for each member, and the throttles already stop one member
taking the whole of the venue's capacity.

### Decision 2. Is the sequencer's reading of the gateways left unlimited, since the gateways' own limit bounds it? Agreed: yes.

Section 4 gives the reason. A limit on the sequencer's side was part of the option chosen for BUG-0251; this design
leaves it out.

### Decision 3. Is the scenario in step 4, which offers orders as fast as the sockets accept them, added to the suites that run on every change? Agreed: yes.

It sends 1.8 million orders and takes about two minutes, as scenario 115 does.

### Decision 4. When a gateway's connection for reports from the leading sequencer is made, does it send again every command it still holds, so that a command whose report the sequencer dropped is answered from the WAL? Agreed: yes.

Section 3.4 gives the reason. Without it, the reports dropped while a gateway had no connection for reports would leave
their commands counted against the limit until their sessions end.

The sequencer's own record of which reports it dropped was considered instead, and rejected: it lives in the memory of
whichever sequencer led, and is lost with it, and the member must be sent the report, not only the gateway told to
forget the command. The gateway's store of unanswered commands survives a change of leader, and sending its commands
again has the sequencer answer each from its WAL.

## 8. Measured, 19 November 2026

On the development machine, in measurement mode, with [gathering sends](../framework/gathering_sends.md) built: scenario
116, 1.8 million orders offered on four sessions as fast as the sockets accept them, the limit 16,384. Every order was
acknowledged at 110,086 orders a second; no lease ran out, no leader changed, no gateway lost its connection to a
sequencer, and the follower matching engine remained a standby; the binary gateway stopped reading its members 55
times. With the limit and without gathering sends, the venue reached about 35,000 orders a second, held there by the
leading sequencer's reactor making one system call for each message. Without the limit, the leader ran alone for most of
an overload, the follower matching engine stopped being a standby, and a gateway could close its own connection to the
sequencer.

With the limit raised to the store's capacity, 262,144, which no gateway reaches, scenario 116 failed: the binary gateway
closed its connection to the sequencer at 65,536 waiting sends.
