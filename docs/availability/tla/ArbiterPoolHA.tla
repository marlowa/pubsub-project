---------------------------- MODULE ArbiterPoolHA ----------------------------
(***************************************************************************)
(* A model of how the two arbiters decide which of them is active, with    *)
(* the witness breaking ties, as the code does it:                         *)
(* applications/arbiter/ArbiterThread.cpp (elect_role, adopt_role,         *)
(* promote_if_nothing_else_can_be_active, handle_arbiter_vote_response)    *)
(* and applications/witness/WitnessThread.cpp (handle_arbiter_vote_request).*)
(*                                                                         *)
(* Only the active arbiter answers arbitration reports from components;   *)
(* a passive one drops them. So "which arbiter is active" decides whose   *)
(* leadership decisions the venue acts on, and two active arbiters can    *)
(* each issue decisions for the same pair of components.                  *)
(*                                                                         *)
(* What is modelled: the two arbiters 1 and 2, each with a role, an epoch *)
(* held only in memory, whether it has seen its peer act as active, a     *)
(* heartbeat (or startup) timeout, and an outstanding vote request; the   *)
(* witness, whose record of the highest epoch it has seen is also held    *)
(* only in memory; three links (arbiter to arbiter, and each arbiter to    *)
(* the witness); crashes and restarts of all three; status queries,       *)
(* status responses, vote requests and vote responses in flight.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    MaxEpoch,
    MaxArbiterCrashes,
    MaxWitnessCrashes,
    MaxLinkFailures,
    Quiet      \* TRUE: no failure while a message is in flight, and no timeout on an
               \* arbiter while a message to it is in flight

Arb == {1, 2}
Peer(a) == IF a = 1 THEN 2 ELSE 1
Max(x, y) == IF x > y THEN x ELSE y
WLink(a) == IF a = 1 THEN "w1" ELSE "w2"
AllLinks == {"aa", "w1", "w2"}

VARIABLES
    aup,         \* aup[a]: arbiter a is running
    arole,       \* arole[a] \in {"unknown", "active", "passive"}
    aepoch,      \* aepoch[a]: held in memory; zero after a restart
    seenActive,  \* seenActive[a]: peer_seen_active_, held in memory
    atimer,      \* atimer[a]: the peer heartbeat (or startup) timeout is armed
    voteWait,    \* voteWait[a]: a vote request is outstanding, with its timeout armed
    wup,         \* the witness is running
    wmax,        \* the highest arbiter epoch the witness has seen, held in memory
    alink,       \* links that are up
    amsgs,       \* messages in flight
    arbCrashes, witCrashes, linkFailures

vars == <<aup, arole, aepoch, seenActive, atimer, voteWait, wup, wmax, alink, amsgs,
          arbCrashes, witCrashes, linkFailures>>

QueryMsg(to, e) == [type |-> "query", to |-> to, epoch |-> e]
ResponseMsg(to, r, e) == [type |-> "response", to |-> to, role |-> r, epoch |-> e]
VoteReq(from, e) == [type |-> "votereq", from |-> from, epoch |-> e]
VoteResp(to, g, e) == [type |-> "voteresp", to |-> to, granted |-> g, epoch |-> e]

OverLink(m, l) ==
    CASE m.type \in {"query", "response"} -> l = "aa"
      [] m.type = "votereq"               -> l = WLink(m.from)
      [] m.type = "voteresp"              -> l = WLink(m.to)

PeerLinkUp == "aa" \in alink /\ aup[1] /\ aup[2]
WitnessReachable(a) == wup /\ WLink(a) \in alink
(* The witness registers an arbiter by its heartbeats, so it counts an      *)
(* arbiter as connected while that arbiter runs and their link is up.       *)
WitnessSees(a) == aup[a] /\ wup /\ WLink(a) \in alink

(* Heartbeats from the peer are arriving and would be accepted. Both an    *)
(* active and a passive arbiter send them; a passive one re-arms its       *)
(* timeout on each heartbeat whose epoch is not below its own.              *)
HeartbeatsFlowing(a) ==
    /\ PeerLinkUp
    /\ arole[Peer(a)] # "unknown"
    /\ aepoch[Peer(a)] >= aepoch[a]

InFlightTo(a) == \E m \in amsgs : (m.type # "votereq" /\ m.to = a)
                                  \/ (m.type = "votereq" /\ m.from = a)
Settled == amsgs = {} /\ \A a \in Arb : (aup[a] /\ arole[a] = "passive" /\ HeartbeatsFlowing(a)) => atimer[a]

-----------------------------------------------------------------------------
Init ==
    /\ aup = [a \in Arb |-> TRUE]
    /\ arole = [a \in Arb |-> "unknown"]
    /\ aepoch = [a \in Arb |-> 0]
    /\ seenActive = [a \in Arb |-> FALSE]
    /\ atimer = [a \in Arb |-> TRUE]
    /\ voteWait = [a \in Arb |-> FALSE]
    /\ wup = TRUE
    /\ wmax = 0
    /\ alink = AllLinks
    \* Each arbiter sends a StatusQuery when the peer connection comes up.
    /\ amsgs = {QueryMsg(1, 0), QueryMsg(2, 0)}
    /\ arbCrashes = 0 /\ witCrashes = 0 /\ linkFailures = 0

-----------------------------------------------------------------------------
(* adopt_role: an arbiter becoming active cancels its timeouts; one        *)
(* becoming passive arms its heartbeat timeout. Adopting the role already  *)
(* held changes nothing.                                                    *)
AdoptRole(a, r) ==
    /\ arole' = [arole EXCEPT ![a] = r]
    /\ atimer' = [atimer EXCEPT ![a] = IF r = arole[a] THEN atimer[a]
                                        ELSE (r = "passive")]
    /\ voteWait' = [voteWait EXCEPT ![a] = IF r = "active" THEN FALSE ELSE voteWait[a]]

(* elect_role(peer epoch, peer role) for an arbiter with no role yet. *)
ElectRole(a, pe, pr) ==
    IF arole[a] # "unknown"
    THEN UNCHANGED <<arole, aepoch, seenActive, atimer, voteWait>>
    ELSE IF pe > aepoch[a]
         THEN /\ aepoch' = [aepoch EXCEPT ![a] = pe]
              /\ seenActive' = [seenActive EXCEPT ![a] = seenActive[a] \/ pr # "passive"]
              /\ AdoptRole(a, "passive")
         ELSE IF pr = "active"
              THEN /\ AdoptRole(a, "passive")
                   /\ UNCHANGED <<aepoch, seenActive>>
              ELSE /\ AdoptRole(a, IF a < Peer(a) THEN "active" ELSE "passive")
                   /\ UNCHANGED <<aepoch, seenActive>>

(* A status query reaches a: it answers with its current role and epoch,   *)
(* then runs elect_role treating the peer's role as unknown, because a      *)
(* query carries no role.                                                   *)
ReceiveQuery(m) ==
    /\ m \in amsgs /\ m.type = "query" /\ aup[m.to]
    /\ LET a == m.to IN
       /\ amsgs' = (amsgs \ {m}) \cup {ResponseMsg(Peer(a), arole[a], aepoch[a])}
       /\ ElectRole(a, m.epoch, "unknown")
    /\ UNCHANGED <<aup, wup, wmax, alink, arbCrashes, witCrashes, linkFailures>>

(* A status response reaches a: a peer saying it is active is remembered,  *)
(* then elect_role runs with the peer's stated role.                        *)
ReceiveResponse(m) ==
    /\ m \in amsgs /\ m.type = "response" /\ aup[m.to]
    /\ LET a == m.to IN
       /\ amsgs' = amsgs \ {m}
       /\ IF arole[a] # "unknown"
          THEN /\ seenActive' = [seenActive EXCEPT ![a] = seenActive[a] \/ m.role = "active"]
               /\ UNCHANGED <<arole, aepoch, atimer, voteWait>>
          ELSE IF m.epoch > aepoch[a]
               THEN /\ aepoch' = [aepoch EXCEPT ![a] = m.epoch]
                    /\ seenActive' = [seenActive EXCEPT ![a] = seenActive[a] \/ m.role # "passive"]
                    /\ AdoptRole(a, "passive")
               ELSE IF m.role = "active"
                    THEN /\ seenActive' = [seenActive EXCEPT ![a] = TRUE]
                         /\ AdoptRole(a, "passive")
                         /\ UNCHANGED aepoch
                    ELSE /\ AdoptRole(a, IF a < Peer(a) THEN "active" ELSE "passive")
                         /\ UNCHANGED <<aepoch, seenActive>>
    /\ UNCHANGED <<aup, wup, wmax, alink, arbCrashes, witCrashes, linkFailures>>

(* A heartbeat from the peer reaches a passive arbiter and re-arms its     *)
(* timeout.                                                                 *)
ReceiveHeartbeat(a) ==
    /\ aup[a] /\ arole[a] = "passive" /\ ~atimer[a] /\ HeartbeatsFlowing(a)
    /\ atimer' = [atimer EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, voteWait, wup, wmax, alink, amsgs,
                   arbCrashes, witCrashes, linkFailures>>

(* promote_if_nothing_else_can_be_active: with no witness to ask, the lower *)
(* id promotes if it has never seen its peer active; otherwise it declines  *)
(* and asks again later.                                                    *)
PromoteIfNothingElse(a) ==
    IF ~seenActive[a] /\ a < Peer(a)
    THEN /\ aepoch' = [aepoch EXCEPT ![a] = aepoch[a] + 1]
         /\ AdoptRole(a, "active")
    ELSE /\ atimer' = [atimer EXCEPT ![a] = TRUE]
         /\ voteWait' = [voteWait EXCEPT ![a] = FALSE]
         /\ UNCHANGED <<arole, aepoch>>

(* The heartbeat (or startup) timeout fires on an arbiter that is not      *)
(* active: ask the witness if it is reachable, otherwise decide alone.      *)
HeartbeatTimeout(a) ==
    /\ aup[a] /\ atimer[a] /\ arole[a] # "active"
    \* A passive arbiter's timeout is re-armed by heartbeats; the startup timeout of an
    \* arbiter with no role yet is not.
    /\ arole[a] = "passive" => ~HeartbeatsFlowing(a)
    \* Timeouts are seconds and messages take milliseconds, so with Quiet no timeout fires
    \* while any message is in flight.
    /\ Quiet => amsgs = {}
    /\ IF WitnessReachable(a)
       THEN /\ atimer' = [atimer EXCEPT ![a] = FALSE]
            /\ voteWait' = [voteWait EXCEPT ![a] = TRUE]
            /\ amsgs' = amsgs \cup {VoteReq(a, aepoch[a])}
            /\ UNCHANGED <<arole, aepoch>>
       ELSE /\ atimer' = [atimer EXCEPT ![a] = FALSE]
            /\ PromoteIfNothingElse(a)
            /\ UNCHANGED amsgs
    /\ UNCHANGED <<aup, seenActive, wup, wmax, alink, arbCrashes, witCrashes, linkFailures>>

VoteTimeout(a) ==
    /\ aup[a] /\ voteWait[a] /\ arole[a] # "active"
    /\ Quiet => amsgs = {}
    /\ voteWait' = [voteWait EXCEPT ![a] = FALSE]
    /\ PromoteIfNothingElse(a)
    /\ UNCHANGED <<aup, seenActive, wup, wmax, alink, amsgs, arbCrashes, witCrashes, linkFailures>>

(* The witness answers a vote request: the new epoch is one past the       *)
(* highest it has seen, and the requester wins unless its peer is also      *)
(* connected to the witness, in which case the lower id wins. It keeps no  *)
(* record of what it granted.                                               *)
WitnessVotes(m) ==
    /\ m \in amsgs /\ m.type = "votereq" /\ wup /\ WLink(m.from) \in alink
    /\ LET a == m.from
           seen == Max(wmax, Max(m.epoch, Max(IF WitnessSees(1) THEN aepoch[1] ELSE 0,
                                              IF WitnessSees(2) THEN aepoch[2] ELSE 0)))
           granted == IF WitnessSees(Peer(a)) THEN 1 ELSE a
       IN /\ wmax' = seen
          /\ amsgs' = (amsgs \ {m}) \cup {VoteResp(a, granted, seen + 1)}
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, atimer, voteWait, wup, alink,
                   arbCrashes, witCrashes, linkFailures>>

(* handle_arbiter_vote_response: the arbiter takes the epoch and the role  *)
(* the witness gave it, whatever role it held before.                       *)
ReceiveVote(m) ==
    /\ m \in amsgs /\ m.type = "voteresp" /\ aup[m.to]
    /\ LET a == m.to IN
       /\ amsgs' = amsgs \ {m}
       /\ aepoch' = [aepoch EXCEPT ![a] = m.epoch]
       /\ voteWait' = [voteWait EXCEPT ![a] = FALSE]
       /\ arole' = [arole EXCEPT ![a] = IF m.granted = a THEN "active" ELSE "passive"]
       /\ atimer' = [atimer EXCEPT ![a] = IF m.granted = a THEN FALSE
                                          ELSE IF arole[a] # "passive" THEN TRUE ELSE atimer[a]]
    /\ UNCHANGED <<aup, seenActive, wup, wmax, alink, arbCrashes, witCrashes, linkFailures>>

-----------------------------------------------------------------------------
(* Failures and recoveries. *)

ArbiterCrash(a) ==
    /\ aup[a] /\ arbCrashes < MaxArbiterCrashes
    /\ Quiet => Settled
    /\ aup' = [aup EXCEPT ![a] = FALSE]
    /\ arole' = [arole EXCEPT ![a] = "unknown"]
    /\ aepoch' = [aepoch EXCEPT ![a] = 0]
    /\ seenActive' = [seenActive EXCEPT ![a] = FALSE]
    /\ atimer' = [atimer EXCEPT ![a] = FALSE]
    /\ voteWait' = [voteWait EXCEPT ![a] = FALSE]
    /\ amsgs' = {m \in amsgs : ~((m.type # "votereq" /\ m.to = a) \/ (m.type = "votereq" /\ m.from = a))}
    /\ arbCrashes' = arbCrashes + 1
    /\ UNCHANGED <<wup, wmax, alink, witCrashes, linkFailures>>

(* A restarted arbiter holds nothing: no role, epoch zero, and its startup *)
(* timeout armed. If the peer link is up the two exchange status queries.   *)
ArbiterRestart(a) ==
    /\ ~aup[a]
    /\ Quiet => Settled
    /\ aup' = [aup EXCEPT ![a] = TRUE]
    /\ atimer' = [atimer EXCEPT ![a] = TRUE]
    /\ amsgs' = amsgs \cup (IF "aa" \in alink /\ aup[Peer(a)]
                            THEN {QueryMsg(a, aepoch[Peer(a)]), QueryMsg(Peer(a), 0)}
                            ELSE {})
    /\ UNCHANGED <<arole, aepoch, seenActive, voteWait, wup, wmax, alink,
                   arbCrashes, witCrashes, linkFailures>>

WitnessCrash ==
    /\ wup /\ witCrashes < MaxWitnessCrashes
    /\ Quiet => Settled
    /\ wup' = FALSE
    /\ wmax' = 0
    /\ amsgs' = {m \in amsgs : m.type \in {"query", "response"}}
    /\ witCrashes' = witCrashes + 1
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, atimer, voteWait, alink, arbCrashes, linkFailures>>

WitnessRestart ==
    /\ ~wup
    /\ Quiet => Settled
    /\ wup' = TRUE
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, atimer, voteWait, wmax, alink, amsgs,
                   arbCrashes, witCrashes, linkFailures>>

LinkFails(l) ==
    /\ l \in alink /\ linkFailures < MaxLinkFailures
    /\ Quiet => Settled
    /\ alink' = alink \ {l}
    /\ amsgs' = {m \in amsgs : ~OverLink(m, l)}
    /\ linkFailures' = linkFailures + 1
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, atimer, voteWait, wup, wmax, arbCrashes, witCrashes>>

LinkRecovers(l) ==
    /\ l \in AllLinks \ alink
    /\ Quiet => Settled
    /\ alink' = alink \cup {l}
    /\ amsgs' = amsgs \cup (IF l = "aa" /\ aup[1] /\ aup[2]
                            THEN {QueryMsg(1, aepoch[2]), QueryMsg(2, aepoch[1])}
                            ELSE {})
    /\ UNCHANGED <<aup, arole, aepoch, seenActive, atimer, voteWait, wup, wmax,
                   arbCrashes, witCrashes, linkFailures>>

-----------------------------------------------------------------------------
Next ==
    \/ \E m \in amsgs : ReceiveQuery(m)
    \/ \E m \in amsgs : ReceiveResponse(m)
    \/ \E m \in amsgs : WitnessVotes(m)
    \/ \E m \in amsgs : ReceiveVote(m)
    \/ \E a \in Arb : ReceiveHeartbeat(a)
    \/ \E a \in Arb : HeartbeatTimeout(a)
    \/ \E a \in Arb : VoteTimeout(a)
    \/ \E a \in Arb : ArbiterCrash(a)
    \/ \E a \in Arb : ArbiterRestart(a)
    \/ WitnessCrash
    \/ WitnessRestart
    \/ \E l \in AllLinks : LinkFails(l)
    \/ \E l \in AllLinks : LinkRecovers(l)

Spec == Init /\ [][Next]_vars

EpochBound == \A a \in Arb : aepoch[a] <= MaxEpoch

-----------------------------------------------------------------------------
(* Properties. *)

TypeOK ==
    /\ arole \in [Arb -> {"unknown", "active", "passive"}]
    /\ aup \in [Arb -> BOOLEAN]

(* At most one arbiter is active at a time. *)
AtMostOneActive == ~(aup[1] /\ aup[2] /\ arole[1] = "active" /\ arole[2] = "active")

(* Two active arbiters that can see each other again, with nothing left in *)
(* flight, have nothing that will ever make one of them stand down.         *)
NoLastingTwoActive ==
    ~(aup[1] /\ aup[2] /\ arole[1] = "active" /\ arole[2] = "active"
      /\ "aa" \in alink /\ amsgs = {})

(* A passive arbiter's epoch is never above the active arbiter's. When it *)
(* is, the passive arbiter treats the active one's heartbeats as coming     *)
(* from a stale peer and ignores them, and so believes it has gone silent.  *)
PassiveNotAheadOfActive ==
    \A a \in Arb :
        (aup[a] /\ aup[Peer(a)] /\ arole[a] = "passive" /\ arole[Peer(a)] = "active")
            => aepoch[a] <= aepoch[Peer(a)]

(* A passive arbiter whose active peer has died always has some way to    *)
(* notice: an armed timeout or an outstanding vote request.                  *)
PassiveCanNotice ==
    \A a \in Arb :
        (aup[a] /\ arole[a] = "passive" /\ ~aup[Peer(a)] /\ amsgs = {})
            => (atimer[a] \/ voteWait[a])

(* Some arbiter is active whenever one is running and has settled. Checked *)
(* as a state property: two running arbiters that are both passive, with    *)
(* nothing in flight and no timeout armed, will stay that way.              *)
NoLastingNoActive ==
    ~(\A a \in Arb : aup[a] /\ arole[a] = "passive" /\ ~atimer[a] /\ ~voteWait[a])
    \/ amsgs # {}

=============================================================================
