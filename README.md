# pubsub_itc_fw

![version](https://img.shields.io/badge/version-v0.4.0-orange)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![license](https://img.shields.io/badge/license-Apache--2.0-green)

**Current version:** v0.4.0 — see [CHANGELOG.md](CHANGELOG.md).

**Development is paused.** [docs/project_status.md](docs/project_status.md) says what works, what is known to be wrong, and where anyone picking the project up should start.

---

# Introduction

This section assumes no prior knowledge of the project or of this kind of software, and uses no
shorthand. If you write software for a living, the technical summary begins further down,
under **Technical summary**.

## What is in this repository

**The library is the project.** Everything else here exists to serve it.

The library is called pubsub. It is a collection of code that handles the difficult and
repetitive parts of writing a program that must respond to events very quickly and very
predictably: managing threads, passing messages between them, sending and receiving data over a
network, and managing memory. A collection of that kind is usually called a framework, because
programs are built on top of it rather than calling into it occasionally.

It is written for one particular setting: the computer systems that stock exchanges and
commodity exchanges run. That setting makes demands which most software never faces, and the
library is shaped by those demands rather than by general ones. What those demands are, and what
the library does about them, is the subject of most of this page.

**The applications exist because of the library, not alongside it.** The repository also holds a
simplified, working version of an exchange, built from the library. It is there for two reasons,
and neither is that the exchange itself is the goal.

The first reason is to use the library the way a real system would use it. A library exercised
only by small test programs written to suit it proves very little. Running something with the
shape and the demands of a real system is what shows whether the library holds up.

The second reason is to find out what the library needs to provide. Building a realistic
application keeps turning up requirements that no amount of designing in the abstract would have
produced. The exchange is, in that sense, a way of asking the library questions that its author
would not have thought to ask.

## What the exchange does

An exchange is a place where people and firms buy and sell things — shares in companies, metals,
currencies. They do not meet each other directly. Instead each sends instructions to the
exchange, and the exchange deals with them.

An instruction is called an **order**. A typical order says: buy one hundred of this, at no more
than this price. The exchange receives the order, records it, decides what happens to it, and
sends back an answer describing what it did.

That is the whole cycle this project is concerned with. An order arrives, and an answer goes
back. The exchange carries out every step of that cycle: it accepts the order, checks it is
valid, writes it down in a permanent record, works out what to do with it, and replies.

**It deliberately stops short of being a real exchange.** The most obvious omission is that it
does not match buyers with sellers. It accepts orders and it cancels them, and that is enough to
exercise everything the framework does, which is the point of it. Other absences are equally
deliberate, and the functional specification in `docs/book` records which ones and why.

## Why speed is the whole point

Everything about how this is built follows from one requirement: the answer must come back
quickly, and it must come back quickly every single time.

Some sense of the timescale is needed. A **millisecond** is a thousandth of a second, which is
roughly how long a camera flash lasts. A **microsecond** is a thousandth of a millisecond, a
millionth of a second. There are more microseconds in one second than there are seconds in eleven
days.

This system answers a typical order in a few hundred microseconds. Most of that time is spent
passing the order between the several separate programs that handle it in turn.

## Why being consistently quick is harder than being quick on average

This is the part that shapes every decision in the project, and it is not obvious.

Suppose a system answers a thousand orders. Nine hundred and ninety come back in a hundred
microseconds, and ten come back in fifty milliseconds — five hundred times slower. The average
across all thousand is still respectable. But somebody sent each of those ten slow orders, and
each of them waited.

For a firm trading on an exchange, the slow ones are what matter. They cannot plan around a
system that is usually fast. So the number worth improving is not the average but the worst
cases, and almost everything difficult in this project comes from chasing them.

These slow outliers are stubborn. They are rarely caused by the program doing too much work.
They are caused by the program being made to wait for something it does not control.

## What makes a program wait, and what is done about it here

Four things interrupt a program that is otherwise ready to run. The project takes a position on
each.

**Asking the operating system for memory.** Most programs request memory whenever they need it
and hand it back when finished. The request usually returns immediately, but occasionally it
takes far longer, and there is no way to know in advance which time will be slow. This project
asks for all the memory it will need when it starts, and then never asks again while it is
running.

**Waiting for another part of the program to finish.** When two threads need the same piece of
data, the usual approach is that one waits while the other works. The waiting one stops
completely, and how long it stops for depends on what else the computer is doing. This project
uses methods that let threads share data without any of them ever being stopped.

**Waiting for the disk.** Every order is written to a permanent record before it takes effect, so
that nothing is lost if a program stops unexpectedly. Writing to disk is slow and unpredictable,
so the work is arranged so that no order waits for a disk write that has not already begun.

**Being moved aside by the operating system.** The operating system decides which program runs on
which processor, and it will move work around without warning. This project asks for particular
threads to stay on particular processors, so that the data they are working with stays in the
fast memory attached to those processors.

## What happens when something breaks

An exchange that stops working is a serious matter, so the framework provides a way to run two
copies of a program at once. One copy is in charge and the other follows along, keeping itself
up to date. If the one in charge stops, the follower takes over.

The difficult part is not the taking over. It is making sure that exactly one of them believes it
is in charge at any moment. Two programs both believing they are in charge would each accept
orders, and the records would disagree. Separate small programs, whose only job is to settle that
question, are used to decide.

## How orders arrive

Firms send orders over a network, in one of two languages.

The first is called **FIX**, which stands for Financial Information eXchange. It is the language
most real exchanges accept, it is written as readable text, and it has been in use since the
early nineteen-nineties.

The second is a compact format of this project's own, in which each message is a fixed
arrangement of bytes rather than text. It is faster to read, because there is no text to pick
apart, and this project measures how much faster.

Both arrive at a program called a **gateway**, whose job is to speak the language, check that
what arrived makes sense, and pass it on.

## Is this a real product?

No, and it is worth being plain about that.

This is one person's project, written to explore how systems of this kind are built and to have
somewhere realistic to measure things. No exchange runs it. It is not for sale and nobody is
supporting it. It is released under the Apache 2.0 licence, so anyone may read it, use it or
build on it.

Development is currently **paused** at version 0.4.0. [The project status](docs/project_status.md)
says exactly what works, what is known to be wrong, and where somebody picking it up should
begin.

## Words you will meet

| Word | What it means here |
|---|---|
| Order | An instruction to buy or sell |
| Execution report | The answer sent back, describing what was done |
| Gateway | The program that firms connect to, which speaks their language |
| Sequencer | The program that puts every order into one definite order and writes it down |
| Matching engine | The program that decides what happens to an order |
| Latency | How long one order takes from arriving to being answered |
| Throughput | How many orders can be handled per second |
| Thread | One sequence of work inside a program; several can run at once |
| Framework | Code providing the common parts, so each program supplies only what is specific to it |

## Where to go next

**If you are curious but do not write software**, this is probably the right place to stop.
[The project status](docs/project_status.md) is readable and says where things stand.

**If you write software and want to know whether this is worth your time**, read
[the project status](docs/project_status.md), then
[the architecture](docs/orientation/architecture.md), which shows the programs and how an order
travels between them.

**If you want to build it and run it**, the quick start is further down this page, and
[building](docs/orientation/building.md) explains the build in full.

**If you want to understand how it works inside**, [the documentation contents](docs/README.md)
lists every chapter. [The framework chapter](docs/framework/README.md) is where the ideas above
are implemented.

---

The rest of this page is written for people who build and run the project.

---

## Technical summary

A low-latency, multi-threaded, event-driven application framework for C++17, built around the **reactor pattern**. It provides inter-thread communication, inter-process communication, pub/sub messaging, timers, high availability, and a binary serialisation DSL — all designed for environments where heap allocation on the hot path is not acceptable.

## Repository layout

| Directory | Contents |
|---|---|
| `libraries/pubsub_itc_fw/` | The framework: headers, sources and unit tests |
| `applications/` | The venue built on it — gateways, sequencer, matching engine, arbiter, auth service |
| `scripts/` | **Every script.** Build, release and deploy tooling, test harnesses, performance and profiling runners, and the shell wrappers that set the third-party environment |
| `db/` | Schema, liquibase changelogs, and the two database scripts the deploy tooling calls |
| `environments/` | One TOML per environment; the source of truth for a deployment's hosts, ports and paths |
| `docs/` | Design notes, the bug list, the roadmap; `docs/README.md` is the way in |
| `python/` | The serialisation DSL and its test suite |

Scripts are run from the repository root — `python3 scripts/deploy.py`, `./scripts/build.sh` — and each resolves the project root from its own location, so the working directory does not matter.

## Features

- **Inter-thread communication (ITC)** via lock-free MPSC queues
- **Inter-process communication (IPC)** via unicast TCP with zero-copy PDU paths
- **Pub/sub messaging** via unicast fanout
- **Timers** via `timerfd` and `epoll`
- **High availability** via primary/secondary instance pairs with external arbiter pool and automatic leader election
- **Binary serialisation DSL** — a Python code generator producing C++17 encode/decode headers; sub-100ns round-trip on typical messages

## Design Principles

- CPU-pinned threads with lock-free fast paths throughout
- No heap allocation on any hot path — pool allocators, bump allocators, and slab allocators used exclusively
- Zero-copy on all inbound and outbound PDU paths
- Deterministic shutdown
- Message ordering preserved

## High Availability

The framework provides a built-in leader-follower protocol for deploying resilient application pairs. Two application instances are deployed — primary and secondary — and leader election is deterministic: the node with the lowest configured `instance_id` wins.

A separate pool of up to three dedicated arbiter processes (arbiter_primary, arbiter_secondary, witness) provides external arbitration to prevent split-brain when both nodes are undecided. Once elected, the peer-to-peer connection between the two application nodes is maintained with heartbeats. If the leader fails, the follower promotes itself and increments the epoch, ensuring that any restarting node can immediately recognise it is stale and rejoin as follower without requiring further arbitration.

The protocol is intentionally simple — there is no need for a full consensus algorithm such as Raft or Paxos given the fixed two-node-plus-arbiter topology.

## Security (TLS and SCRAM)

Transport encryption is implemented with OpenSSL (memory BIOs) and FIX logons authenticate with SCRAM-SHA-256. TLS is opt-in per listener via configuration:

- **Order gateway — encrypted FIX listener.** The gateway registers a TLS FIX listener *alongside* its plain listener. Enable it in the `[fix_tls]` block of `fix_order_gateway.toml`:

  ```toml
  tls_listen_port = 9880          # encrypted FIX endpoint (plain stays on its own port)

  [fix_tls]
  enabled = true
  cert    = "server.crt"
  key     = "server.key"
  ```

- **Authentication service — TLS listener.** Configured with `tls_certificate_path` / `tls_private_key_path`, an optional `tls_ca_path`, and `tls_require_client_certificate` (mutual TLS) in `authentication_service_{a,b}.toml`.

Self-signed certificates for a local sandbox are generated by the deploy flow (`--skip-certs` to reuse existing ones). See [secure_comms.md](docs/operations/secure_comms.md) for the design (why memory BIOs, the handshake state machine, and the SCRAM exchange).

## Serialisation DSL

Messages are defined in a lightweight DSL and compiled to C++17 headers by a Python code generator:

```
message StatusQuery (id=100, version=1)
    i64 instance_id
    i32 epoch
end
```

Supported field types include `i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, and named enum and message references. The wire format is little-endian binary. On little-endian hosts, `list<primitive>` decode is zero-copy.

## Requirements

| Item | Detail |
|---|---|
| Language | C++17 |
| Target compiler | gcc-8.5 / RHEL 8 |
| Build system | CMake + `build.py` |
| Logging | Quill v11.x |
| Test framework | GoogleTest (C++), pytest (DSL tests) |

## Building and running

The fastest path from a fresh clone to a running system:

```bash
./scripts/devsetup.sh                    # build, package and deploy
python3 scripts/devenv.py start          # start every component
python3 scripts/devenv.py status         # see what is running
```

**[Building, deploying and running](docs/orientation/running.md)** covers all of it properly:
the build wrappers and their flags, the RHEL 8 container build, packaging, deployment, the
developer sandbox, and the `clang-format` pre-commit hook.

## Documentation

Start at **[docs/README.md](docs/README.md)**. From there:

**Architecture and design:**
- [Architecture](docs/orientation/architecture.md) — component topology, order flow, port allocation
- [Threading](docs/framework/threading.md) — `ApplicationThread`, Vyukov MPSC queue, lifecycle, stuck-thread detection
- [Reactor](docs/framework/reactor.md) — epoll event loop, connection managers, timers, housekeeping
- [Allocators](docs/framework/allocators.md) — pool, bump, and slab allocators; no heap on hot paths
- [Socket Communications](docs/framework/socket_comms.md) — PDU framing, raw socket protocol handler, backpressure
- [Secure Communications](docs/operations/secure_comms.md) — TLS (OpenSSL memory BIOs), SCRAM-SHA-256
- [Write-Ahead Log](docs/durability/wal.md) — the append-only log primitive: format, segmentation, cursor/replay model
- [WAL and High Availability](docs/availability/wal_and_ha.md) — two-tier commit, replication, leader election, arbiter PSA topology
- [Pub/Sub](docs/pubsub/pubsub.md) — topic fan-out over the WAL; publishing (MEP) and subscribing (`topic_probe`) worked examples
- [Serialisation DSL](docs/framework/serialisation_dsl.md) — DSL syntax, generated C++ API, wire format, benchmarks
- [Sequencer Design](docs/venue/sequencer.md) — routing map, inline WAL handler, replay mode
- [CPU Pinning](docs/framework/cpu_pinning.md) — shared-memory CPU registry, RT scheduling

**Applications:**
- [Order Gateway](docs/venue/fix_order_gateway.md)
- [Sequencer](docs/venue/sequencer_app.md)
- [Matching Engine](docs/venue/matching_engine.md)
- [Admin Service](docs/venue/admin_service.md)
- [FIX Test Client](docs/venue/fix_test_client.md)

**API reference** — run `./scripts/build.sh --doxygen` then open `build/doxygen/html/index.html`

## Namespace

All framework classes live in the `pubsub_itc_fw` namespace.

## License

Apache-2.0