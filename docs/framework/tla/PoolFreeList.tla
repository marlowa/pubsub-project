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
(* change increments the counter. Its C++ type is std::uint64_t, which wraps *)
(* after 2^64 changes. This model uses unbounded natural numbers to leave   *)
(* that wraparound out of the state space. This is a practical assumption: *)
(* at a continuous rate of one successful head change per cycle on a       *)
(* hypothetical 6 GHz processor, a full counter cycle takes about 97 years. *)
(* Each real change also needs a 128-bit compare-and-swap on the shared     *)
(* head, so that rate is deliberately generous. CounterBound limits the    *)
(* explored values for TLC.                                                *)
(* Any thread may take a slot (pop) or give one back (push):                *)
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
(* must find that fault. With UseCounter = TRUE, each successful change    *)
(* increments the tag, so a stale head cannot match while the counter has  *)
(* not repeated. In the C++ code the counter is 64 bits. If it completed a *)
(* full cycle while a thread held a stale head, and the pointer returned   *)
(* to the same slot, the pair could match again. The rate calculation     *)
(* beside HeadPtr in FixedSizeMemoryPool.hpp explains the practical scale  *)
(* of that interval. This model uses unbounded integers and excludes       *)
(* counter wraparound, so it checks the tagged algorithm only before the  *)
(* tag repeats.                                                           *)
(*                                                                         *)
(* The C++ implementation reads the head with inline SSE movdqa assembly   *)
(* and requires a 16-byte-aligned head on x86-64. This specification treats *)
(* each head read as one atomic observation. It does not check the          *)
(* instruction's behaviour, compiler output, or the C++ memory-order rules. *)
(* Slots remain allocated for the pool's lifetime, so a slot's free_next  *)
(* remains addressable. The implementation retries up to nine times when  *)
(* the head appears empty; this model returns immediately for an empty     *)
(* head and does not model pool expansion or a transient empty result.      *)
(*                                                                         *)
(* CounterBound limits the current head counter so TLC can check a finite  *)
(* state space. Transitions that would exceed MaxCounter are excluded. The *)
(* model therefore checks the tagged algorithm before wraparound. It       *)
(* excludes the theoretical wraparound because a full cycle requires      *)
(* 2^64 successful head changes, far beyond the expected continuous        *)
(* operating lifetime at the comparison rate described above.             *)
(*                                                                         *)
(* Each push is assumed to return a slot held by that thread, so invalid   *)
(* pointers and duplicate frees are outside the model. It also does not    *)
(* model object construction, destruction, canaries, or is_constructed.    *)
(* There is no fairness or progress property: retries may continue without *)
(* bound. The model checks reachable states, not whether an operation      *)
(* eventually completes.                                                   *)
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

\* TLC checks the tagged algorithm with an unbounded counter, bounded to
\* MaxCounter for a finite state space. Transitions beyond this bound are
\* excluded, so this model does not explore counter wraparound.
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

\* No slot is lost: every slot is on the free list or held by a thread in
\* every state. A pop transfers a slot from the free list to held in one
\* action; while a push is in progress, AllHeld includes its slot.
NothingLost == FreeList.slots \cup AllHeld = Slots

=============================================================================
