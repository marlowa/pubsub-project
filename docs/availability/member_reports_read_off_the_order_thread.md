# A member's reports read off the thread that sequences orders {#member_reports_read_off_the_order_thread}

**Status: agreed by Andrew on 23 November 2026, each decision in section 6 answered yes; built, except the two new scenarios of section 5.** The reading thread answers one request at a time, in the order asked, so that two answers for one member never reach its gateway mixed. The design for
the first step of [BUG-0113](../bug_list.md#bug_0113),
chosen on 22 November 2026: move the two reads of the WAL that answer a member onto the sequencer's existing reading
thread, before the recovery service is built. The recovery service remains the design for the 100,000 messages kept
for each comp id and for reclaiming the WAL ([BUG-0048](../bug_list.md#bug_0048)); this step removes the danger
BUG-0113 records, and the recovery service later takes the same requests from the same place.

## 1. What happens now

Two requests make the leading sequencer read its WAL on the thread that sequences orders, so that no order is
sequenced until the read finishes (principle F4 of [design_principles.md](../design_principles.md)):

- **A member's resend.** The FIX gateway sends `SessionReplayRequest` with the session's identity (comp id and
  protocol), the WAL position to start after, the most records to return, and how many of the most recent to pass
  over. `SequencerThread::handle_session_replay_request` reads the WAL's segments from the newest backwards,
  decoding every record, keeps the session's execution reports in a window in memory, and sends each as a
  `SessionReplayRecord`, then a `SessionReplayComplete`, on the connection the request came on. The gateway lets
  the session's live traffic resume only once `SessionReplayComplete` arrives, and if a new sequencer leads while
  it waits, it asks again under a new request number and ignores anything still arriving for the old one.
- **Reports produced while the member had no connection.** When the session is bound again and its numbering
  restored, the FIX gateway sends `UndeliveredReportsRequest`. `SequencerThread::handle_undelivered_reports_request`
  takes the range fixed when the session bound (`MemberSessions::take_undelivered`: after the last report
  delivered, through the last report written when it bound), reads the session's reports in that range from the
  WAL in the same way, gathers them in memory, and sends each to the gateway through `ReportForwarding`, as an
  ordinary report.

Each is answered whole before the thread handles another message. A request reaching far back, or a member away
most of a busy day, makes the read long; by the arithmetic under BUG-0113 it could be minutes.

`scripts/check_standards.py` permits both reads by name, as the exceptions for BUG-0113.

## 2. What changes

The sequencer's reading thread (`WalReadingThread`, [following_the_wal.md](following_the_wal.md)) already reads the
WAL for the components that follow it, off the thread that sequences orders. It reads only below the write position
the sequencer publishes, converts each record into the receiver's message, and sends a batch at a time, only as
fast as the receiver's connection takes them, on connections the sequencer's thread owns. Both reads move onto it as
a second kind of work, *answering a member*, beside *serving a follower*:

1. The sequencer's thread receives the request, checks it leads (declining as now if it does not), fixes the range
   to read, and hands the reading thread one command: the session's identity, the range, the most records, the
   number to pass over, and the connection to answer on. It reads nothing.
2. The reading thread reads the range backwards from the newest segment, as the handlers do now, keeps the
   session's execution reports, and sends them oldest first, a batch at a time, on the given connection.
3. When it has sent the last, it sends the closing message (for a resend, `SessionReplayComplete`) and reports to
   the sequencer's thread that the answer is done, for its log and test events.

The reading thread serves followers and answers members in turn, a batch of each at a time, so that a long answer
does not stop a follower being caught up, nor the reverse.

## 3. Reports produced while a member had no connection must not be overtaken

Today a returning member's undelivered reports are all sent before the sequencer's thread handles its next message,
so they always reach the member ahead of any newer report for the same session. Once the reading thread sends them,
the sequencer's thread goes on sending the session's live reports at the same time, and a newer report could reach
the member before an older one: an order shown cancelled before it is shown accepted.

So while a session's undelivered reports are being sent, the sequencer's thread holds that session's live reports,
and sends them once the reading thread reports the undelivered ones sent. `ReportForwarding` already holds reports
for a session that cannot be sent yet, while its session has not been announced by any gateway; this is the same
holding, for one more reason, with the same room and the same rule when the room is full.

A resend needs no such holding: the gateway itself holds the session's live traffic until `SessionReplayComplete`.

## 4. What it does not change

- **What a member is sent.** The same reports, in the same order, marked as possible repeats in the same cases;
  scenarios 22, 23 and 40 check it.
- **The range fixed when a session binds,** which keeps a report sent live from being sent again among the
  undelivered ones.
- **How much is read.** A request reaching far back still reads far back, and holds its window of
  `max_records + skip_most_recent` reports in memory; but on the reading thread, which sequences nothing. Bounding
  that work is the recovery service's (item 4 of what BUG-0113 says its design must settle).
- **A change of leader during an answer.** The reading thread stops answering when its sequencer stops leading, as
  it stops serving followers. The gateway asks the new leader again for a resend, as now. A session's undelivered
  reports are asked for again when it binds to the new leader, as now.

## 5. How it is shown to work

- Scenarios 22, 23 and 40 pass unchanged.
- A new scenario: a member asks for a resend reaching back to the start of a day of many orders, while another
  member goes on sending orders; every one of that member's orders sent while the resend is answered must be
  acknowledged within a time well below how long the read takes, which the scenario measures from the member's own
  messages. With the reads put back on the sequencer's thread it must fail.
- A new scenario for the ordering in section 3: a member away while its resting orders are cancelled logs on again
  while a live report for it is produced, and must receive every report in the order the WAL holds them. With the
  holding taken out it must fail.
- `scripts/check_standards.py`'s two exceptions for BUG-0113 are removed, so that the build fails if either read
  returns to the sequencer's thread.

## 6. Decisions

Each was a separate yes-or-no question, and each was answered yes on 23 November 2026.

1. **Both reads move to the existing reading thread, not a new thread.** Decided yes: it already reads the WAL
   off the order path, under the rules this needs, and adds no thread.
2. **The reading thread sends the answer on the gateway's connection itself**, as it sends records to followers,
   rather than handing each report back for the sequencer's thread to send. Decided yes: handing them back would
   put a copy of every report through the thread that sequences orders, which is what this step removes.
3. **A session's live reports are held while its undelivered reports are sent (section 3).** Decided yes:
   without it a member can see its reports out of order.
4. **The window held in memory for a resend from far back stays as it is until the recovery service,** on the
   reading thread. Decided yes: bounding it is part of the recovery service's design, and on the reading thread
   it costs memory, not orders.
