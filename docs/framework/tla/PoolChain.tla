------------------------------- MODULE PoolChain -------------------------------
(***************************************************************************)
(* A specification of the chain of pools in ExpandablePoolAllocator, with  *)
(* pools added without a lock, as docs/framework/                          *)
(* pools_that_grow_without_a_lock.md (section 5) describes, to show that   *)
(* the lock can be removed before any code is written.                     *)
(*                                                                         *)
(* The allocator hands out nodes from a chain of fixed-size pools. Each    *)
(* pool's own free list is taken as one atomic step that gives a free node *)
(* of that pool or says there is none: that free list is lock-free         *)
(* already, and PoolFreeList.tla checks it. What this specification checks *)
(* is the chain around the pools.                                          *)
(*                                                                         *)
(* Producers allocate. A producer first tries the current pool. If it is   *)
(* full, it walks the chain from its head, one link at a time, trying each *)
(* pool. At the last pool, if every pool was full, it creates a pool of    *)
(* its own, all of whose nodes are free, and links it by a                 *)
(* compare-and-swap of the last pool's link from null to its own pool. If  *)
(* another producer linked a pool there first, the compare-and-swap fails, *)
(* and the producer walks on from the pool now there to the end of the     *)
(* chain and tries again, so that every pool it creates is linked. Only    *)
(* once its pool is linked does it take a node from the chain.             *)
(*                                                                         *)
(* One consumer frees nodes, as the thread that dequeues does: for each    *)
(* node it walks the chain from its head, one link at a time, until it     *)
(* finds the pool the node belongs to, and returns the node to that pool.  *)
(* It runs while producers add pools, as deallocate does.                  *)
(*                                                                         *)
(* Every read and write of a link, of the current pool and of a pool's    *)
(* free list is one atomic step, and the steps of all threads interleave  *)
(* in every possible order, which is what sequentially consistent atomic  *)
(* operations (memory_order_seq_cst) guarantee in C++. The slow path is to *)
(* use those, so that the C++ means what this specification means.         *)
(*                                                                         *)
(* Two faults are to be found by TLC, each in its own configuration:       *)
(*   HandOutBeforeLink = TRUE: a producer takes a node from its own new    *)
(*     pool before linking the pool, so that the consumer can be given a   *)
(*     node whose pool is not in the chain.                                *)
(*   LinkByCompareAndSwap = FALSE: a producer links its pool by a plain    *)
(*     store, so that a pool another producer linked a moment before can   *)
(*     be cut out of the chain.                                            *)
(*                                                                         *)
(* No step waits for another thread, so no thread can wait for another.   *)
(* There is no fairness or progress property: a producer may go on        *)
(* finding that another producer linked first. The number of pools is    *)
(* bounded by MaxPools;                                                     *)
(* a producer that would need more stops, which is a limit of the model,  *)
(* not of the code.                                                        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Producers,             \* the producer threads
    MaxPools,              \* the most pools that may exist, the first made at the start
    NodesPerPool,          \* nodes in each pool
    MaxQueued,             \* the most nodes allocated and not yet freed
    HandOutBeforeLink,     \* the fault: take a node from the new pool before linking it
    LinkByCompareAndSwap   \* TRUE as designed; FALSE is the fault of linking by a plain store

Null == 0
Pools == 1..MaxPools
Nodes == Pools \X (1..NodesPerPool)
NoNode == <<0, 0>>

VARIABLES
    created,     \* how many pools exist; pools 1..created
    link,        \* each pool's link to the next, or Null
    free,        \* each pool's free nodes
    current,     \* the pool producers try first
    pc,          \* where each producer is
    cursor,      \* the pool each producer is at
    mine,        \* the pool each producer made and has not yet linked, or Null
    early,       \* a node taken from a producer's own pool before it was linked (the fault)
    queued,      \* nodes allocated and not yet freed, waiting for the consumer
    cpc,         \* where the consumer is
    cnode,       \* the node the consumer is freeing
    ccursor,     \* the pool the consumer's walk is at
    lostNode,    \* the consumer walked the whole chain without finding a node's pool
    takenTwice,  \* a node was handed out while already handed out
    peak         \* the most nodes ever handed out and not yet freed at once

vars == <<created, link, free, current, pc, cursor, mine, early, queued, cpc, cnode, ccursor, lostNode, takenTwice, peak>>

ProducerSteps == {"idle", "try_current", "walk", "create", "link", "to_end", "stopped"}
ConsumerSteps == {"idle", "walk"}

PoolOf(n) == n[1]

\* The pools reachable from the head, pool 1, and whether the walk meets a pool twice.
RECURSIVE Walk(_, _)
Walk(p, visited) ==
    IF p = Null THEN [pools |-> visited, cycle |-> FALSE]
    ELSE IF p \in visited THEN [pools |-> visited, cycle |-> TRUE]
    ELSE Walk(link[p], visited \cup {p})

Chain == Walk(1, {})

TypeOK ==
    /\ created \in Pools
    /\ link \in [Pools -> Pools \cup {Null}]
    /\ free \in [Pools -> SUBSET Nodes]
    /\ current \in Pools
    /\ pc \in [Producers -> ProducerSteps]
    /\ cursor \in [Producers -> Pools]
    /\ mine \in [Producers -> Pools \cup {Null}]
    /\ early \in [Producers -> Nodes \cup {NoNode}]
    /\ queued \in SUBSET Nodes
    /\ cpc \in ConsumerSteps
    /\ cnode \in Nodes \cup {NoNode}
    /\ ccursor \in Pools \cup {Null}
    /\ lostNode \in BOOLEAN
    /\ takenTwice \in BOOLEAN
    /\ peak \in Nat

\* One pool at the start, every node free.
Init ==
    /\ created = 1
    /\ link = [p \in Pools |-> Null]
    /\ free = [p \in Pools |-> IF p = 1 THEN {p} \X (1..NodesPerPool) ELSE {}]
    /\ current = 1
    /\ pc = [t \in Producers |-> "idle"]
    /\ cursor = [t \in Producers |-> 1]
    /\ mine = [t \in Producers |-> Null]
    /\ early = [t \in Producers |-> NoNode]
    /\ queued = {}
    /\ cpc = "idle"
    /\ cnode = NoNode
    /\ ccursor = Null
    /\ lostNode = FALSE
    /\ takenTwice = FALSE
    /\ peak = 0

\* Handing out node n: it joins the queue. Taking a node already queued is the fault TLC must not find.
HandOut(n) ==
    /\ takenTwice' = (takenTwice \/ n \in queued)
    /\ queued' = queued \cup {n}

ProducerUnchanged == UNCHANGED <<cpc, cnode, ccursor, lostNode>>

(***************************************************************************)
(* A producer allocates.                                                   *)
(***************************************************************************)
Begin(t) ==
    /\ pc[t] = "idle"
    /\ Cardinality(queued) < MaxQueued
    /\ pc' = [pc EXCEPT ![t] = "try_current"]
    /\ UNCHANGED <<created, link, free, current, cursor, mine, early, queued, takenTwice>>
    /\ ProducerUnchanged

\* The fast path: one atomic take from the current pool's free list.
TryCurrent(t) ==
    /\ pc[t] = "try_current"
    /\ IF free[current] # {}
       THEN \E n \in free[current] :
                /\ free' = [free EXCEPT ![current] = @ \ {n}]
                /\ HandOut(n)
                /\ pc' = [pc EXCEPT ![t] = "idle"]
                /\ UNCHANGED cursor
       ELSE /\ pc' = [pc EXCEPT ![t] = "walk"]
            /\ cursor' = [cursor EXCEPT ![t] = 1]
            /\ UNCHANGED <<free, queued, takenTwice>>
    /\ UNCHANGED <<created, link, current, mine, early>>
    /\ ProducerUnchanged

\* The slow path: at the pool it has reached, take a free node and make that pool current; or read its link and move
\* on; or, at the end of the chain, go on to make a pool.
WalkStep(t) ==
    /\ pc[t] = "walk"
    /\ LET p == cursor[t] IN
       IF free[p] # {}
       THEN \E n \in free[p] :
                /\ free' = [free EXCEPT ![p] = @ \ {n}]
                /\ HandOut(n)
                /\ current' = p
                /\ pc' = [pc EXCEPT ![t] = "idle"]
                /\ UNCHANGED cursor
       ELSE IF link[p] # Null
            THEN /\ cursor' = [cursor EXCEPT ![t] = link[p]]
                 /\ UNCHANGED <<free, queued, takenTwice, current, pc>>
            ELSE /\ pc' = [pc EXCEPT ![t] = "create"]
                 /\ UNCHANGED <<free, queued, takenTwice, current, cursor>>
    /\ UNCHANGED <<created, link, mine, early>>
    /\ ProducerUnchanged

\* Making a pool: all its nodes free, linked to nothing, known only to this producer. Under the fault, the producer
\* also takes a node from it now, before linking it.
Create(t) ==
    /\ pc[t] = "create"
    /\ IF created < MaxPools
       THEN LET m == created + 1 IN
            /\ created' = m
            /\ mine' = [mine EXCEPT ![t] = m]
            /\ IF HandOutBeforeLink
               THEN /\ free' = [free EXCEPT ![m] = ({m} \X (1..NodesPerPool)) \ {<<m, 1>>}]
                    /\ early' = [early EXCEPT ![t] = <<m, 1>>]
               ELSE /\ free' = [free EXCEPT ![m] = {m} \X (1..NodesPerPool)]
                    /\ UNCHANGED early
            /\ pc' = [pc EXCEPT ![t] = "link"]
       ELSE /\ pc' = [pc EXCEPT ![t] = "stopped"]
            /\ UNCHANGED <<created, mine, free, early>>
    /\ UNCHANGED <<link, current, cursor, queued, takenTwice>>
    /\ ProducerUnchanged

\* Under the fault, the node taken early is handed out before the pool is linked.
HandOutEarly(t) ==
    /\ HandOutBeforeLink
    /\ early[t] # NoNode
    /\ HandOut(early[t])
    /\ early' = [early EXCEPT ![t] = NoNode]
    /\ UNCHANGED <<created, link, free, current, pc, cursor, mine>>
    /\ ProducerUnchanged

\* Linking: a compare-and-swap of the link of the pool the producer is at, from null to its own pool. On success the
\* producer walks on into its pool to take a node; on failure it learns the pool now linked there and walks on to the
\* end of the chain, keeping its pool, to link it there. Under the fault, a plain store links it whatever was there.
Link(t) ==
    /\ pc[t] = "link"
    /\ LET p == cursor[t] IN
       IF ~LinkByCompareAndSwap \/ link[p] = Null
       THEN /\ link' = [link EXCEPT ![p] = mine[t]]
            /\ cursor' = [cursor EXCEPT ![t] = mine[t]]
            /\ mine' = [mine EXCEPT ![t] = Null]
            /\ pc' = [pc EXCEPT ![t] = "walk"]
       ELSE /\ cursor' = [cursor EXCEPT ![t] = link[p]]
            /\ pc' = [pc EXCEPT ![t] = "to_end"]
            /\ UNCHANGED <<link, mine>>
    /\ UNCHANGED <<created, free, current, early, queued, takenTwice>>
    /\ ProducerUnchanged

\* After finding that another producer linked a pool first: walk to the last pool, one link at a time, then try to link
\* there again.
ToEnd(t) ==
    /\ pc[t] = "to_end"
    /\ LET p == cursor[t] IN
       IF link[p] # Null
       THEN /\ cursor' = [cursor EXCEPT ![t] = link[p]]
            /\ UNCHANGED pc
       ELSE /\ pc' = [pc EXCEPT ![t] = "link"]
            /\ UNCHANGED cursor
    /\ UNCHANGED <<created, link, free, current, mine, early, queued, takenTwice>>
    /\ ProducerUnchanged

(***************************************************************************)
(* The consumer frees a node, finding its pool by walking the chain.       *)
(***************************************************************************)
ConsumerBegin ==
    /\ cpc = "idle"
    /\ \E n \in queued :
        /\ cnode' = n
        /\ queued' = queued \ {n}
        /\ ccursor' = 1
        /\ cpc' = "walk"
    /\ UNCHANGED <<created, link, free, current, pc, cursor, mine, early, lostNode, takenTwice>>

ConsumerWalk ==
    /\ cpc = "walk"
    /\ IF ccursor = PoolOf(cnode)
       THEN /\ free' = [free EXCEPT ![ccursor] = @ \cup {cnode}]
            /\ cpc' = "idle"
            /\ cnode' = NoNode
            /\ ccursor' = Null
            /\ UNCHANGED lostNode
       ELSE IF link[ccursor] # Null
            THEN /\ ccursor' = link[ccursor]
                 /\ UNCHANGED <<free, cpc, cnode, lostNode>>
            ELSE \* The end of the chain without finding the node's pool: deallocate would refuse it.
                 /\ lostNode' = TRUE
                 /\ cpc' = "idle"
                 /\ cnode' = NoNode
                 /\ ccursor' = Null
                 /\ UNCHANGED free
    /\ UNCHANGED <<created, link, current, pc, cursor, mine, early, queued, takenTwice>>

\* peak follows the nodes in use; no action above mentions it.
Next ==
    /\ \/ \E t \in Producers : Begin(t) \/ TryCurrent(t) \/ WalkStep(t) \/ Create(t) \/ HandOutEarly(t) \/ Link(t) \/ ToEnd(t)
       \/ ConsumerBegin
       \/ ConsumerWalk
    /\ peak' = IF Cardinality(queued') > peak THEN Cardinality(queued') ELSE peak

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* What must hold in every reachable state.                                *)
(***************************************************************************)

\* The chain from its head never meets a pool twice.
ChainAcyclic == ~Chain.cycle

\* No pool is the link of two pools, and no pool links to pool 1, the head.
LinkedAtMostOnce ==
    /\ \A p, q \in Pools : (p # q /\ link[p] # Null) => link[p] # link[q]
    /\ \A p \in Pools : link[p] # 1

\* Every node handed out and not yet freed, and every node being freed, belongs to a pool in the chain, so that
\* deallocate finds it.
EveryNodeInTheChain ==
    \A n \in queued \cup (IF cnode = NoNode THEN {} ELSE {cnode}) : PoolOf(n) \in Chain.pools

\* The consumer never fails to find a node's pool.
NeverLost == ~lostNode

\* No node is handed out while it is already handed out, and no node is both free and handed out.
NeverTakenTwice ==
    /\ ~takenTwice
    /\ \A p \in Pools : free[p] \cap queued = {}

\* Every pool made is in the chain, or is the pool its maker has yet to link: no pool is made and then lost.
NoPoolLost ==
    \A p \in 2..created : p \in Chain.pools \/ \E t \in Producers : mine[t] = p

\* How much the chain can grow beyond what was needed. Producers that find the chain full at the same moment may each
\* add a pool, so more pools can be added than the most nodes ever in use needed: but never more nodes, in all the pools
\* made, than the first pool, plus the most nodes ever in use at once, plus one pool for each producer.
PoolsBounded ==
    created * NodesPerPool <= NodesPerPool + peak + NodesPerPool * Cardinality(Producers)

\* The tighter claim, that no more pools are made than the most nodes ever in use needed: checked only to show, by the
\* steps TLC prints when it breaks it, how producers arriving at once add a pool more than was needed.
NoPoolBeyondNeed ==
    created * NodesPerPool <= NodesPerPool + peak + (NodesPerPool - 1)

=============================================================================
