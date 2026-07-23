# Design history — superseded working notes

These documents are **historical working notes**, kept for provenance. They record the
requirements-gathering, the options weighed, and the intermediate designs from which the
pub/sub primitive was built. They contain dated status, ideas that were tried and dropped,
and commit references — they are *not* current reference documentation and are deliberately
kept out of the documentation index.

For the current, authoritative design, read instead:

- **[Pub/Sub](../pubsub.md)** — the pub/sub design as it stands, with publishing and
  subscribing worked examples.
- **[Write-Ahead Log](../wal.md)** — the log primitive pub/sub is built on.

| Superseded note | What it was |
|-----------------|-------------|
| `pubsub_requirements.md` | Requirements-gathering for the pub/sub primitive. |
| `mep_tap.md` | The original MEP/TAP solution sketch (pre-implementation). |
| `pubsub_flow_control.md` | The flow-control / backpressure / control-channel decisions as first argued. |
| `pubsub_mep_rewire_and_tap.md` | The plan and status log for rewiring the MEP onto `TopicPublisher`. |
| `dsl_topic_catalog.md` | The DSL topic-catalog and include-mechanism design and plan. |
