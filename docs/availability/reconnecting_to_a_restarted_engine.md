# Reconnecting promptly to a restarted matching engine {#reconnecting_to_a_restarted_engine}

**Status: agreed, not built.** This is the design for [BUG-0237](../bug_list.md#bug_0237). Option B was agreed with
Andrew on 9 November 2026 (section 5).

## 1. The problem

The sequencer opens the connection on which it sends orders to each matching engine. When an engine dies, the
sequencer's reactor schedules another attempt `connect_retry_interval_` later, 2 seconds by default, and runs due
attempts on its housekeeping tick, every 500 milliseconds in the sequencer. A supervised engine that dies on a healthy
machine is listening again 0.6 to 1.3 seconds later, as measured for [BUG-0029](../bug_list.md#bug_0029), but takes no
orders until the sequencer's next attempt, 2.1 to 2.5 seconds after the death. That wait is the largest part of the
interruption a member sees.

## 2. What the logs show

When the restarted engine is listening for orders, it opens its connection for reports to each sequencer, at once: in
the BUG-0029 measurements, "listening on 127.0.0.1:11020" and the sequencer's "inbound connection 31 established" are
in the same millisecond. So the sequencer is told, by the engine's own connection, the moment a connection to the
engine would succeed.

## 3. Two ways to remove the wait

**A. Try sooner after a lost connection, in the framework.** After an established connection is lost, the reactor tries
again at intervals that start short and grow: 100 milliseconds, then 200, 400, 800, and then the usual 2 seconds. It
needs a retry timer of its own rather than the housekeeping tick, which runs too seldom. Every component's outbound
connections would reconnect sooner after their peer restarts, not only the sequencer's to the engine. The cost is a
few more connection attempts to a peer that is down, and a reconnection still lands up to one interval after the peer
is ready: with the engine ready at 1.3 seconds, the attempt at 1.5 seconds succeeds.

**B. Reconnect when the engine shows it is back, in the sequencer.** When an engine's connection for reports arrives,
the sequencer brings forward the pending attempt to connect to that engine, so that it is made at once instead of when
its interval runs out. This needs one small framework call, `ApplicationThread::connect_now(service)`, which brings a
pending attempt for a service forward to the next pass of the reactor. The reconnection lands within a pass of the
engine being ready, with no wasted attempts, and nothing else in the venue changes. It relies on the engine opening its
connection for reports only once it is listening for orders, which is the order its start-up uses now; a test pins
that.

## 4. How it is known to be right

1. A unit or integration test of `connect_now`: a pending attempt is made at once when brought forward, and a service with
   no pending attempt is unaffected.
2. The measurement of BUG-0029 repeated: kill a supervised leading engine and read, from the Quill logs, the time from its
   death to the sequencer's order connection and to "adopting LEADER role". Before: 2.1 to 2.5 seconds. Expected after:
   the engine's own readiness, 0.6 to 1.3 seconds. Made to fail on purpose by removing the call.
3. Scenario 26, a supervised engine restarted before its peer promotes, and the full high-availability suite.

## 5. Decision

### Decision 1. A or B? (Choose one.)

Agreed: **B**. It reconnects at the moment the engine is ready rather than up to an interval after, wastes no
attempts on a peer that is down, and changes only the sequencer's behaviour towards a restarted engine, where A changes
how every outbound connection in every component is retried. A can still be done later, separately, if other
reconnections prove slow.
