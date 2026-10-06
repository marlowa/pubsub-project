# Generating PDUs from the data dictionary, and the internal envelope {#fix_pdu_generation}

The venue's FIX order messages — NewOrderSingle, OrderCancelRequest, ExecutionReport and
OrderCancelReject — travel between components as binary PDUs. Those PDUs are generated from a
FIX data dictionary, given only the list of messages to emit: every field, type, enumeration,
required flag and field order comes from the dictionary. Anything the venue needs that is not
FIX travels beside the message, in an envelope, not inside it.

This matches how a venue is defined: the exchange publishes a data dictionary listing exactly the
messages, fields and custom tags it uses, and generating each message whole from that dictionary
gives the right PDU with nothing written by hand.

---

## 1. The generator

CMake runs the generator with the dictionary and the message names. There is no other list of
fields:

```
generate_dd_to_dsl.py --dd applications/fix_orders.dd.xml \
          --message NewOrderSingle:1000 --message OrderCancelRequest:1001 \
          --message ExecutionReport:1002 --message OrderCancelReject:1003 \
          --output <build>/generated_dsl/fix_orders.dsl
```

The message list and the PDU ids exist only in that invocation, in the top-level
`CMakeLists.txt`. `--dd` may be given more than once.

The dictionary, `applications/fix_orders.dd.xml`, is a subset of FIX 5.0 SP2: four messages and
126 fields. It is the venue's dictionary in the sense above. It may only contain standard FIX 5.0
SP2 messages and fields, so the PDUs generated from it are a genuine FIX subset.

Everything else is derived from the dictionary:

| Aspect | Derived from |
|--------|--------------|
| Which fields | the message body in the dictionary (all members) |
| Field order | dictionary member order |
| required / optional | the `required='Y/N'` flag on each member |
| Field type | the dictionary type, mapped to a DSL type: `QTY`, `PRICE`, `AMT` and `PRICEOFFSET` → `string`, so no decimal is ever converted on the wire; `UTCTIMESTAMP` and `LOCALMKTDATE` → `datetime_ns`; `INT`, `SEQNUM`, `LENGTH`, `NUMINGROUP`, `DAYOFMONTH` → `i32`; `BOOLEAN` → `bool`; `CHAR` → `char`; anything else, including `MULTIPLECHARVALUE`, → `string` |
| Which fields are enums | any `INT` or `CHAR` field carrying `<value>` entries, as an `enum : i32` or `enum : char` |
| Enum member names | made from the dictionary `description`: made safe as identifiers, limited in length, with a fallback built from the value and an override per value where needed |

The PDU ids are the one thing the dictionary cannot supply: an internal 16-bit wire id per
message, unrelated to the FIX `MsgType`. They are given on the command line as `Name:id`, and the
generated file declares them in a `PduId` enum, which the DSL generator's `--pdu-id-enum` mode
then requires every message to use.

### Components and repeating groups

A FIX message body is not flat. It refers to:

- **Components** (for example `Instrument`, `OrderQtyData`) — a named bundle of members. The
  generator **inlines** them: the component's fields become direct fields of the message,
  recursively. That is why `Symbol` and `SecurityID`, both from `Instrument`, are direct fields
  of the PDU.
- **Repeating groups** (for example `Parties`, counted by `NoPartyIDs`) — a count field and a
  repeated body. The generator emits a **nested message** for the group body and a
  **`list<GroupBody>`** field on the parent.

Cyclic component references are guarded against.

### The whole message, not a trimmed one

The PDU carries the whole FIX message as the dictionary defines it — NewOrderSingle has about 50
fields and its repeating groups — not a selection. A full order is about 1 KB, which is a normal
order. Every stage of the order path handles the full message: the gateway extracts the inbound
groups into the PDU, sizing its decode arena to need; the sequencer, the write-ahead log and the
matching engine publisher carry it as an opaque payload; and the matching engine echoes the
expanded field set on its reports.

### Where the generated file lives

The generated `fix_orders.dsl` is a **build artefact**. It is ignored by git and written to
`build/generated_dsl/`. CMake runs the dictionary generator before the DSL-to-C++ step and makes
the second depend on the first, so an incremental build regenerates both when the dictionary or
the generator changes. The only inputs under source control are the dictionary XML and the
message list in the CMake invocation. `applications/pubsub.dsl` includes the generated file to
declare which messages belong to which topic.

---

## 2. The envelope

Some information every component needs is not FIX: which member's session an order came from,
through which gateway, when it was sequenced, and so on. None of it may be put inside the FIX
message as an invented tag, because the messages must stay a genuine FIX 5.0 SP2 subset. It
travels instead in an envelope around the encoded FIX PDU.

**The envelope is `WalRecord`** (`leader_follower.dsl`, pdu id 103). The same message serves as
the envelope on the wire between components, as the record stored in the write-ahead log, and as
the record replicated to the follower and streamed to the matching engine publisher, so the bytes
stored are the same bytes sent. Its fields:

| Field | Purpose |
|---|---|
| `seq_no`, `pdu_id`, `payload`, `wall_time_ns` | The sequence number, which PDU the payload holds, the encoded FIX PDU itself, and the time the leader sequenced it, which the matching engine uses as the order's time and replay uses as its clock |
| `gateway_session_conn_id`, `origin_gateway_id`, `gateway_instance_id`, `sender_comp_id` | Which client session the message came from: the connection within the gateway, the gateway's protocol and instance, and the member's comp id. Set together, and absent when there is no originating session, as on a report the matching engine produces for an order it is cancelling after a failover |
| `gateway_ingress_ns` | When the gateway read the order off the member's connection, for `order_round_trip_nanoseconds` and `order_path_elapsed_nanoseconds` |
| `poss_resend`, `sent_again`, `cl_ord_id` | Marks on a message that may repeat one already seen, and the ClOrdID, so a repeat can be recognised without decoding the payload |
| `leader_epoch` | Which sequencer leadership sent it |
| `report_engine_epoch`, `report_number`, `reports_forwarded_through_epoch`, `reports_forwarded_through_number` | The numbering of the matching engine's reports, which lets the sequencer tell which reports it has already forwarded |

`leader_follower.dsl` defines each field and says when it is set.

How each component uses it:

- **Gateway**: encodes the FIX message as the payload of a `WalRecord`, sets the session fields
  and the ingress time, and sends it to both sequencers (`forward_order_in_envelope`). On the way
  back it unwraps the report's envelope to find the session.
- **Sequencer**: decodes only the envelope; the FIX payload stays opaque. It stamps `seq_no` and
  `wall_time_ns`, appends the envelope to its log, replicates it to the follower, streams it to
  the external subscribers, and forwards it to the matching engine. Replay, catching up the
  matching engine, and the follower's own log all store and forward the same envelope. Reports
  from the matching engine arrive in an envelope too, and the sequencer routes each to the
  session it names.
- **Matching engine**: unwraps the envelope, takes `wall_time_ns` as the sequencing time, and
  decodes the FIX PDU inside. Its reports go back in an envelope.
- **Matching engine publisher**: reads the sequencer's log as a stream of `WalRecord`s, unwraps
  each, and publishes the FIX PDU inside it on the topic it belongs to. A topic subscriber, such
  as the topic probe, therefore receives the FIX PDU without the envelope.

The envelope holds the payload as `bytes` with a `pdu_id` beside it, so it is one fixed message
whatever it carries, and the DSL needs no tagged-union type. Because the log stores envelopes, a
log written in an earlier layout cannot be read; changing the envelope's layout in a way that is
not backward compatible needs a clean log. Adding an optional field at the end is backward
compatible, because a message that ends where an optional field would begin decodes with that
field absent.

---

## 3. The order form in the test client

A full NewOrderSingle has about 50 fields and several repeating groups, but most orders need a
handful. The order form on the test client's message page
(`java/fix-test-client/src/main/resources/web/messages.html`) is laid out for that:

- **A quick-order bar pinned to the top** (`position: sticky`), holding the everyday fields —
  ClOrdID, SecurityID and Symbol, Side, OrdType, quantity, price — and the **Send** button, so
  sending never needs the button scrolled into view.
- **"More fields"**, a collapsed section for the remaining fields.
- **Underlyings** and **Parties** as collapsed panels, each a small table with an **Add** button
  for another row.
- **Only filled fields are sent**, so a minimal entry produces a minimal NewOrderSingle although
  the form is large.

---

## Open questions

- Group modelling: how deeply to nest groups that contain groups of their own.
- Whether the matching engine should act on more of the fields it carries, or only echo them. It
  uses only a few of them.
