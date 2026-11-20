# The framework

`pubsub_itc_fw`, the reactor and threading library everything else is built on. These documents describe
the primitive rather than the venue: a component here is a building block, not a process you can start.

- [reactor.md](reactor.md) — The event loop: level-triggered epoll, non-blocking sockets, the callback contract
- [threading.md](threading.md) — ApplicationThread, the ITC message system, and the idle-blocking wake strategy
- [application_thread_itc.md](application_thread_itc.md) — EventMessage, who owns each payload, and sending a message from one thread to another
- [single_writer_histogram.md](single_writer_histogram.md) — A histogram the recording thread updates without a lock, read by the scrape without stopping it. **Agreed for BUG-0125, not built**
- [bounding_messages_waiting.md](bounding_messages_waiting.md) — A limit on how many messages from one connection may wait in an application thread's queue, with the reactor not reading the connection while that many wait. **Built for BUG-0234**
- [gathering_sends.md](gathering_sends.md) — The reactor writes the messages waiting for a connection several at a time, with one `sendmsg()` call, instead of one system call for each. **Built**: under overload 68,626 orders a second where it was 18,275, and lower latency at every percentile
- [allocators.md](allocators.md) — Pool, slab and bump allocators, chaining on exhaustion, and the tripwire
- [deleting_without_erased_marks.md](deleting_without_erased_marks.md) — Removing an order from the order map by moving later orders back, so that no slot is left marked erased and the map never copies itself to clear them. **Proposed for BUG-0028**
- [pools_that_grow_without_a_lock.md](pools_that_grow_without_a_lock.md) — Adding a pool to an `ExpandablePoolAllocator` without its mutex, with a spare pool mapped in advance, so that a queue that runs out of nodes makes no thread wait. **Proposed for BUG-0127**
- [tla/findings.md](tla/findings.md) — TLA+ models of the lock-free allocators and queues, checked with TLC: what each checks, and what was found
- [cpu_pinning.md](cpu_pinning.md) — Pinning threads to cores, and the registry that records what is pinned
- [cpu_pinning_anti_affinity.md](cpu_pinning_anti_affinity.md) — Declared core allocation, and background-by-default for everything undeclared
- [socket_comms.md](socket_comms.md) — Inbound and outbound connection management, retries, and idle reaping
- [serialisation_dsl.md](serialisation_dsl.md) — The DSL that generates PDU encoders and decoders
- [configuration_files.md](configuration_files.md) — How configuration files are read, and refusing keys nobody reads. **The refusal is design, not implemented**
- <a href="summary.md">summary.md</a> — Project summary: quick facts, component inventory, and where each piece lives
- [topology.md](topology.md) — Companion to the PlantUML deployment diagram: what it shows and what it deliberately does not

---

Back to the [documentation contents](../README.md).
