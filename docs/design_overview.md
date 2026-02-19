# Design Overview

This document provides a high‑level architectural overview of the PubSubFanout
reactor framework. It explains how the major subsystems interact and the
principles that guide the design.

---

# 1. Architectural Goals

PubSubFanout is designed for environments where:

- **Threads are pinned to specific CPUs**
- **Latency is more important than throughput**
- **Memory allocation must be predictable**
- **Shutdown must be deterministic**
- **Message ordering must be preserved**
- **Lock‑free fast paths are essential**

The framework is intended for long‑running applications that process large
volumes of inter‑thread messages with minimal jitter.

---

# 2. Major Subsystems

## 2.1 Allocator Subsystem
The allocator subsystem provides:

- Lock‑free fixed‑size memory pools  
- Expandable pool chains  
- NUMA‑friendly allocation  
- Behavioural statistics for observability  

It is designed so that:

- Allocation is fast‑path lock‑free  
- Expansion is rare and protected by a mutex  
- Pools are never removed, only added  
- Deallocation is O(N) but off the critical path  

See: [Allocator Design](allocator_design.md)

---

## 2.2 Lock‑Free Queue Subsystem
The queue subsystem implements a Vyukov MPSC queue with:

- Multiple producers  
- A single consumer (the owning thread)  
- Watermark hysteresis  
- Shutdown semantics  
- Node allocation via the allocator subsystem  

The queue is the backbone of inter‑thread communication.

See: [Lock‑Free Queue Design](queue_design.md)

---

## 2.3 Threading Subsystem
Each `ApplicationThread` owns:

- A message queue  
- An allocator  
- A timer registry  
- A run loop  
- Pause/resume state  
- Shutdown logic  

Threads are started, paused, resumed, and shut down by the Reactor.

See: [Threading Model](threading_model.md)

---

## 2.4 Reactor Subsystem
The Reactor:

- Owns all `ApplicationThread` instances  
- Routes messages  
- Manages epoll handlers  
- Propagates shutdown  
- Detects thread inactivity  
- Provides the global lifecycle  

It is the central orchestrator of the system.

See: [Reactor Design](reactor_design.md)

---

## 2.5 Messaging Subsystem
Messages are represented by lightweight envelopes:

- No heap allocation  
- Zero‑copy payloads  
- Explicit type tags  
- Factory methods for safety  

See: [Message Flow](message_flow.md)

---

## 2.6 Logging Subsystem
A unified logging abstraction over Quill:

- File, console, and syslog sinks  
- Per‑destination log‑level filtering  
- Immediate flush support  
- Test‑sink support for unit tests  

See: [Logging Architecture](logging_architecture.md)

---

## 2.7 Instrumentation
The `LatencyRecorder` provides:

- Nanosecond‑bucket histograms  
- Thread‑safe recording  
- Dump formats for external analysis  

See: [Instrumentation](instrumentation.md)

---

# 3. Threading & Memory Model

The framework assumes:

- Threads are pinned to specific CPUs  
- Allocators and queues are thread‑local  
- Cross‑thread communication is explicit  
- Memory ordering is carefully controlled  
- False sharing is avoided via `CacheLine<T>`  

See: [Memory Model](memory_model.md)

---

# 4. Shutdown Semantics

Shutdown is deterministic:

- Reactor sets `is_finished_`  
- Threads detect shutdown and exit their run loops  
- Queues stop accepting new messages  
- Allocators remain valid until thread exit  
- No memory is freed prematurely  

See: [Shutdown Semantics](shutdown_semantics.md)

---

# 5. Watermark Semantics

Queues support:

- High watermark transitions  
- Low watermark transitions  
- Hysteresis to prevent handler storms  
- Producer‑side high watermark callbacks  
- Consumer‑side low watermark callbacks  

See: [Watermark Semantics](watermark_semantics.md)

---

# 6. Summary

PubSubFanout is a deterministic, lock‑free, NUMA‑aware reactor framework built
for high‑performance inter‑thread communication. The design emphasizes:

- Predictability  
- Observability  
- Explicit contracts  
- Minimal jitter  
- Safe shutdown  

The remaining design pages provide detailed explanations of each subsystem.

---

_End of design overview._
