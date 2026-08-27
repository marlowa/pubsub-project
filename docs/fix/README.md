# FIX

The protocol itself: FIX 5.0 SP2 over FIXT.1.1 transport. Every message the venue writes begins `8=FIXT.1.1`.

- [codec.md](codec.md) — The parser and serialiser, and the performance guard on the hot path
- [pdu_generation.md](pdu_generation.md) — Generating PDUs from the data dictionary
- [inbound_sequence_checking.md](inbound_sequence_checking.md) — designed, not built: checking what the member sends, and why its resume bias is the opposite of the outbound one
- [sequence_numbers_and_gaps.md](sequence_numbers_and_gaps.md) — The session layer: numbering, resends, gap fill, and where this venue departs from it
- [load_client.md](load_client.md) — The load client, and why driving FIX under load needs one we own

---

Back to the [documentation contents](../README.md).
