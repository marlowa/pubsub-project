# DD-driven PDU generation and the internal envelope — design (draft) {#fix_pdu_generation}

**Status:** Draft for review. Supersedes the flat, spec-curated generator committed as a
stepping stone (`python/dd_to_dsl` + `fix_equity_orders.spec.toml`). Not yet implemented.

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
dd_to_dsl --dd <dd.xml> [--dd <more.xml>] \
          --message NewOrderSingle --message OrderCancelRequest --message ExecutionReport \
          --output <build>/fix_equity_orders.dsl
```

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
So the generator runs against the standard `FIX50SP2.xml` here just as it will against a
curated exchange DD at work; the pipeline (gateway parse, PDU, WAL, ME, MEP, topic_probe, ER)
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

The generated `.dsl` becomes a **build artifact**: `git rm` it, add to `.gitignore`, and have
CMake run the generator (spec/DD + message names → `.dsl`) before the existing `.dsl → .hpp`
step, chaining the dependency so incremental builds are correct. Source-controlled inputs are
the **DD XML** and the CMake message list (and the trimmed demo DD).

---

## 5. Staged plan

1. **Envelope first** (independent of the generator): introduce `PubSubEnvelope` (`bytes`
   payload + `pdu_id`), move the three internal fields out of the PDUs, update
   gateway/sequencer/ME/MEP/topic_probe/WAL. Land it green before touching generation.
2. **Full-message generator**: component inlining, then repeating groups (`list<>`), then
   auto-derive enums/required/order/types; drop the field-list spec (message names + ids on
   the CLI).
3. **Pipeline for the full NOS**: gateway parses the full inbound NOS into the PDU; ME/ER echo
   the expanded field set; the WAL and topic_probe carry it.
4. **Web UI**: pinned quick-order bar + collapsible advanced fields + group sub-panels (§3).
5. **CMake lifecycle**: generate the `.dsl` at build time from DD + message names; git-rm +
   gitignore it.
6. **Verify**: whole pipeline builds; C++ + DSL tests green; live NOS→ER round-trip through
   topic_probe shows the full DD-derived field set.

---

## Open questions

- Group modelling: how deep to nest (some FIX groups contain sub-groups). Start one level,
  extend as needed.
- Whether the ME should act on any newly-carried fields, or purely echo them for now (leaning
  echo-only; matching logic stays keyed on the existing fields).
