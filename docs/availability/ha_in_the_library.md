# Moving high availability into the library {#ha_in_the_library}

> **Status: the diagnosis and the direction are agreed with Andrew, 11 October 2026.** The design of the
> boundary between the library and the applications is [ha_library_boundary.md](ha_library_boundary.md),
> agreed 12 October 2026. Phases 3 to 9 of [the plan for restoring the design](../recovery_plan.md)
> carry it out.

## 1. What this page covers

Most of the machinery that keeps the venue running when a process or a machine fails lives in the
applications, not in the pubsub library. This page says where it is, why that is a problem, and the
direction agreed for moving it: the library provides a replicated, sequenced write-ahead log (WAL), and each application
is written as a deterministic state machine fed by that WAL. It then lists what is still to be decided.

## 2. Where the high availability code is now

Measured on 11 October 2026:

- **The lease machinery** is in the library (`libraries/pubsub_itc_fw/include/pubsub_itc_fw`): `LeaseVoter`, `LeaseHolder`,
  `LeaseParticipant`, `PairLeaseAgent`, `LeasePromiseStore`, `BackgroundPromiseRecorder`, `EpochStore`
  and `LeaderEpoch`, about 1,700 lines. Principle V3 in [design_principles.md](../design_principles.md)
  records that these classes know nothing of the component using them.
- **The sequencer**, `SequencerThread`, is about 4,800 lines. About a third of its 107 member
  functions are about high availability, judged by their names: peers, followers, leaders, leases,
  epochs, replication, catch-up, repair and running without a follower. Its jobs include replicating its
  WAL to the follower, holding commands back from the matching engine until the follower confirms them,
  trading without a follower, recording that the follower may not take over, repairing a rejoining
  follower's WAL, streaming catch-ups to the matching engine and the publishers, and keeping reports for
  a change of leader. None of these is about sequencing orders; they are about keeping a WAL replicated.
- **The matching engine**, about 3,000 lines, has its own catch-up, its own reconciliation, and the
  replication of its book to its follower.
- **The gateways** keep the commands not yet answered and send them again when the leader changes
  (`UnansweredCommandStore`).

The library, about 10,500 lines, provides almost none of this.

## 3. Why that is a problem

**Every application that wants high availability has to build it again.** A new component would have
to copy the sequencer's ways of doing things, and the sequencer is where most of the defects in this
area have been found.

**It is where the design decayed.** The review of the design principles kept finding breaches in the
same place: three separate mechanisms meeting V7 where one check should; the second writer of the
follower's WAL that breaks F1 and F3; the catch-up that reads the WAL on the thread that sequences
orders (V10, BUG-0133); and V8's work spread across several of the sequencer's jobs. Each is high
availability machinery mixed into an application's business code, where each new failure case was met
by another mechanism.

**The library does not yet do what the project exists to show.** The project's aim is the library,
with applications that bring out its requirements. High availability has brought out a large set of
requirements, and they have been met in the applications instead of becoming features of the library.

## 4. The direction: state machine replication

**Hiding high availability in a library does not need a central broker.** Systems built on a central
message broker can hide failover largely because the broker holds the order of messages, stores them,
and replays them. This project has no broker, deliberately, so those jobs must be done somewhere, and
today the applications do them. The established way to put them in a library without a broker is
**state machine replication**:

- **An application is written as a deterministic state machine.** Given the same commands in the same
  order, it always reaches the same state and produces the same output. It offers the library a few
  entry points, such as "here is the next command" and "take a snapshot of your state", and knows
  nothing of leaders or followers.
- **The library provides a replicated, sequenced WAL.** It elects the leader; replicates the WAL to the
  followers; holds back what a majority has not confirmed; repairs a rejoining follower; brings a
  restarted instance up to date from a snapshot and the rest of the WAL; and recognises a command it
  already holds by its identity.
- **Every instance, leader or follower, feeds the same WAL to its own copy of the application.**
  Followers stay in step by doing exactly what the leader does, not by being sent the leader's state.

This is the problem Raft was designed to solve. It is also the shape of Aeron Cluster, an open-source
library that has no broker, is built on Raft, and is used to run exchange matching engines: the
application implements a small interface (a message has arrived, a timer has fired, take a snapshot),
and the cluster does leadership, replication, replay and snapshots. It is the nearest published example
of what this library would offer, and is worth reading as a reference before the boundary is designed.

## 5. What the venue has built, in those terms

| Built in the applications today | What it is in general terms |
|---|---|
| Majority leases, epochs, and the statement that a follower may not take over (V3, V4) | Electing a leader, and keeping the WAL safe while the leader changes |
| Holding a command back until the follower confirms it (V5) | Acting only on what a majority holds |
| Repairing a rejoining follower's WAL (V10) | Repairing the WAL, which Raft and Kafka call log repair |
| Catch-ups for the matching engine and the publishers | Replaying the WAL from a position |
| One check against sequencing a command twice (V7) | Recognising a duplicate by its identity, as Kafka's idempotent producer does |
| Gateways sending unanswered commands again to a new leader (V6) | A client sending unconfirmed requests again to a new leader |

**What stays in the application** is what only the application can know: what a command means; what
reply a duplicate gets (a command sent again is answered, a reused `ClOrdID` is refused); what
cancel-on-disconnect does; and what a snapshot of the order book contains. The library asks the
application for these through a small interface instead of the application building the machinery
around them.

## 6. Questions the design must answer

These are to be settled in the design of the boundary, not here.

1. **Is the matching engine deterministic?** Today the follower matching engine is kept in step by its
   leader replicating its book to it. Under state machine replication the follower would apply the same
   sequenced commands and reach the same book itself, with no book replication. That works only if the
   engine's result depends on nothing but the commands: no reading of the clock, no randomness, no
   dependence on timing. Time is already injected (`WallClock`), and events driven by the time of day
   are already meant to travel through the WAL ([roadmap.md](../roadmap.md)). Whether there is a reason
   the book is replicated instead has not been established, and must be before anything is decided.
2. **Which parts are replicated state machines, and which are clients?** The sequencer and the
   matching engine fit the model. The gateways do not: they are the clients that send commands to the
   replicated WAL and receive its output, so what the library offers them is the client's side, keeping
   unconfirmed commands and sending them again to a new leader.
3. **Where do the voters live?** The arbiters and the witness are voters with no WAL. Whether the
   library provides them as a ready-made service, or provides the classes they are built from, is to be
   decided.
4. **How do snapshots fit?** The matching engine's open-order region
   ([open_order_checkpoint.md](../durability/open_order_checkpoint.md)) is already a snapshot kept up to
   date continuously, built on `MappedSlotStore`, which is in the library. The design must say how a
   snapshot and a position in the WAL are taken and restored together.
5. **How do timers fit?** A timer whose ringing changes the application's state must ring at the same
   point in the WAL on every instance, so it travels through the WAL. Timers for the process's own
   housekeeping do not. This interacts with the timer design in [BUG-0128](../bug_list.md#bug_0128).
6. **The library must still work without high availability.** Not every use of the library wants a
   replicated WAL, so the replicated WAL is something an application chooses, not something every
   application pays for.

## 7. What this changes in the plan

Phases 3 to 9 of [the plan](../recovery_plan.md) move the high availability machinery into the library
in the order set out in section 12 of [ha_library_boundary.md](ha_library_boundary.md):

- **The lease classes are the first and least risky step** (phase 3). They are already independent of
  the component using them, so moving them into the library is mostly a move.
- **The replicated WAL leaves the sequencer for the library** (phases 4 and 5), with one writing thread.
- **The matching engine becomes deterministic and runs in lockstep with its follower** (phases 7 and 8).
- **Dividing the sequencer and the matching engine into parts comes last** (phase 10), because it is
  easier once most of what is in them has left.

## 8. Next steps

The design of the boundary, [ha_library_boundary.md](ha_library_boundary.md), says what the library
offers, what an application supplies, how each of the pieces built so far maps onto it, and answers the
questions in section 6. The work is phase 3 onwards of [the plan](../recovery_plan.md), each phase
starting with its own design, agreed before any code is written.
