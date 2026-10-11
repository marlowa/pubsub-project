# Durability

The write-ahead log, and what can be rebuilt from it.

- [wal.md](wal.md) — The log primitive: format, commit, and the guarantees it offers
- [replay.md](replay.md) — notes toward a design, not designed: what replay would need, why a WAL beats a packet capture, and the timer rule
- [open_order_checkpoint.md](open_order_checkpoint.md) — how the matching engine recovers its open orders after a process restart, from a memory-mapped region, and why replaying the log is too slow (built)

---

Back to the [documentation contents](../README.md).
