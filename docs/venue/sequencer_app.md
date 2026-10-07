# Sequencer Application {#sequencer_app}

## Role

This document is about running the sequencer: its instances, its connections, its configuration and
its ports. What it does with orders, cancels and reports is in [Sequencer Design](sequencer.md).

Two instances run from one binary, `bin/sequencer`: `sequencer_primary` (instance 1) and
`sequencer_secondary` (instance 2), configured by `sequencer_primary.toml` and
`sequencer_secondary.toml`, which differ only in the instance number and the values the environment
file supplies.

```
sequencer <logfile> <config.toml> [--replay]
```

## Startup

**Startup order does not matter.** At start each sequencer connects out to:

<!-- verify: present applications/sequencer/SequencerThread.cpp "connect_to_service(endpoint.service_name())" -->
- the report listener of every gateway instance that has an enabled `[[gateway]]` entry;
- the primary matching engine's order listener, and with high availability on, the secondary's;
- with high availability on, both arbiters and its peer sequencer.

Each connection is retried every two seconds until it answers, without limit. Starting the
sequencers before the gateways costs at most one retry interval before reports can flow, and loses
nothing, because there are no orders yet to report on.

Both instances make all of these connections, whichever of them leads, so a follower that takes the
lead already holds every connection it needs.

**With high availability off** (`[ha] ha_enabled = false`), the sequencer takes the next leadership
epoch and leads at once. It connects to no arbiter, no peer and no secondary matching engine, and
nothing waits for a follower.

**With high availability on**, it leads only while a majority of three voters (itself, its peer and
the arbiter pool) has granted it a lease that has not run out. It asks both other voters, and renews
its lease every renewal interval while it leads. When its lease runs out it stops leading at once.
See [Deciding leadership by majority, with leases](../availability/majority_leases.md). The value of
`ha_enabled` comes from the environment file's `[ha] enabled`, the one switch for the whole venue.

**On every start**, the sequencer opens its log, finds the last sequence number in it so that
numbering carries on from there, and starts writing a snapshot of the log's position every 30
seconds. With high availability on, it also fills its table of the command identifiers in the log,
in the background (see [Sequencer Design](sequencer.md)).

## Replay mode

`--replay` is an offline tool, run against a copy of a log. The sequencer reads every record in the
log, ignoring the snapshot, becomes leader without recording a new epoch (so it cannot overwrite the
epoch of a live instance sharing the directory), connects only to the matching engine, and once both
its connections to the engine are up sends it every order and cancel in the log with its original
time of sequencing. The engine then produces the same reports, with the same times, as the original
run.

## Ports

In the development environment. The preprod, prod and test-1 environment files use the same numbers for the sequencers' own listeners.

| Port | Instance | What listens or connects |
|------|----------|--------------------------|
| 11001, 11002 | primary, secondary | Each sequencer's order listener: the gateways connect here |
| 11021, 11022 | primary, secondary | Each sequencer's report listener: the matching engines connect here |
| 11003, 11004 | primary, secondary | Each sequencer's peer listener; each connects to the other's |
| 11030, 11031 | primary, secondary | Each sequencer's listener for downstream subscribers, such as the matching engine publishers |
| 11010, 11011 | gateways | The FIX gateway instances' report listeners, which the sequencers connect to |
| 11110, 11111 | gateways | The binary gateway instances' report listeners |
| 11020, 11023 | engines | The primary and secondary matching engines' order listeners |
| 11200, 11201 | arbiters | The two arbiters |

## Configuration

| Section and key | Purpose |
|---|---|
| `[network] listen_port`, `er_listen_port` | This sequencer's order listener and report listener |
| `[[gateway]] protocol`, `instance`, `enabled`, `host`, `port` | One entry for each gateway instance: its protocol (1 for FIX, 2 for binary), its instance number, and its report listener. An entry with `enabled = false` is skipped, and a sequencer with no enabled entry refuses to start |
| `[matching_engine]`, `[matching_engine_secondary]` | The two matching engines' order listeners |
| `[ha] ha_enabled`, `instance_id` | The venue-wide high availability switch, and this instance's number |
| `[ha] arbiter_primary_*`, `arbiter_secondary_*` | The two arbiters |
| `[peer] listen_port`, `host`, `port` | The connection to the peer sequencer: this instance listens on one port and connects to the other's |
| `[lease]` | The lease period, the allowance for clock drift and the renewal interval, from the environment's shared values so that every voter agrees |
| `[commands] identifiers_reserved` | How many command identifiers the table that stops a command being sequenced twice is reserved for: 200 million. Read only with high availability on |
| `[wal_subscriber] listen_port` | The listener for downstream subscribers |
| `[wal] directory`, `segment_size`, `snapshot_interval_seconds` | Where the log is kept, the size of each segment file (4 MiB), and how often the snapshot is written (30 seconds) |
| `[metrics]` | The Prometheus endpoint, and the bucket bounds for the log write time and the order path histograms |
| `[reactor]`, `[logging]`, `[event_queue_pool]`, `[command_queue_pool]` | CPU pinning and waiting, log levels, and the pools the reactor's queues take their entries from |

## See Also

- [Sequencer Design](sequencer.md) — what the sequencer does with commands and reports
- [WAL and High Availability](../availability/wal_and_ha.md) — the commit and replication rules, and failover
- [Architecture](../orientation/architecture.md) — the order flow and the components
