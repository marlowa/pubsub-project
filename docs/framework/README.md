# The framework

`pubsub_itc_fw`, the reactor and threading library everything else is built on. These documents describe
the primitive rather than the venue: a component here is a building block, not a process you can start.

- [reactor.md](reactor.md) — The event loop: edge-triggered epoll, non-blocking sockets, the callback contract
- [threading.md](threading.md) — ApplicationThread, the ITC message system, and the idle-blocking wake strategy
- [application_thread_itc.md](application_thread_itc.md) — The ITC message system in full, from the original design overview
- [allocators.md](allocators.md) — Pool, slab and bump allocators, chaining on exhaustion, and the tripwire
- [cpu_pinning.md](cpu_pinning.md) — Pinning threads to cores, and the registry that records what is pinned
- [cpu_pinning_anti_affinity.md](cpu_pinning_anti_affinity.md) — Declared core allocation, and background-by-default for everything undeclared
- [socket_comms.md](socket_comms.md) — Inbound and outbound connection management, retries, and idle reaping
- [serialisation_dsl.md](serialisation_dsl.md) — The DSL that generates PDU encoders and decoders
- <a href="summary.md">summary.md</a> — Project summary: quick facts, component inventory, and where each piece lives
- [topology.md](topology.md) — Companion to the PlantUML deployment diagram: what it shows and what it deliberately does not

---

Back to the [documentation contents](../README.md).
