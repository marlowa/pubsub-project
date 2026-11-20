# Pools that grow without a lock {#pools_that_grow_without_a_lock}

**Status: agreed with Andrew, 20 November 2026, for [BUG-0127](../bug_list.md#bug_0127); the TLA+ model of section 6
is written and holds; the code is not yet built.** Two pieces
of work, neither a substitute for the other: the queues' pools are sized so that they do not grow during the day, and
any growth is reported as an error (section 4); and the lock is removed from growing, if and only if a TLA+ model
checked with TLC proves the lock-free version safe (sections 5 and 6). Section 8 lists the decisions.

## 1. The problem

Every queue between the reactor and an application thread is a `LockFreeMessageQueue`, and each takes the nodes that
carry its messages from an `ExpandablePoolAllocator`. The allocator hands out nodes from a chain of fixed-size pools.
While the pool in use has a free node, taking one is a compare-and-swap on that pool's free list, with no lock. When it
has none, the allocator takes a mutex, `expansion_mutex_`, looks through every pool in the chain for a free node, and,
if there is none, maps a new pool from the operating system, writes its free list, links it into the chain, and calls
the component's handler for an exhausted pool, which logs a Warning; all while holding the mutex. Every other thread
that wants a node from the same queue waits for it.

Growing is meant as a safeguard that is never needed: a pool sized for its queue does not run out. It does. In the run
of `ha_test.py` on 20 November 2026, scenario 116, in which a load client offers 1,800,000 orders on four sessions as
fast as the sockets accept them, the binary order gateway's command queue ran out of nodes 15 times in 118
milliseconds, from 19:48:07.394 to 19:48:07.512, each time adding a pool of 4,096 nodes:

```
BinaryOrderGatewayCommandPool exhausted: chaining new pool slab (4096 objects)
```

Sixteen pools hold 65,536 nodes; at its deepest the queue held 64,920 commands (section 2), in a queue sized for 4,096.

## 2. Why the pool was too small

The command queue holds what the gateway's thread asks the reactor to send, one node for each send
(`ApplicationThread::send_pdu`). For each order, with high availability on, the binary gateway sends twice: the order
to each sequencer (`BinaryOrderGatewayThread::forward_envelope_to_sequencers`). For each answered order it sends a
third time: the execution report to the member.

The gateway stops reading its members once 16,384 of its commands are unanswered (`[unanswered_commands] limit`,
[a_gateway_limits_its_unanswered_commands.md](../availability/a_gateway_limits_its_unanswered_commands.md)); in
scenario 116 it reached that limit 56 times. 16,384 unanswered orders can be 32,768 sends to the sequencers waiting for
the reactor, eight times the pool, before any execution report is counted. The pool's size,
4,096, predates the limit on unanswered commands, and was not changed when the limit was set.

Measured on 20 November 2026, with temporary counting in the reactor, the queue held 64,920 commands at its deepest:
29,992 orders for each sequencer and 4,936 execution reports. The orders for each sequencer exceed the gateway's limit
of 16,384 because the limit stops the gateway reading its members, but the orders already read are still forwarded;
nothing bounds how many those can be, [BUG-0279](../bug_list.md#bug_0279). So the command queue cannot be sized from
settings until that is bounded.

## 3. Which queues use the allocator

`ExpandablePoolAllocator` has one user, `LockFreeMessageQueue`, for its nodes. Each component has two kinds of queue:

- **Each application thread's queue**, which the reactor fills with what arrives on the thread's connections. The
  reactor stops reading a connection while too many of its messages wait
  ([bounding_messages_waiting.md](bounding_messages_waiting.md)), so these are bounded by connection.
- **The reactor's command queue**, which the component's threads fill with what they ask the reactor to do, sending
  above all. The queue itself has no bound, but in a gateway the limit on unanswered commands bounds the sends to the
  sequencers, as section 2 describes.

## 4. Sizing, and growth as an error

1. **Measure what the queue holds at its peak.** Done for the binary order gateway (section 2); the FIX order gateway,
   which sends each order to both sequencers in the same way, is to be measured the same way.
2. **Size each gateway's command queue from what bounds it:** two sends for each order it can have read but not yet
   answered, plus the reports in flight, with the figure and how it was reached in the configuration's comment, so that
   the settings cannot drift apart again. This needs [BUG-0279](../bug_list.md#bug_0279) first: today nothing bounds
   the orders read but not yet answered. Each other component's queues are
   checked against what bounds them in the same way.
3. **Growth is an error.** The handlers for an exhausted pool log at Error rather than Warning, and each allocator's
   count of pools added, which it already keeps (`expansion_events_`), is published as a metric, so that a queue that
   grows during the day is seen at once.

## 5. Growing without the lock

The mutex in the slow path guards four things. Each can be done safely without it:

1. **Looking through the chain for a pool with a free node.** Each pool's free list is already lock-free, a
   compare-and-swap stack checked by the TLA+ model `PoolFreeList` ([tla/findings.md](tla/findings.md)). Walking the
   chain reads each pool's link to the next, and a link changes once, from null to a new pool, because pools are never
   removed. `deallocate` already walks the chain without the lock, at the same time as pools are added.
2. **Making a pool the current one.** The current pool is only where to look first. Two threads that set it at once
   each set a pool that had a free node; either is correct.
3. **Adding a pool.** A thread that finds every pool full maps a pool and writes its free list, and only then links
   it, by a compare-and-swap on the last pool's link from null to its own pool. If two threads try at once, exactly one
   compare-and-swap succeeds; the other learns the winner's pool from its failed compare-and-swap, looks there for a
   node, and links its own pool after the winner's in the same way. Each pool is linked at most once, because only the
   thread that made it links it and a link from null succeeds once; the chain cannot form a cycle, because a link only
   ever points to a pool in no chain before; and the hazard known as ABA, a compare-and-swap fooled by a value that
   went and came back, cannot arise, because no link returns to null and no pool is freed or reused while the
   allocator lives.
4. **Handing out a node only from a pool in the chain.** A node handed out from a pool not yet linked could be freed by
   the consumer, whose `deallocate` would not find its pool. So a thread never takes a node from its own new pool before
   that pool is linked.

The slow path uses sequentially consistent atomic operations (`memory_order_seq_cst`) throughout. It is rare, so the
cost does not matter, and with them the C++ guarantees the single order of events that a TLA+ model assumes. The fast
path is unchanged.

The handler for an exhausted pool is called by the thread that added the pool, after adding it, holding no lock. Two
threads adding pools at once may call it at once; every handler in the venue only logs, which the logger allows.

## 6. Proving it: TLA+ first

Tests cannot show that code shared between threads has no race: they pass while a race remains. ThreadSanitizer cannot
check this code either, because the builds for Valgrind and ThreadSanitizer (`USING_VALGRIND`) replace the pool's
lock-free free list with a mutex, so that those tools can follow it, and so test different code.

So the lock-free slow path is modelled in TLA+ before any C++ is written, and checked with TLC, beside the existing
models in [tla/](tla/findings.md). The model has several threads allocating and freeing at once, pools of one or two
nodes so that every allocation can fill a pool, and the steps of section 5 as separate atomic actions. TLC checks every
interleaving of them within its bounds, at least three threads and four pools, not a sample of them. It checks these
invariants:

- the chain from its head is acyclic, and holds each pool at most once;
- every node handed out belongs to a pool in the chain, so that `deallocate` finds it;
- no node is handed out to two threads at once;
- every allocation either returns a node or adds a pool, so that no thread waits for another.

The model is also checked with a deliberate fault, a node handed out before its pool is linked, to show that TLC finds
it. If the model holds, the C++ follows it step for step, which is checked by reading it against the model. If it does
not hold, the lock stays.

**The result.** The model is `PoolChain.tla`. With three producers and four pools of one node each, TLC visits
11,711,383 distinct states, every one reachable within those bounds, and every invariant holds; with three producers
and three pools of two nodes each, so that a pool can be partly used while producers race past it, 5,067,730 states,
and every invariant holds. Each of two deliberate faults is found: a node handed out before its pool is linked breaks
`EveryNodeInTheChain`, and a pool linked by a plain store rather than a compare-and-swap breaks `NoPoolLost`. The
figures, and the steps TLC prints for each fault, are in [tla/findings.md](tla/findings.md), section 5.

## 7. What is not changed

`FixedSizeMemoryPool` is unchanged, and so is `deallocate`. The allocators for variable-sized blocks,
`ExpandableSlabAllocator`, are not part of this. No spare pool is kept: with the queues sized correctly, growing should
not happen, and the thread that grows a pool maps it itself, holding no lock.

## 8. Decisions

Agreed with Andrew, 20 November 2026:

1. Chaining pools during the day is wrong whether or not it takes a lock: the right size is to be found and set, and
   chaining that still happens is logged at Error and counted as a metric.
2. The lock is removed from growing only if a TLA+ model, checked with TLC, proves the lock-free version safe.
