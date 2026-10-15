--------------------------- MODULE QueueOfEmptySlabs ---------------------------
(***************************************************************************)
(* A specification of the queue on which empty slabs are given back to the *)
(* owner of a slab allocator (EmptySlabQueue in libraries/pubsub_itc_fw),  *)
(* and of the rule by which the owner destroys the slabs it takes from it  *)
(* (ExpandableSlabAllocator::drain_empty_slab_queue).                      *)
(*                                                                         *)
(* The queue is a linked list of nodes, one node inside each slab, with a  *)
(* node of the queue's own at the start. Any thread may put a node on the  *)
(* queue; only the owner takes nodes off. Putting node n on the queue is   *)
(* three steps:                                                            *)
(*                                                                         *)
(*   1. set n's link to nothing;                                           *)
(*   2. swap n into the queue's tail, in one atomic exchange, which gives  *)
(*      back the node that was the tail before;                            *)
(*   3. set that earlier node's link to n.                                 *)
(*                                                                         *)
(* Between steps 2 and 3, n is the tail but cannot yet be reached from the *)
(* head. The owner keeps a head, which is always the node taken last (or   *)
(* the queue's own node, before anything is taken). To take a node it      *)
(* reads the head's link: if it names a node, that node becomes the head   *)
(* and its slab is taken; if not and the tail is the head, the queue is    *)
(* empty; if not and the tail is not the head, a thread is between steps 2 *)
(* and 3, and the owner tries again.                                       *)
(*                                                                         *)
(* So a node goes on being used after the owner has taken it: as the head, *)
(* whose link the owner reads next time, and as the tail, whose link a     *)
(* thread putting the next node on the queue will set in step 3. The slab  *)
(* that holds it must not be destroyed while either can happen. The owner  *)
(* therefore takes every node it can, destroys the slabs taken except the  *)
(* last, and keeps that one back until a later drain takes another. With   *)
(* KeepLastTaken = FALSE it destroys every slab it takes at once, and TLC  *)
(* must find a node used after its slab was destroyed; with KeepLastTaken  *)
(* = TRUE, as in the code, it must find nothing.                           *)
(*                                                                         *)
(* Each slab's node is put on the queue at most once, because a slab is    *)
(* given back once and then destroyed: SlabReclaim.tla checks that. Here   *)
(* each slab has a process that puts its node on the queue once, at a      *)
(* moment of its choosing, standing for whichever thread gives the slab    *)
(* back. In the code the owner itself sometimes puts a node on the queue,  *)
(* and so cannot be between those steps while it drains; letting every     *)
(* process interleave with the owner allows more behaviours than the code, *)
(* not fewer. A step that reads or writes a node "touches" its slab; a     *)
(* touch of a slab already destroyed sets useAfterFree.                    *)
(*                                                                         *)
(* Not modelled: the memory orderings of the C++ atomics; every thread     *)
(* sees every write in one order.                                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    NumSlabs,       \* how many slabs there are
    KeepLastTaken   \* whether the owner keeps back the last slab it takes until a later drain

Slabs == 1..NumSlabs
QueueOwnNode == 0
Nodes == Slabs \cup {QueueOwnNode}
NoNode == -1
NoSlab == 0

VARIABLES
    head,         \* the owner's head: the node taken last
    tail,         \* the queue's tail
    link,         \* each node's link to the next node, or NoNode
    putStep,      \* where each slab's process is in putting its node on the queue
    earlier,      \* the node each process got back from the exchange in step 2
    ownerStep,    \* where the owner is in draining
    taken,        \* the slabs taken in the drain in progress
    keptBack,     \* the slab kept back from the last drain, or NoSlab
    destroyed,    \* which slabs have been destroyed
    timesTaken,   \* how many times each slab has been taken off the queue
    useAfterFree  \* a node was used after its slab was destroyed

vars == <<head, tail, link, putStep, earlier, ownerStep, taken, keptBack, destroyed, timesTaken, useAfterFree>>

\* Whether using node n touches a destroyed slab. The queue's own node is never destroyed.
Destroyed(n) == n /= QueueOwnNode /\ destroyed[n]

TypeOK ==
    /\ head \in Nodes
    /\ tail \in Nodes
    /\ link \in [Nodes -> Nodes \cup {NoNode}]
    /\ putStep \in [Slabs -> {"not_yet", "set_link", "exchange", "link_earlier", "done"}]
    /\ earlier \in [Slabs -> Nodes]
    /\ ownerStep \in {"idle", "drain", "destroy"}
    /\ taken \in Seq(Slabs)
    /\ keptBack \in Slabs \cup {NoSlab}
    /\ destroyed \in [Slabs -> BOOLEAN]
    /\ timesTaken \in [Slabs -> Nat]
    /\ useAfterFree \in BOOLEAN

Init ==
    /\ head = QueueOwnNode
    /\ tail = QueueOwnNode
    /\ link = [n \in Nodes |-> NoNode]
    /\ putStep = [s \in Slabs |-> "not_yet"]
    /\ earlier = [s \in Slabs |-> QueueOwnNode]
    /\ ownerStep = "idle"
    /\ taken = <<>>
    /\ keptBack = NoSlab
    /\ destroyed = [s \in Slabs |-> FALSE]
    /\ timesTaken = [s \in Slabs |-> 0]
    /\ useAfterFree = FALSE

(***************************************************************************)
(* Putting slab s's node on the queue (EmptySlabQueue::enqueue).           *)
(***************************************************************************)
PutBegin(s) ==
    /\ putStep[s] = "not_yet"
    /\ putStep' = [putStep EXCEPT ![s] = "set_link"]
    /\ UNCHANGED <<head, tail, link, earlier, ownerStep, taken, keptBack, destroyed, timesTaken, useAfterFree>>

PutSetLink(s) ==
    /\ putStep[s] = "set_link"
    /\ link' = [link EXCEPT ![s] = NoNode]
    /\ putStep' = [putStep EXCEPT ![s] = "exchange"]
    /\ useAfterFree' = (useAfterFree \/ Destroyed(s))
    /\ UNCHANGED <<head, tail, earlier, ownerStep, taken, keptBack, destroyed, timesTaken>>

PutExchange(s) ==
    /\ putStep[s] = "exchange"
    /\ earlier' = [earlier EXCEPT ![s] = tail]
    /\ tail' = s
    /\ putStep' = [putStep EXCEPT ![s] = "link_earlier"]
    /\ UNCHANGED <<head, link, ownerStep, taken, keptBack, destroyed, timesTaken, useAfterFree>>

PutLinkEarlier(s) ==
    /\ putStep[s] = "link_earlier"
    /\ link' = [link EXCEPT ![earlier[s]] = s]
    /\ putStep' = [putStep EXCEPT ![s] = "done"]
    /\ useAfterFree' = (useAfterFree \/ Destroyed(earlier[s]))
    /\ UNCHANGED <<head, tail, earlier, ownerStep, taken, keptBack, destroyed, timesTaken>>

(***************************************************************************)
(* The owner draining the queue (EmptySlabQueue::try_dequeue, called by    *)
(* ExpandableSlabAllocator::drain_empty_slab_queue).                       *)
(***************************************************************************)
OwnerBeginDrain ==
    /\ ownerStep = "idle"
    /\ ownerStep' = "drain"
    /\ taken' = <<>>
    /\ UNCHANGED <<head, tail, link, putStep, earlier, keptBack, destroyed, timesTaken, useAfterFree>>

\* The head's link names a node: it becomes the head, and its slab is taken (reading the slab's id
\* from the node).
OwnerTake ==
    LET n == link[head]
    IN
    /\ ownerStep = "drain"
    /\ n /= NoNode
    /\ head' = n
    /\ taken' = Append(taken, n)
    /\ timesTaken' = [timesTaken EXCEPT ![n] = @ + 1]
    /\ useAfterFree' = (useAfterFree \/ Destroyed(head) \/ Destroyed(n))
    /\ UNCHANGED <<tail, link, putStep, earlier, ownerStep, keptBack, destroyed>>

\* The head's link names nothing and the tail is the head: the queue is empty. (If the tail is not the
\* head, a thread is between steps 2 and 3, and the owner reads the link again: no step.)
OwnerFindEmpty ==
    /\ ownerStep = "drain"
    /\ link[head] = NoNode
    /\ tail = head
    /\ ownerStep' = "destroy"
    /\ useAfterFree' = (useAfterFree \/ Destroyed(head))
    /\ UNCHANGED <<head, tail, link, putStep, earlier, taken, keptBack, destroyed, timesTaken>>

\* Destroy the slabs taken. With KeepLastTaken, the last slab taken is kept back until a later drain
\* takes another, and the one kept back before is destroyed then.
OwnerDestroy ==
    LET last == taken[Len(taken)]
        reclaim == IF taken = <<>> THEN {}
                   ELSE IF KeepLastTaken
                        THEN {taken[i] : i \in 1..(Len(taken) - 1)} \cup (IF keptBack = NoSlab THEN {} ELSE {keptBack})
                        ELSE {taken[i] : i \in 1..Len(taken)}
    IN
    /\ ownerStep = "destroy"
    /\ keptBack' = IF taken /= <<>> /\ KeepLastTaken THEN last ELSE keptBack
    /\ destroyed' = [s \in Slabs |-> destroyed[s] \/ s \in reclaim]
    /\ ownerStep' = "idle"
    /\ UNCHANGED <<head, tail, link, putStep, earlier, taken, timesTaken, useAfterFree>>

Next ==
    \/ \E s \in Slabs : PutBegin(s) \/ PutSetLink(s) \/ PutExchange(s) \/ PutLinkEarlier(s)
    \/ OwnerBeginDrain
    \/ OwnerTake
    \/ OwnerFindEmpty
    \/ OwnerDestroy

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* No node is used after the slab it belongs to has been destroyed.
NoUseAfterFree == ~useAfterFree

\* No slab is taken off the queue twice, which would have the owner destroy it twice.
TakenAtMostOnce == \A s \in Slabs : timesTaken[s] <= 1

\* When the owner has found the queue empty and no thread is part way through putting a node on it,
\* every node that was put on the queue has been taken: none is lost.
NothingLost ==
    (/\ ownerStep = "destroy"
     /\ tail = head
     /\ \A s \in Slabs : putStep[s] \in {"not_yet", "done"})
    => \A s \in Slabs : putStep[s] = "done" => timesTaken[s] = 1

=============================================================================
