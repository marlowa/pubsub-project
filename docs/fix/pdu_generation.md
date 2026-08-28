# DD-driven PDU generation and the internal envelope — design (draft) {#fix_pdu_generation}

**Status:** Implemented and accepted. Both pieces are in place — the full-message DD generator
(§1) and the internal envelope (§2, stages 1b–1f), with the vestigial internal fields since
deleted from the generated PDUs (1g). `fix_orders.dsl` is generated into the build tree from
`applications/fix_orders.dd.xml` at build time and is no longer source controlled (§4). The
flat, spec-curated input this superseded (`fix_orders.spec.toml`) is gone; `python/dd_to_dsl`
remains as the full-message generator.

The acceptance gate has been met: a full clean build followed by `ha_test.py --scenario all`
passes, including cancel-on-failover ER routing over the envelope, which `109df59` pins with an
assertion in the `me_ha` scenario. Note that on-disk WALs written before the envelope are
format-incompatible, so any such run must start from a clean WAL. See §5 for per-stage status.

## Goal

Generate the internal message PDUs (the serialisation `.dsl`) **directly from a FIX data
dictionary**, given only the list of *which messages* to emit. Everything else — fields,
types, enums, required/optional, order — comes from the DD. This is driven by the work use
case: at a venue, the exchange ships a DD that already lists exactly the messages, fields,
and **custom tags** in use, so "generate the whole message from that DD" yields exactly the
right PDU with no hand-authoring.

Two pieces make this exact:

1. **A full-message DD generator** — expands a whole DD message, including components and
   repeating groups.
2. **An internal envelope** — so pipeline-internal fields that are *not* in the DD stop
   living inside the FIX-derived PDU.

---

## 1. Full-message DD generator

CMake invokes the generator with a DD and message names; there is **no field-listing
spec**:

```
generate_dd_to_dsl.py --dd <dd.xml> [--dd <more.xml>] \
          --message NewOrderSingle:1000 --message OrderCancelRequest:1001 \
          --message ExecutionReport:1002 \
          --output <build>/fix_orders.dsl
```

This is what the top-level `CMakeLists.txt` runs; the message list and their PDU ids live in
that invocation and nowhere else.

Everything below is derived from the DD:

| Aspect | Derived from |
|--------|--------------|
| Which fields | the message body in the DD (all members) |
| Field order | DD member order |
| required / optional | the `required='Y/N'` flag on each member |
| Field type | DD field type → DSL type (`QTY/PRICE/AMT → string`, `UTCTIMESTAMP → datetime_ns`, `INT → i32`, else `string`) |
| Which fields are enums | any DD field carrying `<value>` entries |
| Enum member names | sanitised from the DD `description` — prose/punctuation-safe, length-capped, value-based fallback, per-value override. (Already built and kept.) |

### Components and repeating groups

A FIX message body is not flat. It references:

- **Components** (e.g. `Instrument`, `OrderQtyData`) — a named bundle of members. **Inline**
  them: the component's fields become direct fields of the message, recursively. This is how
  `Symbol`/`SecurityID` (both `Instrument`) already appear directly in today's PDU.
- **Repeating groups** (e.g. `Parties` → `NoPartyIDs`) — a count field plus a repeated body.
  Emit a **nested message** for the group body and a **`list<GroupBody>`** field on the
  parent. The DSL already supports nested message references and `list<T>`.

Cyclic component references are guarded (as the existing `fix_dictionary` expansion already
does).

### A full NOS in the demo (decided)

The demo grows up to carry the **full** FIX `NewOrderSingle` — all ~50 fields and its
repeating groups. A full order packet is ~1 KB; that is a normal order, not something to trim.
So the generator runs against the standard `FIX50SP2.xml` here just as it would against a
venue's own curated data dictionary; the pipeline (gateway parse, PDU, WAL, ME, MEP, topic_probe, ER)
is expanded to handle the full message. The ergonomic problem this creates lives in the **UI**
(see §3), not in the message.

### Non-DD residue

Two things are genuinely not in the DD:

- **PDU ids** (1000/1001/1002) — an internal i16 wire id per message, unrelated to the FIX
  `msgtype` string. Assigned by us. **Decided:** passed inline on the CLI as `Name:id`
  (`--message NewOrderSingle:1000`), so the whole "what to generate" instruction stays in the
  CMake invocation with no extra file.
- **Internal pipeline fields** (`gateway_session_conn_id`, `sequenced_at`,
  `sender_comp_id`) — see the envelope below; they leave the PDU entirely.

---

## 2. The internal envelope

Today the NOS/ER PDUs carry appended internal fields that are not FIX:

- `gateway_session_conn_id` — set by the gateway; used by the sequencer to route the ER back
  to the exact originating FIX session.
- `sequenced_at` — stamped by the sequencer; used by the ME as `transact_time` during replay.
- `sender_comp_id` — set by the gateway; retained for audit/logging in the WAL.

These block "PDU is 100% from the DD". The fix: carry them in an **envelope** that wraps the
DD-derived PDU, rather than inside it.

**The pattern already exists.** `WalRecord` (`leader_follower.dsl:161`) is already an
envelope — `seq_no` + `pdu_id` + `bytes payload` + `wall_time_ns` (which *is* `sequenced_at`).
The WAL/replication path already carries the PDU as an opaque payload with the sequenced time
in the wrapper. The **only outlier is the live gateway→sequencer→ME path**, which is why the
sequencer hand-copies ~20 PDU fields just to inject `sequenced_at`. So this work is *unifying
the live path onto the existing wrapper*, plus adding the two routing fields — not inventing a
new mechanism. Whether we **extend/reuse `WalRecord`** or add a **new peer envelope** is the
one open refinement (see below).

The proposal is to make one envelope the **single home** for routing/sequencing metadata:

```
message PubSubEnvelope
    i16 pdu_id                              # which DD-derived PDU is inside
    optional i32 gateway_session_conn_id    # gateway -> sequencer -> ER routing
    optional string sender_comp_id          # audit
    optional datetime_ns sequenced_at       # stamped by the sequencer
    bytes payload                           # the encoded DD-derived PDU
end
```

Flow after the change:

- **Gateway**: encodes the pure (DD-derived) NOS as `payload`, sets `gateway_session_conn_id`
  and `sender_comp_id` on the envelope.
- **Sequencer**: stamps `sequenced_at` on the envelope; WALs the envelope; routes the ER by
  the envelope's `gateway_session_conn_id`.
- **ME**: reads the envelope for `sequenced_at`, decodes `payload` as the FIX PDU.
- **MEP / topic_probe**: publish/parse the envelope; the DD-derived PDU is the payload.

This is core-path surgery (gateway, sequencer, ME, WAL record layout, MEP, topic_probe) but
it removes every non-DD field from the generated PDUs.

**Decided:** `bytes payload` + `pdu_id`. The envelope stays one static struct regardless of
which PDU it carries (simple, matches the existing wrap). No DSL tagged-union support is added.

### WAL storage format — the real blast radius (found during 1a)

This is deeper than "rewire three components", and the next session must plan for it:

- `wal_.append(seq, pdu_id, payload, size, wall_time)` (`Wal.hpp:149`) has **no routing
  fields**. Today `gateway_session_conn_id` survives in the WAL *only because it sits inside
  the NOS payload*. Moving it to the envelope means the WAL must instead store the **envelope**
  (record `pdu_id = WalRecord`, payload = the wrapped FIX PDU) — a **WAL storage-format change**.
- That cascades to everything that reads the WAL: **replay** (`SequencerThread` ~1050–1120),
  **snapshot restore** (~1580–1620), the **leader→follower replication** path (already sends
  `WalRecord`, `SequencerThread:1140`), the **MEP**, and **topic_probe** — all of which today
  dispatch on the record's `pdu_id` and would move to decode-envelope-then-payload.
- The ME decodes `NewOrderSingleView` **directly** from the payload (`MatchingEngineThread:246`);
  it moves to decode-envelope-then-payload.
- **Existing on-disk WALs become format-incompatible** → a clean WAL is required.
- The **730 unit/integration tests will not fully exercise replay/failover with the new
  format** — 1b–1g must be validated with the **live HA system + a failover test**
  (`ha_test.py`), not just the build.

Recommended landing order for the transplant: define the wire/format change with the WAL
storing the envelope first, migrate replay + snapshot + replication together, then gateway
wrap / sequencer passthrough (deleting the ~20-field hand-copy) / ME unwrap, then ER path,
then MEP + topic_probe, then delete the three internal fields from the DD PDUs (1g), with a
live NOS→ER→topic round-trip and a failover test as the acceptance gate.

### Implemented: 1b–1g (Option B — WalRecord as the on-wire + on-disk envelope)

Done as one coherent, build-green changeset. Decision: **Option B** — the WAL
stores the WalRecord envelope itself (record `pdu_id = WalRecord`, payload = the wrapped FIX
PDU + routing fields), so the persisted bytes are byte-identical to the follower-replication
and external-subscriber (MEP) streams, and every reader decodes envelope-then-payload.

- **Gateway** wraps each NOS/OCR in a `WalRecord` (`forward_order_in_envelope`), putting
  `gateway_session_conn_id` + `sender_comp_id` on the envelope and leaving them **unset**
  inside the FIX PDU; it unwraps the ER envelope to route by the envelope's conn id.
- **Sequencer**: the order path decodes only the envelope (FIX payload opaque — the ~20-field
  hand-copy is deleted), stamps `seq_no` + `wall_time_ns`, then `append_envelope_to_wal` +
  `send_wal_record(envelope)` + `stream_wal_record_to_external_subscribers(envelope)` + forwards
  the same stamped envelope to the ME. The ER path wraps the bare ME ER in an envelope carrying
  the routing conn id (the ~40-field hand-copy is deleted). Replay, WAL catch-up
  (`stream_wal_record_to_me`), and follower ingest (`handle_wal_record` + inline handler) all
  store/forward the wrapped envelope.
- **ME** unwraps the envelope, reads `wall_time_ns` as the sequencing time (passed into
  `handle_new_order_single` / `handle_order_cancel_request` as `sequenced_at_ns`), and decodes
  the inner FIX PDU.
- **MEP / topic_probe** unchanged: the MEP already unwraps `WalRecord` and republishes the
  inner (now pure) FIX PDU on its topic.

1g has since removed the three internal fields from the generated PDUs entirely, so nothing in
`fix_orders.dsl` is now anything but DD-derived. **Existing on-disk WALs are format-incompatible
→ start from a clean WAL.** Build and all unit/integration tests are green, and the
replay/failover acceptance gate (live HA + `ha_test.py --scenario all`) has been run and passes.

---

## 3. Web UI for a full NOS

A full NOS has ~50 scalar fields plus repeating groups, but the common case is a handful of
fields. The form must let a minimal order be entered quickly, with a **Send button that is
always reachable without scrolling**. Approach:

- **A pinned "quick order" bar** (`position: sticky; top: 0`) holding the everyday fields —
  ClOrdID, SecurityID (+ Symbol), Side, OrdType, Qty, Price — and the **Send** button. It
  stays visible however far the rest of the form is scrolled, so sending never requires
  scrolling the button into view.
- **Collapsible "More fields" section** (collapsed by default) for the remaining scalar
  fields, grouped by purpose (Instrument, Pricing, Quantities, Timing, Routing, Misc).
- **Repeating groups** (e.g. Parties) as collapsible sub-panels with **Add row / remove-row**
  controls — a small editable table per group, collapsed by default.
- **Send only what is filled:** empty optional fields are not put on the wire (already the
  behaviour), so a minimal entry produces a minimal NOS even though the form is large.
- Optional niceties: a field filter/search box; save/load named templates.

This keeps the everyday order a few keystrokes and one always-visible click, while making the
full message reachable when needed.

---

## 4. Lifecycle

The generated `.dsl` is a **build artifact**. It is gitignored and produced into
`build/generated_dsl/`, where CMake runs the DD generator before the existing `.dsl → .hpp`
step, chaining the dependency so incremental builds are correct. The only source-controlled
inputs are the **DD XML** (`applications/fix_orders.dd.xml`) and the message list in the CMake
invocation.

---

## 5. Staged plan

1. **Envelope first** — **done.** Landed as Option B: `WalRecord` is the envelope on the wire
   and on disk, carrying `gateway_session_conn_id` + `sender_comp_id`, with the three internal
   fields since deleted from the PDUs (1g).
2. **Full-message generator** — **done.** Components inlined, repeating groups as nested
   messages plus `list<>`, enums/required/order/types all derived from the DD; message names
   and PDU ids passed on the CLI, no field-list spec.
3. **Pipeline for the full NOS** — **done.** The gateway extracts the inbound groups into the
   PDU (growing its arena to need), and the ME echoes the expanded field set.
4. **Web UI** — **done.** Sticky quick-order bar, collapsible "More fields", and Parties group
   rows in the test client's message page.
5. **CMake lifecycle** — **done.** See §4.
6. **Verify** — **done.** Build and the C++/DSL suites are green, a live NOS→ER round-trip
   through `topic_probe` shows the full DD-derived field set including groups, and
   `ha_test.py --scenario all` passes from a clean build and a clean WAL. That last one
   matters: the unit and integration suites do not exercise replay or failover against the
   envelope format, so a green build alone is not evidence for it.

---

## Open questions

- Group modelling: how deep to nest (some FIX groups contain sub-groups). Start one level,
  extend as needed.
- Whether the ME should act on any newly-carried fields, or purely echo them for now (leaning
  echo-only; matching logic stays keyed on the existing fields).
