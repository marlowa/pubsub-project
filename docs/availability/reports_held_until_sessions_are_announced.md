# Reports held until the gateways have announced their sessions {#ha_reports_held_until_sessions_are_announced}

The design for [BUG-0190](../bug_list.md#bug_0190): a sequencer that leads before every gateway has told it which
sessions are connected holds, rather than drops, a report for a session it does not know of, and sends it once the
session is announced. **Status: agreed by Andrew on 24 October 2026** (section 6: question 1 yes, 2A, 3A), and built.

## 1. The problem

A sequencer keeps where each member's session is connected only in memory, in `session_destinations_`. A
sequencer that restarts therefore knows of no session. The gateways tell it: whenever one of a gateway's links to a
sequencer comes up, the gateway sends `SessionBound` down that link for every session it holds, an established
session with `still_connected` set ([session_binding.md](session_binding.md), "When a link to a sequencer comes
up").

A sequencer that restarts quickly can take the lead before every gateway's link to it is up. In scenario 27 the
restarted primary took the lead 0.1 seconds before the member's session was announced to it, and in scenario 36 the
restarted secondary under 0.05 seconds before. A report for a session not yet announced is dropped in that interval.

Not every report is at risk. A gateway sends its announcements down a link before any command it sends again on that
link, and a link delivers in order, so a report for an order sent on a link that is up always finds its session
known. The reports at risk are for orders the venue took before the restart: a report a matching engine repeats while
it catches up with the new leader, a cancel sent by an engine that cancels its book on promotion, and a report for a
session on a gateway whose link comes up later than the others'. A member not told of such a report cannot tell its
order from one the venue lost.

## 2. What is decided already

Andrew, 24 October 2026: the sequencer holds the report rather than dropping it, while some gateway has not yet
announced its sessions, and sends it once the session is announced. Nothing waits for the gateways, and no report is
lost because a time limit ran out. A sequencer still takes the lead without waiting for the gateways
([BUG-0182](../bug_list.md#bug_0182), decision 2A).

## 3. The design

### 3.1 Knowing that a gateway has announced every session it holds

The sequencer cannot tell, from the announcements alone, when a gateway has finished: a gateway holding no session
sends none. So each gateway ends its announcements with a new message, **`SessionsAnnounced` (id 129)**, sent down
the same link straight after the last `SessionBound`, and sent even when it holds no session. It carries the
gateway's protocol and instance, and how many sessions it announced.

The sequencer keeps, for each gateway instance in its configuration (`gateway_endpoints`), whether that instance has
announced its sessions on its present link:

- **Not announced** when the sequencer starts, and again whenever the link that instance's `SessionsAnnounced`
  arrived on is lost. The sequencer learns which inbound link belongs to which instance from the message itself.
- **Announced** when its `SessionsAnnounced` arrives.

**The sessions are known** when every configured instance is announced.

A gateway instance that is down never announces, so its sessions are never known from it. Its members reconnect to
their backup instance, and that logon binds their sessions in the ordinary way, which releases what is held for them
(3.3). What is held for a member that never reconnects is bounded (3.4).

### 3.2 Holding

While this sequencer leads, a report about to be sent for a session it does not know of is:

- **held**, if the sessions are not yet known;
- **dropped**, as today, if they are: counted in `sequencer_reports_dropped_unbound`, with a Warning and test event
  211 for the first report for a session since it was last connected. The session really is connected nowhere, and
  its member is sent the report when it logs on again ([BUG-0088](../bug_list.md#bug_0088)).

A report is held at the point where it would have been sent, after the record it depends on is committed. Nothing
about committing or numbering changes. The report has already been written to the WAL, as today.

### 3.3 Releasing

When a session is announced with `still_connected`, every report held for it is sent to where it is now connected,
oldest first, before anything else is sent for it. Because everything here runs on the sequencer's own thread, no
report for the session can overtake the held ones.

When a session is bound by a logon instead, what happens depends on the protocol:

- **A FIX session.** Once its numbering is settled, the gateway asks for every report produced since the last one
  the member was delivered (`UndeliveredReportsRequest`), and the sequencer sends those from the WAL
  ([session_binding.md](session_binding.md), "Reports produced while the session had no connection"). The held
  reports are among them, so the held copies are discarded; sending them as well would give the member each one
  twice.
- **A binary session.** The binary gateway makes no such request, so the held reports are sent, as for an
  announcement.

When the sessions become known (the last configured instance announces), every report still held is for a session
that is connected nowhere, and is dropped as in 3.2.

A held report has not been forwarded, so the position the leader gives its follower for the reports it has
forwarded (`reports_forwarded_through`) stops just before the oldest held report, as it stops before the oldest report
waiting for the follower. A follower that takes the lead therefore keeps, and forwards, its copy of every report the
leader was holding.

When this sequencer stops leading, everything held is discarded. The instance that takes the lead forwards what it
kept while following, as it does today ([KeptReportsForwarded](test_events.md)).

### 3.4 Where held reports are kept, and how many

In a new class, `ReportsHeldForUnknownSessions`, in `applications/sequencer`, used only by the sequencer's thread and
tested on its own. It allocates its room once, when constructed, as `KeptReportStore` does: a block of bytes for the
reports and a ring of entries saying where each is, so nothing is allocated when a report is held. It takes no lock.

The room is set by two constants beside the kept-report store's, 16 MiB and 32,768 reports. When it is full, the
oldest held report is dropped to make room, counted in a new metric `sequencer_held_reports_dropped_full`, and
logged as a Warning once for each run of drops. Holding lasts from taking the lead until the gateways have announced,
normally under a second, so the room is expected never to fill.

### 3.5 What a test can see

Two sequencer test events, at the next free ids:

- **`ReportsHeld` (212)** when the first report is held after taking the lead, with how many gateway instances have
  not yet announced.
- **`HeldReportsReleased` (213)** each time held reports are sent for a session, with the comp id and how many, and
  once when the sessions become known, with how many were dropped.

## 4. The false alarm for `INJECT-TEST`

`inject_order` hands a command straight to a sequencer under the comp id `INJECT-TEST`, which no gateway holds, so
that a test can see what the matching engine does with a command no gateway would pass. Its reports are, correctly,
for a session connected nowhere, and each run of the scenarios gives the Warning and event 211 once for it on each
sequencer. A reader of the Quill log cannot tell that from a member left untold.

The options are in section 6, question 2.

## 5. How it is tested

- **Unit tests** of `ReportsHeldForUnknownSessions`: a report held while the sessions are not known and dropped once
  they are; released in order when its session binds; nothing overtakes a held report; discarded on losing the lead;
  the oldest dropped when full; a gateway whose link is lost becoming not announced again. Each made to fail on
  purpose.
- **The full set of scenarios**, from cleared state.
- **A scenario in which the window happens on purpose**, `ha_test.py` scenario 77, by option A of section 6, question
  3. The gateways wait 40 seconds before announcing their sessions on a link that comes up. The leading sequencer is
  killed and restarted; while it is restarting, the leading engine's reports are blocked and 1,000 orders are sent,
  which it applies without the member being sent anything; then the other sequencer and the leading engine are killed.
  The restarted sequencer leads before it is told of the member's session, and the engine that takes the lead sends
  the reports it kept of those orders ([matching_engine_lockstep.md](matching_engine_lockstep.md), section 3). It
  passes when the sequencer sent `ReportsHeld` and then `HeldReportsReleased` for the member's comp id, no report was
  dropped, and the member received a New report for each of the 1,000. With no wait before the announcements, nothing
  is held and it fails.

## 6. Decisions

These are separate questions.

**Question 1. Is the design in section 3 agreed?** In particular the new message `SessionsAnnounced`, the rule that
the sessions are known only when every configured gateway instance has announced, and the fixed room of 16 MiB and
32,768 reports with the oldest dropped when full. Reply yes, or say what to change.

**Question 2. What should be done about the Warning for `INJECT-TEST`?** Choose one.

- **A (recommended): name, in the sequencer's `[test_events]` section, the comp ids whose reports are expected to
  have no session.** `ha_test.py` lists `INJECT-TEST` there, as it already lists it for the matching engine's
  `report_orders_of`. A report for such a comp id is still counted, but logged at Debug with no event 211. Only a
  sequencer started by a test has a `[test_events]` section, so a deployed venue cannot be told to keep quiet about
  any member.
- **B: leave it.** The Warning is accurate, and it appears once per sequencer per run.

**Question 3. How should a scenario make the window happen?** Choose one.

- **A (recommended): a test-only setting in each gateway's `[test_events]` section that delays its announcements on
  a sequencer link that comes up, by a stated time, and a scenario that uses it.** The scenario:
  1. The member places orders that rest on the book.
  2. The leading sequencer, the primary, is killed; the secondary leads.
  3. The primary is restarted, with the gateway delaying its announcements to it by five seconds.
  4. Within those five seconds, the secondary sequencer and the leading matching engine are killed together. The
     restarted primary takes the lead before it is told of the member's session, and the other matching engine is
     promoted and, under `open_orders_on_promotion = "cancel"`, cancels every resting order, sending a report for
     each.
  5. Passes when the member receives a cancel for every resting order, the primary sent `ReportsHeld` and then
     `HeldReportsReleased` for the member's comp id, and no report was dropped.

  Without the change it fails at step 5: the cancels are dropped. Whether step 4 can be made to fall inside the
  window reliably is to be measured; the delay can be made longer if not.
- **B: no scenario, only the unit tests and the full set.** Cheaper, but nothing would show the whole path working in
  the running venue.

**Andrew's answers, 24 October 2026:** 1 yes, 2A, 3A.
