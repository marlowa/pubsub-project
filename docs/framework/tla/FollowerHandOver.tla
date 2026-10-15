---------------------------- MODULE FollowerHandOver ----------------------------
(***************************************************************************)
(* A specification of how a follower of a WAL is caught up by the reading  *)
(* thread and then handed over to the thread that owns the WAL, which      *)
(* sends it each new record from then on: WalReadingThread and             *)
(* WalFollowerService in libraries/pubsub_itc_fw, as designed in           *)
(* docs/availability/following_the_wal.md, section 3.4.                    *)
(*                                                                         *)
(* The owning thread releases records one at a time, numbered from 1; the  *)
(* follower may be sent a record only once it is released (its limit).    *)
(* For each follower the owning thread is in one of three modes: not       *)
(* serving it, having the reading thread catch it up, or having handed it  *)
(* over. While the reading thread catches a follower up, the owning thread *)
(* sends that follower nothing.                                            *)
(*                                                                         *)
(*  1. The follower asks, saying the last record it holds. The owning      *)
(*     thread numbers a new serving and tells the reading thread to serve  *)
(*     the follower from that record.                                      *)
(*  2. The reading thread reads the limit, sends every record up to it,    *)
(*     and reports to the owning thread the position it reached, with the  *)
(*     serving's number.                                                   *)
(*  3. The owning thread ignores a report from an earlier serving. For the *)
(*     serving in progress, if the position equals the limit now, nothing  *)
(*     has been released since and the follower is handed over; otherwise  *)
(*     the reading thread is told to carry on, and step 2 repeats.         *)
(*                                                                         *)
(* Every send from either thread goes on one queue into the reactor, which *)
(* sends them in the order they were queued. The reading thread queues its *)
(* last record before it reports, and the owning thread queues its first   *)
(* new record only after it has handed the follower over. Commands to the  *)
(* reading thread and reports to the owning thread go on their own queues, *)
(* each in order.                                                          *)
(*                                                                         *)
(* The follower's connection may be lost, at most MaxLosses times. Sends   *)
(* still queued for the lost connection are then dropped, the owning       *)
(* thread tells the reading thread to stop, and the follower asks again on *)
(* its new connection, from the last record it holds. A report from the    *)
(* serving that was stopped may still be on its way, and must not hand     *)
(* the follower over.                                                      *)
(*                                                                         *)
(* With CompareOnReport = FALSE, the owning thread hands the follower over *)
(* on any report from the serving in progress, without comparing the      *)
(* position with the limit, and TLC must find a record the follower never *)
(* receives. With CompareOnReport = TRUE, as designed, it must find        *)
(* nothing.                                                                *)
(*                                                                         *)
(* Not modelled: batches and the pacing by the connection, which change    *)
(* only when records are sent, not which or in what order; here the        *)
(* reading thread sends one record at a time. The pause of new orders      *)
(* after three short rounds, which makes the hand-over end but does not    *)
(* change what is sent. The memory orderings of the C++ atomics.           *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC

CONSTANTS
    MaxRecords,       \* bound on the records released
    FollowerStart,    \* the last record the follower holds at the start
    MaxLosses,        \* bound on how often the follower's connection is lost
    CompareOnReport   \* whether the owning thread compares the position reported with the limit now

VARIABLES
    limit,            \* the last record released
    connection,       \* the number of the follower's current connection
    losses,           \* how often the connection has been lost
    mode,             \* the owning thread's mode for the follower: "none", "catching_up" or "handed_over"
    serving,          \* the number of the owning thread's serving in progress
    toOwner,          \* messages to the owning thread: the follower's requests and the reading thread's reports
    toReader,         \* commands to the reading thread
    readerServing,    \* the serving the reading thread is doing, or 0
    readerConnection, \* the connection the reading thread sends on
    readerStep,       \* "idle", "read_limit", "send" or "waiting"
    readerSent,       \* the last record the reading thread has sent, or the follower held
    readerLimit,      \* the limit as the reading thread last read it
    toReactor,        \* sends queued for the reactor, each [conn, rec], oldest first
    followerLast,     \* the last record the follower holds
    followerAskedOn,  \* the connection on which the follower last asked, or 0
    misordered        \* the follower was sent a record that did not follow the last it held

vars == <<limit, connection, losses, mode, serving, toOwner, toReader, readerServing, readerConnection,
          readerStep, readerSent, readerLimit, toReactor, followerLast, followerAskedOn, misordered>>

Records == 0..MaxRecords

Init ==
    /\ limit = FollowerStart
    /\ connection = 1
    /\ losses = 0
    /\ mode = "none"
    /\ serving = 0
    /\ toOwner = <<>>
    /\ toReader = <<>>
    /\ readerServing = 0
    /\ readerConnection = 0
    /\ readerStep = "idle"
    /\ readerSent = 0
    /\ readerLimit = 0
    /\ toReactor = <<>>
    /\ followerLast = FollowerStart
    /\ followerAskedOn = 0
    /\ misordered = FALSE

TypeOK ==
    /\ limit \in Records
    /\ connection \in 1..(MaxLosses + 1)
    /\ losses \in 0..MaxLosses
    /\ mode \in {"none", "catching_up", "handed_over"}
    /\ serving \in Nat
    /\ toOwner \in Seq([kind : {"request"}, conn : Nat, after : Records] \cup [kind : {"report"}, serving : Nat, reached : Records])
    /\ toReader \in Seq([kind : {"serve"}, serving : Nat, conn : Nat, after : Records] \cup [kind : {"continue", "stop"}, serving : Nat])
    /\ readerStep \in {"idle", "read_limit", "send", "waiting"}
    /\ readerSent \in Records
    /\ readerLimit \in Records
    /\ toReactor \in Seq([conn : Nat, rec : Records])
    /\ followerLast \in Records
    /\ misordered \in BOOLEAN

(***************************************************************************)
(* The owning thread.                                                      *)
(***************************************************************************)

\* A record is released. A follower handed over is sent it at once.
Release ==
    /\ limit < MaxRecords
    /\ limit' = limit + 1
    /\ toReactor' = IF mode = "handed_over" THEN Append(toReactor, [conn |-> connection, rec |-> limit + 1]) ELSE toReactor
    /\ UNCHANGED <<connection, losses, mode, serving, toOwner, toReader, readerServing, readerConnection,
                   readerStep, readerSent, readerLimit, followerLast, followerAskedOn, misordered>>

\* The follower's request: a new serving, from the last record it holds.
OwnerTakeRequest ==
    LET m == Head(toOwner)
    IN
    /\ toOwner /= <<>>
    /\ m.kind = "request"
    /\ toOwner' = Tail(toOwner)
    /\ IF m.conn = connection
       THEN /\ serving' = serving + 1
            /\ mode' = "catching_up"
            /\ toReader' = Append(toReader, [kind |-> "serve", serving |-> serving + 1, conn |-> connection, after |-> m.after])
       ELSE UNCHANGED <<serving, mode, toReader>>
    /\ UNCHANGED <<limit, connection, losses, readerServing, readerConnection, readerStep, readerSent, readerLimit,
                   toReactor, followerLast, followerAskedOn, misordered>>

\* The reading thread's report. One from an earlier serving is ignored.
OwnerTakeReport ==
    LET m == Head(toOwner)
    IN
    /\ toOwner /= <<>>
    /\ m.kind = "report"
    /\ toOwner' = Tail(toOwner)
    /\ IF m.serving = serving /\ mode = "catching_up"
       THEN IF ~CompareOnReport \/ m.reached = limit
            THEN /\ mode' = "handed_over"
                 /\ UNCHANGED toReader
            ELSE /\ toReader' = Append(toReader, [kind |-> "continue", serving |-> serving])
                 /\ UNCHANGED mode
       ELSE UNCHANGED <<mode, toReader>>
    /\ UNCHANGED <<limit, connection, losses, serving, readerServing, readerConnection, readerStep, readerSent,
                   readerLimit, toReactor, followerLast, followerAskedOn, misordered>>

\* The follower's connection is lost. Sends still queued for it will be dropped by the reactor.
LoseConnection ==
    /\ losses < MaxLosses
    /\ losses' = losses + 1
    /\ connection' = connection + 1
    /\ mode' = "none"
    /\ toReader' = Append(toReader, [kind |-> "stop", serving |-> serving])
    /\ UNCHANGED <<limit, serving, toOwner, readerServing, readerConnection, readerStep, readerSent, readerLimit,
                   toReactor, followerLast, followerAskedOn, misordered>>

(***************************************************************************)
(* The reading thread.                                                     *)
(***************************************************************************)

ReaderTakeCommand ==
    LET c == Head(toReader)
    IN
    /\ toReader /= <<>>
    /\ readerStep \in {"idle", "waiting"}
    /\ toReader' = Tail(toReader)
    /\ CASE c.kind = "serve" ->
                /\ readerServing' = c.serving
                /\ readerConnection' = c.conn
                /\ readerSent' = c.after
                /\ readerStep' = "read_limit"
         [] c.kind = "continue" ->
                /\ readerStep' = IF c.serving = readerServing THEN "read_limit" ELSE readerStep
                /\ UNCHANGED <<readerServing, readerConnection, readerSent>>
         [] c.kind = "stop" ->
                /\ readerServing' = 0
                /\ readerStep' = "idle"
                /\ UNCHANGED <<readerConnection, readerSent>>
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toOwner, readerLimit, toReactor, followerLast,
                   followerAskedOn, misordered>>

ReaderReadLimit ==
    /\ readerStep = "read_limit"
    /\ readerLimit' = limit
    /\ readerStep' = "send"
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toOwner, toReader, readerServing, readerConnection,
                   readerSent, toReactor, followerLast, followerAskedOn, misordered>>

ReaderSend ==
    /\ readerStep = "send"
    /\ readerSent < readerLimit
    /\ readerSent' = readerSent + 1
    /\ toReactor' = Append(toReactor, [conn |-> readerConnection, rec |-> readerSent + 1])
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toOwner, toReader, readerServing, readerConnection,
                   readerStep, readerLimit, followerLast, followerAskedOn, misordered>>

\* Every record up to the limit as read has been queued: report, and send nothing more until told.
ReaderReport ==
    /\ readerStep = "send"
    /\ readerSent >= readerLimit
    /\ toOwner' = Append(toOwner, [kind |-> "report", serving |-> readerServing, reached |-> readerLimit])
    /\ readerStep' = "waiting"
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toReader, readerServing, readerConnection, readerSent,
                   readerLimit, toReactor, followerLast, followerAskedOn, misordered>>

(***************************************************************************)
(* The reactor and the follower.                                           *)
(***************************************************************************)

\* The oldest queued send goes out, or is dropped if its connection has been lost.
ReactorSend ==
    LET s == Head(toReactor)
    IN
    /\ toReactor /= <<>>
    /\ toReactor' = Tail(toReactor)
    /\ IF s.conn = connection
       THEN /\ misordered' = (misordered \/ s.rec /= followerLast + 1)
            /\ followerLast' = IF s.rec = followerLast + 1 THEN s.rec ELSE followerLast
       ELSE UNCHANGED <<misordered, followerLast>>
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toOwner, toReader, readerServing, readerConnection,
                   readerStep, readerSent, readerLimit, followerAskedOn>>

\* The follower asks on each new connection, from the last record it holds.
FollowerAsk ==
    /\ followerAskedOn < connection
    /\ followerAskedOn' = connection
    /\ toOwner' = Append(toOwner, [kind |-> "request", conn |-> connection, after |-> followerLast])
    /\ UNCHANGED <<limit, connection, losses, mode, serving, toReader, readerServing, readerConnection, readerStep,
                   readerSent, readerLimit, toReactor, followerLast, misordered>>

Next ==
    \/ Release
    \/ OwnerTakeRequest
    \/ OwnerTakeReport
    \/ LoseConnection
    \/ ReaderTakeCommand
    \/ ReaderReadLimit
    \/ ReaderSend
    \/ ReaderReport
    \/ ReactorSend
    \/ FollowerAsk

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* The follower is sent every record exactly once and in order on each connection: never a gap, never
\* the same record twice.
NoGapNoRepeat == ~misordered

\* Nothing is queued for the follower that has not been released.
NeverAboveLimit == \A i \in 1..Len(toReactor) : toReactor[i].rec <= limit

\* Once the follower is handed over and every queued send has gone out, it holds every record released:
\* nothing fell between the last record the reading thread sent and the first the owning thread sent.
NothingMissedAtHandOver == (mode = "handed_over" /\ toReactor = <<>>) => followerLast = limit

=============================================================================
