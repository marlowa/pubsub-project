# Bounding the messages waiting from one connection {#bounding_messages_waiting}

**Status: designed, not built.** This is the framework change chosen on 9 November 2026 for
[BUG-0234](../bug_list.md#bug_0234), written for review before any code. Section 8 lists the decisions it
needs.

## 1. The problem

The reactor reads each socket that carries framework messages (PDUs) with `PduParser`. For each complete
message it takes a block from the inbound slab allocator, which grows without limit, and puts the message
on the owning application thread's queue. It then carries on reading until the socket has nothing more.
Nothing counts how many messages from one connection are waiting in the thread's queue, and nothing stops
reading because there are too many.

So when a thread falls behind the peer sending to it, the backlog does not stay in the peer's socket,
where TCP would slow the peer down. It moves into the thread's queue, in memory, as fast as the network
delivers it. `ApplicationThread::pause_reading` cannot help much either: it stops the reactor reading
from then on, but everything already queued is still delivered.

That is what was measured for BUG-0234 on 9 November 2026. A matching engine catching up paused the
commands from the sequencer once 4,096 of its reports were waiting to be written, but up to 34,121 were
outstanding, because tens of thousands of commands were already in its queue when the pause took effect,
and the notification that would end the pause waited behind them.

For connections that carry raw bytes the framework already has this kind of limit: the raw-bytes handler
pauses reading when its buffer is full, and the reactor removes the socket from epoll until the
application has consumed some of it (`InboundConnectionManager::on_data_ready`, "read backpressure
engaged"). Connections that carry PDUs have nothing equivalent.

## 2. What is wanted

A connection can be given a limit on how many of its messages may be waiting in the owning thread's
queue. While that many are waiting, the reactor does not read the connection, so the kernel's receive
buffer fills and TCP stops the peer sending. Once the thread has taken enough of them off its queue, the
reactor reads the connection again. Nothing is lost or refused, and no message is held anywhere but the
peer's socket and the thread's queue.

## 3. The design

### 3.1 The count, and who changes it

Each limited connection has a counter of its messages waiting in the thread's queue, shared by the reactor
thread and the owning application thread, and changed by both without a lock:

- **The reactor adds one** in `PduParser::dispatch_pdu`, when it puts a message on the thread's queue.
- **The application thread takes one away** in `ApplicationThread`, when it takes a `FrameworkPdu` message
  off its queue, just before calling `on_framework_pdu_message`. The count is of messages waiting, not of
  payloads not yet released: an application that keeps a payload for later, as some do, is no longer
  holding up the connection once it has taken the message.

The counter is an atomic integer. One thread only ever adds and the other only ever subtracts, so plain
atomic increments and decrements are enough.

**Where the counter lives.** The reactor owns a fixed table of counters, sized at start-up from the most
connections it is configured for. A limited connection is given an entry when the limit is set; the
message carries a pointer to the entry, beside the connection id and the slab handle it already carries.
An entry is freed only once its connection has gone and its count has returned to zero, so a message
still in the queue after its connection has closed never points at an entry given to another connection.

### 3.2 Stopping and starting reading

- **Stopping.** After adding one, the reactor compares the count with the limit. At the limit it stops
  reading the connection, by taking EPOLLIN off the socket as the raw-bytes case does, and sets a flag in
  the entry saying it has stopped for this reason. `PduParser` stops at the end of the message it has
  just completed, so a message is never cut in half.
- **Starting again.** After taking one away, the application thread checks the flag. If the flag is set and
  the count has fallen to half the limit, it clears the flag with a compare-and-exchange, so that it does so
  once, and sends the reactor a command to read the connection again. The command is sent only at that
  crossing, never for each message, so the cost on the thread's path is one atomic subtraction and one
  load of the flag.

Half the limit, rather than one below it, so that reading is not stopped and started for every message
when the thread and the peer run at almost the same speed.

**Alongside the existing pauses.** A connection is read only when nothing has stopped it: not this limit,
not `pause_reading` by the application, and not the raw-bytes handler's full buffer. The existing code
already keeps the application's pause apart from the handler's, and this is a third reason kept the same
way, so that ending one never ends another.

### 3.3 How a connection is limited

`ApplicationThread::limit_messages_waiting(connection, limit)`, called by the owning thread, for an
inbound or outbound connection carrying PDUs, at any time after it is established. A limit of zero removes
it. The limit must be at least 16, so that it is never so small that reading stops and starts constantly.
Connections are not limited unless the application asks: see decision 1.

### 3.4 What this does for the matching engine

The engine limits each order connection from a sequencer to 1,024 messages waiting (decision 2). When its
pacing pauses the commands, at most 1,024 more are already queued, so the reports outstanding stay below
about 4,096 plus 1,024, far below the 16,384 the engines-on-the-WAL check now allows and the 65,536 the
framework allows by default. The notification that ends the pause waits behind at most 1,024 messages, so
pacing reacts within a few milliseconds instead of after tens of thousands of commands.

## 4. What it costs

On the reactor's path, one atomic increment and one comparison for each message on a limited connection.
On the application thread's path, one atomic decrement and one load. On an unlimited connection, one
check that there is no entry. The table of counters is fixed at start-up.

## 5. What it does not change

Connections without a limit behave exactly as now. Outbound sends, the limit on waiting sends, and the
raw-bytes path are unchanged.

## 6. Risks

- **A limited peer's sends back up.** When reading stops, the peer's sends to this connection wait in the
  peer's own queue of waiting sends, and if that reaches its limit the peer closes the connection. For the
  matching engine the peer is the sequencer's reading thread, which already sends only as fast as the
  connection takes, so nothing waits in its queue. A connection should be limited only where the peer
  copes with being slowed down, which is why the limit is set per connection and not for all
  (decision 1).
- **A thread that stops taking messages off its queue** stops its limited connections being read for as
  long as it is stuck. That is the purpose: the alternative is a queue that grows until memory runs out.

## 7. Steps, and how each is known to be right

Each test below is made to fail on purpose first, by removing the limit or the restart, as required for any
change to the reactor or to the hand-over to an application thread.

1. **Unit tests** of the counter table and the stop-and-start rule: at the limit reading stops once; at half
   the limit it starts once; a second start is never sent; an entry is not reused while its count is
   above zero after its connection has gone.
2. **An integration test** with a real reactor, real sockets and a registered application thread that takes
   messages slowly: a peer floods a limited connection, and the number waiting in the thread's queue never
   exceeds the limit, every message arrives once and in order, and the peer is slowed by TCP. With the limit
   removed, the number waiting must grow far past it.
3. **A soak test** running for minutes, with the consumer alternating between slow and fast and several
   limited and unlimited connections: no message lost, repeated or reordered; no connection left stopped;
   every count back to zero at the end.
4. **Fairness tests.** Several peers sending at once to one thread, one of them limited and flooding: the
   others' messages, and the thread's timers and messages from other threads, go on being served promptly.
   And several limited peers flooding at once: each is served, none starved.
5. **The matching engine** limits its order connections from the sequencers, and scenario 80's check is run
   again: the most reports outstanding must stay below 4,096 plus the limit. Then the full high-availability
   suite, with the WALs cleared first.

## 8. Decisions

### Decision 1. Is the limit set per connection by the application, with no connection limited unless asked? (Yes or no.)

Recommended: **yes.** Limiting every connection by default would slow every peer that outruns its reader,
including a gateway sending to the sequencer: the gateway's sends would wait in its own queue, and once that
queue reached its limit the gateway would close the connection, which is the rule added for
[BUG-0112](../bug_list.md#bug_0112). Each connection where being slowed
is safe can be limited as it is understood.

### Decision 2. Is the matching engine's limit on its order connections from the sequencers 1,024 messages? (Yes, or give a number.)

Recommended: **1,024.** Small enough that pacing reacts within a few milliseconds and the reports outstanding
stay far below any limit on waiting sends; large enough that, at the engine's rate of applying commands,
reading stops and starts no more than a few hundred times a second during a catch-up and never in normal
running.
