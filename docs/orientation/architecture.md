# Architecture {#architecture}

## What it is

A low-latency, multi-threaded, event-driven application framework built on the **reactor pattern**,
and a simplified exchange built on it to show that the framework's latency and correctness hold up
under the demands of a real system.

**What the framework provides:**

- Communication between threads through lock-free queues with many producers and one consumer
- Communication between processes over TCP, with incoming messages handed to the application
  thread without being copied
- Pool, bump and slab allocators, so that nothing on the order path allocates from the heap
- Timers, through `timerfd` and `epoll`
- High availability, composed differently for each component: a pair whose leader holds a majority
  lease, for the sequencer, the matching engine and its publisher; two instances serving at once,
  for the authentication service; and two gateway instances per protocol, each member provisioned
  to a primary and a backup ([WAL and HA](../availability/wal_and_ha.md#ha_component_specific))
- A serialisation language: message definitions from which a Python generator writes the C++
  encoders and decoders

**The applications** are a working venue that accepts orders and cancels them. It does not match
buyers with sellers; the functional specification in `docs/book` records what it does and does not do.

---

## Design principles

| Principle | How it is met |
|---|---|
| Nothing on the order path allocates from the heap | Pool, bump and slab allocators; slab-backed message payloads |
| Fast paths take no locks | Lock-free queues; compare-and-swap in the pool allocator |
| Incoming messages are not copied | The slab chunk a message arrived in is handed to the application thread |
| Threads stay on their cores | A shared registry of CPUs; each thread claims its cores at startup |
| Shutdown is deterministic | A lifecycle state machine; a descriptor that wakes `epoll`; joins with a timeout |
| One order of events | The leading sequencer alone decides the order in which the matching engine sees commands |
| The log is the record | The sequencer's write-ahead log holds every order; everything else can be rebuilt from it |

---

## Framework and applications

The framework (`libraries/pubsub_itc_fw/`) provides the infrastructure. The applications
(`applications/`) use it and contain no framework logic. The framework knows nothing of what a
message means: message shapes are defined in the serialisation language, and the generated headers
encode and decode them.

---

## The processes

One instrument, as the venue is built and tested today. How the venue would be divided for several
instruments is not designed.

```
   Members, FIX                                        Members, binary protocol
       │                                                          │
       ▼                                                          ▼
 ┌──────────────────────┐                          ┌───────────────────────────┐
 │ FIX gateway  a  /  b │                          │ Binary gateway  a  /  b   │
 │ checks each command, │                          │ checks each command,      │
 │ applies the session's│                          │ applies the session's     │
 │ throttles            │                          │ throttles                 │
 └─────────┬────────────┘                          └─────────────┬─────────────┘
           │   every order and cancel, to BOTH sequencers        │
           └───────────────────────┬──────────────────────────────┘
                                   ▼
 ┌──────────────────────────────┐   records, and acknowledgements   ┌──────────────────────────────┐
 │ Sequencer (leading)          │ ────────────────────────────────► │ Sequencer (following)        │
 │ numbers each command,        │ ◄──────────────────────────────── │ writes its log only from the │
 │ writes it to the log,        │                                   │ leader's records; forwards   │
 │ sends it to the engine,      │                                   │ nothing                      │
 │ routes each report back      │                                   │                              │
 └───────┬───────────▲──────────┘                                   └──────────────────────────────┘
         │ commands  │ reports (the engine sends each to both sequencers)
         ▼           │
 ┌──────────────────────────────┐   every change to the book        ┌──────────────────────────────┐
 │ Matching engine (leading)    │ ────────────────────────────────► │ Matching engine (following)  │
 │ keeps the book and its open  │                                   │ keeps a copy of the book     │
 │ orders on disk; reports      │                                   │                              │
 └──────────────────────────────┘                                   └──────────────────────────────┘

 ┌──────────────────────────────┐
 │ Matching engine publisher    │  reads the sequencer's log and publishes topics to subscribers
 │ (a leading and a following   │
 │ instance)                    │
 └──────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────────────────────────────┐
 │ Arbiter  primary  /  secondary,  and the witness                                     │
 │ The active arbiter is the third voter in each pair's majority lease. The arbiters    │
 │ decide which of them is active the same way, with the witness as their third voter.  │
 │ Not on the order path. The witness must not share power or a switch with either      │
 │ arbiter machine.                                                                     │
 └──────────────────────────────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────┐   ┌──────────────────────────────┐
 │ Authentication service a / b │ ◄─│ Admin service (Java)         │  writes credentials to the
 │ both serve; the gateways     │   │ and the database             │  database and sends each
 │ check each logon with one    │   └──────────────────────────────┘  change to both instances
 └──────────────────────────────┘
```

### The connections

| Connection | Direction | What travels on it |
|---|---|---|
| Members to gateways | Member → gateway | FIX 5.0 SP2, or the binary protocol, over TCP |
| Commands | Gateway → both sequencers | Each order or cancel in a `WalRecord` envelope; only the leader acts on it |
| Reports to gateways | Leading sequencer → gateway | Each report, routed to the session that placed the order |
| Commands to the engine | Leading sequencer → leading engine | Numbered commands, in order |
| Reports from the engine | Engine → both sequencers | Each report; only the leader forwards it |
| Log replication | Leading → following sequencer | Every record; the follower acknowledges each |
| Book replication | Leading → following engine | Every change to the book (`BookUpdate`) |
| Leases | Each instance ↔ its peer and both arbiters | `LeaseRequest`, `LeaseGrant`, `LeaseRefusal` |
| Arbiters | Arbiter ↔ arbiter, arbiter ↔ witness | The same lease messages, and the highest epoch granted in each group |
| Topics | Publisher → subscribers | Records from the sequencer's log, by topic |
| Authentication | Gateway ↔ both authentication services | The SCRAM-SHA-256 exchange for each logon |

---

## How an order travels

```
Member sends a new order
   │
   ▼
Gateway
   - FIX: checks the message against the FIX dictionary and the venue's rules
     binary: decodes it and checks it against the protocol's definition and the venue's rules
   - refuses it, with a reply, if it fails, if the venue is not accepting orders, or if the
     session is over its limit per second
   - wraps it in a WalRecord envelope naming the session, and sends it to both sequencers
   │
   ▼
Sequencer (leading)
   - gives it the next sequence number
   - writes it to the log, and sends the record to the following sequencer
   - sends it to the leading matching engine
   │
   ▼
Matching engine (leading)
   - refuses it if its ClOrdID is already in use by the session, or is too long for the book
   - otherwise puts it on the book, records it in its open-order region, and sends the change
     to the following engine
   - sends the execution report to both sequencers
   │
   ▼
Sequencer (leading)
   - holds the report until the following sequencer has acknowledged the order's record
   - sends it to the gateway the order's session is now connected to
   │
   ▼
Gateway
   - FIX: writes the FIX ExecutionReport, numbered in the session's sequence
     binary: relays the report as it arrived
   │
   ▼
Member receives the execution report
```

The leading sequencer alone decides the order in which the matching engine sees commands. The log
holds every order the venue took; the engines' books and the routing of reports can be rebuilt
from it.

---

## Ports

The development environment's values, from `environments/dev.toml` as deployed into
`installed/etc`. Each environment file sets its own.

| Port | Listener |
|---|---|
| 9879, 9881 | FIX gateway a, b: members (9880, 9882 with TLS) |
| 9890, 9891 | Binary gateway a, b: members |
| 11010, 11011 | FIX gateway a, b: reports from the sequencers |
| 11110, 11111 | Binary gateway a, b: reports from the sequencers |
| 11001, 11002 | Sequencer primary, secondary: commands from the gateways |
| 11003, 11004 | Sequencer primary, secondary: the peer |
| 11021, 11022 | Sequencer primary, secondary: reports from the matching engines |
| 11030, 11031 | Sequencer primary, secondary: the publishers, reading the log |
| 11020, 11023 | Matching engine primary, secondary: commands from the sequencers |
| 11025, 11026 | Matching engine primary, secondary: the peer |
| 11040 to 11043 | Matching engine publisher: topic subscribers |
| 11044, 11045 | Matching engine publisher primary, secondary: the peer |
| 11070, 11071 | Authentication service a, b: the gateways |
| 11072, 11073 | Authentication service a, b: the admin service |
| 11100 | Witness: the arbiters |
| 11200, 11201 | Arbiter primary, secondary: the components |
| 11203, 11204 | Arbiter primary, secondary: the other arbiter |
| 9201 to 9214 | Each process's Prometheus metrics |

---

## The components

| Component | Language | What it does |
|---|---|---|
| `fix_order_gateway` | C++ | The FIX 5.0 SP2 session layer; checks each command; SCRAM authentication |
| `binary_order_gateway` | C++ | The same venue over the binary protocol, with the same checks |
| `sequencer` | C++ | Numbers every command, writes the log, routes reports |
| `matching_engine` | C++ | The order book, the open-order region, execution reports |
| `matching_engine_publisher` | C++ | Reads the sequencer's log and publishes topics |
| `arbiter`, `witness` | C++ | The third voters in the majority leases |
| `authentication_service` | C++ | Holds the SCRAM-SHA-256 credentials and each comp id's settings; answers the gateways |
| `admin_service` | Java | Web pages for firms and comp ids; writes the database and tells the authentication service |
| `fix_test_client` | Java | A FIX and binary test client with Groovy scripting and message capture |

---

## See also

- [WAL and High Availability](../availability/wal_and_ha.md) and [Majority leases](../availability/majority_leases.md)
- [Roadmap](../roadmap.md)
- [Reactor](../framework/reactor.md)
- [Threading](../framework/threading.md) and [CPU pinning](../framework/cpu_pinning.md)
