# Architecture

## What It Is

A low-latency, multi-threaded, event-driven application framework using the **reactor pattern**.
The goal is a foundation on which a real exchange system skeleton could be built, demonstrating
that the framework's latency and correctness properties hold under load.

**Framework primitives:**

- Inter-thread communication (ITC) via lock-free MPSC queues
- Inter-process communication (IPC) via unicast TCP with zero-copy PDU paths
- Lock-free pool, bump, and slab allocators — no heap allocation on any hot path
- Timers via `timerfd` and `epoll`
- High availability via primary/secondary instance pairs with external arbiter pool
- Binary serialisation DSL (Python code generator → C++17 headers; sub-100ns encode/decode)

**Sample applications** (framework validation, not production code):

- Order Gateway — FIX 5.0 SP2 client connectivity, SCRAM authentication
- Sequencer — total order assignment, WAL, HA
- Matching Engine — order book stub, execution report generation
- Matching Engine Publisher — WAL follower + topic fanout (planned)
- Arbiter / Witness — leader election

---

## Design Principles

| Principle | Implementation |
|-----------|---------------|
| No heap allocation on hot paths | Pool, bump, and slab allocators; slab-backed PDU payloads |
| Lock-free fast paths | Vyukov MPSC queues; tagged CAS in pool allocator |
| Zero-copy PDU paths | Slab-allocated inbound payload handed directly to application thread |
| CPU-pinned threads | Shared-memory CPU registry; each thread claims a CPU at startup |
| Deterministic shutdown | Lifecycle state machine; notify fd wakes epoll; join with timeout |
| Message ordering preserved | Single sequencer is the sole writer to the ME input stream |
| WAL = truth | Sequencer WAL is the authoritative record; all other state is derived |

---

## Framework vs Applications

The framework (`libraries/pubsub_itc_fw/`) provides the infrastructure. Applications
(`applications/`) use the framework but contain no framework-level logic. The framework
makes no assumptions about message content; the DSL defines message shapes and generates
the encode/decode headers that applications use.

---

## Component Topology

The diagram below shows a single-instrument deployment. The framework is currently validated
at this scale; multi-instrument scaling is an open design question.

```
                    ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
                    │  FIX client │  │  FIX client │  │  FIX client │
                    │      A      │  │      B      │  │      C      │
                    └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
                           │                │                │
                           │  FIX wire (TCP, port 9879)
                           │  orders, ERs, heartbeats
                           ▼                ▼                ▼
                    ┌──────────────────────────────────────────────┐
                    │               FIX Gateway POOL               │
                    │   (N gateways; FIX clients pin to one each)  │
                    │                                              │
                    │  - parses FIX, encodes PDU                   │
                    │  - maintains comp-id ↔ ConnectionID table    │
                    │  - reconnects on sequencer leader change      │
                    └────┬────────────────────────────────────┬────┘
                         │ order PDUs (→ leader only)         │ ER PDUs (← leader only)
                         ▼                                    ▼
        ┌──────────────────────────┐       ┌──────────────────────────┐
        │   Sequencer: PRIMARY     │       │   Sequencer: SECONDARY   │
        │   (currently LEADER)     │       │   (currently FOLLOWER)   │
        │                          │       │                          │
        │  - assigns seqNo         │◄──────┤  - tails leader WAL      │
        │  - appends to WAL        │  WAL  │  - never sends to ME     │
        │  - routes FixSession     │  repl │  - never sends to GW     │
        │  - sends to ME           │       │  - on promotion: replays │
        │  - sends ERs to GW       │──────►│    WAL, becomes leader   │
        │                          │ WalAck│                          │
        │  ┌──────────────────┐    │       │  ┌──────────────────┐    │
        │  │ WAL (mmap, disk) │    │       │  │ WAL (mmap, disk) │    │
        │  └──────────────────┘    │       │  └──────────────────┘    │
        └────┬──────────────▲──────┘       └──────────────────────────┘
             │ order PDUs   │ ER PDUs
             ▼              │
        ┌──────────────────────────┐       ┌──────────────────────────┐
        │   Matching Engine:       │       │   Matching Engine:       │
        │   PRIMARY (LEADER)       │       │   SECONDARY (FOLLOWER)   │
        │                          │       │                          │
        │  - receives orders in    │──────►│  - tails book updates    │
        │    seqNo order           │ book  │  - on primary failure:   │
        │  - mutates book          │ repl  │    reconcile vs WAL,     │
        │  - emits ERs             │       │    issue cancel ERs      │
        └──────────────────────────┘       └──────────────────────────┘

        ╔═════════════════════════════════════════════════════════╗
        ║            ARBITER POOL  (PSA + witness)                ║
        ║                                                         ║
        ║  ┌─────────────────┐  ┌─────────────────┐  ┌────────┐  ║
        ║  │ Arbiter PRIMARY │  │ Arbiter SECONDARY│  │WITNESS │  ║
        ║  │  (ACTIVE)       │  │  (PASSIVE)       │  │(votes) │  ║
        ║  └─────────────────┘  └─────────────────┘  └────────┘  ║
        ║  3 votes total; majority = 2; split-brain prevented     ║
        ║  Witness must be on independent power + network switch  ║
        ║  Components NEVER contact the witness directly          ║
        ║  NEVER on the order/ER data path                        ║
        ╚═════════════════════════════════════════════════════════╝
```

### Channel summary

| Channel | Direction | Protocol |
|---------|-----------|----------|
| FIX | FIX clients ↔ Gateway pool | TCP, FIX 5.0 SP2 wire |
| Orders | Gateway → Sequencer | TCP, PDU; gateway sends to leader only |
| ERs | Sequencer → Gateway | TCP, PDU; leader sends only |
| ME orders | Sequencer → ME | TCP, PDU; leader sends only |
| ME ERs | ME → Sequencer | TCP, PDU; ME sends to leader only |
| WAL replication | Leader → Follower | TCP, dedicated; follower sends acks |
| Arbiter control | Components ↔ Active arbiter | TCP; not on data path |
| Arbiter HA | Arbiter ↔ Arbiter, Arbiter ↔ Witness | Internal lease+epoch + witness vote |

---

## Order Flow (happy path)

```
FIX client sends NewOrderSingle
    │  raw FIX bytes
    ▼
Order Gateway
    - parses FIX message
    - SCRAM authenticates client (once, at logon)
    - encodes NewOrderSingle PDU
    │  PDU (port 7001)
    ▼
Sequencer (leader)
    - assigns sequence number
    - appends to WAL
    - forwards PDU to Matching Engine
    │  PDU (port 7020)  ─── simultaneously ──►  WAL record (port 7003)
    ▼                                           Sequencer (follower)
Matching Engine                                     - appends to own WAL
    - matches order (or fills immediately in stub)  - sends WalAck
    - generates ExecutionReport PDU             ◄───
    │  PDU (port 7021)
    ▼
Sequencer (leader)
    - receives WalAck from follower
    - releases buffered ER (gated on WalAck)
    - routes ER to correct gateway via SenderCompID
    │  PDU (port 7010)
    ▼
Order Gateway
    - encodes FIX ExecutionReport
    │  raw FIX bytes
    ▼
FIX client receives ExecutionReport
```

The sequencer is the **sole writer** to the ME's input stream, imposing total order on all
messages. The WAL is the authoritative record of every committed order. All downstream
state (routing tables, book replicas) is derived from the WAL and can be rebuilt by replay.

---

## Port Allocation

| Port | Usage |
|------|-------|
| 9879 | FIX clients → gateway (inbound) |
| 7001 | gateway → sequencer primary (order PDUs) |
| 7002 | gateway → sequencer secondary (order PDUs; HA) |
| 7003/7004 | sequencer peer-to-peer WAL replication |
| 7010 | sequencer → gateway (ER forwarding inbound) |
| 7020 | sequencer → ME primary (sequenced order PDUs) |
| 7021 | ME → sequencer ER listener |
| 7022 | ME → sequencer secondary ER listener (HA) |
| 7070 | gateway → authentication service primary |
| 7071 | gateway → authentication service secondary |
| 7100 | sequencer → arbiter |
| 7030–7047 | MEP/TAP (planned; see [MEP and TAP design](design/mep_tap.md)) |

---

## Component Summary

| Component | Language | Role |
|-----------|----------|------|
| `order_gateway` | C++ | FIX 5.0 SP2 session layer; PDU encode/decode; SCRAM auth |
| `sequencer` | C++ | Total order assignment; WAL; leader-follower HA |
| `matching_engine` | C++ | Order book; execution report generation |
| `matching_engine_publisher` | C++ | WAL follower; topic fanout to downstream consumers (planned) |
| `arbiter` / `witness` | C++ | External leader election; lease+epoch; split-brain prevention |
| `authentication_service` | C++ | SCRAM-SHA-256 credential store; responds to gateway auth requests |
| `admin_service` | Java | Web UI for firm/comp-id CRUD; credential lifecycle management |
| `fix_test_client` | Java | FIX test harness; Groovy scripting; message capture |

---

## See Also

- [Roadmap](roadmap.md) — slice plan and outstanding items
- [WAL and High Availability](design/wal_and_ha.md) — detailed HA design
- [Reactor](design/reactor.md) — event loop internals
- [Threading](design/threading.md) and [CPU Pinning](design/cpu_pinning.md)
