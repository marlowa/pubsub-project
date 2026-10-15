------------------------------ MODULE PoolFreeList ------------------------------
(***************************************************************************)
(* A specification of the free list of FixedSizeMemoryPool, on its         *)
(* lock-free path (libraries/pubsub_itc_fw, FixedSizeMemoryPool.hpp: the   *)
(* code compiled when USING_VALGRIND is not defined, which is the code the *)
(* venue runs).                                                            *)
(*                                                                         *)
(* The pool holds a fixed number of slots. The free slots form a stack: a  *)
(* linked list through each slot's free_next field, whose top is the       *)
(* head. The head is a pointer and a counter together in one 16-byte word, *)
(* changed only by a 16-byte compare-and-exchange (CMPXCHG16B), and every  *)
(* change adds one to the counter. Any thread may take a slot (pop) or     *)
(* give one back (push):                                                   *)
(*                                                                         *)
(*   pop:  read the head; if its pointer is a slot, read that slot's       *)
(*         free_next; then compare-and-exchange the head from what was     *)
(*         read to (that free_next, counter + 1). If the exchange fails,   *)
(*         start again.                                                    *)
(*   push: read the head; set the slot's free_next to the head's pointer;  *)
(*         then compare-and-exchange the head from what was read to (the   *)
(*         slot, counter + 1). If the exchange fails, start again.         *)
(*                                                                         *)
(* The counter is what makes this safe. Without it, a thread that read the *)
(* head as slot A, with A's free_next as B, can be held up while others    *)
(* take A, take B, and give A back; its exchange then sees A at the head   *)
(* again and succeeds, making B the head although B is in use. With the    *)
(* counter, the head it read no longer matches and the exchange fails.     *)
(* With UseCounter = FALSE the exchange compares only the pointer, and TLC *)
(* must find that fault; with UseCounter = TRUE, as in the code, it must   *)
(* find nothing.                                                           *)
(*                                                                         *)
(* The code reads the head with one 16-byte load (movdqa). If that load    *)
(* were torn, so that the pointer and the counter came from different      *)
(* moments, the exchange that follows compares all 16 bytes against the    *)
(* head as it is and fails, and the thread starts again; and a slot's      *)
(* free_next can always be read, because slots are never freed while the   *)
(* pool exists. So a torn read costs a retry and nothing else, and the     *)
(* head is read here in one step. The code also gives up after nine empty  *)
(* reads and reports the pool exhausted; here a pop that finds the stack   *)
(* empty returns at once, which is the same as far as the free list is     *)
(* concerned.                                                              *)
(*                                                                         *)
(* The counter grows without limit, so the state space is bounded by a    *)
(* constraint, MaxCounter, on the head's counter.                          *)
(*                                                                         *)
(* Not modelled: the memory orderings of the C++ atomics; every thread     *)
(* sees every write in one order. The canary and the is_constructed flag,  *)
(* which are written by the caller, not by the free list.                  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    NumSlots,     \* slots in the pool
    Threads,      \* the threads using it
    MaxHeld,      \* the most slots one thread holds at once
    UseCounter,   \* whether the exchange compares the counter as well as the pointer
    MaxCounter    \* bound on the head's counter, to keep the model finite

Slots == 1..NumSlots
Null == 0

VARIABLES
    head,       \* [ptr |-> the top slot or Null, counter |-> a number]
    freeNext,   \* each slot's free_next
    step,       \* where each thread is
    held,       \* the slots each thread holds
    seen,       \* the head as each thread last read it
    seenNext,   \* in a pop, the free_next each thread read from the top slot
    pushing,    \* in a push, the slot each thread is giving back
    gotTwice    \* a slot was taken while another thread held it

vars == <<head, freeNext, step, held, seen, seenNext, pushing, gotTwice>>

Steps == {"idle", "pop_read_head", "pop_read_next", "pop_exchange", "push_read_head", "push_set_next", "push_exchange"}

\* Whether the head matches what a thread read, as the exchange compares it.
Matches(h, s) == h.ptr = s.ptr /\ (~UseCounter \/ h.counter = s.counter)

\* The slots reachable from the head along free_next, and whether the walk meets a slot twice (a cycle).
RECURSIVE Walk(_, _)
Walk(p, visited) ==
    IF p = Null THEN [slots |-> visited, cycle |-> FALSE]
    ELSE IF p \in visited THEN [slots |-> visited, cycle |-> TRUE]
    ELSE Walk(freeNext[p], visited \cup {p})

FreeList == Walk(head.ptr, {})

\* Every slot some thread holds, including one it is part way through giving back.
AllHeld == UNION {held[t] : t \in Threads} \cup {pushing[t] : t \in {u \in Threads : step[u] \in {"push_read_head", "push_set_next", "push_exchange"}}}

TypeOK ==
    /\ head \in [ptr : Slots \cup {Null}, counter : Nat]
    /\ freeNext \in [Slots -> Slots \cup {Null}]
    /\ step \in [Threads -> Steps]
    /\ held \in [Threads -> SUBSET Slots]
    /\ seen \in [Threads -> [ptr : Slots \cup {Null}, counter : Nat]]
    /\ seenNext \in [Threads -> Slots \cup {Null}]
    /\ pushing \in [Threads -> Slots \cup {Null}]
    /\ gotTwice \in BOOLEAN

\* Every slot starts on the free list, slot 1 at the top.
Init ==
    /\ head = [ptr |-> 1, counter |-> 0]
    /\ freeNext = [s \in Slots |-> IF s < NumSlots THEN s + 1 ELSE Null]
    /\ step = [t \in Threads |-> "idle"]
    /\ held = [t \in Threads |-> {}]
    /\ seen = [t \in Threads |-> [ptr |-> Null, counter |-> 0]]
    /\ seenNext = [t \in Threads |-> Null]
    /\ pushing = [t \in Threads |-> Null]
    /\ gotTwice = FALSE

(***************************************************************************)
(* Taking a slot (pop_slot_from_free_list).                                *)
(***************************************************************************)
PopBegin(t) ==
    /\ step[t] = "idle"
    /\ Cardinality(held[t]) < MaxHeld
    /\ step' = [step EXCEPT ![t] = "pop_read_head"]
    /\ UNCHANGED <<head, freeNext, held, seen, seenNext, pushing, gotTwice>>

\* An empty stack: the pool is exhausted, and the thread gets nothing.
PopReadHead(t) ==
    /\ step[t] = "pop_read_head"
    /\ seen' = [seen EXCEPT ![t] = head]
    /\ step' = [step EXCEPT ![t] = IF head.ptr = Null THEN "idle" ELSE "pop_read_next"]
    /\ UNCHANGED <<head, freeNext, held, seenNext, pushing, gotTwice>>

PopReadNext(t) ==
    /\ step[t] = "pop_read_next"
    /\ seenNext' = [seenNext EXCEPT ![t] = freeNext[seen[t].ptr]]
    /\ step' = [step EXCEPT ![t] = "pop_exchange"]
    /\ UNCHANGED <<head, freeNext, held, seen, pushing, gotTwice>>

PopExchange(t) ==
    LET s == seen[t].ptr
    IN
    /\ step[t] = "pop_exchange"
    /\ IF Matches(head, seen[t])
       THEN /\ head' = [ptr |-> seenNext[t], counter |-> head.counter + 1]
            /\ held' = [held EXCEPT ![t] = @ \cup {s}]
            /\ gotTwice' = (gotTwice \/ s \in AllHeld)
            /\ step' = [step EXCEPT ![t] = "idle"]
       ELSE /\ step' = [step EXCEPT ![t] = "pop_read_head"]
            /\ UNCHANGED <<head, held, gotTwice>>
    /\ UNCHANGED <<freeNext, seen, seenNext, pushing>>

(***************************************************************************)
(* Giving a slot back (push_slot_to_free_list).                            *)
(***************************************************************************)
PushBegin(t) ==
    /\ step[t] = "idle"
    /\ \E s \in held[t] :
        /\ pushing' = [pushing EXCEPT ![t] = s]
        /\ held' = [held EXCEPT ![t] = @ \ {s}]
    /\ step' = [step EXCEPT ![t] = "push_read_head"]
    /\ UNCHANGED <<head, freeNext, seen, seenNext, gotTwice>>

PushReadHead(t) ==
    /\ step[t] = "push_read_head"
    /\ seen' = [seen EXCEPT ![t] = head]
    /\ step' = [step EXCEPT ![t] = "push_set_next"]
    /\ UNCHANGED <<head, freeNext, held, seenNext, pushing, gotTwice>>

PushSetNext(t) ==
    /\ step[t] = "push_set_next"
    /\ freeNext' = [freeNext EXCEPT ![pushing[t]] = seen[t].ptr]
    /\ step' = [step EXCEPT ![t] = "push_exchange"]
    /\ UNCHANGED <<head, held, seen, seenNext, pushing, gotTwice>>

PushExchange(t) ==
    /\ step[t] = "push_exchange"
    /\ IF Matches(head, seen[t])
       THEN /\ head' = [ptr |-> pushing[t], counter |-> head.counter + 1]
            /\ step' = [step EXCEPT ![t] = "idle"]
            /\ pushing' = [pushing EXCEPT ![t] = Null]
       ELSE /\ step' = [step EXCEPT ![t] = "push_read_head"]
            /\ UNCHANGED <<head, pushing>>
    /\ UNCHANGED <<freeNext, held, seen, seenNext, gotTwice>>

Next ==
    \E t \in Threads :
        \/ PopBegin(t) \/ PopReadHead(t) \/ PopReadNext(t) \/ PopExchange(t)
        \/ PushBegin(t) \/ PushReadHead(t) \/ PushSetNext(t) \/ PushExchange(t)

Spec == Init /\ [][Next]_vars

\* Keeps the model finite: the counter only grows.
CounterBound == head.counter <= MaxCounter

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* No slot is taken while another thread holds it.
NeverTakenTwice == ~gotTwice

\* The free list never runs in a circle, and never contains a slot a thread holds.
FreeListSound ==
    /\ ~FreeList.cycle
    /\ FreeList.slots \cap AllHeld = {}

\* No slot is lost: every slot is on the free list or held by a thread, whenever no thread is part way
\* through taking one.
NothingLost ==
    (\A t \in Threads : step[t] \notin {"pop_read_head", "pop_read_next", "pop_exchange"})
    => FreeList.slots \cup AllHeld = Slots

=============================================================================
