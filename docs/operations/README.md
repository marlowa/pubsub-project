# Operations

Running the venue, watching it, and securing it.

- **[filesystem_requirements.md](filesystem_requirements.md) — the filesystem holding the write-ahead logs and the small synced state files must be mounted `lazytime`, and the matching engines' open-order regions must be on a different filesystem. Without the first, the sequencer stalls for hundreds of milliseconds; without the second, leases run out at startup. Read this before deploying to a new machine.**
- [metrics.md](metrics.md) — The Prometheus endpoint, what is exported, and why metrics stay out of the control plane
- [secure_comms.md](secure_comms.md) — TLS and SCRAM
- [trading_day_load.md](trading_day_load.md) — The compressed trading-day profile, and what a passing run does and does not prove
- [latency_findings.md](latency_findings.md) — What is established about this venue's latency, what measurement has ruled out, and what is still unexplained. Read it before changing a machine setting in the hope of a faster venue

---

Back to the [documentation contents](../README.md).
