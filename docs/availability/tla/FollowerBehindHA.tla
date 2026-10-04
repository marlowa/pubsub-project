--------------------------- MODULE FollowerBehindHA ---------------------------
(***************************************************************************)
(* A specification of rule 11 of the leadership design, which keeps an     *)
(* instance whose log lacks commands the matching engine has acted on from *)
(* leading. The rule is described in                                       *)
(* docs/availability/a_follower_behind_does_not_lead.md.                   *)
(*                                                                         *)
(* Instances 1 and 2 may lead; voter 3, the arbiter, never does. A leader  *)
(* normally has the matching engine act on a command only once its peer    *)
(* holds it. When it acts on a command its peer does not hold, rule 11     *)
(* applies:                                                                *)
(*                                                                         *)
(*  11. A leader acts on a command its peer does not hold only once a      *)
(*      voter other than itself has recorded that its peer may not lead.   *)
(*      The statement travels on the leader's lease requests. Each one     *)
(*      carries a number, so that a late message cannot undo a newer one,  *)
(*      and a grant says which statement the voter recorded. A leader      *)
(*      starts each leadership saying that its peer may not lead, and says *)
(*      that it may lead only once the peer holds every command the leader *)
(*      holds. The leader is itself a voter, and records each statement on *)
(*      its own disk before it sends it, so that after a restart it still  *)
(*      refuses a peer that is behind. A voter refuses a lease to an       *)
(*      instance it has recorded may not lead, and an instance that has    *)
(*      recorded that it may not lead does not ask to lead.                *)
(*                                                                         *)
(* How leadership is modelled. MajorityLeaseHA.tla checks the lease rules  *)
(* and shows that two instances never act as leader at the same moment.    *)
(* This specification takes that as given and models leadership as one     *)
(* step: when no instance is acting, an instance becomes leader if one      *)
(* other voter that is running grants it. A leader may stop acting at any   *)
(* moment, which stands for its lease running out. This leaves out the      *)
(* timing of leases, which is what makes it small enough to check with      *)
(* more crashes, commands and statements than the lease specification       *)
(* allows. A voter may refuse or ignore any request, so every behaviour the *)
(* lease rules allow is included, and some they do not.                     *)
(*                                                                         *)
(* Each instance's log is the set of commands it holds, kept on disk, and   *)
(* "acted" is the set of commands the matching engine has acted on. A       *)
(* leader writes a command, copies its log to its peer, and has the engine  *)
(* act on a command, each as a separate step, so a leader can stop or die   *)
(* between any two of them. A leader knows whether its peer holds a command *)
(* because the peer's acknowledgement says so; the specification lets the   *)
(* leader read the peer's log directly, which allows every behaviour the    *)
(* acknowledgements allow.                                                  *)
(*                                                                         *)
(* Messages carrying statements may be delayed, reordered or lost. A grant  *)
(* that echoes a statement either reaches the leader at once or is lost. A  *)
(* later echo would only let the leader act later, which it may always     *)
(* choose to do, so this allows every behaviour that delayed echoes allow.  *)
(* MaxInFlight bounds the statements in flight at one time, to keep the     *)
(* model finite.                                                            *)
(*                                                                         *)
(* Not modelled: the order of commands within a log; how a follower's log   *)
(* is repaired when it rejoins a leader whose log differs; the operator's   *)
(* tool that lets the follower lead after the old leader's machine is lost. *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    MaxCommands,           \* bound on the commands written in one behaviour
    MaxStatements,         \* bound on the statements one leader makes in one leadership
    MaxElections,          \* bound on the leaderships in one behaviour
    MaxCrashes,            \* crashes of instances 1 and 2 in one behaviour
    MaxThirdRestarts,      \* crashes of voter 3 in one behaviour
    MaxInFlight,           \* bound on the statements in flight at one time
    \* The following are rule 11. Each is TRUE in the design, except InstancesRecordFirst.
    \* Setting one to FALSE removes that part, to show whether it is needed.
    RecordStatement,       \* FALSE: a leader acts on a command its peer lacks at once
    LeaderStartsBarred,    \* FALSE: a leader starts each leadership saying its peer may lead
    LeaderRecordsOwn,      \* FALSE: a leader does not record its own statement
    \* TRUE: an instance writes a statement it receives to disk before granting. FALSE: it
    \* records the statement in memory, grants, and writes it to disk in a later step, so a
    \* crash before that step loses it. The design does the latter.
    InstancesRecordFirst,
    ThirdKeepsRecord       \* FALSE: voter 3 loses its record when it restarts

Cand == {1, 2}
Third == 3
Voters == {1, 2, 3}
None == 0
Peer(s) == IF s = 1 THEN 2 ELSE 1
Others(s) == {Peer(s), Third}
Max(a, b) == IF a > b THEN a ELSE b

NoKey == <<0, 0>>
KeyAbove(a, b) == a[1] > b[1] \/ (a[1] = b[1] /\ a[2] > b[2])

VARIABLES
    up,          \* up[v]: voter v is running
    leader,      \* the instance acting as leader, or None
    lepoch,      \* lepoch[s]: the epoch of s's latest leadership
    elections,   \* the number of leaderships so far; each one's epoch is its count
    log,         \* log[s]: the commands instance s holds, kept on disk
    acted,       \* the commands the matching engine has acted on
    nextCmd,     \* the number of commands written so far
    barred,      \* barred[v]: the instance voter v has recorded may not lead, or None
    barKey,      \* barKey[v]: <<epoch, number>> of the statement voter v recorded
    barDisk,     \* barDisk[v], barKeyDisk[v]: the same, as written to disk; for voter 3,
    barKeyDisk,  \* as kept on disk and by the passive arbiter
    stmt,        \* stmt[s]: [bar, num] -- what leader s says about its peer, and its number
    confirmed,   \* confirmed[s]: the highest statement number a grant to s has echoed
    msgs,        \* statements in flight
    crashes, thirdRestarts,
    actors       \* history: the instances that have had the engine act on a command

vars == <<up, leader, lepoch, elections, log, acted, nextCmd, barred, barKey, barDisk, barKeyDisk,
          stmt, confirmed, msgs, crashes, thirdRestarts, actors>>
recVars == <<barred, barKey, barDisk, barKeyDisk>>

(* A leader's statement about its peer, carried on a lease request. *)
Statement(s, v) ==
    [type |-> "st", from |-> s, to |-> v, key |-> <<lepoch[s], stmt[s].num>>, bar |-> stmt[s].bar]

(* Instance s records its own statement, durably, with LeaderRecordsOwn. *)
OwnRecord(s, bar, key) ==
    LET b == IF bar THEN Peer(s) ELSE None IN
    IF LeaderRecordsOwn
    THEN /\ barred' = [barred EXCEPT ![s] = b]
         /\ barKey' = [barKey EXCEPT ![s] = key]
         /\ barDisk' = [barDisk EXCEPT ![s] = b]
         /\ barKeyDisk' = [barKeyDisk EXCEPT ![s] = key]
    ELSE UNCHANGED recVars

-----------------------------------------------------------------------------
Init ==
    /\ up = [v \in Voters |-> TRUE]
    /\ leader = None
    /\ lepoch = [s \in Cand |-> 0]
    /\ elections = 0
    /\ log = [s \in Cand |-> {}]
    /\ acted = {}
    /\ nextCmd = 0
    /\ barred = [v \in Voters |-> None]
    /\ barKey = [v \in Voters |-> NoKey]
    /\ barDisk = [v \in Voters |-> None]
    /\ barKeyDisk = [v \in Voters |-> NoKey]
    /\ stmt = [s \in Cand |-> [bar |-> TRUE, num |-> 0]]
    /\ confirmed = [s \in Cand |-> 0]
    /\ msgs = {}
    /\ crashes = 0
    /\ thirdRestarts = 0
    /\ actors = {}

-----------------------------------------------------------------------------
(* Leadership. *)

(* Instance x becomes leader: nobody is acting, x has not recorded that it may *)
(* not lead, and one other running voter that has not recorded that x may not  *)
(* lead grants it. Its first statement is made and recorded at once.           *)
Elect(x) ==
    /\ leader = None /\ up[x] /\ barred[x] # x /\ elections < MaxElections
    /\ \E v \in Others(x) : up[v] /\ barred[v] # x
    /\ leader' = x
    /\ elections' = elections + 1
    /\ lepoch' = [lepoch EXCEPT ![x] = elections + 1]
    /\ stmt' = [stmt EXCEPT ![x] = [bar |-> LeaderStartsBarred, num |-> 1]]
    /\ confirmed' = [confirmed EXCEPT ![x] = 0]
    /\ OwnRecord(x, LeaderStartsBarred, <<elections + 1, 1>>)
    /\ UNCHANGED <<up, log, acted, nextCmd, msgs, crashes, thirdRestarts, actors>>

(* The leader's lease runs out, and it stops acting. *)
StopActing(s) ==
    /\ leader = s
    /\ leader' = None
    /\ UNCHANGED <<up, lepoch, elections, log, acted, nextCmd, recVars, stmt, confirmed, msgs,
                   crashes, thirdRestarts, actors>>

-----------------------------------------------------------------------------
(* Statements and their echoes. *)

(* The leader sends its current statement to both other voters, as it does on *)
(* every lease request.                                                        *)
SendStatement(s) ==
    /\ leader = s
    /\ msgs' = msgs \cup {Statement(s, v) : v \in Others(s)}
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, recVars, stmt, confirmed,
                   crashes, thirdRestarts, actors>>

(* A statement reaches voter v, which grants and records it if it is newer than *)
(* the one it holds. Voter 3's record is durable at once. An instance's is       *)
(* durable at once with InstancesRecordFirst, and otherwise only after DiskWrite. *)
(* If the voter now holds the statement, its grant echoes it, and the echo either *)
(* reaches the leader at once, if that leadership is still current, or is lost.   *)
ReceiveStatement(m) ==
    /\ m \in msgs /\ m.type = "st" /\ up[m.to]
    /\ LET v == m.to
           rec == KeyAbove(m.key, barKey[v])
           b == IF m.bar THEN Peer(m.from) ELSE None
           durable == v = Third \/ InstancesRecordFirst
       IN /\ barred' = IF rec THEN [barred EXCEPT ![v] = b] ELSE barred
          /\ barKey' = IF rec THEN [barKey EXCEPT ![v] = m.key] ELSE barKey
          /\ barDisk' = IF rec /\ durable THEN [barDisk EXCEPT ![v] = b] ELSE barDisk
          /\ barKeyDisk' = IF rec /\ durable THEN [barKeyDisk EXCEPT ![v] = m.key] ELSE barKeyDisk
          /\ msgs' = msgs \ {m}
          /\ \/ /\ (rec \/ barKey[v] = m.key) /\ leader = m.from /\ m.key[1] = lepoch[m.from]
                /\ confirmed' = [confirmed EXCEPT ![m.from] = Max(@, m.key[2])]
             \/ UNCHANGED confirmed
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, stmt,
                   crashes, thirdRestarts, actors>>

(* A message is lost, or refused. *)
Lose(m) ==
    /\ m \in msgs
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, recVars, stmt, confirmed,
                   crashes, thirdRestarts, actors>>

(* Leader s starts saying that its peer may not lead: it records the statement *)
(* itself and sends it to both voters at once.                                 *)
StartAlone(s) ==
    /\ leader = s /\ ~stmt[s].bar /\ stmt[s].num < MaxStatements
    /\ stmt' = [stmt EXCEPT ![s] = [bar |-> TRUE, num |-> @.num + 1]]
    /\ OwnRecord(s, TRUE, <<lepoch[s], stmt[s].num + 1>>)
    /\ msgs' = msgs \cup {[type |-> "st", from |-> s, to |-> v, key |-> <<lepoch[s], stmt[s].num + 1>>, bar |-> TRUE]
                          : v \in Others(s)}
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, confirmed, crashes, thirdRestarts, actors>>

(* Leader s says that its peer may lead again, once its peer holds every command *)
(* the leader holds. From this step on it acts only on commands its peer holds.  *)
StopAlone(s) ==
    /\ leader = s /\ stmt[s].bar /\ stmt[s].num < MaxStatements
    /\ log[s] \subseteq log[Peer(s)]
    /\ stmt' = [stmt EXCEPT ![s] = [bar |-> FALSE, num |-> @.num + 1]]
    /\ OwnRecord(s, FALSE, <<lepoch[s], stmt[s].num + 1>>)
    /\ msgs' = msgs \cup {[type |-> "st", from |-> s, to |-> v, key |-> <<lepoch[s], stmt[s].num + 1>>, bar |-> FALSE]
                          : v \in Others(s)}
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, confirmed, crashes, thirdRestarts, actors>>

(* An instance's background write of its record reaches the disk. *)
DiskWrite(v) ==
    /\ v \in Cand /\ up[v]
    /\ <<barDisk[v], barKeyDisk[v]>> # <<barred[v], barKey[v]>>
    /\ barDisk' = [barDisk EXCEPT ![v] = barred[v]]
    /\ barKeyDisk' = [barKeyDisk EXCEPT ![v] = barKey[v]]
    /\ UNCHANGED <<up, leader, lepoch, elections, log, acted, nextCmd, barred, barKey, stmt, confirmed,
                   msgs, crashes, thirdRestarts, actors>>

-----------------------------------------------------------------------------
(* Commands. *)

(* The leader writes a new command to its own log. *)
Write(s) ==
    /\ leader = s /\ nextCmd < MaxCommands
    /\ nextCmd' = nextCmd + 1
    /\ log' = [log EXCEPT ![s] = @ \cup {nextCmd + 1}]
    /\ UNCHANGED <<up, leader, lepoch, elections, acted, recVars, stmt, confirmed, msgs,
                   crashes, thirdRestarts, actors>>

(* The leader copies its log to its peer. *)
Replicate(s) ==
    /\ leader = s /\ up[Peer(s)]
    /\ ~(log[s] \subseteq log[Peer(s)])
    /\ log' = [log EXCEPT ![Peer(s)] = @ \cup log[s]]
    /\ UNCHANGED <<up, leader, lepoch, elections, acted, nextCmd, recVars, stmt, confirmed, msgs,
                   crashes, thirdRestarts, actors>>

(* The leader may act on a command its peer does not hold: a voter has echoed its *)
(* current statement that its peer may not lead. Without rule 11, at any time.    *)
MayActAlone(s) ==
    IF RecordStatement THEN stmt[s].bar /\ confirmed[s] >= stmt[s].num ELSE TRUE

(* The leader has the matching engine act on a command in its log. *)
Act(s) ==
    /\ leader = s
    /\ \E c \in log[s] \ acted :
         /\ c \in log[Peer(s)] \/ MayActAlone(s)
         /\ acted' = acted \cup {c}
    /\ actors' = actors \cup {s}
    /\ UNCHANGED <<up, leader, lepoch, elections, log, nextCmd, recVars, stmt, confirmed, msgs,
                   crashes, thirdRestarts>>

-----------------------------------------------------------------------------
(* Failures. *)

(* A voter crashes. It comes back with the record it had made durable; voter 3's *)
(* is durable only with ThirdKeepsRecord. Messages to it are lost.               *)
Crash(v) ==
    /\ up[v]
    /\ IF v = Third THEN thirdRestarts < MaxThirdRestarts ELSE crashes < MaxCrashes
    /\ up' = [up EXCEPT ![v] = FALSE]
    /\ leader' = IF leader = v THEN None ELSE leader
    /\ LET keep == v # Third \/ ThirdKeepsRecord IN
       /\ barred' = [barred EXCEPT ![v] = IF keep THEN barDisk[v] ELSE None]
       /\ barKey' = [barKey EXCEPT ![v] = IF keep THEN barKeyDisk[v] ELSE NoKey]
       /\ barDisk' = [barDisk EXCEPT ![v] = IF keep THEN barDisk[v] ELSE None]
       /\ barKeyDisk' = [barKeyDisk EXCEPT ![v] = IF keep THEN barKeyDisk[v] ELSE NoKey]
    /\ msgs' = {m \in msgs : m.to # v}
    /\ crashes' = IF v = Third THEN crashes ELSE crashes + 1
    /\ thirdRestarts' = IF v = Third THEN thirdRestarts + 1 ELSE thirdRestarts
    /\ UNCHANGED <<lepoch, elections, log, acted, nextCmd, stmt, confirmed, actors>>

Restart(v) ==
    /\ ~up[v]
    /\ up' = [up EXCEPT ![v] = TRUE]
    /\ UNCHANGED <<leader, lepoch, elections, log, acted, nextCmd, recVars, stmt, confirmed, msgs,
                   crashes, thirdRestarts, actors>>

-----------------------------------------------------------------------------
Next ==
    \/ \E s \in Cand : Elect(s) \/ StopActing(s) \/ SendStatement(s) \/ StartAlone(s) \/ StopAlone(s)
                       \/ Write(s) \/ Replicate(s) \/ Act(s)
    \/ \E m \in msgs : ReceiveStatement(m) \/ Lose(m)
    \/ \E v \in Voters : DiskWrite(v) \/ Crash(v) \/ Restart(v)

Spec == Init /\ [][Next]_vars

(* Keeps the model finite. *)
InFlightBound == Cardinality(msgs) <= MaxInFlight

-----------------------------------------------------------------------------
(* Properties. *)

TypeOK ==
    /\ up \in [Voters -> BOOLEAN]
    /\ leader \in Cand \cup {None}
    /\ barred \in [Voters -> Cand \cup {None}]
    /\ acted \subseteq 1..MaxCommands

(* Rule 11's purpose: the instance acting as leader holds every command the *)
(* matching engine has acted on.                                            *)
LeaderHoldsActed == leader # None => acted \subseteq log[leader]

(* Checked to show the specification is not vacuous, and so expected to be     *)
(* broken: after one instance has had the engine act on a command, the other    *)
(* can lead. If rule 11 stopped every change of leader, this would hold.        *)
NoChangeOfLeaderAfterActing == ~(leader # None /\ \E s \in actors : s # leader)

=============================================================================
