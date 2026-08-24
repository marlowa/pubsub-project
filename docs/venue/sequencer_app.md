# Sequencer Application {#sequencer_app}

## Role

The sequencer is the **sole writer** to the matching engine's input stream, imposing total
order on all messages (the Aeron sequencer pattern). It:

- Accepts order PDUs from all gateways.
- Assigns a monotonically increasing sequence number to each order.
- Appends the order to its Write-Ahead Log.
- Stamps `sequenced_at` (wall-clock nanoseconds) on each forwarded PDU so the ME can use
  it as `TransactTime` on ERs for replay determinism.
- Forwards the sequenced PDU to the matching engine.
- Receives execution report PDUs from the ME and routes them to the correct gateway by
  `(SenderCompID, TargetCompID)`.
- Replicates WAL records to its peer (follower) over a dedicated TCP channel.
- Gates ER emission on receiving a WalAck from the follower (two-machine durability).

## Startup and Configuration

**Startup order:** the gateway must start before the sequencer. The sequencer connects
outbound to the gateway's ER inbound listener (port 7010); if the sequencer starts first it
retries at a 2-second interval.

**`ha_enabled = false`** (default for single-instance dev runs): the sequencer immediately
adopts `Role::leader` in `on_initial_event`, skips the arbiter and peer connections, and
skips WAL replication. ERs are emitted immediately without waiting for a WalAck.

**`ha_enabled = true`**: the sequencer connects to its peer and to the arbiter, runs the
startup election flow (StatusQuery / StatusResponse / ArbitrationReport /
ArbitrationDecision), and begins WAL replication on the peer connection.

**`--replay` flag:** when passed on the command line, the sequencer loads its most recent
snapshot and replays the WAL tail before joining the cluster. Used for cold restart and for
testing WAL recovery.

## HA Pair Operation

Two sequencer instances form a primary/secondary pair. At any moment exactly one is leader
and the other is follower. The arbiter pool mediates transitions.

**Leader responsibilities:**
- Assign seqNo and append to WAL.
- Forward sequenced order PDUs to ME.
- Stream WAL records (`WalRecord`, pdu_id=103) to the follower.
- Buffer ERs from ME in `pending_er_` (keyed by seq_no) until the follower acks.
- Emit buffered ERs to the gateway after receiving `WalAck` (pdu_id=104).

**Follower responsibilities:**
- Tail the leader's WAL records over the replication channel.
- Append records to its own local WAL.
- Send `WalAck` per record.
- Maintain pre-warmed connections to gateway and ME (no data flows while passive).
- On promotion: replay any unapplied WAL tail, adopt leader role, begin forwarding.

**WAL replication via inline handler:** the sequencer installs an `InlinePduHandler` on
the replication channel. `WalRecord` PDUs (pdu_id=103) are handled directly on the reactor
thread — decoded into the local WAL and a `WalAck` sent back — without an ITC queue
round-trip. This eliminates a wakeup-latency hop on the critical path. If the framer has
pending data (backpressure), the PDU falls through to the normal ITC path.

**Degraded mode:** if the follower disconnects entirely, the leader flushes all ERs
buffered in `pending_er_` immediately and continues as single-machine-durable until a new
follower is paired in by the arbiter.

## Implemented Slices

| Slice | Description | Status |
|-------|-------------|--------|
| 1 | seqNo on wire and in `EventMessage`; `PduHeader.seq_no` field | Done |
| 2 | In-memory WAL (`SequencerWal`) | Done |
| 3 | mmap'd WAL on disk, segmented files, CRC32, no fsync | Done |
| 4 | Snapshot (single; not yet rolling dual-snapshot) | Done |
| 5 | `(SenderCompID, TargetCompID)` routing map stamped on every forwarded ER | Done |
| 6 | Leader-follower state machine (`Role::unknown/leader/follower`, epoch, arbiter contact) | Done |
| 7 | Network WAL replication; `pending_er_` buffer; WalAck-gated ER emission | Done |
| 8 | Arbiter PSA+witness topology | Done |

## Port Allocation

| Port | Usage |
|------|-------|
| 7001 | Inbound order PDUs — gateway → sequencer primary |
| 7002 | Inbound order PDUs — gateway → sequencer secondary |
| 7003/7004 | Peer-to-peer WAL replication channel |
| 7010 | Outbound ER forwarding — sequencer → gateway ER listener |
| 7020 | Outbound order PDUs — sequencer → ME |
| 7021 | Inbound ER PDUs — ME → sequencer primary |
| 7022 | Inbound ER PDUs — ME → sequencer secondary (reserved) |
| 7100 | Sequencer → arbiter |

## Configuration

Key `sequencer.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] order_listener_port` | Inbound order PDU port (default 7001) |
| `[network] er_listener_port` | Inbound ER PDU port from ME (default 7021) |
| `[network] gateway_er_host / er_port` | Gateway ER inbound endpoint (default port 7010) |
| `[network] me_host / me_port` | ME order listener (default port 7020) |
| `[network] peer_*` | Peer sequencer for HA (used when `ha_enabled=true`) |
| `[network] arbiter_*` | Arbiter endpoint |
| `[wal] directory` | WAL segment storage path |
| `[wal] segment_size` | Segment file size in bytes |
| `ha_enabled` | Enables peer and arbiter connections, WAL replication, ER gating |
| `heartbeat_interval_seconds` | Default 5 s |
| `heartbeat_timeout_seconds` | Default 15 s |

## See Also

- [WAL and High Availability](../availability/wal_and_ha.md) — WAL format, two-tier commit, replication channel, failover
- [Architecture](../orientation/architecture.md) — full order flow and component topology
