# pubsub_itc_fw deployment topology — companion to the PlantUML diagram

This file explains what `pubsub_itc_fw_topology.puml` shows, why it is drawn
the way it is, and what the diagram is and is not trying to convey. Read this
alongside the diagram. The diagram is meant to be rendered with PlantUML
(`plantuml pubsub_itc_fw_topology.puml`) to produce a PNG or SVG.

For the reasoning behind the high availability design, see
[WAL and High Availability](../availability/wal_and_ha.md). This file is
descriptive; that file explains why.

## What the diagram represents

A single-site, single-instrument deployment of the order path and the
processes that decide leadership: one gateway protocol's two instances, the
sequencer pair, the matching engine pair, and the arbiter pool. Every
component drawn is a separate machine, and each machine is meant to fail
independently of every other — different power supplies, different network
switches, ideally different network segments. The witness is especially
sensitive to this; see [Witness](../venue/witness.md).

The diagram shows TCP connections at the application level, not the
underlying network infrastructure (no switches, no firewalls, no load
balancers). It is a logical-deployment view, not a physical-network view.

Not every component is drawn; see "What the diagram does NOT show" below.

## Why machines are drawn as rectangles

One application instance per machine. The high availability design relies on
machines failing independently: when a sequencer machine dies, its sequencer
process dies with it, but no other component is directly affected. Drawing
machines as rectangles makes this explicit. Two rectangles drawn separately
mean two distinct physical (or virtualised-but-independently-failing) hosts.

The environment files follow this, with one exception: `prod.toml`,
`preprod.toml` and `test-1.toml` place the `_a` instances of the FIX gateway
and the binary gateway on one host. The development environment runs every
component on one machine.

## Vocabulary on the diagram

The diagram uses **configured identity** labels (PRIMARY, SECONDARY) for the
components, because configured identity is set at deploy time and is fixed for
the life of an instance. Configured identity is what the TOML says.

The diagram does **not** label runtime role (LEADER, FOLLOWER) on the boxes,
because runtime role changes during failover and the diagram is a static
snapshot.

The two pairs of terms are not interchangeable. See the glossary in
[WAL and High Availability](../availability/wal_and_ha.md).

## Components shown

### FIX clients (representative; external)

`fix8-client-1` and `fix8-client-2` are drawn as representative external FIX
clients. Real deployments have many such clients. Each client is logged on to
one gateway instance at a time.

The fix8 clients are part of the system's environment, not part of the
framework's deployment. They are drawn so that the entry-point of the FIX wire
is visible.

### Gateway instances

`gateway-1` and `gateway-2` are the two instances of a gateway protocol, `a` and `b`. Nothing elects
a gateway: each member is provisioned to a primary and a backup instance and may log on to either,
but no other. Each gateway:

- Terminates the sessions of the members connected to it.
- Checks each order and cancel, wraps it in a `WalRecord` envelope that names the member's session,
  and sends it to both sequencers. Only the leading sequencer acts on it.
- Receives execution reports from the sequencers and sends each to the member's session.
- Holds open connections to both sequencers, so that whichever leads can be reached without setting
  up a connection on failover.

When a gateway machine fails, its members reconnect to their backup instance, and find their orders
and reports there, because a session is identified by its comp id and protocol rather than by its
connection. This is not the same kind of high availability as the sequencer, matching engine and
arbiter pairs; see [Gateway High Availability](../availability/gateway_ha.md).

### Sequencer pair

`sequencer-primary` and `sequencer-secondary` are two machines, each running
one sequencer instance. The leading sequencer is the central authority for
ordering: every command receives a sequence number and is written to the
leader's write-ahead log before anything downstream acts on it.

Each sequencer machine has its own local-disk write-ahead log (memory-mapped,
in segments), shown as a database symbol inside the machine box. The leader
writes its log; the follower receives the leader's records over the
replication connection and writes its own copy.

### Matching engine pair

`ME-primary` and `ME-secondary` are two machines, each running one matching
engine instance. Each holds an order book in memory.

The configured primary sends book updates to the configured secondary, so the
secondary holds a copy of the book and can take over knowing which orders
were resting. What a promoted engine then does with those orders is a stated
policy, `order_book.open_orders_on_promotion`: `cancel`, which every
environment uses, cancels each one and reports the cancel to its member;
`keep` leaves them on the book. See "What happens to resting orders on
promotion" in [WAL and High Availability](../availability/wal_and_ha.md).

The order book is shown as a database symbol inside each machine box. The
secondary's book is annotated as "replicated".

### Arbiter pool (two arbiters and a witness)

Three machines: `arbiter-primary`, `arbiter-secondary`, `witness`.

An instance of a component pair leads only while a majority of three voters —
itself, its peer and the arbiter pool — has granted it a lease that has not
run out. The arbiter pool is that third voter, and it votes through whichever
of its two arbiters is **active**. Which arbiter is active is decided the same
way, among three voters: the two arbiters and the witness.

Each arbiter keeps, on disk, the promise of its own vote in deciding which
arbiter is active; in memory, it keeps the highest epoch granted in each
component group. The witness keeps nothing on disk and is never a candidate:
it only answers the arbiters' lease requests.

Three votes in each decision, majority two: the design tolerates the loss of
any one voter. The witness's value depends entirely on its placement in a
failure-independent location; this is annotated on the witness's machine box.
See [Arbiter](../venue/arbiter.md), [Witness](../venue/witness.md) and
[Deciding leadership by majority, with leases](../availability/majority_leases.md).

## Connection types

The diagram uses two visual conventions to distinguish data-plane from
control-plane connections.

### Data-plane connections (solid black arrows)

These carry order flow, execution report flow, FIX traffic, and replication of
state that is part of processing orders:

1. **FIX wire** — `fix8-client-N` ↔ `gateway-N`. Bidirectional FIX text
   protocol. The gateway terminates the FIX session, encodes orders, decodes
   execution reports.

2. **Commands** — `gateway-N` → `sequencer-X`. Each gateway holds open
   connections to both sequencers and sends every command to both. Only the
   leading sequencer acts on it. A gateway learns that a new sequencer leads
   from the higher leader epoch on the `OrderAcceptance` messages it receives,
   and then sends again every command still unanswered.

3. **Execution reports** — `sequencer-X` → `gateway-N`. Each sequencer opens a
   connection to every gateway process's execution report listener, which is
   why the arrows point from sequencer to gateway. The same connections carry
   `OrderAcceptance`.

4. **Sequenced commands** — `sequencer-X` → `ME-Y`. Each sequencer holds
   connections to both matching engines.

5. **Execution reports from the matching engine** — `ME-Y` → `sequencer-X`.
   Each matching engine connects to both sequencers' report listeners and
   sends each report to both.

6. **Write-ahead log replication (sequencer pair)** — `sequencer-primary` ↔
   `sequencer-secondary`. Two separate arrows: the leader pushes log records
   to the follower; the follower sends acknowledgements back. The leader does
   not release an execution report to a gateway until the follower has
   acknowledged the record it depends on, so the acknowledgement is on the
   critical path.

7. **Book replication (matching engine pair)** — `ME-primary` → `ME-secondary`.
   One arrow: the configured primary sends book updates and nothing is
   acknowledged. The secondary's book is kept current so it can take over
   knowing which orders were resting.

The connections between the two instances of a pair, in items 6 and 7, also
carry each instance's lease requests to its peer and the peer's answers.

### Control-plane connections (dotted blue arrows)

These carry the decisions about who leads and never carry order data:

8. **Component ↔ arbiter pair** — Each of `sequencer-primary`,
   `sequencer-secondary`, `ME-primary`, `ME-secondary` opens connections to
   both arbiter machines and sends each lease request to both, because which
   arbiter is active can change. The active arbiter answers with a grant or a
   refusal; the passive one stays silent.

   The gateways have no connection to the arbiter pool. They send every
   command to both sequencers, so they do not need to know which one leads.

9. **Arbiter pair internal** — `arbiter-primary` ↔ `arbiter-secondary`. Each
   asks the other for a lease when deciding which of them is active, and the
   active one tells the passive one the highest epoch granted in each
   component group (`ArbiterStateRecord`).

10. **Arbiter ↔ witness** — Each arbiter sends lease requests to the witness
    when it wants to become, or stay, the active arbiter. The witness answers
    with a grant or a refusal.

## Why bidirectional connections are drawn as two arrows

Where two endpoints exchange messages in both directions and the directions
mean different things — like "leader pushes log records" vs "follower
acknowledges them" — drawing two separate arrows lets each direction be
labelled with what flows in that direction. A single double-headed arrow would
lose that information.

## What the diagram does NOT show

These are deliberately out of scope for this view:

- **Other venue components.** The matching engine publisher pair, which reads
  the sequencer's log and publishes it as topics, and which also leads by
  lease; the authentication service pair, which the gateways ask to check
  logons; the admin service; and the Prometheus server. They are off the path
  that orders take through the venue.

- **The second gateway protocol.** The venue runs a FIX gateway and a binary
  gateway, each with instances `a` and `b`. The diagram shows one protocol's
  two instances; the other is connected in the same way.

- **DR site.** The design is for one site only. A disaster recovery site
  would need a second copy of most of this topology at a remote site, with
  replication between the sites.

- **Multiple instruments.** A real exchange runs many instruments. This
  diagram shows the single-instrument case.

- **Internal framework structure.** The diagram does not show the reactor,
  the slab allocator, the PDU framing layer, the FIX parser, or any other
  framework-internal mechanism. The boxes are processes; their internal
  structure is invisible at this level.

- **Operational infrastructure.** PTP grandmasters, monitoring agents, log
  aggregators, configuration management. These exist but are not part of the
  system architecture being depicted.

- **Network plumbing.** Switches, firewalls, load balancers, VLANs. The
  diagram is logical-deployment, not physical-network. The failure-
  independence requirements (different power, different switch) are
  annotated as text but the network elements themselves are not drawn.

## How to render

```
plantuml pubsub_itc_fw_topology.puml
```

Produces `pubsub_itc_fw_topology.png` by default. Use `-tsvg` for SVG output.

PlantUML's auto-layout will struggle with the number of connections in this
diagram. Some manual adjustment of the .puml file (adding `together { }`
groupings, hidden links to coax positioning, or alternative renderers like
Graphviz with different settings) may be necessary to get a clean visual
result. The text content of the diagram is correct regardless of how
PlantUML chooses to lay it out.

## Where the design is described

- [WAL and High Availability](../availability/wal_and_ha.md): the glossary of
  primary, secondary, leader and follower; why high availability is specific
  to each component; the write-ahead log; how long a failover takes; and what
  happens at each point of failure.
- [Deciding leadership by majority, with leases](../availability/majority_leases.md):
  the lease rules every voter follows.
- [Gateway High Availability](../availability/gateway_ha.md): why the gateways
  are two independent instances rather than a leader and a follower.
- [Arbiter](../venue/arbiter.md) and [Witness](../venue/witness.md): the
  arbiter pool's protocol in detail.
