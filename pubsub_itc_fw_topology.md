# pubsub_itc_fw deployment topology — companion to the PlantUML diagram

This file explains what `pubsub_itc_fw_topology.puml` shows, why it is drawn
the way it is, and what the diagram is and is not trying to convey. Read this
alongside the diagram. The diagram is meant to be rendered with PlantUML
(`plantuml pubsub_itc_fw_topology.puml`) to produce a PNG or SVG.

For the architectural reasoning behind the design, see the WAL and HA Design
section of `pubsub_itc_fw_summary.md`. This file is descriptive; that file is
prescriptive.

## What the diagram represents

A single-site, single-instrument deployment of the WAL+HA design described in
the project summary. Every component drawn is a separate machine. Each machine
is failure-independent from every other machine in the topology — different
power supplies, different network switches, ideally different network
segments. The PSA+witness arbiter pool is especially sensitive to this; see
the project summary's "Arbiter PSA topology" section for why.

The diagram shows TCP connections at the application level, not the underlying
network infrastructure (no switches, no firewalls, no load balancers). It is a
logical-deployment view, not a physical-network view.

## Why machines are drawn as rectangles

One application instance per machine. The HA design relies on machine-level
failure independence: when a sequencer machine dies, its sequencer process
dies with it, but no other component is directly affected. Drawing machines as
rectangles makes this explicit. Two rectangles drawn separately mean two
distinct physical (or virtualised-but-independently-failing) hosts.

## Vocabulary on the diagram

The diagram uses **configured identity** labels (PRIMARY, SECONDARY) for the
components, because configured identity is set at deploy time and is fixed for
the life of an instance. Configured identity is what the toml says.

The diagram does **not** label runtime role (LEADER, FOLLOWER) on the boxes,
because runtime role changes during failover and the diagram is a static
snapshot. The arrows are labelled in terms of what flows when a component is
acting as leader (e.g. "order PDUs (only when leader)").

The two pairs of terms are not interchangeable. See the project summary's
"Glossary" section for the discipline.

## Components shown

### FIX clients (representative; external)

`fix8-client-1` and `fix8-client-2` are drawn as representative external FIX
clients. Real deployments have many such clients. Each client pins to one
gateway (its FIX session is a TCP connection with that gateway). On gateway
failure, the client reconnects to a different gateway in the pool, with
FIX-level resend covering any gap.

The fix8 clients are part of the system's environment, not part of the
framework's deployment. They are drawn so that the entry-point of the FIX wire
is visible.

### FIX gateway pool

`gateway-1` and `gateway-2` are two representative gateway machines. A
production deployment can have more. Gateways are not in primary/secondary HA
pairs; they are an N-way pool. Each gateway:

- Terminates FIX sessions for some subset of clients.
- Encodes received FIX orders into PDUs and sends them to the current
  sequencer leader.
- Receives ER PDUs from the sequencer leader and translates them back into
  FIX execution reports for the originating session.
- Holds open TCP connections to both sequencers (so it can reach whichever is
  currently leader without waiting for new connection setup on failover).

Gateway machine failure causes the FIX clients on that gateway to reconnect
to a different gateway. This is not the same kind of HA as the
sequencer/ME/arbiter pairs; see the project summary's "Gateway pool" section.

### Sequencer pair

`sequencer-primary` and `sequencer-secondary` are two machines, each running
one sequencer instance. They are the central authority for ordering — every
order PDU receives a seqNo and is durably written to the leader's WAL before
any downstream effect happens.

Each sequencer machine has its own local-disk WAL (mmap'd, segmented), shown
as a database symbol inside the machine box. The sequencer leader writes its
WAL; the sequencer follower receives WAL records via the replication channel
and writes its own copy.

### ME pair

`ME-primary` and `ME-secondary` are two machines, each running one matching
engine instance. Each ME maintains an order book in memory.

The ME-primary's book is the authoritative book for the current trading
session. The ME-secondary maintains a replicated copy of the book so that on
ME-primary failure the secondary knows about all in-flight orders. The
specific intent for the framework is that on ME failover, the secondary uses
its replicated book to issue cancellation messages for outstanding orders, so
that gateways can deliver appropriate cancel notifications to FIX clients
("your order has been cancelled because the matching engine failed over").

This is more than the halt-on-failure baseline noted in the project summary,
and matches the architectural intent for this framework as discussed.

The order book is shown as a database symbol inside each ME machine box. The
secondary's book is annotated as "replicated".

### Arbiter pool (PSA + witness)

Three machines: `arbiter-primary`, `arbiter-secondary`, `witness`.

The two arbiter instances each hold a copy of the leadership-state map
(`(component_id, leader_instance_id, epoch, lease_expiry)` records, one per
component pair). The witness holds no state. It exists solely to vote in
elections of which of the two arbiters is currently the active one.

Three votes total, majority is two: the design tolerates any single-machine
failure without losing the ability to make leadership decisions. The witness's
value depends entirely on its placement in a failure-independent location;
this is annotated on the witness's machine box and is critical.

See the project summary's "Arbiter PSA topology" section for the full
reasoning.

## Connection types

The diagram uses two visual conventions to distinguish data-plane from
control-plane connections.

### Data-plane connections (solid black arrows)

These carry order flow, ER flow, FIX traffic, and replication of state that is
part of the order-processing semantics:

1. **FIX wire** — `fix8-client-N` ↔ `gateway-N`. Bidirectional FIX text
   protocol. The gateway terminates the FIX session, encodes orders, decodes
   ERs.

2. **Order PDUs** — `gateway-N` → `sequencer-X`. Each gateway holds open
   connections to both sequencer machines and sends order PDUs on whichever is
   currently the leader. The non-leader rejects sends at the application
   layer, so connections to it stay open but inert until promotion.

3. **ER PDUs** — `sequencer-X` → `gateway-N`. Symmetrically, each sequencer
   has connections to both gateways for ER routing. Note that the framework's
   current implementation has the sequencer initiating these outbound
   connections (to the gateway's ER inbound listener); this is shown in the
   diagram by the arrow direction. ERs flow only from the current sequencer
   leader.

4. **Sequenced order PDUs** — `sequencer-X` → `ME-Y`. The leader sequencer
   sends to both ME machines so that both can build the order book. Only the
   leader sends.

5. **ME ER PDUs** — `ME-Y` → `sequencer-X`. The leader ME sends ERs back to
   whichever sequencer is currently leader. The secondary ME's ER emissions
   are discarded by the receiving sequencer until the secondary ME is itself
   promoted to leader.

6. **WAL replication (sequencer pair)** — `sequencer-primary` ↔
   `sequencer-secondary`. Two separate arrows: leader pushes WAL records to
   follower; follower sends acks back. The leader does not send an ER to a
   gateway until the follower has acked the underlying order's WAL record, so
   the ack channel is on the critical path for ER emission.

7. **Book replication (ME pair)** — `ME-primary` ↔ `ME-secondary`. Two
   separate arrows: leader pushes book updates to follower; follower sends
   acks back. The follower's book is kept current so it can take over with
   knowledge of in-flight orders.

### Control-plane connections (dotted blue arrows)

These carry leadership-management traffic and never carry order data:

8. **Component ↔ arbiter pair** — Each of `sequencer-primary`,
   `sequencer-secondary`, `ME-primary`, `ME-secondary` opens connections to
   both arbiter machines. Heartbeats and lease-renewal requests flow from the
   component to the arbiter; lease grants and active-arbiter-pointer
   information flow back. The component sends heartbeats to whichever arbiter
   is the currently-active one; the passive arbiter accepts the connection
   but redirects requests to the active arbiter.

   Note: the gateway has no connection to the arbiter pool. Gateways learn
   leader-status implicitly (by which sequencer accepts their order PDUs as
   leader); they never query the arbiter directly. This is a deliberate
   simplification — gateways are not in primary/secondary HA pairs and do not
   need their own leadership decisions.

9. **Arbiter pair internal** — `arbiter-primary` ↔ `arbiter-secondary`. The
   active arbiter replicates the leadership-state map to the passive arbiter;
   passive sends acks. Bidirectional, two separate arrows.

10. **Arbiter ↔ witness** — Each arbiter heartbeats to the witness for
    liveness. The witness can be queried for its vote when one of the
    arbiters is contemplating promotion. Bidirectional, two separate arrows
    on each pair (arbiter-primary ↔ witness, arbiter-secondary ↔ witness).

## Why bidirectional connections are drawn as two arrows

Where two endpoints exchange messages in both directions and the directions
mean different things — like "leader pushes WAL records" vs "follower acks
WAL records" — drawing two separate arrows lets each direction be labelled
with what flows in that direction. A single double-headed arrow would lose
that information.

## What the diagram does NOT show

These are deliberately out of scope for this view:

- **DR site.** The current design is main-site only (this is documented as an
  open question in the project summary). When DR is added, the diagram will
  need extension — likely a second copy of most of this topology at a remote
  site, with cross-site replication channels.

- **Multiple instruments.** A real exchange runs many instruments. The
  scaling story (sharded sequencer, instrument groups, sequencer per
  instrument) is open in the project summary. This diagram shows the
  single-instrument case.

- **Internal framework structure.** The diagram does not show the reactor,
  the slab allocator, the PDU framing layer, the FIX parser, or any other
  framework-internal mechanism. The boxes are processes; their internal
  structure is invisible at this level.

- **Operational infrastructure.** PTP grandmasters, monitoring agents (e.g.
  Nagios), log aggregators, configuration management. These exist but are
  not part of the system architecture being depicted.

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

## Cross-references to the project summary

- "Glossary -- terms that must not be confused": vocabulary of primary /
  secondary / leader / follower used here.
- "Architecture: per-component HA with shared primitives": the structural
  pattern this diagram visualises.
- "Gateway pool": why the gateways are drawn as a pool rather than a pair.
- "Arbiter PSA topology": detailed protocol mechanics for the arbiter pool
  and witness.
- "Failover speed targets": the runtime-role transitions this diagram does
  not show explicitly but which the topology supports.
- "Implementation staging": which slices of the design will activate which
  parts of this topology. The diagram shows the topology at completion of
  all slices; earlier slices have parts of it.
