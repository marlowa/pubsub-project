# WAL and High Availability {#wal_and_ha}

This document is the overview of how the venue keeps trading when a process or a machine fails. It
says, for each component, what is duplicated, how the duplicate takes over, and what a member sees.
The detail lives in the documents it links to:

| Subject | Document |
|---|---|
| Which instance of a pair leads, and why two never lead at once | [Majority leases](majority_leases.md) |
| The write-ahead log itself: format, segments, replay | [Write-Ahead Log](../durability/wal.md) |
| How the matching engine's open orders survive a restart | [Open-order checkpoint](../durability/open_order_checkpoint.md) |
| How a member keeps trading when a gateway dies | [Gateway High Availability](gateway_ha.md) |
| What the venue does when no matching engine is running | [Order acceptance](order_acceptance.md) |
| A process that dies and is restarted on the same machine | [Process death](process_death.md) |

---

## Design philosophy

The sequencer's write-ahead log is the venue's record of what happened. Every order is written to it
before anything acts on it, and everything downstream of it -- the matching engine's book, the
execution reports, what a gateway sends a member -- can be rebuilt from it. Two questions are kept
apart: which instance may write the log, which is the leadership protocol, and what the log already
holds, which no leadership change can alter. A follower learns what has been committed by receiving
the records, never by inferring it.

---

## HA is component-specific {#ha_component_specific}

There is no single mechanism. Each component's failure model is different, so each uses only the
pieces it needs.

| Component | How it is duplicated | Who decides who leads | Uses the write-ahead log | What a member sees on failover |
|---|---|---|---|---|
| **Sequencer** | A pair: leader and follower | A majority of three voters: the two sequencers and the active arbiter | Yes: the leader writes it, the follower receives every record | A pause of a few seconds. Orders sent during it are lost ([BUG-0103](../bug_list.md#bug_0103)) |
| **Matching engine** | A pair: leader and follower, the follower holding a copy of the book | A majority of three voters, as for the sequencer | It has no log of its own; it catches up from the sequencer's | A pause. What happens to resting orders is a configured policy (below) |
| **Matching engine publisher** | A pair, as for the sequencer | A majority of three voters | It reads the sequencer's log | A subscriber reconnects and asks for records from the sequence number it had reached |
| **Arbiters** | Two arbiters and a witness | A majority of the two arbiters and the witness | No | None: arbiters are not on the order path |
| **Order gateways** | Two instances of each protocol; each member session is provisioned to a primary and a backup instance | Nobody: the member chooses which instance to connect to | No | The member reconnects to its other instance |
| **Authentication service** | Two instances, both serving | Nobody: the gateway uses whichever answers | No: the database is the record | None for a session already logged on |

Three things follow from the table:

- **Only the pairs elect a leader**: the sequencers, the matching engines, the publishers, and the
  two arbiters between themselves. They hold state with a single writer, so the hand-over must leave
  exactly one writer at every moment.
- **Only the sequencer writes the log**, and only the sequencers, the matching engines and the
  publishers read it. A gateway keeps the open orders of its own sessions and nothing more; the
  authentication service's state is in the database.
- **What a member sees differs by component**: a short pause for a sequencer or matching engine, a
  reconnection for a gateway, nothing for an authentication service or an arbiter.

### Glossary

Two pairs of words with separate meanings. They apply only to the components that elect a leader.

| Word | Meaning |
|---|---|
| **primary** | The instance configured with the lower `instance_id`. Fixed at deployment. It starts first and so normally leads, but nothing else depends on it. |
| **secondary** | The instance configured with the higher `instance_id`. |
| **leader** | The instance that currently holds a majority lease. Either instance can be the leader. Everything a leader does on the order path depends on the lease, never on being the primary. |
| **follower** | The other instance. It receives the leader's state and sends nothing on the order path. |
| **active / standby** | Not used, because they are read as either of the pairs above. Between the two arbiters, the one that votes is called *active* and the other *passive*. |

---

## Write-ahead log

The log's format, its segments, the single-writer rule and replay are described in
[Write-Ahead Log](../durability/wal.md). This section covers what the sequencer's pair adds.

### Two levels of commit

"Committed" means two different things, and both are needed before a member is told anything.

| Level | What makes it so | What it releases |
|---|---|---|
| **Written on the leader** | The record has been appended to the leading sequencer's log | The order is sent on to the matching engine |
| **Held on both machines** | The following sequencer has received the record, written it to its own log, and acknowledged it (`WalAck`) | The execution report for that order is sent to the member's gateway |

The leader holds back each execution report until the follower has acknowledged the record of the
order the report answers. So a member is never told about an order that only one machine holds: if
the leader's machine died a moment after the report went out, the follower would still have the
order in its log, and would take over with it.

This gives durability on two machines without the cost of a quorum vote of the kind Raft uses. There
is exactly one follower, and records reach it as a stream over a single connection, so there is no
vote to collect: one acknowledgement is the whole of the agreement.

**There is no `fsync` for each record.** Forcing every record to disk would add tens to hundreds of
microseconds to every order, and it is not what makes a record safe here. A record is safe because
it is held on two machines; the copy on disk is written by the operating system's page cache in the
background, and when segments are created and filled. Losing both machines at the same instant is
the case this does not cover, and it is the case disaster recovery to a second site is for (not
designed; see the open questions below).

### Replication

The leader streams every record to the follower over a connection used for nothing else, separate
from the connections that carry orders and reports. The follower appends each record to its own log,
on its own machine and its own disk, and acknowledges it. The two logs are identical, record for
record, because the follower's log is written only from this stream.

A follower learns what has been committed by receiving the records. It never infers a commit from
timing or from the leader's silence. Its part is passive: it receives, it acknowledges, and it sends
nothing on the order path -- not to the matching engine, and not to any gateway.

Each gateway sends every order to both sequencers, and the matching engine sends every report to
both. Only the leader acts on them. The follower discards the copies it is sent directly, because
its log must match its leader's exactly. The consequence for orders sent in the seconds after the
leader has died and before the follower takes over is
[BUG-0103](../bug_list.md#bug_0103): the follower discards them, and nothing else holds them.

**The replication connection cannot be dropped to protect the leader.** If the follower falls behind,
the leader keeps writing to its own log and keeps sending orders to the matching engine, but it holds
back the execution reports until the follower has acknowledged the records they answer. Members then
see a delay in their reports, not a loss of durability. If the follower stops altogether, reports
stop until it is back, because sending them would mean telling members about orders that only one
machine holds.

**One slow connection does not hold up the others.** Every send is non-blocking, and each connection
keeps its own partly sent message while the peer cannot take more. A matching engine or gateway that
stops reading delays only the messages addressed to it.

### Snapshots and reclaiming disk

The sequencer records where its log has reached every thirty seconds (`Wal::take_snapshot`): the last
sequence number, the number of records and the position of the end of the log, in a 48-byte file. It
holds no other state, and taking it deletes nothing. `Wal::truncate_below`, which deletes segments
below a given sequence number, exists but nothing in the venue calls it, so the log is never
reclaimed while the venue runs. How long it must be kept waits on a decision about how long
execution reports must remain available to a member that asks for them again.

### When something is wrong with the log

| Situation | What happens |
|---|---|
| The last record is incomplete | Replay stops before it: it was never committed |
| A record in the middle is damaged | Replay stops at it; nothing after it is applied |
| The disk is full | Segments are written out in full when created, so the failure comes when a new segment cannot be written: the writer raises an exception |

---

## How the leader is chosen and kept out when deposed

The full design, its rules, the failure table and the model checking are in
[Majority leases](majority_leases.md). In outline:

- **An instance leads only while a majority of three voters agrees**: itself, its peer, and the
  third voter, which for a component pair is the active arbiter. The agreement is a lease that lasts
  three seconds and is renewed every second. Its own vote plus one other is a majority.
- **Each voter promises its vote to one instance at a time.** A component instance writes the
  promise to disk before it grants it, so a restarted process keeps its promises.
- **A leader whose lease runs out stops acting at once**: it sends nothing more on the order path
  until it holds a lease again. This is what keeps a deposed or cut-off leader out. It does not need
  to be told: its own lease, timed on its own clock, ends before any voter's promise does.
- **Losing every arbiter does not stop trading**: the leader renews with its peer alone. Trading halts
  only if the arbiters and the leader fail together, because the follower cannot then tell a dead
  leader from its own isolation.

**The epoch.** Each leadership generation has an epoch number, which also records which instance
leads in it, so no two instances ever lead at the same epoch. Voters never grant an epoch below the
highest they have granted, and an instance that learns of a higher epoch than its own stops leading.
The epoch travels on the lease messages and on the matching engine's announcement of its role; it is
not on orders or reports.

**What stops a deposed leader's messages being acted on**, besides its own lease ending:

- a follower sequencer forwards nothing to the matching engine and no report to a gateway;
- a matching engine that holds no lease discards every order it is sent;
- the sequencer sends orders only to the matching engine that announced it leads, and refuses an
  announcement whose epoch is older than one it has accepted.

### STONITH was considered and rejected

Power fencing -- "shoot the other node in the head", STONITH -- removes a suspect node by force
before its peer takes over: through a managed power supply, the machine's management controller
(IPMI, iLO, DRAC), or a reservation on shared storage. It guarantees that the old leader is gone.
It was evaluated and is not used, for three reasons:

- **It is a property of the deployment, not of the software.** It needs an out-of-band management
  network, hardware that can power-cycle a peer, and credentials to do so. A developer's machine has
  none of these, and the venue must run the same way there as on production hosts, so a fencing
  path would be one that is never exercised where it is written and tested.
- **It is not needed for safety.** A majority of three voters, with leases, already keeps two
  leaders from acting at once: a leader cut off from its peer and the arbiters stops when its lease
  runs out, before any voter can grant the lead to anyone else. Two nodes without fencing or a third
  voter would be unsafe; with a third voter they are not.
- **It adds operational weight** of the kind production clustering software such as Pacemaker
  exists to manage, out of proportion to a venue whose pairs already resolve leadership between
  themselves.

**What it costs not to have it.** A node that has lost its lease keeps running as a process. It
cannot act as leader, because it stops sending on the order path and nothing it sends is acted on,
but it may hold its connections and memory until someone stops it. The decision and the discussion
behind it are in the [decision record](design_notes.md#ha_no_stonith).

---

## Sequencer HA

### Where the routing state lives

The sequencer routes each execution report back to the gateway session that placed the order. The
key is the session's identity -- its comp id and protocol -- carried on the order's envelope with the
gateway's protocol and instance and the session's connection. A member that reconnects, to the same
gateway instance or to its backup, announces where it now is (`SessionBound`), and reports for
orders it placed earlier follow it. [Session binding](session_binding.md) has the detail.

### Connections

Each gateway and each matching engine holds connections to both sequencers, and sends to both. Only
the leader acts on what it receives. A gateway does not need to know which sequencer leads, and a
change of leader needs no reconnection.

### Segments and failover time

Each log segment is written out in full when it is created, so appending a record never allocates a
disk block, and a helper thread prepares the next segment before the current one fills. In a failover
the follower's log is already in memory, and taking over costs the lease rules' timing: about five
and a half seconds in `ha_test.py` scenario 1, including the new leader's first orders.

---

## Matching engine HA

### What the follower holds

The leading engine sends every change to its book to the follower (`BookUpdate`), so the follower
holds a copy. Each engine also writes its open orders into a memory-mapped region as it accepts them
and reads it back when it starts ([Open-order checkpoint](../durability/open_order_checkpoint.md)), so
an engine that restarts has its own record of what it held.

### Catching up before acting

An engine that takes the lead, or starts, does nothing on the order path until it has caught up with
the sequencer's log. It tells the sequencer the last sequence number its book reflects
(`MePositionRequest`). The sequencer sends it every record after that one and then says how many it
sent and the last sequence number (`MePositionAck`). The engine applies each record, and checks that
it has received every record the sequencer said it sent, before it acts (R-0101). If any are missing
it asks again; it does not act on an incomplete catch-up. Each report it sends while catching up is
marked as a possible repeat, because an earlier engine may already have sent the same report (R-0122).

**Why the order matters.** Suppose the leading engine accepts an order and trades it, sends the
execution report, and the sequencer writes that report to its log -- and then the engine dies before
the change to its book reaches the follower. The follower's copy of the book still shows the order as
open. If the follower acted on that copy as soon as it took the lead, it would treat a trade that has
already happened as an order still resting, and could cancel it. A trade that has legally happened
must never be undone.

Catching up first closes that gap. The records the follower is sent include everything the old
leader did that reached the sequencer's log, so once it has applied them its book agrees with the log,
and any order still on it is genuinely open. The price is time: nothing on the order path happens
until the catch-up is complete. A delay is acceptable; undoing a trade is not.

If a sequencer and the matching engine fail at the same time, the engine's catch-up waits for the
sequencers to settle on a leader, because only a leading sequencer answers a catch-up.

### What happens to resting orders on promotion

**The options considered.** When the leading engine fails, something must be decided about the orders
resting on its book. These were the choices, and what became of each:

| Option | What it means | Decision |
|---|---|---|
| Start cold | A new engine starts with an empty book and rebuilds it by replaying the sequencer's log from the start of the day | Rejected: replaying a day's orders takes too long to be a failover, and it needs the whole day's log to be kept |
| Run in lockstep | Both engines process every command in parallel; failover switches to the follower's output, which is already identical | Not built. It needs every decision the engine makes to be deterministic, so that two engines given the same commands always produce the same results. A possible later direction |
| Halt | The engine's failure stops trading until an operator recovers it | Kept as the last resort, for when an engine cannot establish what it held (below) |
| Take over and cancel | The follower, which holds a copy of the book, takes the lead, catches up, then cancels every resting order and reports each cancel | Built, and selected by `"cancel"`, which every environment uses |
| Take over and keep | As above, but the orders stay on the book and members go on holding them | Built, and selected by `"keep"`. What must be shown before it is used is still being established |

Cancelling gives a member an explicit "your order has been cancelled" rather than silence, and needs
neither the time of a cold start nor the determinism of lockstep. Keeping is better for members, who
lose nothing, and is what an engine that restarts on its own already does.

The choice is a stated policy, `order_book.open_orders_on_promotion`, required in every deployment:

- **`cancel`**: the promoted engine cancels every order still on its caught-up book and reports each
  cancel. Every environment is set to this.
- **`keep`**: the promoted engine keeps them, and members go on holding what they placed.

An engine that *starts*, rather than taking over from a peer, keeps the orders it recovered from its
region whatever the policy says (R-0018). Where the region is missing or damaged, the engine cannot
say what it held: it establishes the open orders from the sequencer's record, cancels each, reports
each cancel, and halts (R-0102, R-0123).

### Orders sent while no matching engine is running

There are moments with no matching engine to send orders to: after the leading engine dies and before
its follower takes over, or while every engine is down. What happens to the orders gateways send in
those moments:

1. **The leading sequencer writes every order to its log before it checks for a matching engine.**
   Writing is unconditional for the leader; only sending the order on depends on an engine being
   there.
2. **With no engine connected, the sequencer defers sending the order** rather than dropping it, and
   logs that it has done so. The order is already in the log.
3. **The engine that next leads or starts is sent every deferred order while catching up**, because
   the catch-up sends everything after the position the engine reports.
4. **The member is answered then**: the execution report for an order sent during the gap arrives
   late, when the engine has caught up, and the order is on the book. Nothing is silently lost.

What happens next to the orders on the caught-up book is the promotion policy above.

**If the outage lasts longer than any failover plausibly takes**, the sequencer stops accepting
orders, and tells the gateways so (`OrderAcceptance`). The gateways then refuse new orders and cancels
with a reply, so members are not left holding orders the venue cannot act on. Acceptance resumes on
its own when an engine returns. [Order acceptance](order_acceptance.md) has the thresholds and the
reasoning.

## Order gateways

A member's session is a connection to one gateway process, so when the process dies the member must
reconnect. Each protocol runs two instances, and each comp id is provisioned in the database to a
primary and optionally a backup instance; a gateway refuses a logon from a member provisioned
elsewhere. When a member's connection drops, the gateway keeps its resting orders for a grace period
configured per comp id, and a member that logs on again within it -- to either instance -- keeps them
and can cancel them. [Gateway High Availability](gateway_ha.md) has the detail, including what a FIX
member can and cannot recover by resending.

---

## Authentication service HA {#ha_auth_service}

Two instances, `a` and `b`, both run and both serve. Neither leads. They reflect state they do not
write: the admin service writes each credential change to the database and sends it to both
instances (`SetCredentialRequest`, `RemoveCredentialRequest`, `RestoreCredentialRequest`), and each
instance loads the full set from the database export when it starts. The two never diverge, so
there is nothing to elect.

A gateway holds a connection to both and tries one first. When that instance dies, the gateway
authenticates the next logon with the other. A session already logged on is unaffected, because it
does not authenticate again. `ha_test.py` scenario 17 kills instance `a` and logs a member on through
`b`.

---

## Arbiters

### What they do

The arbiters are the third voter for each component pair: the sequencers, the matching engines and
the matching engine publishers. They are not on the order path and hold no venue state. All an
arbiter does for a component pair is grant or refuse a lease when an instance asks, and promise its
vote to one instance at a time.

The name follows common usage. In MongoDB, for example, an arbiter is a member of a replica set
that votes in elections but holds no data and can never become primary, which is this role exactly.

### Two arbiters and a witness

The arbiters are themselves duplicated, and they decide which of them votes by the same lease rules,
with a third process, the witness, as their third voter:

- **Arbiter primary and arbiter secondary.** Each is a full arbiter. The one that holds a lease from
  the other or from the witness is *active* and votes for the components; the other is *passive*.
  The active arbiter tells the passive one the highest epoch it has granted in each component group
  (`ArbiterStateRecord`), so a change of active arbiter does not forget it.
- **The witness.** A small process that keeps nothing. It answers the arbiters' lease requests, and
  never becomes active or passive itself.

An arbiter that becomes active grants no component lease for one lease period, because it cannot
know what the previously active arbiter had promised. During that wait, each component leader renews
its lease with its peer alone, so a change of active arbiter costs nothing while both instances of
every pair are running.

**How the components reach them.** Each component instance connects to both arbiters and sends every
lease request to both. The active arbiter answers; the passive one stays silent, because a refusal
from it would cancel the request the active arbiter is answering under the same id. Nothing connects
to the witness except the two arbiters.

### Three machines, and no more

Three votes need a majority of two, so any single machine can fail. Adding a second witness would
make four votes, needing three for a majority, and then losing any one machine would leave the
arbiters one failure from having no majority at all: more machines, less resilience. The way to
tolerate two failures would be five voters, which is the territory of a full consensus cluster.
Three is the chosen number.

**The witness must be independent of both arbiter machines**: on different power, a different network
switch, and ideally a different network segment. If it shared infrastructure with one arbiter, a
single failure could remove two of the three votes, and the arbiters could not choose an active one
at exactly the moment it was needed.

### Why not a consensus library

Ready-made implementations of Raft and Paxos for C++, such as NuRaft and braft, were considered for
the arbiters and rejected:

- the state to agree on is tiny -- one record for each component group -- so a full consensus
  library would be far larger than the problem;
- C++ implementations of Raft are less widely used than those for Java or Go, and taking one on
  means inheriting subtle defects in code the project did not write;
- the lease rules used instead are small enough to be specified in TLA+ and model checked
  exhaustively, which was done ([Model checking](tla/findings.md)), so their correctness is shown
  rather than assumed.

[Arbiter](../venue/arbiter.md) and [Witness](../venue/witness.md) describe the two applications.

## Time

**Leases are timed on each machine's steady clock** (`std::chrono::steady_clock`), which never goes
backwards and is not changed when the wall clock is corrected. Each machine measures how long has
passed on its own clock, so a difference between two machines' clock readings does not matter. What
matters is a difference in the *rate* at which they run, and each lease is shortened by an allowance
for that ([Majority leases](majority_leases.md), section 9). On Linux the steady clock counts from
when the machine booted, and is the same for every process on the machine, which is what lets a
restarted process read back the expiry times of the promises it wrote to disk.

**Timestamps that cross machines** -- `TransactTime` on execution reports, and the time the
sequencer stamps on each record -- come from the wall clock. Auditors expect timestamps taken on
different machines to agree, so the wall clocks must be synchronised closely.

**The intended synchronisation is PTP (IEEE 1588), not NTP.** PTP keeps clocks on a correctly built
local network within a microsecond of one another; NTP's accuracy is in milliseconds. PTP needs
network cards that timestamp packets in hardware, a grandmaster clock disciplined by GPS, and
switches that act as boundary or transparent clocks. That is standard infrastructure at a real
exchange. The venue relies on it being present and does not implement PTP itself.

---

## How long a failover takes

| What fails | What happens, and how long it takes |
|---|---|
| The leading sequencer | Its follower takes over once its promise and the arbiter's to the old leader have run out: about one lease period plus one renewal interval. `ha_test.py` scenario 1 measures about five and a half seconds, including the new leader's first orders. Orders sent in that time are lost ([BUG-0103](../bug_list.md#bug_0103)) |
| The leading matching engine | Its follower takes over on the same timing, then catches up with the sequencer before it acts. Orders sent meanwhile are deferred, not lost, and answered once it has caught up |
| A leading process, restarted by its supervisor within the lease period | It keeps the lead, because its peer and the arbiter promised their votes to it. The interruption is the restart: about 3.5 seconds for a matching engine from dying to leading ([Process death](process_death.md)) |
| A gateway | The member reconnects to its other provisioned instance, typically within seconds; resting orders are kept for the comp id's grace period |
| An authentication service | None for a session already logged on; the next logon uses the other instance |
| The active arbiter | The other arbiter becomes active once the lease between them has run out, then waits one lease period before granting component leases. Component leaders renew with their peers meanwhile, so nothing changes for members |
| Every arbiter | Nothing, while both instances of each pair are running. If a leader then fails too, trading halts until an arbiter returns ([Majority leases](majority_leases.md), section 7) |

---

## What happens at each point of failure

| Situation | What happens |
|---|---|
| The leading sequencer dies before writing an order to its log | The order was never taken. The member receives no reply ([BUG-0103](../bug_list.md#bug_0103)) |
| The leading sequencer dies after writing an order and replicating it | The follower holds it, takes over, and sends it to the matching engine as part of the engine's catch-up; the member is answered |
| The leading sequencer dies after sending an order to the engine but before the report reached the member | The follower holds the order. The engine sends its report to both sequencers, but the follower discards reports while it is still following, and on taking the lead it does not ask for them again, so the member is not sent the report (read in the code, recorded with [BUG-0103](../bug_list.md#bug_0103)) |
| A matching engine restarts | It reads its open orders back from its region and catches up with the sequencer from the last position its book reflects |
| The log's last record is incomplete | It was never committed; replay stops before it |
| A record in the middle of the log is damaged | Replay stops at it; nothing after it is applied |
| The disk holding the log is full | Writing a new segment fails and the writer raises an exception |
| Every arbiter and the witness are unreachable | Each leader renews with its peer; no leader can be replaced until an arbiter returns |

---

## Open design questions

| Question | Status |
|---|---|
| Orders sent during a change of sequencer leader | Lost without a reply; the remedy needs a design ([BUG-0103](../bug_list.md#bug_0103)) |
| Disaster recovery to a second site | Not designed |
| Several instruments: one sequencer, or one per group of instruments | Not designed |
| How long the log must be kept | Waiting on a decision about how long execution reports must be available |

---

## See also

- [Majority leases](majority_leases.md) and [the model checking](tla/findings.md)
- [Write-Ahead Log](../durability/wal.md) and [replay](../durability/replay.md)
- [Architecture](../orientation/architecture.md)
- [Sequencer](../venue/sequencer_app.md), [Matching engine](../venue/matching_engine.md),
  [Arbiter](../venue/arbiter.md), [Witness](../venue/witness.md)
- [Secure comms](../operations/secure_comms.md), for the authentication service and TLS
