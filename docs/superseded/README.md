# Superseded working notes

These documents are **historical working notes, kept for provenance**. They record the
requirements-gathering and the intermediate designs from which the pub/sub primitive was built.
They carry dated status, ideas that were tried and dropped, and commit references. They are not
current reference documentation.

A note belongs here only when the work it describes is **finished**. Three documents that used to
sit in this folder did not meet that test and have been moved to [Pub/sub](../pubsub/README.md):
`flow_control.md` says of itself "Not yet implemented", and the two TAP documents describe a
component that does not exist yet. Filing live design as history hid it from the contents page and
left the code citing a path that no longer resolved.

| Superseded note | What it was | Why it is finished |
|---|---|---|
| [pubsub_requirements.md](pubsub_requirements.md) | Requirements-gathering for the pub/sub primitive | "implemented and live-verified as of 2026-07-11" |
| [dsl_topic_catalog.md](dsl_topic_catalog.md) | The DSL topic-catalog and include-mechanism design | "All four steps DONE" |

For the current design, read [Pub/sub](../pubsub/README.md) and
[Write-Ahead Log](../durability/wal.md).

---

Back to the [documentation contents](../README.md).
