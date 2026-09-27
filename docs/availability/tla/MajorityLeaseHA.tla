--------------------------- MODULE MajorityLeaseHA ---------------------------
(***************************************************************************)
(* A specification of a proposed design for deciding which of two          *)
(* instances leads, in which an instance may lead only while a majority of *)
(* three voters has granted it a lease that has not yet run out. The       *)
(* design is described in docs/availability/majority_leases.md.            *)
(*                                                                         *)
(* This is a design, not a model of the code. The code today is modelled   *)
(* by SequencerPairHA.tla and ArbiterPoolHA.tla.                           *)
(*                                                                         *)
(* The same design serves two places in the venue, so one specification    *)
(* covers both:                                                            *)
(*                                                                         *)
(*   - A pair of component instances, such as the two sequencers or the    *)
(*     two matching engines. The voters are the two instances and the      *)
(*     arbiter tier, meaning whichever arbiter is active at the time.      *)
(*   - The two arbiters. The voters are the two arbiters and the witness.  *)
(*                                                                         *)
(* In both, voters 1 and 2 are the instances that may lead and voter 3 is  *)
(* the third party, which never leads.                                     *)
(*                                                                         *)
(* The rules of the design:                                                *)
(*                                                                         *)
(*   1. A voter grants a lease to at most one instance at a time. When it  *)
(*      grants one, it promises not to grant to anyone else for the lease  *)
(*      period, counted from the moment it granted.                        *)
(*   2. An instance leads only while it holds an unexpired grant from at   *)
(*      least one voter other than itself. With its own vote, that is two  *)
(*      of the three: a majority. It counts each grant's period from the   *)
(*      moment it sent the request, which is earlier than the moment the   *)
(*      voter granted it, so the instance always believes its lease ends   *)
(*      no later than the voter believes its promise ends.                 *)
(*   3. A leader keeps its lease by asking both other voters again, over   *)
(*      and over. Either one granting is enough.                           *)
(*   4. An instance that is leading, or asking to lead, votes for itself,  *)
(*      so it grants nothing to its peer, with one exception in rule 9.    *)
(*   5. An instance that has granted its peer a lease does not ask to lead *)
(*      until that grant has run out.                                      *)
(*   6. A voter that restarts has forgotten what it promised, so it grants *)
(*      nothing for one lease period after it starts.                      *)
(*   7. Each voter remembers the highest epoch it has granted and grants   *)
(*      no lower one. An instance's epoch is kept on disk; the third       *)
(*      party's is kept only in memory. Epochs record which instance leads *)
(*      in them, as fix_common/LeaderEpoch.hpp does. A voter that refuses  *)
(*      a request says in its refusal the highest epoch it has granted.    *)
(*   8. An instance that learns of an epoch higher than the one it leads   *)
(*      in, or asks to lead in, stops and asks again above it.             *)
(*   9. An instance asking to lead that receives a request from its peer   *)
(*      at a higher epoch than its own gives up and grants the request.    *)
(*      Without this, two instances asking at the same moment with the     *)
(*      third voter down would refuse each other for ever.                 *)
(*  10. An instance whose request to lead fails waits at least one tick    *)
(*      before asking again.                                               *)
(*                                                                         *)
(* Why one specification covers the pair with the arbiter pool as a single *)
(* voter: at most one arbiter is active at a time, which this same design  *)
(* guarantees when it is applied to the arbiters. When a different arbiter *)
(* becomes active, it knows nothing of what the previous one promised, so  *)
(* it obeys rule 6 as if it had restarted. For the pair, a change of       *)
(* active arbiter is therefore the third voter restarting.                 *)
(*                                                                         *)
(* Time is modelled as a clock that advances one tick at a time. Every     *)
(* period is a count of ticks remaining, which each tick reduces by one.   *)
(* A message can take any number of ticks to arrive, or be lost. Safety    *)
(* does not depend on how long a message takes. The liveness property does *)
(* depend on it, and is checked with Prompt = TRUE, under which every      *)
(* message arrives within the tick it was sent in and a leader asks for    *)
(* renewal in every tick.                                                  *)
(*                                                                         *)
(* Not modelled: clock drift between machines (in the design, the holder   *)
(* of a lease shortens it by the largest drift allowed); the order flow;   *)
(* the write-ahead log; how the new leader catches up; receivers on the    *)
(* order path, which are a second source of the higher epoch in rule 8.    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Lease,              \* the lease period, in ticks
    MaxEpoch,           \* bound on epoch generations, to keep the model finite
    MaxCrashes,         \* crashes of instances 1 and 2 in one behaviour
    MaxThirdRestarts,   \* crashes of the third voter in one behaviour
    MaxLinkFailures,    \* link failures in one behaviour
    ThirdUpAtStart,     \* FALSE: the third voter is down when the instances start
    ThirdStaysDown,     \* TRUE: the third voter, once down, never restarts
    Prompt,             \* TRUE: every message arrives within the tick it was sent in, and
                        \* a leader asks each reachable voter for renewal in every tick
    \* The following are the design's rules. Each is TRUE in the design. Setting one to
    \* FALSE removes that rule, to show that the rule is needed.
    HolderCountsFromSend,  \* rule 2: FALSE counts the lease from when the grant arrived
    WaitOutOwnGrant,       \* rule 5
    RestartWaits,          \* rule 6
    \* TRUE: an instance keeps its promises on disk, so a crash does not make it forget them,
    \* and it needs no wait after restarting (rule 6 applies to the third voter only). A kept
    \* promise goes on running down while the instance is down. The vote an instance gave
    \* itself is not kept: a restarted instance holds no role.
    InstancesKeepPromises,
    PeerVotes,             \* an instance votes for its peer at all; FALSE leaves the third
                           \* voter as the only voter other than the instance itself
    CandidateYields,       \* rule 9
    \* TRUE adds degraded self-promotion: an instance asking to lead that receives no grant
    \* may lead on its own vote alone. It is not part of the design.
    DegradedPromotion

Cand == {1, 2}
Third == 3
Voters == {1, 2, 3}
None == 0
Peer(s) == IF s = 1 THEN 2 ELSE 1
Others(s) == {Peer(s), Third}
Max(a, b) == IF a > b THEN a ELSE b

Link(a, b) == IF {a, b} = {1, 2} THEN "12" ELSE IF {a, b} = {1, 3} THEN "13" ELSE "23"
AllLinks == {"12", "13", "23"}

(* The next epoch above x in which instance l leads (LeaderEpoch::next_for). *)
NextFor(x, l) ==
    LET base == (x \div 4) * 4 IN IF base + l > x THEN base + l ELSE base + 4 + l

VARIABLES
    up,          \* up[v]: voter v is running
    role,        \* role[s], for s in 1..2: "unknown", "follower", "candidate" or "leader"
    epoch,       \* epoch[v]: the highest epoch v has granted or led in. Kept on disk by
                 \* instances 1 and 2; lost by the third voter when it restarts.
    candEpoch,   \* candEpoch[s]: the epoch s is asking to lead in, while a candidate
    promise,     \* promise[v]: [to, left] -- v has promised not to grant anyone but "to"
                 \* for "left" more ticks. An instance leading or asking to lead holds a
                 \* promise to itself that does not run down.
    quiet,       \* quiet[v]: ticks remaining in which v, newly restarted, grants nothing
    held,        \* held[s][v]: ticks remaining, as s counts them, on v's grant to s
    sent,        \* sent[s][v]: s has asked v for renewal in the current tick
    backoff,     \* backoff[s]: s gave up asking to lead in the current tick (rule 10)
    alone,       \* alone[s]: s leads on its own vote only (DegradedPromotion)
    link,        \* the links that are up
    msgs,        \* messages in flight; each carries "rem", the ticks remaining before
                 \* it is useless, and is dropped when that reaches zero
    crashes, thirdRestarts, linkFailures,
    leaderAt,    \* history: every <<instance, epoch>> that has led
    regressed    \* history: an instance began leading below an epoch the other led in

vars == <<up, role, epoch, candEpoch, promise, quiet, held, sent, backoff, alone, link, msgs,
          crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

NoPromise == [to |-> None, left |-> 0]

(* A request to be granted a lease, sent both to ask to lead and to keep a lease. *)
Req(from, to, e) == [type |-> "req", from |-> from, to |-> to, epoch |-> e, rem |-> Lease]
(* A grant. It carries back the ticks remaining on the request it answers. *)
Ack(from, to, e, r) == [type |-> "ack", from |-> from, to |-> to, epoch |-> e, rem |-> r]
(* A refusal. It carries the highest epoch the voter has granted. *)
Nack(from, to, e) == [type |-> "nack", from |-> from, to |-> to, epoch |-> e, rem |-> 1]

LinkUp(a, b) == Link(a, b) \in link

(* s holds an unexpired grant from some voter other than itself. *)
HasGrant(s) == \E v \in Others(s) : held[s][v] > 0

(* s is leading and entitled to act as leader: it would send order flow. *)
Acting(s) == role[s] = "leader" /\ (HasGrant(s) \/ alone[s])

RegressesAt(s, e) == \E p \in leaderAt : p[1] # s /\ p[2] > e

(* Whether voter v would grant a lease at epoch e to instance x now. *)
WouldGrant(v, x, e) ==
    /\ up[v]
    /\ quiet[v] = 0
    /\ v \in Cand => PeerVotes
    /\ (promise[v].left = 0 \/ promise[v].to = x)
    /\ e >= epoch[v]

(* Rule 9: candidate v gives way to a request from its peer at a higher epoch. *)
Yields(v, x, e) ==
    /\ CandidateYields /\ PeerVotes
    /\ v \in Cand /\ up[v] /\ role[v] = "candidate" /\ quiet[v] = 0
    /\ e > candEpoch[v] /\ e >= epoch[v]

ClearedHeld == [v \in Voters |-> 0]
NoneSent == [v \in Voters |-> FALSE]

(* A renewal that a leader owes a voter in the current tick, under Prompt. *)
RenewalOwed(s, v) == up[s] /\ Acting(s) /\ ~alone[s] /\ v \in Others(s) /\ LinkUp(s, v)

-----------------------------------------------------------------------------
Init ==
    /\ up = [v \in Voters |-> IF v = Third THEN ThirdUpAtStart ELSE TRUE]
    /\ role = [s \in Cand |-> "unknown"]
    /\ epoch = [v \in Voters |-> 0]
    /\ candEpoch = [s \in Cand |-> 0]
    /\ promise = [v \in Voters |-> NoPromise]
    /\ quiet = [v \in Voters |-> 0]
    /\ held = [s \in Cand |-> ClearedHeld]
    /\ sent = [s \in Cand |-> NoneSent]
    /\ backoff = [s \in Cand |-> FALSE]
    /\ alone = [s \in Cand |-> FALSE]
    /\ link = AllLinks
    /\ msgs = {}
    /\ crashes = 0 /\ thirdRestarts = 0 /\ linkFailures = 0
    /\ leaderAt = {}
    /\ regressed = FALSE

-----------------------------------------------------------------------------
(* Instance s asks to lead. It may do so at any time that the rules allow, *)
(* which covers a heartbeat timeout firing late as well as on time.         *)
StartCandidacy(s) ==
    /\ up[s] /\ role[s] \in {"unknown", "follower"} /\ quiet[s] = 0 /\ ~backoff[s]
    /\ WaitOutOwnGrant => (promise[s].left = 0 \/ promise[s].to = s)
    /\ LET e == NextFor(epoch[s], s) IN
       /\ role' = [role EXCEPT ![s] = "candidate"]
       /\ candEpoch' = [candEpoch EXCEPT ![s] = e]
       /\ promise' = [promise EXCEPT ![s] = [to |-> s, left |-> Lease]]
       /\ held' = [held EXCEPT ![s] = ClearedHeld]
       /\ msgs' = msgs \cup {Req(s, v, e) : v \in {w \in Others(s) : LinkUp(s, w)}}
    /\ UNCHANGED <<up, epoch, quiet, sent, backoff, alone, link, crashes, thirdRestarts, linkFailures,
                   leaderAt, regressed>>

(* A candidate gives up, which any timeout may cause. It releases the vote *)
(* it gave itself. Any grant still on its way to it is ignored on arrival.  *)
(* Under Prompt, it gives up only once every answer has arrived.            *)
AbandonCandidacy(s) ==
    /\ up[s] /\ role[s] = "candidate"
    /\ Prompt => ~\E m \in msgs : m.to = s \/ m.from = s
    /\ role' = [role EXCEPT ![s] = "follower"]
    /\ promise' = [promise EXCEPT ![s] = NoPromise]
    /\ backoff' = [backoff EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<up, epoch, candEpoch, quiet, held, sent, alone, link, msgs,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

(* Degraded self-promotion, which the design removes: a candidate leads on *)
(* its own vote alone.                                                      *)
PromoteAlone(s) ==
    /\ DegradedPromotion
    /\ up[s] /\ role[s] = "candidate"
    /\ role' = [role EXCEPT ![s] = "leader"]
    /\ epoch' = [epoch EXCEPT ![s] = candEpoch[s]]
    /\ alone' = [alone EXCEPT ![s] = TRUE]
    /\ leaderAt' = leaderAt \cup {<<s, candEpoch[s]>>}
    /\ regressed' = (regressed \/ RegressesAt(s, candEpoch[s]))
    /\ UNCHANGED <<up, candEpoch, promise, quiet, held, sent, backoff, link, msgs,
                   crashes, thirdRestarts, linkFailures>>

(* A leader asks voter v to renew its lease, at most once in each tick. *)
SendRenewal(s, v) ==
    /\ up[s] /\ Acting(s) /\ ~alone[s] /\ v \in Others(s) /\ LinkUp(s, v)
    /\ ~sent[s][v]
    /\ sent' = [sent EXCEPT ![s][v] = TRUE]
    /\ msgs' = msgs \cup {Req(s, v, epoch[s])}
    /\ UNCHANGED <<up, role, epoch, candEpoch, promise, quiet, held, backoff, alone, link,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

(* A request reaches voter v, which grants or refuses it. An instance that *)
(* grants its peer a lease becomes, or stays, its follower.                 *)
ReceiveReq(m) ==
    /\ m \in msgs /\ m.type = "req" /\ up[m.to]
    /\ LET v == m.to
           x == m.from
       IN IF WouldGrant(v, x, m.epoch) \/ Yields(v, x, m.epoch)
          THEN /\ promise' = [promise EXCEPT ![v] = [to |-> x, left |-> Lease]]
               /\ epoch' = [epoch EXCEPT ![v] = m.epoch]
               /\ role' = IF v \in Cand THEN [role EXCEPT ![v] = "follower"] ELSE role
               /\ msgs' = (msgs \ {m}) \cup {Ack(v, x, m.epoch, m.rem)}
          ELSE /\ msgs' = (msgs \ {m}) \cup {Nack(v, x, epoch[v])}
               /\ UNCHANGED <<promise, epoch, role>>
    /\ UNCHANGED <<up, candEpoch, quiet, held, sent, backoff, alone, link,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

(* A grant reaches instance x. A candidate that receives one for the epoch *)
(* it asked for now holds a majority and leads; a leader that receives one  *)
(* for its own epoch extends its lease. Any other grant is out of date.     *)
ReceiveAck(m) ==
    /\ m \in msgs /\ m.type = "ack" /\ up[m.to]
    /\ LET x == m.to
           v == m.from
           left == IF HolderCountsFromSend THEN m.rem ELSE Lease
       IN IF role[x] = "candidate" /\ m.epoch = candEpoch[x]
          THEN /\ role' = [role EXCEPT ![x] = "leader"]
               /\ epoch' = [epoch EXCEPT ![x] = m.epoch]
               /\ held' = [held EXCEPT ![x][v] = left]
               /\ leaderAt' = leaderAt \cup {<<x, m.epoch>>}
               /\ regressed' = (regressed \/ RegressesAt(x, m.epoch))
          ELSE IF role[x] = "leader" /\ ~alone[x] /\ m.epoch = epoch[x]
               THEN /\ held' = [held EXCEPT ![x][v] = Max(held[x][v], left)]
                    /\ UNCHANGED <<role, epoch, leaderAt, regressed>>
               ELSE UNCHANGED <<role, epoch, held, leaderAt, regressed>>
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<up, candEpoch, promise, quiet, sent, backoff, alone, link,
                   crashes, thirdRestarts, linkFailures>>

(* A refusal reaches instance x. If it names a newer epoch than the one x  *)
(* leads in, or asks to lead in, x stops (rule 8). Either way x learns the  *)
(* epoch, so that its next request is above it.                             *)
ReceiveNack(m) ==
    /\ m \in msgs /\ m.type = "nack" /\ up[m.to]
    /\ LET x == m.to
           newer == \/ role[x] = "leader" /\ m.epoch > epoch[x]
                    \/ role[x] = "candidate" /\ m.epoch > candEpoch[x]
       IN IF newer
          THEN /\ role' = [role EXCEPT ![x] = "follower"]
               /\ promise' = [promise EXCEPT ![x] = NoPromise]
               /\ held' = [held EXCEPT ![x] = ClearedHeld]
               /\ alone' = [alone EXCEPT ![x] = FALSE]
               /\ epoch' = [epoch EXCEPT ![x] = Max(epoch[x], m.epoch)]
               /\ backoff' = [backoff EXCEPT ![x] = TRUE]
          ELSE /\ epoch' = IF role[x] \in {"unknown", "follower"}
                           THEN [epoch EXCEPT ![x] = Max(epoch[x], m.epoch)]
                           ELSE epoch
               /\ UNCHANGED <<role, promise, held, alone, backoff>>
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<up, candEpoch, quiet, sent, link, crashes, thirdRestarts, linkFailures,
                   leaderAt, regressed>>

(* A message addressed to a voter that is down is lost. *)
DropToDown(m) ==
    /\ m \in msgs /\ ~up[m.to]
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<up, role, epoch, candEpoch, promise, quiet, held, sent, backoff, alone, link,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

(* A leader whose lease has run out stops leading. Until it does, it is    *)
(* not acting (see Acting), so this step only tidies its state.             *)
LeaseRunsOut(s) ==
    /\ up[s] /\ role[s] = "leader" /\ ~Acting(s)
    /\ role' = [role EXCEPT ![s] = "follower"]
    /\ promise' = [promise EXCEPT ![s] = NoPromise]
    /\ held' = [held EXCEPT ![s] = ClearedHeld]
    /\ UNCHANGED <<up, epoch, candEpoch, quiet, sent, backoff, alone, link, msgs,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

(* One tick of the clock. Every period remaining is reduced by one, except  *)
(* the vote an instance gives itself while it leads or asks to lead.         *)
Dec(n) == IF n > 0 THEN n - 1 ELSE 0
SelfHeld(v) == v \in Cand /\ role[v] \in {"candidate", "leader"} /\ promise[v].to = v
Tick ==
    /\ Prompt => /\ msgs = {}
                 /\ \A s \in Cand, v \in Voters : RenewalOwed(s, v) => sent[s][v]
    /\ promise' = [v \in Voters |-> IF SelfHeld(v) THEN promise[v]
                                    ELSE [promise[v] EXCEPT !.left = Dec(@)]]
    /\ quiet' = [v \in Voters |-> Dec(quiet[v])]
    /\ held' = [s \in Cand |-> [v \in Voters |-> Dec(held[s][v])]]
    /\ sent' = [s \in Cand |-> NoneSent]
    /\ backoff' = [s \in Cand |-> FALSE]
    /\ msgs' = {[m EXCEPT !.rem = m.rem - 1] : m \in {n \in msgs : n.rem > 1}}
    /\ UNCHANGED <<up, role, epoch, candEpoch, alone, link, crashes, thirdRestarts, linkFailures,
                   leaderAt, regressed>>

-----------------------------------------------------------------------------
(* Failures and recoveries. *)

OverLink(m, l) == Link(m.from, m.to) = l

Crash(v) ==
    /\ up[v]
    /\ IF v = Third THEN thirdRestarts < MaxThirdRestarts ELSE crashes < MaxCrashes
    /\ up' = [up EXCEPT ![v] = FALSE]
    /\ role' = IF v \in Cand THEN [role EXCEPT ![v] = "unknown"] ELSE role
    \* An instance keeps its epoch on disk; the third voter holds its only in memory.
    /\ epoch' = IF v = Third THEN [epoch EXCEPT ![v] = 0] ELSE epoch
    /\ promise' = [promise EXCEPT ![v] = IF v \in Cand /\ InstancesKeepPromises /\ promise[v].to # v
                                          THEN promise[v] ELSE NoPromise]
    /\ quiet' = [quiet EXCEPT ![v] = 0]
    /\ held' = IF v \in Cand THEN [held EXCEPT ![v] = ClearedHeld] ELSE held
    /\ alone' = IF v \in Cand THEN [alone EXCEPT ![v] = FALSE] ELSE alone
    /\ msgs' = {m \in msgs : m.to # v}
    /\ crashes' = IF v = Third THEN crashes ELSE crashes + 1
    /\ thirdRestarts' = IF v = Third THEN thirdRestarts + 1 ELSE thirdRestarts
    /\ UNCHANGED <<candEpoch, sent, backoff, link, linkFailures, leaderAt, regressed>>

Restart(v) ==
    /\ ~up[v]
    /\ v = Third => ~ThirdStaysDown
    /\ up' = [up EXCEPT ![v] = TRUE]
    /\ quiet' = [quiet EXCEPT ![v] = IF RestartWaits /\ ~(v \in Cand /\ InstancesKeepPromises) THEN Lease ELSE 0]
    /\ UNCHANGED <<role, epoch, candEpoch, promise, held, sent, backoff, alone, link, msgs,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

LinkFails(l) ==
    /\ l \in link /\ linkFailures < MaxLinkFailures
    /\ link' = link \ {l}
    /\ msgs' = {m \in msgs : ~OverLink(m, l)}
    /\ linkFailures' = linkFailures + 1
    /\ UNCHANGED <<up, role, epoch, candEpoch, promise, quiet, held, sent, backoff, alone,
                   crashes, thirdRestarts, leaderAt, regressed>>

LinkRecovers(l) ==
    /\ l \in AllLinks \ link
    /\ link' = link \cup {l}
    /\ UNCHANGED <<up, role, epoch, candEpoch, promise, quiet, held, sent, backoff, alone, msgs,
                   crashes, thirdRestarts, linkFailures, leaderAt, regressed>>

-----------------------------------------------------------------------------
Next ==
    \/ \E s \in Cand : StartCandidacy(s) \/ AbandonCandidacy(s) \/ PromoteAlone(s)
                       \/ LeaseRunsOut(s)
    \/ \E s \in Cand, v \in Voters : SendRenewal(s, v)
    \/ \E m \in msgs : ReceiveReq(m) \/ ReceiveAck(m) \/ ReceiveNack(m) \/ DropToDown(m)
    \/ Tick
    \/ \E v \in Voters : Crash(v) \/ Restart(v)
    \/ \E l \in AllLinks : LinkFails(l) \/ LinkRecovers(l)

Spec == Init /\ [][Next]_vars

(* For the liveness property: every step the design takes happens once it  *)
(* has been possible for long enough, and whatever has failed recovers,     *)
(* except a third voter that ThirdStaysDown keeps down.                     *)
Fairness ==
    /\ WF_vars(Tick)
    /\ WF_vars(\E m \in msgs : ReceiveReq(m) \/ ReceiveAck(m) \/ ReceiveNack(m) \/ DropToDown(m))
    /\ \A s \in Cand : /\ WF_vars(StartCandidacy(s))
                       /\ WF_vars(AbandonCandidacy(s))
                       /\ WF_vars(LeaseRunsOut(s))
                       /\ \A v \in Voters : WF_vars(SendRenewal(s, v))
    /\ \A v \in Voters : WF_vars(Restart(v))
    /\ \A l \in AllLinks : WF_vars(LinkRecovers(l))

LiveSpec == Spec /\ Fairness

(* Keeps the model finite. *)
EpochBound == /\ \A v \in Voters : epoch[v] \div 4 <= MaxEpoch
              /\ \A s \in Cand : candEpoch[s] \div 4 <= MaxEpoch

-----------------------------------------------------------------------------
(* Properties. *)

TypeOK ==
    /\ up \in [Voters -> BOOLEAN]
    /\ role \in [Cand -> {"unknown", "follower", "candidate", "leader"}]
    /\ promise \in [Voters -> [to : Voters \cup {None}, left : 0..Lease]]
    /\ quiet \in [Voters -> 0..Lease]
    /\ held \in [Cand -> [Voters -> 0..Lease]]

(* The property the design exists for: two instances never act as leader  *)
(* at the same moment, whatever fails and however long messages take.       *)
AtMostOneActing == ~(Acting(1) /\ Acting(2))

(* An epoch names at most one leader, ever. *)
EpochNamesOneLeader == \A p, q \in leaderAt : p[2] = q[2] => p[1] = q[1]

(* A new leader's epoch is not below one the other instance has led in.   *)
(* This does not hold: see findings.md section 11. It is checked to show    *)
(* the one way it fails.                                                    *)
NoRegression == ~regressed

(* Liveness: from some point on, an instance acts as leader for good. With *)
(* ThirdStaysDown this says that losing the third voter does not stop the   *)
(* pair, which is the concern degraded self-promotion was meant to address. *)
EventuallyLeaderForGood == <>[](\E s \in Cand : Acting(s))

=============================================================================
