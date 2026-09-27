--------------------------- MODULE SequencerPairHA ---------------------------
(***************************************************************************)
(* A model of how the two instances of the sequencer decide which of them *)
(* leads, as the code does it: applications/sequencer/SequencerThread.cpp *)
(* for the instances, and applications/arbiter/ArbiterThread.cpp with     *)
(* LeadershipDecision.hpp for the arbiter they report to.                 *)
(*                                                                         *)
(* One active arbiter is modelled here. Which of the two arbiters is      *)
(* active, and whether two can be, is the subject of ArbiterPoolHA.tla.   *)
(*                                                                         *)
(* What is modelled:                                                       *)
(*   - two instances, 1 (the primary) and 2 (the secondary), each with a  *)
(*     role, an epoch that survives a restart, a heartbeat timeout, and   *)
(*     an outstanding arbitration request with a retry count;             *)
(*   - the arbiter's record of who leads, which is lost when it restarts, *)
(*     and the period after a restart in which it declines to arbitrate; *)
(*   - three links, any of which can fail and recover: instance to        *)
(*     instance, and each instance to the arbiter;                        *)
(*   - crashes and restarts of either instance and of the arbiter;        *)
(*   - messages in flight, so that a reply can arrive after the state it *)
(*     describes has changed: status responses, arbitration reports and   *)
(*     arbitration decisions.                                             *)
(*                                                                         *)
(* What is not modelled: the order flow, the write-ahead log, the         *)
(* matching engine pair, lease expiry by time (a leader that stops        *)
(* renewing is modelled by its link or process failing instead), and      *)
(* timing, which is replaced by allowing any enabled step to happen next. *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    MaxEpoch,            \* bound on epochs, to keep the model finite
    MaxCrashes,          \* how many instance crashes may happen in one behaviour
    MaxLinkFailures,     \* how many link failures may happen in one behaviour
    MaxArbiterRestarts,  \* how many arbiter restarts may happen in one behaviour
    MaxAttempts,         \* arbitration reports sent before degrading; the code uses 6
    LateHeartbeats,      \* TRUE allows a follower's timeout to fire although its
                         \* peer is alive and sending, as a stalled process would see
    DegradedPromotion,   \* TRUE, as in the code: an instance that cannot get a decision
                         \* promotes itself. FALSE removes that path, to see what else fails.
    FailuresAfterStart,  \* TRUE: failures begin only once the pair has first settled, with
                         \* one leader, one follower, and the arbiter holding a confirmed
                         \* record. Used to look past the cold-start races.
    LateStart,           \* TRUE: the primary starts later than the secondary, so the
                         \* secondary's startup timeout can fire before its peer exists.
    Quiet                \* TRUE: messages arrive promptly. No failure, restart or timeout
                         \* happens while any message is in flight. Anything found with
                         \* Quiet = TRUE does not depend on an event landing inside a
                         \* message's flight time.

Seq == {1, 2}
Peer(s) == IF s = 1 THEN 2 ELSE 1
Max(a, b) == IF a > b THEN a ELSE b
ArbLink(s) == IF s = 1 THEN "a1" ELSE "a2"
AllLinks == {"ss", "a1", "a2"}
NoLeader == 0

VARIABLES
    up,           \* up[s]: the process is running
    role,         \* role[s] \in {"unknown", "leader", "follower"}
    epoch,        \* epoch[s]: persisted, so it survives a crash
    timer,        \* timer[s]: the peer heartbeat timeout (also the startup timeout) is armed
    pending,      \* pending[s]: an arbitration report is outstanding, with its timeout armed
    attempts,     \* attempts[s]: arbitration reports sent in the current round
    arbUp,        \* the arbiter process is running
    learning,     \* the arbiter started recently and declines when it knows nothing
    rec,          \* the arbiter's record for the sequencer group
    link,         \* the set of links that are up
    msgs,         \* messages in flight
    crashes, linkFailures, arbRestarts,
    settled,      \* the pair has settled once, as FailuresAfterStart describes
    restartedInRound, \* restartedInRound[s]: the arbiter restarted during s's current
                  \* round of arbitration reports (see ArbitrationTimeout)
    leaderAt,     \* history: every <<instance, epoch>> that has held leadership
    belowRecord,  \* history: the arbiter issued a new generation whose epoch was not
                  \* above the epoch already in its own record
    regressed     \* history: some instance became leader at an epoch below one
                  \* another instance had already led in

vars == <<up, role, epoch, timer, pending, attempts, arbUp, learning, rec, link, msgs,
          crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

EmptyRec == [leader |-> NoLeader, epoch |-> 0, confirmed |-> FALSE]

StatusMsg(to, r, e) == [type |-> "status", to |-> to, role |-> r, epoch |-> e]
ReportMsg(from, e) == [type |-> "report", from |-> from, epoch |-> e]
DecisionMsg(to, l, e) == [type |-> "decision", to |-> to, leader |-> l, epoch |-> e]

(* The arbiter can hear from, and reach, instance s. *)
Connected(s) == up[s] /\ arbUp /\ ArbLink(s) \in link

(* Heartbeats from the peer are arriving and would be accepted: the peer   *)
(* runs a heartbeat timer once it holds a role, and a heartbeat with a     *)
(* lower epoch than the receiver's is ignored.                             *)
HeartbeatsFlowing(s) ==
    /\ "ss" \in link /\ up[Peer(s)]
    /\ role[Peer(s)] # "unknown"
    /\ epoch[Peer(s)] >= epoch[s]

(* Quiet also means that what flows continuously has caught up: a follower *)
(* receiving heartbeats has followed its leader's epoch and has its timeout  *)
(* armed, and a leader connected to the arbiter has had its lease recorded. *)
HeartbeatsCaughtUp ==
    \A s \in Seq : (up[s] /\ role[s] = "follower" /\ HeartbeatsFlowing(s))
                    => (timer[s] /\ epoch[s] >= epoch[Peer(s)])
LeasesCaughtUp ==
    \A s \in Seq : (role[s] = "leader" /\ Connected(s) /\ epoch[s] >= rec.epoch)
                    => rec = [leader |-> s, epoch |-> epoch[s], confirmed |-> TRUE]
NothingInFlight == msgs = {} /\ HeartbeatsCaughtUp /\ LeasesCaughtUp
InFlightTo(s) == \E m \in msgs : (m.type # "report" /\ m.to = s)
                                 \/ (m.type = "report" /\ m.from = s)

(* An instance becomes leader at epoch e. History is recorded here. *)
RegressesAt(s, e) == \E p \in leaderAt : p[1] # s /\ p[2] > e

-----------------------------------------------------------------------------
Init ==
    /\ up = [s \in Seq |-> IF LateStart THEN s = 2 ELSE TRUE]
    /\ role = [s \in Seq |-> "unknown"]
    /\ epoch = [s \in Seq |-> 0]
    /\ timer = [s \in Seq |-> up[s]]         \* the startup election timeout
    /\ pending = [s \in Seq |-> FALSE]
    /\ attempts = [s \in Seq |-> 0]
    /\ arbUp = TRUE
    /\ learning = TRUE
    /\ rec = EmptyRec
    /\ link = AllLinks
    \* Each instance sends a StatusQuery on connecting and each answers with a StatusResponse.
    /\ msgs = IF LateStart THEN {} ELSE {StatusMsg(1, "unknown", 0), StatusMsg(2, "unknown", 0)}
    /\ crashes = 0 /\ linkFailures = 0 /\ arbRestarts = 0
    /\ settled = FALSE
    /\ restartedInRound = [s \in Seq |-> FALSE]
    /\ leaderAt = {}
    /\ belowRecord = FALSE
    /\ regressed = FALSE

-----------------------------------------------------------------------------
(* The effect of instance s adopting a role at epoch e, used by every     *)
(* action that changes a role. adopt_role re-arms the heartbeat timeout   *)
(* only on a transition into follower; a follower told again that it      *)
(* follows keeps whatever timer state it had.                              *)
Adopt(s, r, e) ==
    /\ role' = [role EXCEPT ![s] = r]
    /\ epoch' = [epoch EXCEPT ![s] = e]
    /\ timer' = [timer EXCEPT ![s] = IF r = "leader" THEN FALSE
                                     ELSE IF role[s] # "follower" THEN TRUE
                                     ELSE timer[s]]
    /\ leaderAt' = IF r = "leader" THEN leaderAt \cup {<<s, e>>} ELSE leaderAt
    /\ regressed' = (regressed \/ (r = "leader" /\ RegressesAt(s, e)))
    /\ UNCHANGED belowRecord

(* resolve_with_visible_peer: the new generation is one past the higher of *)
(* the two epochs, and the lower instance id leads.                         *)
ResolveTarget(s, peerEpoch) == Max(epoch[s], peerEpoch) + 1
ResolveRole(s) == IF s < Peer(s) THEN "leader" ELSE "follower"

(* A status response from the peer reaches s, and elect_role runs on it.   *)
ReceiveStatus(m) ==
    /\ m \in msgs /\ m.type = "status"
    /\ LET s == m.to IN
       /\ up[s]
       /\ msgs' = msgs \ {m}
       /\ IF role[s] # "unknown"
          THEN UNCHANGED <<role, epoch, timer, leaderAt, belowRecord, regressed>>
          ELSE IF m.role = "leader"
               THEN IF m.epoch > epoch[s] THEN Adopt(s, "follower", m.epoch)
                    ELSE IF m.epoch < epoch[s]
                         THEN Adopt(s, ResolveRole(s), ResolveTarget(s, m.epoch))
                         ELSE Adopt(s, "follower", epoch[s])
               ELSE Adopt(s, ResolveRole(s), ResolveTarget(s, m.epoch))
       /\ UNCHANGED <<up, pending, attempts, arbUp, learning, rec, link,
                      crashes, linkFailures, arbRestarts, settled, restartedInRound>>

(* A heartbeat from the peer reaches a follower: it follows a newer epoch  *)
(* and re-arms its timeout. A leader ignores it.                           *)
ReceiveHeartbeat(s) ==
    /\ up[s] /\ role[s] = "follower" /\ HeartbeatsFlowing(s)
    /\ ~timer[s] \/ epoch[Peer(s)] > epoch[s]
    /\ epoch' = [epoch EXCEPT ![s] = Max(epoch[s], epoch[Peer(s)])]
    /\ timer' = [timer EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<up, role, pending, attempts, arbUp, learning, rec, link, msgs,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

(* The heartbeat (or startup) timeout fires on an instance that does not   *)
(* lead. With an arbiter connection it reports; with none it promotes      *)
(* itself in a new generation (the degraded path).                         *)
HeartbeatTimeout(s) ==
    /\ up[s] /\ timer[s] /\ role[s] # "leader"
    /\ Quiet => msgs = {}
    /\ LateHeartbeats \/ ~HeartbeatsFlowing(s)
    /\ restartedInRound' = [restartedInRound EXCEPT ![s] = FALSE]
    /\ UNCHANGED settled
    /\ IF Connected(s)
       THEN /\ timer' = [timer EXCEPT ![s] = FALSE]
            /\ pending' = [pending EXCEPT ![s] = TRUE]
            /\ attempts' = [attempts EXCEPT ![s] = 1]
            /\ msgs' = msgs \cup {ReportMsg(s, epoch[s])}
            /\ UNCHANGED <<role, epoch, leaderAt, belowRecord, regressed>>
       ELSE IF DegradedPromotion
            THEN /\ Adopt(s, "leader", epoch[s] + 1)
                 /\ UNCHANGED <<pending, attempts, msgs>>
            ELSE /\ timer' = [timer EXCEPT ![s] = TRUE]
                 /\ UNCHANGED <<role, epoch, pending, attempts, msgs, leaderAt, belowRecord, regressed>>
    /\ UNCHANGED <<up, arbUp, learning, rec, link, crashes, linkFailures, arbRestarts>>

(* The arbitration timeout fires: retry while attempts remain and an      *)
(* arbiter is connected, otherwise promote in a new generation.           *)
ArbitrationTimeout(s) ==
    /\ up[s] /\ pending[s] /\ role[s] # "leader"
    /\ Quiet => msgs = {}
    \* Timing, which the model otherwise leaves out. Six reports three seconds apart span
    \* fifteen seconds, which outlasts the arbiter's ten-second learning period, and an active
    \* arbiter that is not learning answers every report it receives. So a round can run out
    \* while the arbiter is still reachable only if the arbiter restarted during the round.
    /\ (attempts[s] >= MaxAttempts /\ Connected(s)) => restartedInRound[s]
    \* And a decision already on its way is delivered well within the three-second
    \* arbitration timeout.
    /\ ~\E m \in msgs : m.type = "decision" /\ m.to = s
    /\ IF attempts[s] < MaxAttempts /\ Connected(s)
       THEN /\ attempts' = [attempts EXCEPT ![s] = attempts[s] + 1]
            /\ msgs' = msgs \cup {ReportMsg(s, epoch[s])}
            /\ UNCHANGED <<role, epoch, timer, pending, leaderAt, belowRecord, regressed>>
       ELSE IF DegradedPromotion
            THEN /\ pending' = [pending EXCEPT ![s] = FALSE]
                 /\ attempts' = [attempts EXCEPT ![s] = 0]
                 /\ Adopt(s, "leader", epoch[s] + 1)
                 /\ UNCHANGED msgs
            ELSE /\ pending' = [pending EXCEPT ![s] = FALSE]
                 /\ attempts' = [attempts EXCEPT ![s] = 0]
                 /\ timer' = [timer EXCEPT ![s] = TRUE]
                 /\ UNCHANGED <<role, epoch, msgs, leaderAt, belowRecord, regressed>>
    /\ UNCHANGED <<up, arbUp, learning, rec, link, crashes, linkFailures, arbRestarts, settled, restartedInRound>>

(* The arbiter handles an arbitration report: decide_and_broadcast.       *)
(* Note that when the record is not confirmed, the code passes an         *)
(* incumbent epoch of zero to LeadershipDecision, so the epoch it knows   *)
(* from its own record is not used.                                       *)
ArbiterDecides(m) ==
    /\ m \in msgs /\ m.type = "report"
    /\ arbUp /\ ArbLink(m.from) \in link
    /\ LET s == m.from
           p == Peer(s)
           peerConnected == Connected(p)
           hasIncumbent == rec.confirmed
           incumbentConnected == hasIncumbent /\ rec.leader # NoLeader /\ Connected(rec.leader)
           leader == IF incumbentConnected THEN rec.leader
                     ELSE IF peerConnected THEN 1 ELSE s
           newEpoch == IF incumbentConnected THEN rec.epoch
                       ELSE Max(IF hasIncumbent THEN rec.epoch ELSE 0, m.epoch) + 1
       IN IF learning /\ ~hasIncumbent
          THEN \* Declines, silently: nothing is sent back.
               /\ msgs' = msgs \ {m}
               /\ UNCHANGED <<rec, belowRecord>>
          ELSE /\ belowRecord' = (belowRecord \/ (~incumbentConnected /\ newEpoch <= rec.epoch))
               /\ rec' = [leader |-> leader, epoch |-> newEpoch, confirmed |-> TRUE]
               /\ msgs' = (msgs \ {m})
                          \cup {DecisionMsg(s, leader, newEpoch)}
                          \cup (IF peerConnected THEN {DecisionMsg(p, leader, newEpoch)} ELSE {})
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, learning, link,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, regressed>>

(* An arbitration decision reaches s: handle_arbitration_decision.         *)
(* set_epoch refuses to move backwards, so the epoch taken is the higher   *)
(* of the instance's own and the decision's.                                *)
ReceiveDecision(m) ==
    /\ m \in msgs /\ m.type = "decision"
    /\ LET s == m.to
           e == Max(epoch[s], m.epoch)
       IN /\ up[s]
          /\ msgs' = msgs \ {m}
          /\ pending' = [pending EXCEPT ![s] = FALSE]
          /\ attempts' = [attempts EXCEPT ![s] = 0]
          /\ IF m.leader = s THEN Adopt(s, "leader", e) ELSE Adopt(s, "follower", e)
    /\ UNCHANGED <<up, arbUp, learning, rec, link, crashes, linkFailures, arbRestarts, settled, restartedInRound>>

(* A leader's lease reaches the arbiter. It is sent on becoming leader     *)
(* and with every arbiter heartbeat. The arbiter refuses a lease whose     *)
(* epoch is below the one on record, whether or not that record is        *)
(* confirmed.                                                               *)
LeaseArrives(s) ==
    /\ role[s] = "leader" /\ Connected(s)
    /\ epoch[s] >= rec.epoch
    /\ rec # [leader |-> s, epoch |-> epoch[s], confirmed |-> TRUE]
    /\ rec' = [leader |-> s, epoch |-> epoch[s], confirmed |-> TRUE]
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, learning, link, msgs,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

-----------------------------------------------------------------------------
(* Failures and recoveries. *)

(* Messages that travel over a link are lost when it fails. *)
OverLink(m, l) ==
    CASE m.type = "status"   -> l = "ss"
      [] m.type = "report"   -> l = ArbLink(m.from)
      [] m.type = "decision" -> l = ArbLink(m.to)

(* The arbiter stops treating an instance as the incumbent when it loses   *)
(* its connection.                                                          *)
Unconfirm(s) == IF rec.leader = s THEN [rec EXCEPT !.confirmed = FALSE] ELSE rec

InstanceCrash(s) ==
    /\ up[s] /\ crashes < MaxCrashes
    /\ Quiet => NothingInFlight
    /\ FailuresAfterStart => settled
    /\ up' = [up EXCEPT ![s] = FALSE]
    /\ role' = [role EXCEPT ![s] = "unknown"]
    /\ timer' = [timer EXCEPT ![s] = FALSE]
    /\ pending' = [pending EXCEPT ![s] = FALSE]
    /\ attempts' = [attempts EXCEPT ![s] = 0]
    /\ rec' = Unconfirm(s)
    /\ msgs' = {m \in msgs : ~(m.type = "status" /\ m.to = s)
                             /\ ~(m.type = "decision" /\ m.to = s)}
    /\ crashes' = crashes + 1
    /\ UNCHANGED <<epoch, arbUp, learning, link, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

(* A restarted instance comes back with no role, its persisted epoch, and  *)
(* its startup timeout armed. If the peer link is up, the two exchange     *)
(* status. If the arbiter holds a confirmed record, it sends that decision  *)
(* when the instance registers.                                             *)
InstanceRestart(s) ==
    /\ ~up[s]
    /\ Quiet => NothingInFlight
    /\ up' = [up EXCEPT ![s] = TRUE]
    /\ timer' = [timer EXCEPT ![s] = TRUE]
    /\ LET statusMsgs == IF "ss" \in link /\ up[Peer(s)]
                         THEN {StatusMsg(s, role[Peer(s)], epoch[Peer(s)]),
                               StatusMsg(Peer(s), "unknown", epoch[s])}
                         ELSE {}
           registered == arbUp /\ ArbLink(s) \in link /\ rec.confirmed
           decisionMsgs == IF registered THEN {DecisionMsg(s, rec.leader, rec.epoch)} ELSE {}
       IN msgs' = msgs \cup statusMsgs \cup decisionMsgs
    /\ UNCHANGED <<role, epoch, pending, attempts, arbUp, learning, rec, link,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

LinkFails(l) ==
    /\ l \in link /\ linkFailures < MaxLinkFailures
    /\ Quiet => NothingInFlight
    /\ FailuresAfterStart => settled
    /\ link' = link \ {l}
    /\ msgs' = {m \in msgs : ~OverLink(m, l)}
    /\ rec' = CASE l = "a1" -> Unconfirm(1)
                [] l = "a2" -> Unconfirm(2)
                [] OTHER    -> rec
    /\ linkFailures' = linkFailures + 1
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, learning,
                   crashes, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

LinkRecovers(l) ==
    /\ l \in AllLinks \ link
    /\ link' = link \cup {l}
    /\ msgs' = msgs \cup
               (IF l = "ss" /\ up[1] /\ up[2]
                THEN {StatusMsg(1, role[2], epoch[2]), StatusMsg(2, role[1], epoch[1])}
                ELSE IF l # "ss"
                     THEN LET s == IF l = "a1" THEN 1 ELSE 2 IN
                          IF up[s] /\ arbUp /\ rec.confirmed
                          THEN {DecisionMsg(s, rec.leader, rec.epoch)} ELSE {}
                     ELSE {})
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, learning, rec,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

ArbiterCrash ==
    /\ arbUp /\ arbRestarts < MaxArbiterRestarts
    /\ Quiet => NothingInFlight
    /\ FailuresAfterStart => settled
    /\ arbUp' = FALSE
    /\ rec' = EmptyRec
    /\ msgs' = {m \in msgs : m.type = "status"}
    /\ arbRestarts' = arbRestarts + 1
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, learning, link,
                   crashes, linkFailures, settled, restartedInRound, leaderAt, belowRecord, regressed>>

ArbiterRestart ==
    /\ ~arbUp
    /\ arbUp' = TRUE
    /\ learning' = TRUE
    /\ restartedInRound' = [s \in Seq |-> restartedInRound[s] \/ pending[s]]
    /\ UNCHANGED settled
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, rec, link, msgs,
                   crashes, linkFailures, arbRestarts, leaderAt, belowRecord, regressed>>

PairSettles ==
    /\ ~settled
    /\ {role[1], role[2]} = {"leader", "follower"}
    /\ epoch[1] = epoch[2]
    /\ rec.confirmed /\ rec.leader # NoLeader
    /\ role[rec.leader] = "leader" /\ rec.epoch = epoch[rec.leader]
    /\ ~learning
    /\ \A m \in msgs : m.type # "status"
    /\ settled' = TRUE
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, learning, rec, link, msgs,
                   crashes, linkFailures, arbRestarts, restartedInRound, leaderAt, belowRecord, regressed>>

ArbiterStopsLearning ==
    /\ arbUp /\ learning
    /\ learning' = FALSE
    /\ UNCHANGED <<up, role, epoch, timer, pending, attempts, arbUp, rec, link, msgs,
                   crashes, linkFailures, arbRestarts, settled, restartedInRound, leaderAt, belowRecord, regressed>>

-----------------------------------------------------------------------------
Next ==
    \/ \E m \in msgs : ReceiveStatus(m)
    \/ \E m \in msgs : ArbiterDecides(m)
    \/ \E m \in msgs : ReceiveDecision(m)
    \/ \E s \in Seq : ReceiveHeartbeat(s)
    \/ \E s \in Seq : HeartbeatTimeout(s)
    \/ \E s \in Seq : ArbitrationTimeout(s)
    \/ \E s \in Seq : LeaseArrives(s)
    \/ \E s \in Seq : InstanceCrash(s)
    \/ \E s \in Seq : InstanceRestart(s)
    \/ \E l \in AllLinks : LinkFails(l)
    \/ \E l \in AllLinks : LinkRecovers(l)
    \/ ArbiterCrash
    \/ ArbiterRestart
    \/ ArbiterStopsLearning
    \/ PairSettles

Spec == Init /\ [][Next]_vars

(* Keeps the model finite. *)
EpochBound == \A s \in Seq : epoch[s] <= MaxEpoch

-----------------------------------------------------------------------------
(* Properties. *)

TypeOK ==
    /\ up \in [Seq -> BOOLEAN]
    /\ role \in [Seq -> {"unknown", "leader", "follower"}]
    /\ epoch \in [Seq -> 0..(MaxEpoch + 2)]
    /\ timer \in [Seq -> BOOLEAN]
    /\ pending \in [Seq -> BOOLEAN]

(* The property epoch fencing depends on: an epoch names at most one       *)
(* leader, ever. Every receiver accepts a PDU whose epoch equals the one   *)
(* it holds, so two leaders sharing an epoch are both believed.             *)
EpochNamesOneLeader ==
    \A p, q \in leaderAt : p[2] = q[2] => p[1] = q[1]

(* A new leader's epoch is not below any epoch another instance has led in.*)
(* Receivers discard a PDU with an epoch below the one they hold, so a      *)
(* leader at a lower epoch is ignored by anyone who saw the higher one.     *)
NoRegression == ~regressed

(* The arbiter never starts a new generation at or below the epoch in its  *)
(* own record. LeadershipDecision.hpp says the new epoch is taken from the  *)
(* higher of the record and the report, but ArbiterThread passes the        *)
(* record's epoch only when the record is confirmed.                        *)
ArbiterNeverIssuesBelowItsRecord == ~belowRecord

(* Two instances never lead at the same time. The design does not claim   *)
(* this: a partitioned leader keeps running and is fenced by its epoch.     *)
(* Checked to show the situations in which fencing is what stands between  *)
(* the venue and two leaders.                                               *)
AtMostOneLeader == ~(role[1] = "leader" /\ role[2] = "leader")

(* A follower that is running always has some way to notice that its      *)
(* peer is gone: an armed heartbeat timeout, or an outstanding arbitration  *)
(* request. Without one, a dead peer leaves the pair with no leader.        *)
FollowerCanNotice ==
    \A s \in Seq :
        (up[s] /\ role[s] = "follower" /\ ~up[Peer(s)])
            => \/ timer[s] \/ pending[s]
               \/ \E m \in msgs : (m.type = "report" /\ m.from = s)
                                 \/ (m.type = "decision" /\ m.to = s)

(* The dangerous case of two leaders: both running and both holding the   *)
(* same epoch, so that every receiver accepts both.                         *)
NoTwoLeadersSharingAnEpoch ==
    ~(up[1] /\ up[2] /\ role[1] = "leader" /\ role[2] = "leader" /\ epoch[1] = epoch[2])

=============================================================================
