# Writing several waiting messages to a connection in one system call {#gathering_sends}

**Status: proposed, not agreed.** The design for the throughput ceiling found while building the limit on unanswered
commands ([BUG-0251](../bug_list.md#bug_0251)). Section 8 lists the decisions it needs.

## 1. The problem

The reactor writes each framework message (PDU) to its socket with a system call of its own. Under load that is the
limit on how fast the venue goes.

Measured on 19 November 2026 on the development machine: 1.8 million orders offered by four sessions as fast as the
sockets accept them, with each gateway limiting its unanswered commands to 16,384. The venue sustained about 21,000
orders a second. Profiles of every thread (`perf record`, and a sampler of thread states every 10 ms; kept in
`tmp/bug0251/` in the development checkout) showed:

| Thread | What it was doing |
|---|---|
| The leading sequencer's reactor | busy all the time; no time at all in the idle loop (`Reactor::poll_for_work`); 80% of its time in `send()` system calls (`TcpSocket::send` → `tcp_sendmsg`) |
| The follower sequencer's reactor | idle 50% of its time |
| The binary gateway's reactor | idle 62% of its time |
| The working thread of each sequencer, engine and gateway | running in 11% to 30% of the samples |

The leading sequencer writes two records for each order, the order and its report, and sends each record separately to
the follower sequencer, to the two publishers and to the follower matching engine, and also the order to the leading
engine and the report to the member's gateway: about ten messages, so ten system calls, for each order. At about 4 to 5
microseconds each, one reactor thread makes about 200,000 a second, which is about 21,000 orders a second.

Much of each call's cost does not depend on its size: entering the kernel, the TCP stack building and sending a segment,
and, on this machine, the firewall's rules (`nf_hook_slow`, 38% of the leading sequencer's reactor time), which are
applied to every packet. A call that writes ten messages costs little more than one that writes one.

## 2. What already exists

- **A queue of control commands.** An application thread asks the reactor to send a message by putting a command on
  the reactor's queue. The reactor takes up to 64 commands in each pass (`Reactor::process_control_commands`,
  `commands_per_pass`), and acts on each in turn.
- **One write for each command.** For a send, the connection manager (`OutboundConnectionManager` or
  `InboundConnectionManager`) writes the message at once if the connection is not still writing an earlier one and
  nothing is waiting (`send_or_wait`, `start_send`), through `PduFramer::send_prebuilt` and `TcpSocket::send`, one
  `send()` call with `MSG_NOSIGNAL`.
- **A queue of waiting sends for each connection** (`WaitingSendQueue`). A message that cannot be written at once,
  because the connection is still writing an earlier one, waits there in order, holding its slab chunk, and is written
  when the socket has room (`start_waiting_sends`). The queue's limits, 65,536 sends or 32 MiB, close a connection whose
  peer has stopped reading.
- **A partly written message.** When the socket takes only part of a message, the framer remembers how far it got
  (`PduFramer::continue_send`), the connection keeps the chunk, and the reactor watches the socket for room (EPOLLOUT)
  and writes the rest.

## 3. The design

### 3.1 Gathering the messages of one pass

During a pass of control commands, a send for a connection is not written at once. It is added to the back of that
connection's queue of waiting sends, as a send waiting behind an earlier one is today, and the connection is noted as
having something to write in this pass. At the end of the pass, each connection noted is written: as many of its
waiting messages as fit in one call are written together, with one `sendmsg()` call, which takes a list of separate
buffers (an `iovec` for each message's chunk) and writes them in order, as if they were one. If messages remain, it is
called again until the queue is empty or the socket is full.

The order of messages on a connection is the order the application asked for, as today: each is added to the back of
the queue and written from the front.

A message alone in its pass is written at the end of that pass, which is moments after it would be written today: the
rest of the pass is at most 63 other commands. So a lone order's latency does not change by more than that pass. Under
load, many messages for the same connection arrive in one pass, and are written in one call.

### 3.2 The write itself

`sendmsg()`, not `writev()`, because `sendmsg()` takes the flag `MSG_NOSIGNAL`, as the present `send()` does: a write
to a connection whose peer has gone then fails with an error, instead of raising SIGPIPE and killing the process.

Each call writes at most a fixed number of messages, 64 (decision 2). The system's own limit (`IOV_MAX`, 1,024 on
Linux) is never reached, so `EINVAL` for too many buffers cannot happen, and one connection with a long queue cannot
hold the reactor in one call for long.

### 3.3 What the call can return, and what is done

| The call returns | Meaning | What is done |
|---|---|---|
| every byte asked for | all the messages were written | each message's chunk goes back to its allocator; the next messages, if any, are written |
| fewer bytes, more than zero | the socket took only some: a partial write, which is normal | the chunks of the messages written in full go back; the message cut part way stays at the front of the queue, with how far into it was written, and the rest of the queue behind it; the socket is watched for room (EPOLLOUT), and the writing goes on from that point |
| `-1` with `EAGAIN` or `EWOULDBLOCK` | the socket has no room | nothing was written; every message stays in the queue; the socket is watched for room |
| `-1` with `EINTR` | a signal interrupted the call before anything was written | the call is made again at once |
| `-1` with `EPIPE`, `ECONNRESET`, `ENOTCONN` or `ETIMEDOUT` | the connection is gone | the connection is closed as a failed send closes it today: every waiting chunk goes back to its allocator (`WaitingSendQueue::release_all`), the application is told the connection is lost, and an outbound connection is scheduled to be made again |
| `-1` with `ENOBUFS` or `ENOMEM` | the kernel is short of memory for the moment | treated as `EAGAIN`, with a Warning the first time on the connection: nothing was written, and the socket is watched for room (decision 4) |
| `-1` with anything else, such as `EBADF`, `EFAULT` or `EINVAL` | a defect in the reactor | the connection is closed as for a connection gone, and the error is logged at Error with the call's arguments |

Whatever the call returns, no message is ever written twice and none is skipped: the queue is the only record of what
remains, and it advances only by the bytes the call says it wrote.

### 3.4 Room to write again

When the socket has room again (EPOLLOUT), the reactor writes from the queue in the same way, gathering up to 64
messages in each call, starting part way into the first if it was cut. Today it writes the rest of one message and
then each waiting message with a call of its own.

### 3.5 Where it applies, and where not

Every connection carrying framework messages over TCP without TLS, in both directions: outbound connections
(`OutboundConnectionManager`) and inbound ones (`InboundConnectionManager`, through the framework's protocol handler).

Not TLS connections, whose handler encrypts each message into its own buffer and writes that (decision 3), and not raw
bytes connections, such as the members' FIX sessions, which the gateways write themselves.

### 3.6 The waiting queue's limits

They are unchanged, and now count every message not yet written, including those gathered in this pass. A connection
whose queue is full is closed as today. A pass adds at most 64 messages to a connection, so the change itself never
fills a queue.

### 3.7 What is measured

The reactor's histograms of how long a command waited before the reactor took it are unchanged. A new histogram records
how many messages each call wrote, so that how much gathering happens under each load can be seen in Grafana, and a
counter how many calls returned a partial write.

## 4. The firewall

The firewall's share is a property of this machine: `ufw` and Docker are both active here, and their rules are applied
to loopback traffic. How much of the ceiling it accounts for is measured separately, by the same overload run with the
firewall's rules and without them. The design does not depend on the answer: whatever a packet costs, fewer, larger
packets cost less.

## 5. What it does not change

- The application's interface: `send_pdu` and its kind are unchanged.
- The order of messages on each connection.
- What a member receives.
- The limits on waiting sends, and when a connection is closed for exceeding them.

## 6. Risks

- **Latency of a lone message.** Bounded by one pass of at most 64 commands, which is microseconds. Measured with the
  latency run (`perf_run.py`) before and after; a rise at the median or the 99th percentile beyond the run-to-run
  spread is a reason not to go ahead.
- **Fairness between connections.** One connection's long queue is written 64 messages a call, and each connection noted
  in a pass is written once before any is written again, so no connection waits for another's whole queue.
- **A defect in the partial-write bookkeeping** would corrupt the byte stream. Section 7's tests write every possible
  cut, byte by byte.

## 7. Steps, and how each is known to be right

1. **The gathered write, in the connection managers,** with unit tests driving a fake socket that takes a given number
   of bytes in each call: every cut from one byte to the whole, `EAGAIN` first, `EINTR`, each error in section 3.3, and
   64 or more messages waiting. Each test made to fail on purpose before it is trusted.
2. **The pass gathers.** Unit tests that messages asked for in one pass reach the peer in order and in fewer calls than
   messages, and that a lone message is written in the pass it was asked for.
3. **The fairness and soak tests** that a foundational change to the reactor needs: two connections, one with a long
   queue, both served; and a soak of the integration tests run many times over.
4. **Throughput.** The overload of scenario 116, before and after, with the machine in measurement mode.
5. **Latency.** The latency run before and after, in measurement mode.

## 8. Decisions

### Decision 1. Does the reactor gather the sends of each pass of control commands, writing each connection's messages at the end of the pass with one `sendmsg()` call for up to 64 of them? (Yes or no.)

The alternative is to gather only messages already waiting, which leaves the common case, one message written at once,
as it is, and so does not help.

### Decision 2. Does each call write at most 64 messages, a provisional figure to be measured? (Yes or no.)

### Decision 3. Are TLS connections left as they are? (Yes or no.)

No TLS connection carries the order path.

### Decision 4. Is `ENOBUFS` or `ENOMEM` from the call treated as "no room now", with a Warning, rather than closing the connection? (Yes or no.)

Both mean the kernel is short of memory for the moment, not that the connection has failed.
