# PubSubFanout — Reactor Framework Documentation

Welcome to the documentation for **PubSubFanout**, a high‑performance,
thread‑pinned, lock‑free reactor framework designed for ultra‑low‑latency
inter‑thread communication and deterministic application behaviour.

This documentation set is divided into two major parts:

---

## 1. API Reference
Generated automatically from the source code.  
It includes:

- Classes, structs, enums, and free functions  
- Member documentation  
- Collaboration and inheritance diagrams  
- Cross‑references between subsystems  

---

## 2. Design Documentation
These pages describe the architectural intent behind the framework:

- [Overall Design Overview](design_overview.md)
- [Allocator Design](allocator_design.md)
- [Lock‑Free Queue Design](queue_design.md)
- [Threading Model](threading_model.md)
- [Reactor Design](reactor_design.md)
- [Message Flow](message_flow.md)
- [Shutdown Semantics](shutdown_semantics.md)
- [Watermark Semantics](watermark_semantics.md)
- [Memory Model](memory_model.md)
- [Logging Architecture](logging_architecture.md)
- [Instrumentation](instrumentation.md)
- [Utilities](utilities.md)

These documents are intended for:

- Framework maintainers  
- Performance engineers  
- Contributors implementing new handlers, threads, or allocators  
- Anyone diagnosing behaviour in production  

---

## Architectural Philosophy

PubSubFanout is built around several core principles:

- **Deterministic thread ownership**  
  Each `ApplicationThread` owns its queue, allocator, and timers.

- **Lock‑free fast paths**  
  The common case avoids locks entirely (allocators, queues, message routing).

- **Explicit contracts**  
  Shutdown, lifetime, and ownership rules are spelled out and enforced.

- **Observability without interference**  
  Statistics and diagnostics never alter behaviour.

- **Fail‑fast correctness**  
  Violations of preconditions are surfaced immediately via `PreconditionAssertion`.

---

## Subsystem Overview

- **Allocator Subsystem**  
  Lock‑free, expandable pools with NUMA‑friendly behaviour.

- **Queue Subsystem**  
  Vyukov MPSC queue with watermark hysteresis and shutdown semantics.

- **Threading Subsystem**  
  Deterministic thread lifecycle, pause/resume, timers, and message dispatch.

- **Reactor Subsystem**  
  Central orchestrator for events, threads, and shutdown propagation.

- **Messaging Subsystem**  
  Zero‑copy envelopes for inter‑thread and reactor events.

- **Logging Subsystem**  
  Unified logging abstraction over Quill with file, console, and syslog sinks.

- **Instrumentation**  
  Nanosecond‑resolution latency histograms for performance studies.

---

## Getting Started

If you are new to the framework, begin with:

1. [Design Overview](design_overview.md)  
2. [Threading Model](threading_model.md)  
3. [Message Flow](message_flow.md)

These three pages will give you a complete mental model of how the system behaves.

---

## Contributing

Contributions should follow the architectural contracts described in the design
documents. In particular:

- Avoid introducing blocking operations on hot paths  
- Respect allocator and queue lifetime rules  
- Maintain deterministic shutdown behaviour  
- Preserve NUMA‑aware and cache‑friendly layout choices  

---

_End of mainpage._
