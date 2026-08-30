# Durability

The write-ahead log, and what can be rebuilt from it.

- [wal.md](wal.md) — The log primitive: format, commit, and the guarantees it offers
- [replay.md](replay.md) — NOTES: what replay would need, why a WAL beats a packet capture, and the timer rule
- [open_order_checkpoint.md](open_order_checkpoint.md) — DESIGN NOTE: recovering the matching engine's open orders after a process restart, and why the log cannot do it

---

Back to the [documentation contents](../README.md).
