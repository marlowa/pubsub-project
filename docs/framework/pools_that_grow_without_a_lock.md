# Pools that grow without a lock {#pools_that_grow_without_a_lock}

**Status: proposed for [BUG-0127](../bug_list.md#bug_0127), for agreement with Andrew before any code.** Section 8 lists
the decisions, each a separate yes-or-no question.

## 1. The problem

Every queue between the reactor and an application thread is a `LockFreeMessageQueue`, and each takes the nodes that
carry its messages from an `ExpandablePoolAllocator`. The allocator hands out nodes from a chain of fixed-size pools.
While the pool in use has a free node, taking one is a compare-and-swap on that pool's free list, with no lock. When it
has none, the allocator takes a mutex, `expansion_mutex_`, looks through every pool in the chain for a free node, and,
if there is none, maps a new pool from the operating system, writes its free list, links it into the chain, and calls
the component's handler for an exhausted pool, which logs a Warning; all while holding the mutex.

So when a pool runs out, the thread that wanted a node waits for the operating system and for the log call, and every
other thread that wants a node from the same queue waits for it on the mutex. A queue's producers are the threads that
send to it: for the reactor's command queue, every application thread that asks the reactor to send something; for an
application thread's queue, the reactor and any thread that sends to it. Principle F2 says nothing on the order path
takes a lock, and F5 that nothing on it allocates from the operating system during the day.

It happens. In the run of `ha_test.py` on 20 November 2026, scenario 116, in which orders are offered faster than the
venue answers them, the binary order gateway's command queue ran out of nodes 15 times in 118 milliseconds, from
19:48:07.394 to 19:48:07.512, each time adding a pool of 4,096 nodes:

```
BinaryOrderGatewayCommandPool exhausted: chaining new pool slab (4096 objects)
```

That is about 61,000 commands waiting at once for the reactor to act on them, against a queue sized at start for 4,096.
The time each addition took is not measured on its own: the 15 came about 8 milliseconds apart, which is an upper bound
on each, and every producer of that queue waited for each one.

## 2. Which queues use the allocator, and what bounds them

`ExpandablePoolAllocator` has one user in the venue, `LockFreeMessageQueue`, for its nodes. Its other former user, the
gateways' pool of open orders, was removed with [BUG-0072](../bug_list.md#bug_0072). Each component has two kinds of
queue:

- **Each application thread's queue**, which the reactor fills with what arrives on the thread's connections. Since
  [BUG-0234](../bug_list.md#bug_0234) the reactor stops reading a connection while too many of its messages wait
  ([bounding_messages_waiting.md](bounding_messages_waiting.md)), so these are bounded by connection.
- **The reactor's command queue**, which every application thread fills with what it asks the reactor to do, sending
  above all. Nothing bounds it: `enqueue` never fails, and the queue's watermarks only call a handler. It is the one that
  ran out in scenario 116.

Because the command queue has no bound, no initial size can promise that it never runs out. A pool that runs out must
therefore be able to grow without the lock.

## 3. What must hold

1. **No lock on the allocation path**, fast or slow.
2. **No thread waits for another.** A thread that wants a node never waits for a thread that is adding a pool.
3. **The operating system is asked for memory off the order path whenever it can be.** When it cannot, only the thread
   that needs the pool pays for it.
4. **Pools are never removed** while the allocator lives, as now, so that a thread walking the chain never reads a pool
   that has gone.
5. **Every node is still found by `deallocate`**, which walks the chain to find the pool a node belongs to.

## 4. The proposal

### 4.1 Looking through the chain without the lock

Pools are only ever added to the end of the chain, each pool's link to the next is read with an acquire load, and the
pool that is current is published with a release store. Looking through the chain for a pool with a free node is
therefore already safe without the lock, and so is making the pool found the current one: two threads that do it at
once both publish a pool that has free nodes, and either will do.

### 4.2 Adding a pool without the lock

The chain gets an atomic pointer to its last pool. A thread that has found every pool full adds one by a
compare-and-swap on the last pool's link to the next, from null to the new pool, and then moves the pointer to the last
pool on. If another thread added a pool first, the compare-and-swap fails; the thread then looks through the new pool
for a free node, and keeps its own pool as the spare of section 4.3, or links it after the other, so that nothing is
mapped for nothing.

### 4.3 A spare pool, ready in advance

Each allocator keeps one spare pool, mapped and with its free list written, held in an atomic pointer. A thread that
must add a pool takes the spare with one atomic exchange and links it in as in section 4.2, so that adding a pool costs
it a few atomic operations and no call to the operating system.

The spare is mapped again, off the order path, once it has been taken. Which thread does that is decision 3.

If the spare has been taken and not yet replaced, as when 15 pools are needed in 118 milliseconds, the thread that needs
a pool maps one itself, without the lock. It pays for the mapping; no other thread waits for it.

### 4.4 The handler for an exhausted pool

The handler is called by the thread that added the pool, after it has added it, and holds no lock. Two threads adding
pools at once may call it at once, so a handler must allow that. Every handler in the venue only logs, which the logger
allows.

### 4.5 The lock table

`expansion_mutex_` is removed, and with it its entry in the table of permitted locks in `scripts/check_standards.py`.

## 5. Sizing

The command queue's pools are sized by each component's configuration, `command_queue_pool_objects_per_slab` and
`command_queue_pool_initial_slabs`. With growth costing a few atomic operations, sizing is about memory, not about
pauses: a queue that grows during the day takes memory the day did not plan for, which
[BUG-0010](../bug_list.md#bug_0010) rules out. So each component's pools are to be sized for the commands it can have
waiting under the load it is tested with, and a growth during the day is to stay a Warning in the log and be counted, so
that it is seen.

The size needed is not yet known: scenario 116 needed about 61,000 for the binary order gateway. Whether the command
queue should have a bound of its own, as the inbound queues do, is a separate question, decision 5.

## 6. What it costs

- **Memory:** one spare pool for each queue, 4,096 nodes for the command queues as now configured.
- **Time on the order path:** none while the pool in use has nodes, as now. When it runs out, a few atomic operations
  where there is a spare, and the mapping, paid by one thread only, where there is not.
- **Code:** the chain's append, which is the delicate part. It is to be tested by many producers on pools of a few nodes,
  so that pools are added constantly, under ThreadSanitizer as well as in the ordinary build.

## 7. What is not changed

`FixedSizeMemoryPool`, the pool itself, is unchanged: its free list is lock-free already, except in builds for Valgrind
and ThreadSanitizer, which use a mutex so that those tools can follow it. `deallocate` is unchanged. The allocators for
variable-sized blocks, `ExpandableSlabAllocator`, are not part of this.

## 8. Decisions

Each a separate yes-or-no question:

1. Make adding a pool lock-free, as section 4.2 describes, removing `expansion_mutex_`?
2. Keep one spare pool ready in each allocator, as section 4.3 describes, so that adding a pool usually costs no call to
   the operating system?
3. Map the replacement spare on the reactor's thread, at its housekeeping tick, rather than on a new helper thread? The
   reactor's thread is on the order path, but it would map a pool only after one has been used, which in a day sized
   correctly is never; a new thread for something that should never happen goes against preferring an existing thread.
4. Model the append and the spare in TLA+ before the code, beside the models of the pools' free list and the slab
   allocators ([tla/findings.md](tla/findings.md)), since a lock-free append is where such code goes wrong?
5. Record the reactor's command queue having no bound as a bug of its own, to be designed separately, rather than
   bounding it here?
