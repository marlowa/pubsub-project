------------------------------ MODULE SlabReclaim ------------------------------
(***************************************************************************)
(* A specification of how the slab allocator gives back a slab once every  *)
(* chunk allocated from it has been freed: SlabAllocator and               *)
(* ExpandableSlabAllocator in libraries/pubsub_itc_fw.                     *)
(*                                                                         *)
(* One thread, the owner, allocates. It takes chunks from its current      *)
(* slab, one after another, and when the current slab has no room left it  *)
(* makes a new slab current. Any thread may free a chunk. A slab that is   *)
(* no longer current and has no chunk outstanding must be given back: its  *)
(* node is put on a queue (EmptySlabQueue), and the owner, each time it    *)
(* allocates, first takes every node from the queue and destroys the       *)
(* slabs, keeping back the last one taken until the next time (the queue   *)
(* still uses that node). The current slab is never put on the queue: when *)
(* it has no chunk outstanding, the owner starts again from its beginning. *)
(*                                                                         *)
(* The question is who puts a slab on the queue, and whether that thread   *)
(* can touch the slab after the owner has destroyed it. Two designs are    *)
(* modelled, chosen by the constant Design:                                *)
(*                                                                         *)
(*  "before_bug_0157": each slab counts its outstanding chunks. A thread   *)
(*      freeing a chunk takes the count down; if it took it to zero, it    *)
(*      reads whether the slab is current and, if not, claims the right to *)
(*      put it on the queue with a compare-and-exchange, and does. The     *)
(*      owner, making the slab no longer current, clears its flag, reads   *)
(*      the count and, if it is zero, makes the same claim. The claim lets *)
(*      only one of them put it on the queue.                              *)
(*                                                                         *)
(*  "now": the count holds the chunks outstanding plus one, held by the    *)
(*      owner for as long as the slab is current. A thread freeing a chunk *)
(*      takes the count down, and the owner, making the slab no longer     *)
(*      current, gives up its one. Whichever takes the count to zero puts  *)
(*      the slab on the queue. There is no flag to read and no claim.      *)
(*                                                                         *)
(* docs/bug_list.md, BUG-0157, describes the fault in the first design:    *)
(* a freeing thread that took the count to zero and was then held up could *)
(* read the slab after the owner had claimed it, queued it, taken it, and  *)
(* destroyed it. TLC must find that fault with Design = "before_bug_0157", *)
(* which shows this specification can find it, and must find nothing with *)
(* Design = "now".                                                         *)
(*                                                                         *)
(* Each step of a thread is one atomic action, so every interleaving of    *)
(* the threads is explored. A step that reads or writes a slab, its count, *)
(* its flags or its queue node "touches" it; a touch of a slab already     *)
(* destroyed sets useAfterFree.                                            *)
(*                                                                         *)
(* Each chunk is freed by a process of its own, which stands for whichever *)
(* thread frees it: what matters is only that frees interleave with each   *)
(* other and with the owner in every possible way. A slab holds Capacity   *)
(* chunks. Slabs are numbered in the order they are made; the allocator    *)
(* reuses the slot of a destroyed slab, but a new slab in that slot is a   *)
(* new object, and a stale handle is refused by the slot's generation      *)
(* count, which is not modelled.                                           *)
(*                                                                         *)
(* Not modelled: the steps inside the queue, which QueueOfEmptySlabs.tla   *)
(* checks; here putting a node on the queue and taking one off are each    *)
(* one step. The memory orderings of the C++ atomics: every thread sees    *)
(* every write in one order.                                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Design,      \* "before_bug_0157" or "now"
    MaxChunks,   \* bound on the chunks allocated in one behaviour
    MaxSlabs,    \* bound on the slabs made in one behaviour
    Capacity     \* chunks one slab holds

ASSUME Design \in {"before_bug_0157", "now"}

Chunks == 1..MaxChunks
Slabs == 1..MaxSlabs
NoSlab == 0

VARIABLES
    ownerStep,       \* where the owner is in allocating
    current,         \* the owner's current slab
    made,            \* how many slabs have been made
    room,            \* chunks left in each slab before it is full
    count,           \* each slab's count: chunks outstanding, plus the owner's one in "now"
    isCurrent,       \* each slab's flag saying it is current
    claimed,         \* each slab's claim to be put on the queue ("before_bug_0157" only)
    queue,           \* the slabs on the queue, oldest first
    taken,           \* the slabs taken off the queue in the drain in progress
    keptBack,        \* the slab kept back from the last drain, or NoSlab
    destroyed,       \* which slabs have been destroyed
    allocated,       \* how many chunks have been allocated
    chunkSlab,       \* the slab each chunk came from, or NoSlab before it is allocated
    chunkStep,       \* where each chunk's freeing is
    timesQueued,     \* how many times each slab has been put on the queue
    useAfterFree,    \* a slab was touched after it was destroyed
    currentReclaimed \* the current slab was taken off the queue to be destroyed

vars == <<ownerStep, current, made, room, count, isCurrent, claimed, queue, taken, keptBack, destroyed,
          allocated, chunkSlab, chunkStep, timesQueued, useAfterFree, currentReclaimed>>

OwnerSteps == {"start", "drain", "check", "allocate", "clear_current", "read_count", "claim",
               "give_up_hold", "put_on_queue", "make_slab", "out_of_slabs"}
ChunkSteps == {"unallocated", "held", "read_current", "claim", "put_on_queue", "done"}

\* The value useAfterFree takes when a step touches slab s.
Touch(s) == useAfterFree' = (useAfterFree \/ destroyed[s])

TypeOK ==
    /\ ownerStep \in OwnerSteps
    /\ current \in Slabs
    /\ made \in Slabs
    /\ room \in [Slabs -> 0..Capacity]
    /\ count \in [Slabs -> -MaxChunks..(MaxChunks + 1)]
    /\ isCurrent \in [Slabs -> BOOLEAN]
    /\ claimed \in [Slabs -> BOOLEAN]
    /\ queue \in Seq(Slabs)
    /\ taken \in Seq(Slabs)
    /\ keptBack \in Slabs \cup {NoSlab}
    /\ destroyed \in [Slabs -> BOOLEAN]
    /\ allocated \in 0..MaxChunks
    /\ chunkSlab \in [Chunks -> Slabs \cup {NoSlab}]
    /\ chunkStep \in [Chunks -> ChunkSteps]
    /\ timesQueued \in [Slabs -> Nat]
    /\ useAfterFree \in BOOLEAN
    /\ currentReclaimed \in BOOLEAN

\* Slab 1 is made current when the allocator is made.
Init ==
    /\ ownerStep = "start"
    /\ current = 1
    /\ made = 1
    /\ room = [s \in Slabs |-> Capacity]
    /\ count = [s \in Slabs |-> IF s = 1 /\ Design = "now" THEN 1 ELSE 0]
    /\ isCurrent = [s \in Slabs |-> s = 1]
    /\ claimed = [s \in Slabs |-> FALSE]
    /\ queue = <<>>
    /\ taken = <<>>
    /\ keptBack = NoSlab
    /\ destroyed = [s \in Slabs |-> FALSE]
    /\ allocated = 0
    /\ chunkSlab = [c \in Chunks |-> NoSlab]
    /\ chunkStep = [c \in Chunks |-> "unallocated"]
    /\ timesQueued = [s \in Slabs |-> 0]
    /\ useAfterFree = FALSE
    /\ currentReclaimed = FALSE

(***************************************************************************)
(* The owner. Each allocation first drains the queue (drain_empty_slab_    *)
(* queue), then starts the current slab again if nothing is outstanding    *)
(* from it, then takes a chunk, making a new slab current first if the     *)
(* current one is full (append_new_slab). Once MaxChunks are allocated the *)
(* owner goes on draining, so that slabs emptied later are given back.     *)
(***************************************************************************)
OwnerStart ==
    /\ ownerStep = "start"
    /\ ownerStep' = "drain"
    /\ taken' = <<>>
    /\ UNCHANGED <<current, made, room, count, isCurrent, claimed, queue, keptBack, destroyed,
                   allocated, chunkSlab, chunkStep, timesQueued, useAfterFree, currentReclaimed>>

\* Take the oldest node off the queue. Taking it reads the node, which is part of the slab.
OwnerTakeFromQueue ==
    /\ ownerStep = "drain"
    /\ queue /= <<>>
    /\ taken' = Append(taken, Head(queue))
    /\ queue' = Tail(queue)
    /\ Touch(Head(queue))
    /\ UNCHANGED <<ownerStep, current, made, room, count, isCurrent, claimed, keptBack, destroyed,
                   allocated, chunkSlab, chunkStep, timesQueued, currentReclaimed>>

\* The queue is empty: destroy what was taken, keeping back the last slab taken until the next drain,
\* and destroying the one kept back from the drain before.
OwnerFinishDrain ==
    LET last == IF taken = <<>> THEN NoSlab ELSE taken[Len(taken)]
        reclaim == IF taken = <<>> THEN {}
                   ELSE {taken[i] : i \in 1..(Len(taken) - 1)} \cup (IF keptBack = NoSlab THEN {} ELSE {keptBack})
    IN
    /\ ownerStep = "drain"
    /\ queue = <<>>
    /\ keptBack' = IF taken = <<>> THEN keptBack ELSE last
    /\ destroyed' = [s \in Slabs |-> destroyed[s] \/ s \in reclaim]
    /\ currentReclaimed' = (currentReclaimed \/ current \in reclaim)
    /\ ownerStep' = "check"
    /\ UNCHANGED <<current, made, room, count, isCurrent, claimed, queue, taken, allocated, chunkSlab,
                   chunkStep, timesQueued, useAfterFree>>

\* Nothing outstanding from the current slab: start it again from its beginning.
OwnerCheck ==
    LET outstanding == IF Design = "now" THEN count[current] - 1 ELSE count[current]
    IN
    /\ ownerStep = "check"
    /\ ownerStep' = IF allocated = MaxChunks THEN "start" ELSE "allocate"
    /\ room' = IF allocated < MaxChunks /\ outstanding = 0 THEN [room EXCEPT ![current] = Capacity] ELSE room
    /\ Touch(current)
    /\ UNCHANGED <<current, made, count, isCurrent, claimed, queue, taken, keptBack, destroyed,
                   allocated, chunkSlab, chunkStep, timesQueued, currentReclaimed>>

OwnerAllocate ==
    LET c == allocated + 1
    IN
    /\ ownerStep = "allocate"
    /\ IF room[current] > 0
       THEN /\ chunkSlab' = [chunkSlab EXCEPT ![c] = current]
            /\ chunkStep' = [chunkStep EXCEPT ![c] = "held"]
            /\ count' = [count EXCEPT ![current] = @ + 1]
            /\ room' = [room EXCEPT ![current] = @ - 1]
            /\ allocated' = c
            /\ ownerStep' = "start"
       ELSE /\ ownerStep' = "clear_current"
            /\ UNCHANGED <<chunkSlab, chunkStep, count, room, allocated>>
    /\ Touch(current)
    /\ UNCHANGED <<current, made, isCurrent, claimed, queue, taken, keptBack, destroyed, timesQueued,
                   currentReclaimed>>

\* The current slab is full: it stops being current.
OwnerClearCurrent ==
    /\ ownerStep = "clear_current"
    /\ isCurrent' = [isCurrent EXCEPT ![current] = FALSE]
    /\ ownerStep' = IF Design = "now" THEN "give_up_hold" ELSE "read_count"
    /\ Touch(current)
    /\ UNCHANGED <<current, made, room, count, claimed, queue, taken, keptBack, destroyed, allocated,
                   chunkSlab, chunkStep, timesQueued, currentReclaimed>>

\* "before_bug_0157": if nothing is outstanding, no freeing thread will put the slab on the queue later,
\* so the owner claims it.
OwnerReadCount ==
    /\ ownerStep = "read_count"
    /\ ownerStep' = IF count[current] = 0 THEN "claim" ELSE "make_slab"
    /\ Touch(current)
    /\ UNCHANGED <<current, made, room, count, isCurrent, claimed, queue, taken, keptBack, destroyed,
                   allocated, chunkSlab, chunkStep, timesQueued, currentReclaimed>>

OwnerClaim ==
    /\ ownerStep = "claim"
    /\ IF ~claimed[current]
       THEN /\ claimed' = [claimed EXCEPT ![current] = TRUE]
            /\ ownerStep' = "put_on_queue"
       ELSE /\ ownerStep' = "make_slab"
            /\ UNCHANGED claimed
    /\ Touch(current)
    /\ UNCHANGED <<current, made, room, count, isCurrent, queue, taken, keptBack, destroyed, allocated,
                   chunkSlab, chunkStep, timesQueued, currentReclaimed>>

\* "now": give up the owner's one. If that takes the count to zero, the owner puts the slab on the queue.
OwnerGiveUpHold ==
    /\ ownerStep = "give_up_hold"
    /\ count' = [count EXCEPT ![current] = @ - 1]
    /\ ownerStep' = IF count[current] = 1 THEN "put_on_queue" ELSE "make_slab"
    /\ Touch(current)
    /\ UNCHANGED <<current, made, room, isCurrent, claimed, queue, taken, keptBack, destroyed, allocated,
                   chunkSlab, chunkStep, timesQueued, currentReclaimed>>

OwnerPutOnQueue ==
    /\ ownerStep = "put_on_queue"
    /\ queue' = Append(queue, current)
    /\ timesQueued' = [timesQueued EXCEPT ![current] = @ + 1]
    /\ ownerStep' = "make_slab"
    /\ Touch(current)
    /\ UNCHANGED <<current, made, room, count, isCurrent, claimed, taken, keptBack, destroyed, allocated,
                   chunkSlab, chunkStep, currentReclaimed>>

OwnerMakeSlab ==
    LET s == made + 1
    IN
    /\ ownerStep = "make_slab"
    /\ IF made < MaxSlabs
       THEN /\ made' = s
            /\ current' = s
            /\ isCurrent' = [isCurrent EXCEPT ![s] = TRUE]
            /\ count' = [count EXCEPT ![s] = IF Design = "now" THEN 1 ELSE 0]
            /\ room' = [room EXCEPT ![s] = Capacity]
            /\ ownerStep' = "allocate"
       ELSE /\ ownerStep' = "out_of_slabs"
            /\ UNCHANGED <<made, current, isCurrent, count, room>>
    /\ UNCHANGED <<claimed, queue, taken, keptBack, destroyed, allocated, chunkSlab, chunkStep, timesQueued,
                   useAfterFree, currentReclaimed>>

(***************************************************************************)
(* Freeing chunk c, by whichever thread holds it (SlabAllocator::          *)
(* deallocate).                                                            *)
(***************************************************************************)
FreeTakeCountDown(c) ==
    LET s == chunkSlab[c]
    IN
    /\ chunkStep[c] = "held"
    /\ count' = [count EXCEPT ![s] = @ - 1]
    /\ chunkStep' = [chunkStep EXCEPT ![c] =
                        IF count[s] /= 1 THEN "done"
                        ELSE IF Design = "now" THEN "put_on_queue" ELSE "read_current"]
    /\ Touch(s)
    /\ UNCHANGED <<ownerStep, current, made, room, isCurrent, claimed, queue, taken, keptBack, destroyed,
                   allocated, chunkSlab, timesQueued, currentReclaimed>>

\* "before_bug_0157": a current slab is started again by the owner, not put on the queue.
FreeReadCurrent(c) ==
    LET s == chunkSlab[c]
    IN
    /\ chunkStep[c] = "read_current"
    /\ chunkStep' = [chunkStep EXCEPT ![c] = IF isCurrent[s] THEN "done" ELSE "claim"]
    /\ Touch(s)
    /\ UNCHANGED <<ownerStep, current, made, room, count, isCurrent, claimed, queue, taken, keptBack,
                   destroyed, allocated, chunkSlab, timesQueued, currentReclaimed>>

FreeClaim(c) ==
    LET s == chunkSlab[c]
    IN
    /\ chunkStep[c] = "claim"
    /\ IF ~claimed[s]
       THEN /\ claimed' = [claimed EXCEPT ![s] = TRUE]
            /\ chunkStep' = [chunkStep EXCEPT ![c] = "put_on_queue"]
       ELSE /\ chunkStep' = [chunkStep EXCEPT ![c] = "done"]
            /\ UNCHANGED claimed
    /\ Touch(s)
    /\ UNCHANGED <<ownerStep, current, made, room, count, isCurrent, queue, taken, keptBack, destroyed,
                   allocated, chunkSlab, timesQueued, currentReclaimed>>

FreePutOnQueue(c) ==
    LET s == chunkSlab[c]
    IN
    /\ chunkStep[c] = "put_on_queue"
    /\ queue' = Append(queue, s)
    /\ timesQueued' = [timesQueued EXCEPT ![s] = @ + 1]
    /\ chunkStep' = [chunkStep EXCEPT ![c] = "done"]
    /\ Touch(s)
    /\ UNCHANGED <<ownerStep, current, made, room, count, isCurrent, claimed, taken, keptBack, destroyed,
                   allocated, chunkSlab, currentReclaimed>>

Next ==
    \/ OwnerStart
    \/ OwnerTakeFromQueue
    \/ OwnerFinishDrain
    \/ OwnerCheck
    \/ OwnerAllocate
    \/ OwnerClearCurrent
    \/ OwnerReadCount
    \/ OwnerClaim
    \/ OwnerGiveUpHold
    \/ OwnerPutOnQueue
    \/ OwnerMakeSlab
    \/ \E c \in Chunks : FreeTakeCountDown(c) \/ FreeReadCurrent(c) \/ FreeClaim(c) \/ FreePutOnQueue(c)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* No thread touches a slab after the owner has destroyed it. This is the property BUG-0157 broke.
NoUseAfterFree == ~useAfterFree

\* The owner never destroys its current slab (drain_empty_slab_queue throws if it would).
CurrentNeverDestroyed == ~currentReclaimed /\ ~destroyed[current]

\* A slab is put on the queue at most once: twice would have the owner destroy it twice.
QueuedAtMostOnce == \A s \in Slabs : timesQueued[s] <= 1

\* A slab is destroyed only when no chunk from it is still held.
DestroyedOnlyWhenEmpty ==
    \A s \in Slabs : destroyed[s] => \A c \in Chunks : ~(chunkSlab[c] = s /\ chunkStep[c] = "held")

\* Once every chunk has been freed and the owner has drained the queue, every slab that is not
\* current has been destroyed or is the one kept back: no slab is lost.
NothingLost ==
    (/\ \A c \in Chunks : chunkStep[c] \in {"unallocated", "done"}
     /\ queue = <<>>
     /\ ownerStep = "check")
    => \A s \in 1..made : s = current \/ destroyed[s] \/ s = keptBack \/ s \in {taken[i] : i \in 1..Len(taken)}

=============================================================================
