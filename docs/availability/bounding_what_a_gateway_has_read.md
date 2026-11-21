# Bounding what a gateway has read and not yet answered {#bounding_what_a_gateway_has_read}

**Status: agreed with Andrew, 20 November 2026, for [BUG-0279](../bug_list.md#bug_0279); not yet built.** Option 3.1:
each member's connection to the binary gateway may have a set number of messages waiting, and each gateway instance
serves at most a set number of members. Section 6 lists the decisions.

## 1. The problem

A gateway stops reading its members' connections once 16,384 of its commands are unanswered
([a_gateway_limits_its_unanswered_commands.md](a_gateway_limits_its_unanswered_commands.md)), and its configuration says
that this bounds every queue between the gateway and the matching engine. It does not. When the gateway stops reading,
the orders the reactor has already read from the members' connections, and queued for the gateway's thread, are still
taken off the queue and forwarded to the sequencers. Nothing limits how many those can be.

Measured on 20 November 2026, `ha_test.py` scenario 116, a load client offering 1,800,000 orders on four sessions as fast
as the sockets accept them: 29,992 orders were waiting to be sent to each sequencer at once, 13,608 more than the limit,
about 3,400 for each member, and with the execution reports going back to the members the binary order gateway's
command queue held 64,920 commands, in a queue sized for 4,096
([pools_that_grow_without_a_lock.md](../framework/pools_that_grow_without_a_lock.md), section 2).

Until the orders read but not yet answered have a bound that settings state, the gateway's command queue cannot be
sized: whatever size is chosen, more members, or members sending faster, can exceed it, and the queue then grows during
the day, which [BUG-0010](../bug_list.md#bug_0010) rules out.

## 2. What already bounds each gateway's reading

- **The binary order gateway** reads its members' connections as framework messages. The framework can limit how many
  messages from one connection may wait in a thread's queue, with the reactor not reading that connection while that
  many wait ([bounding_messages_waiting.md](../framework/bounding_messages_waiting.md), `limit_messages_waiting`). The
  matching engine and the sequencer use it; the binary gateway does not, so its members' connections are read as fast as
  the network delivers.
- **The FIX order gateway** reads its members' connections as raw bytes, into a buffer of 65,536 bytes for each
  connection (`raw_buffer_capacity`), and stops reading a connection when its buffer is three-quarters full. At 150 to
  250 bytes an order, that is a few hundred orders for each member at most. So the FIX gateway's excess is bounded for
  each member already; it has not been measured.
- **Neither gateway limits how many members may be connected to it at once.** So a bound for each member does not by
  itself give a bound for the gateway.

## 3. The options

### 3.1 Limit what each member's connection may have waiting, and how many members a gateway serves

The binary gateway asks the framework to stop reading a member's connection while a set number of its messages, for
example 64, wait in the gateway's queue, as the matching engine and the sequencer already do. The FIX gateway keeps its
buffer as it is. Each gateway instance gets a setting for the most members it serves at once, and refuses a logon beyond
it with a reason the member can act on.

The orders a gateway can have read and not yet answered are then at most the limit on unanswered commands, plus, for
each member it serves, what that member's connection may have waiting: every term a setting. For the binary gateway with
the present limit of 16,384, 64 messages for each member and, for example, 500 members, that is 16,384 + 32,000 =
48,384 orders, and the command queue is sized for two sends of each, one to each sequencer, plus the reports in flight.

- **Gains:** no order is refused for this; it uses a mechanism already built and already in use; the bound is stated in
  settings and can be checked at start.
- **Costs:** the command queue is sized for the most members, not for the usual number, which costs memory, to be
  measured; a limit on members per gateway is a new setting and a new reason for refusing a logon, and how many
  members each gateway instance is to serve has to be decided for each environment.

### 3.2 Refuse an order read beyond the limit

The gateway's thread counts an order as unanswered when it takes it off its queue, not when it sends it. An order taken
while the count is at the limit is not forwarded: it is refused, with a reason saying the venue is busy, as orders
beyond a member's throttle already are.

The orders sent and not yet answered are then never more than the limit, exactly, whatever the number of members, and
the command queue is sized from the limit alone.

- **Gains:** an exact bound from one setting, for both gateways, whatever the number of members.
- **Costs:** under overload, members' orders are refused that today are accepted late. Scenario 116 expects every order
  acknowledged, and would expect refusals instead. A member must be ready to send again an order refused this way.

### 3.3 Both

Option 3.1, so that the excess is small and refusals are rare, and option 3.2 as the bound that always holds.

## 4. Recommendation

Option 3.1 first. It refuses no order, uses what the framework already does, and states the bound in settings, which is
what sizing the command queue needs. Option 3.2 changes what a member is told under overload, which is a question of the
venue's behaviour towards members rather than of its memory, and is better decided on its own if 3.1 proves not enough.

## 5. What it costs to check

- Scenario 116 run again, with the reactor's count of pools added (`reactor_command_queue_pools_added`,
  [pools_that_grow_without_a_lock.md](../framework/pools_that_grow_without_a_lock.md), section 4) required to stay at
  zero, and the command queue's peak measured as before.
- The same for the FIX gateway, whose excess has not been measured.
- A scenario with more members than a gateway instance is to serve, showing the logon beyond the limit refused.

## 6. Decisions

Agreed with Andrew, 20 November 2026, each yes:

1. The messages waiting from each member's connection to the binary gateway are limited, as option 3.1 describes.
2. Each gateway instance has a setting for the most members it serves at once, and refuses a logon beyond it with a
   reason the member can act on.
3. Refusing orders read beyond the limit, option 3.2, is left for later, to be decided only if option 3.1 proves not
   enough.
