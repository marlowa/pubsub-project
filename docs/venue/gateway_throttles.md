# Gateway throttles {#gateway_throttles}

**Status: implemented in both gateways.** The requirements it meets are R-0148, R-0149, R-0150 and
R-0151 in the functional specification (`docs/book`, the order gateways section of the applications
chapter); `ha_test.py` scenario 56 verifies the first three. It has no open questions
([section 9](#gateway_throttles_open)).

---

## 1. What a throttle does

A throttle limits how fast a member's session may send commands of one kind. Each session has three
throttles, one for each kind of command:

| Throttle | Limits | FIX message | Binary PDU |
|----------|--------|-------------|------------|
| Place | Placing a new order | `NewOrderSingle` (35=D) | `NewOrderSingle` |
| Amend | Amending an open order | `OrderCancelReplaceRequest` (35=G) | none yet |
| Cancel | Cancelling an open order | `OrderCancelRequest` (35=F) | `OrderCancelRequest` |

**Each limit is a maximum number of commands per second.** A limit of **zero means no limit**, and
zero is the default.

**The limits are configured for each comp id, and applied to each session.** Every session of a comp
id has the same three limits, and each session counts its own commands. If a comp id has two
sessions open at once, each may send up to the limit.

The venue cannot yet amend an order: the FIX gateway ignores an `OrderCancelReplaceRequest`, and the
binary protocol has no amend message. The amend limit is provisioned, carried to the gateway and held
by every session all the same, so that amending is throttled from the day it exists, with nothing
else to add then but the check in the amend handler.

---

## 2. The rule: a sliding window of one second

**A command is accepted if fewer than N commands of its kind were accepted on the session in the
last second**, where N is the limit. The second is measured back from the moment the command
arrives, not from the start of a clock second. So a session can never send more than N commands of a
kind in any one-second period, however the period is placed.

Three details of the rule:

- **A command that arrives exactly one second after the oldest command still counted is accepted.**
  The oldest command has then left the window.
- **A rejected command does not count towards the limit.** Only accepted commands are counted.
  Otherwise a member that kept retrying while throttled would keep itself throttled indefinitely.
- **Only commands the throttle is asked about are counted.** A command the gateway rejects for
  another reason first, such as a missing field or the venue not accepting orders, never reaches
  the throttle, and does not count.

**Time comes from `std::chrono::steady_clock`**, which only moves forwards. The system clock can be
corrected in either direction, and a correction would make a sliding window count wrongly. The
gateway reads the clock once for each command and gives that time to the throttle.

---

## 3. What a member sees

**A command over the limit is rejected, and the member is told why.** It is not delayed, held back
or forwarded later, and the session is not disconnected. Nothing about it reaches the sequencer.

- **FIX, a new order:** the gateway sends an `ExecutionReport` with `ExecType` and `OrdStatus`
  Rejected, as it already does when the venue is not accepting orders
  (`send_reject_execution_report`), with `OrdRejReason` 99 (Other) and a `Text` such as
  *"Throttled: at most 50 new orders per second for this session"*.
- **FIX, a cancel or an amend:** the gateway sends an `OrderCancelReject` (35=9), with
  `CxlRejResponseTo` 1 for a cancel or 2 for an amend, `CxlRejReason` 99 (Other), the limit in
  `Text`, and `OrdStatus` New: the order is open. The throttle has not touched the order, and the
  reply must say so: a rejected `ExecutionReport` would tell the member its order had been
  rejected. The gateway refuses every cancel this way, whatever the reason
  ([BUG-0099](../bug_list.md#bug_0099), R-0151).
- **Binary:** the gateway sends the binary protocol's equivalent replies: a rejected
  `ExecutionReport` for a new order, and an `OrderCancelReject` (PDU 1003) for a cancel, with the
  same fields and values as the FIX message. The binary protocol is generated from
  `applications/fix_orders.dd.xml`, and `OrderCancelReject` is one of the messages generated from it.
  The binary gateway decodes every command to check it before the throttle is asked, so the
  refusal can name the command from the decoded fields.

**The text of every throttle refusal states the limit**, with the kind of command it applies to and
that it is counted for each session, for example *"Throttled: at most 50 cancels per second for this
session"*. A member told only that it was throttled cannot tell how far to slow down, and the gateway
knows the number (R-0150).

The throttle refuses a command exactly as the gateway refuses it for any other reason, so the replies
never differ by reason except in their text.

---

## 4. Where the check is made

In each gateway, the throttle is checked after the checks that already exist, and immediately before
the command is passed to the sequencer:

1. The command is checked as it is today. Both gateways check that required fields are present and
   hold allowed values, that lengths are within limits, that a sequencer is connected and that the
   venue is accepting orders (R-0152). A command refused here is refused as today, and the throttle
   is not consulted.
2. The gateway reads the clock and asks the session's throttle for the command's kind.
3. If the throttle accepts it, the command is passed to the sequencer. If not, the gateway rejects it
   (section 3).

A session whose limit for a kind is zero has no throttle for that kind, and step 2 is skipped.

Cancels the gateway makes itself, for example when a session disconnects and its orders are
cancelled (cancel-on-disconnect), are not member commands and are never throttled.

---

## 5. How the limits reach the gateway

The limits follow the same route as cancel-on-disconnect, which is per comp id for the same reason:
it is a decision made about a member when it is provisioned, not something a member chooses. No
gateway connects to the database; the values arrive with the session when it logs on.

| Step | What changes |
|------|--------------|
| Database | Three columns on the `comp_id` table: `max_place_per_second`, `max_amend_per_second`, `max_cancel_per_second`. Integer, not null, default 0, and checked to be between 0 and 100,000, the largest permitted limit (section 6). Added by a new Liquibase changeset, `db/changelog/v4_gateway_throttles.xml`. |
| Admin service | The three limits can be given when a comp id is created and changed when it is edited: `CompIdHandler.create` and `CompIdHandler.update` both take them, the comp id form (`templates/comp-ids/form.ftl`) shows them in both cases, and `CompIdDao` reads and writes the columns. Both paths refuse a value that is not a whole number from 0 to 100,000, so a mistake is caught where it is typed, not when the member logs on. Creating a comp id without giving the limits leaves them at 0, no limit. |
| Export | `db/export_credentials.py` writes them into `credentials.toml` beside the comp id's other settings. |
| Authentication service | Loads them with the comp id, and **writes them back out** whenever it rewrites `credentials.toml` after a credential change. It rewrites the whole file, so a field it does not write back is lost without a word; the cancel-on-disconnect settings and the gateway pinning are written back for the same reason. |
| `AuthenticationResult` | Three new trailing fields, `i32 max_place_per_second`, `i32 max_amend_per_second`, `i32 max_cancel_per_second`, sent when authentication succeeds. |
| Gateways | Each session's throttles are created when its logon is granted, from those three values. |

Unlike the cancel-on-disconnect grace period, the columns are never empty, and there is no default in
the gateways' configuration. The database always says what the limit is, and an operator has one
place to look.

**When a change takes effect.** A session's limits are fixed when it logs on, and do not change while
it is open. A change made in the admin service reaches the database at once, but the authentication
service reads the export only when it starts, as it does for cancel-on-disconnect; there is no admin
message that carries these settings to a running authentication service. Since the venue's software
starts afresh each day, a change made during the day takes effect from the comp id's first logon on
the next trading day.

---

## 6. The data structures

Three small classes, none of them using a third-party library:

| Class | Where | Why there |
|-------|-------|-----------|
| `FixedCapacityRingBuffer<T>` | The framework library, `libraries/pubsub_itc_fw` | A general-purpose container. The throttle is its first user, not its only one, so it belongs with the framework's other containers and is tested on its own there. |
| `RollingWindowThrottle` | `applications/fix_common` | The throttling rule, which both gateways use; both already link `fix_common`. |
| `SessionThrottles` | `applications/fix_common` | A session's three throttles. |

**`FixedCapacityRingBuffer<T>`** is a first-in, first-out container with a capacity fixed when it is
created. Elements are added at the back and removed from the front. **Adding to a full buffer is
refused**: the call reports failure and changes nothing. It never overwrites an element, unlike many
ring buffers, including Boost's `circular_buffer`, which replace the oldest element when full. A
caller that wants to overwrite can remove the oldest element first; a buffer that overwrites by
itself cannot be made to refuse, and refusing is what the throttle needs. It records the position of
the oldest element and the number of elements held, which avoids the usual difficulty of telling a
full buffer from an empty one when read and write positions are equal.

It follows the conventions of the framework's other containers, such as `IncrementalRehashMap`:

- **Its storage is raw memory from `operator new`**, taken once when the buffer is created and never
  resized, using the aligned form of `operator new` when the element type needs more alignment than
  the default. Elements are constructed in place when added and destroyed when removed, or when the
  buffer itself is destroyed. So an element type need not have a default constructor, and no element
  exists in a slot that is not in use.
- **It can report its allocation** through an optional `AllocationGrowthReporter`, like the framework's
  other containers, so a component can account for the memory its buffers hold.
- **A misuse is refused with `PreconditionAssertion`**: a capacity of zero, and asking for or removing
  the front element of an empty buffer. Refusing to add to a full buffer is not a misuse, and is
  reported by the result of the call, which is marked `[[nodiscard]]`.
- **It cannot be copied, but it can be moved.** Copying would allocate, which the buffer otherwise
  does only when created, and nothing needs it. Moving passes the storage to the new owner without
  allocating, and leaves the buffer moved from with no storage and a capacity of zero.

**`RollingWindowThrottle`** enforces one limit. It holds a ring buffer of the times of the commands it
has accepted, with room for exactly N times. Asked whether a command arriving at time T may be
accepted, it first removes from the front every time that is one second or more before T. Times are
added in order, so these are always at the front. It then tries to add T. If the buffer is full, N
commands were accepted in the last second and the answer is no; otherwise T is recorded and the
answer is yes. Each time is added once and removed once, so the cost of a check does not depend on N.

**`SessionThrottles`** holds a session's three throttles. Each is absent when its limit is zero.

**Memory: everything is allocated up front, and nothing ever grows.** A session's throttles allocate
all the storage they will ever use when the session logs on, each sized for exactly its configured
limit. Nothing is allocated after that: checking a command, accepting it and rejecting it never use
the heap, and no throttle ever grows. A full buffer does not need to grow, because a full buffer is
the throttle refusing the command.

**The largest permitted limit is 100,000 commands per second for each kind.** The venue is a low
latency system, not an ultra-low latency one: processing an order from start to end takes tens of
microseconds, so one command every 10 microseconds is the fastest any member could usefully send.

Each throttle holds one 8-byte time for each command it allows, so its memory follows the limit
configured, not the largest permitted one:

| Limit for one kind | That throttle's memory | A session with all three at that limit |
|--------------------|------------------------|----------------------------------------|
| 50 | 400 bytes | 1.2 kilobytes |
| 10,000 | 80 kilobytes | 240 kilobytes |
| 100,000 | 800 kilobytes | 2.4 megabytes |

Memory is therefore large only when many sessions are given limits near the largest: a thousand
sessions at 100,000 for all three kinds would hold 2.4 gigabytes. Whoever provisions a member with a
very high limit should know that. If it ever matters, the times can be held as 4-byte offsets in
microseconds rather than 8-byte times, which halves it. A more typical session, with limits of 50,
10 and 50, holds 880 bytes.

**Threading.** Each gateway handles a session on one thread, so the throttles need no locking.

---

## 7. What operators can see

- **A metric for each gateway and kind of command:** how many commands have been rejected by a
  throttle: `throttled_new_orders_total`, `throttled_amends_total` and `throttled_cancels_total`,
  told apart by the component label (`fix_common/ThrottleRefusalMetrics.hpp`).
- **A log line when a session starts being throttled, and when it stops**, not one for each rejected
  command. A member sending far too fast would otherwise fill the log, and what an operator needs is
  that it happened and for how long. A session counts as throttled from its first rejected command
  until a command of that kind is next accepted, or until the session ends. The lines name the comp
  id, the session, the kind of command and the limit, and the second line says how many commands were
  refused.
- **A log line at every logon naming the limits applied**, including zeros, so an operator can see
  what a session was given and a value lost on the way shows up as a number.

The functions that send a refusal log each reply at Debug. Each caller logs its reason at the level
the reason deserves, so a line per reply at Info would only repeat it, once for every command a
throttled member sends.

---

## 8. Testing

**Unit tests**, with the time given by each test, so nothing waits. The ring buffer's are in
`libraries/pubsub_itc_fw/tests/FixedCapacityRingBufferTest.cpp`; the throttle's are in
`applications/fix_common/tests`, built as the program `fix_common_tests`. That is a program of its
own because the check that deciding about a command never uses the heap replaces the global
`operator new`, which would apply to every test linked with it.

- the ring buffer, in the framework's own tests (`FixedCapacityRingBufferTest.cpp`), thoroughly,
  because it is a general-purpose container that other code will rely on:
  - empty when created, with the capacity it was given; a capacity of zero is refused;
  - elements leave in the order they arrived;
  - adding to a full buffer fails and changes nothing, and after one element is removed exactly one
    more can be added;
  - correct after its positions have wrapped round the end of its storage many times, at every
    capacity from 1 upwards;
  - an element type with no default constructor works;
  - every element added is destroyed exactly once, whether it is removed or the buffer is destroyed
    holding it, checked with an element type that counts its constructions and destructions;
  - an element type that needs more than the default alignment is stored correctly aligned;
  - the allocation is reported once to an `AllocationGrowthReporter`, and nothing is reported or
    allocated by adding or removing elements;
  - asking for or removing the front of an empty buffer is refused;
- a throttle: exactly N commands within a second are accepted and command N+1 is rejected; one more
  is accepted once a second has passed since the first; a command exactly one second after the oldest
  is accepted; rejected commands do not count; correct after many wraps;
- a limit of zero creates no throttle, and every command is accepted;
- the three kinds of command are throttled independently, and so are two sessions;
- checking a command does not use the heap.

**An end-to-end test in `ha_test.py`, scenario 56,** run against the FIX gateway. It uses the comp id `THROTTLED`, a test fixture of its own, so its limits
never throttle the comp ids other tests use: a comp id provisioned with a small limit sends more commands
than the limit within a second, and the test requires the commands beyond the limit to be rejected
with the throttle's text, and the others accepted. It checks the limit the gateway actually applied,
not merely that something was rejected: a step on the route that dropped a value would leave the
session unlimited, or on some other number, and only a test that checks the number can tell. A second comp
id with a limit of zero sends the same burst, and nothing is rejected.

---

## 9. Open questions {#gateway_throttles_open}

None.

---

## See also

- [fix_order_gateway.md](fix_order_gateway.md) and [binary_order_gateway.md](binary_order_gateway.md)
- [gateway_ha.md](../availability/gateway_ha.md), for cancel-on-disconnect, whose route these limits follow
- [authentication_service.md](authentication_service.md) and [admin_service.md](admin_service.md)
