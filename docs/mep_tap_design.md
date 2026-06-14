# MEP, TAP, and Supporting Infrastructure — Design

This document captures the design agreed in the session of 2026-06-12. It covers four
inter-dependent areas of work: a DSL extension, two new application components (MEP and TAP),
and the sequencer changes needed to support them. Nothing here is implemented yet. The document
is the reference for the implementation sessions that follow.

---

## 1. Context and Motivation

The system mirrors the architecture of a production exchange. The matching engine processes
orders and produces execution reports; the sequencer is the sole authority over order
sequencing and WAL persistence. Two categories of downstream consumers exist that the
current system does not serve:

- **Market data** — needs both the order stream (NOS + OCR) and the execution report stream
  (ER) at low latency relative to other non-core consumers.
- **TAP (Trade Activity Publisher)** — needs the order stream for publication to an enterprise
  bus (Kafka, Pulsar, or equivalent, not yet decided). It also needs ERs internally to manage
  its L3 order book and to know when orders are matched (filled), so it knows when to retire
  them from the book after the enterprise bus acknowledges receipt.

Neither consumer has the low-latency requirements of the core order flow. They are downstream
broadcast consumers, not participants in the order pipeline.

The WAL-follower-per-consumer pattern (used for the secondary sequencer) does not scale cleanly
to multiple consumers with fanout semantics. A topic-based pub/sub primitive is now justified
for the first time. This document designs that primitive, the component that hosts it (MEP),
and the first subscriber application (TAP).

---

## 2. DSL Extension: `array<MessageType>[N]`

### 2.1 Why it is needed

The topic delivery PDU (`TopicPage`) contains a fixed-size array of `TopicRecord` entries.
`TopicRecord` is itself a DSL message. The current `ArrayType` in the DSL only accepts
`PrimitiveType` elements (e.g., `array<i8>[512]`). To write `array<TopicRecord>[16]` we must
extend `ArrayType` to accept `ReferenceType` (named message) elements as well.

This extension is a prerequisite for writing `topics.dsl` and for the `TopicPage` PDU design
to be correct and bounded. Using `list<TopicRecord>` as a stopgap is explicitly rejected
because it produces open-ended PDUs.

### 2.2 Changes required

**`python/dsl/ast.py`**

Change `ArrayType.element_type` from `PrimitiveType` to `Union[PrimitiveType, ReferenceType]`:

```python
@dataclass
class ArrayType(Type):
    """A fixed-length array of primitive or named-message elements."""
    element_type: Union[PrimitiveType, ReferenceType]
    length: int
    line: int = 0
```

**`python/dsl/parser.py`**

The `array<T>[N]` parser currently only accepts primitive type names in the `T` position.
Extend it to also accept identifiers that may resolve to named messages (validated later by
the Validator). The parser should not attempt to resolve names itself.

**`python/dsl/validator.py`**

When `ArrayType.element_type` is a `ReferenceType`:
- Verify the referenced name is declared as a `MessageDecl` (not an `EnumDecl`) within the
  same DSL file. Enum types in arrays must use `PrimitiveType` directly, not `ReferenceType`.
- Emit a clear error if the referenced message is not yet declared at the point of reference
  (or require forward declaration, consistent with the existing message reference rules).

**`python/dsl/generator_cpp.py`**

The following methods require new `isinstance(type_node.element_type, ReferenceType)` branches:

| Method | Change |
|---|---|
| `_cpp_type_owning` | `array<Msg>[N]` → `std::array<Msg, N>` |
| `_cpp_type_view` | `array<Msg>[N]` → `std::array<MsgView, N>` (or `std::array<Msg, N>` if Msg is enum) |
| `_is_fixed_size` | Fixed only if the referenced message is itself fixed-size; call `_is_fixed_size` recursively on the referenced message's `MessageDecl` |
| `_emit_fixed_encoded_size` | Sum `fixed_encoded_size(element)` × N when element is a fixed-size message |
| `_emit_encode_fast_field` | Loop N times, call `encode_fast(element, write_cursor)` per element |
| `_emit_encoded_size_field` | Loop N times, sum `encoded_size(element)` |
| `_emit_encode_field` | Loop N times, call `encode(element, ...)` per element |
| `_emit_decode_field` | Loop N times, call `decode_Msg(element, ...)` per element |
| `_emit_skip_field` | Loop N times, call `skip_Msg(...)` per element |
| `_emit_max_decode_arena_bytes` | Multiply referenced message's arena bytes by N |
| `_emit_max_encode_arena_bytes` | Multiply referenced message's arena bytes by N |

**`python/dsl/generator_java.py`**

Apply equivalent changes for the Java backend. `array<Msg>[N]` maps to `Msg[] field = new Msg[N]`
in Java. Encode/decode loops follow the same pattern as the C++ generator.

### 2.3 Tests required

Add to `python/tests/test_roundtrip_nested_lists.py` (the existing roundtrip test file):

**Test 1 — `test_roundtrip_array_of_message`** (the new 3rd example)

Demonstrates `array<MessageType>[N]` end-to-end:

```python
def test_roundtrip_array_of_message():
    mod = build("""
        message Item (id=1)
            i32 value
        end

        message Batch (id=2)
            i16 item_count
            array<Item>[4] items
        end
    """)

    batch = mod.Batch(
        item_count=3,
        items=[mod.Item(value=10), mod.Item(value=20), mod.Item(value=30), mod.Item(value=0)],
    )

    buf = mod.encode_Batch(batch)
    batch2 = mod.decode_Batch(buf)

    assert batch2.item_count == 3
    assert batch2.items[0].value == 10
    assert batch2.items[1].value == 20
    assert batch2.items[2].value == 30
```

**Test 2 — `test_array_of_message_is_fixed_size`**

Verifies that an `array<FixedMessage>[N]` message is treated as fixed-size by the generator
(i.e., `encode_fast` is emitted):

```python
def test_array_of_message_is_fixed_size():
    from dsl.parser import Parser
    from dsl.validator import Validator
    from dsl.generator_cpp import CppGenerator
    text = """
        message Item (id=1)
            i32 value
        end
        message Batch (id=2)
            array<Item>[4] items
        end
    """
    ast = Parser(text).parse()
    Validator(ast).validate()
    code = CppGenerator("ns").emit(ast)
    assert "encode_fast" in code
```

**Test 3 — `test_array_of_variable_message_is_not_fixed_size`**

Verifies that if the element message contains a `string` field (variable-size), the containing
`array<Msg>[N]` message is NOT treated as fixed-size:

```python
def test_array_of_variable_message_is_not_fixed_size():
    from dsl.parser import Parser
    from dsl.validator import Validator
    from dsl.generator_cpp import CppGenerator
    text = """
        message Item (id=1)
            string label
        end
        message Batch (id=2)
            array<Item>[4] items
        end
    """
    ast = Parser(text).parse()
    Validator(ast).validate()
    code = CppGenerator("ns").emit(ast)
    assert "encode_fast" not in code
```

Additionally, add to the existing `test_generator_cpp.py`:

**Test 4 — `test_array_of_message_struct_generation`**

Checks that `std::array<Item, 4> items` appears in the generated struct.

**Test 5 — `test_array_of_message_encode_decode_signatures`**

Checks that the encode/decode loop for array elements calls `encode(message.items[i], ...)`.

---

## 3. New File: `topics.dsl`

Location: `applications/topics.dsl`
Namespace: `pubsub_itc_fw_app`
Generated header: `build/generated_dsl/topics.hpp`

This file defines the framework topic pub/sub protocol. It is independent of any specific
application topic (orders, execution reports, etc.) and is reusable for any future topic.

```
# ---------------------------------------------------------------------------
# topics.dsl
#
# Framework topic pub/sub protocol.
#
# A topic is a named event stream hosted by a publisher (e.g., MEP).
# Subscribers connect to the publisher's inbound listener, present a cursor,
# and receive a stream of TopicPage PDUs.
#
# PDU ID range: 107-115 reserved for this protocol.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# TopicRecord -- one event payload within a TopicPage.
#
# payload_size: actual number of valid bytes in payload_bytes (0..512).
# payload_bytes: the raw encoded DSL message payload (NOS, OCR, ER, etc.).
#   Fixed at 512 bytes per slot. Covers all realistic NOS/OCR/ER encodings
#   with margin. See docs/mep_tap_design.md for the sizing calculation.
#   If new fields are added to any of these messages, verify this constant
#   remains sufficient.
# ---------------------------------------------------------------------------
message TopicRecord (id=109)
    i64        seq_no
    i16        pdu_id
    datetime_ns wall_time_ns
    i16        payload_size
    array<i8>[512] payload_bytes
end

# ---------------------------------------------------------------------------
# TopicSubscribeRequest -- subscriber -> publisher on connection.
#
# subscriber_id: name for this subscriber (e.g. "tap_primary", "market_data").
#   Used by MEP for logging and per-connection cursor tracking only.
#   Not validated against any list; the topic API is open to any process.
# topic_name:    e.g. "orders" or "execution_reports".
# from_seq_no:   cursor. 0 = start of publisher's WAL (full replay).
#               -1 = start from current head (no replay).
#               Any other value = resume from that seq_no.
# ---------------------------------------------------------------------------
message TopicSubscribeRequest (id=107)
    string subscriber_id
    string topic_name
    i64    from_seq_no
end

# ---------------------------------------------------------------------------
# TopicSubscribeAck -- publisher -> subscriber.
#
# accepted_from_seq_no: the seq_no the publisher will actually start from.
#   May differ from the requested cursor if the requested position is before
#   the publisher's oldest retained WAL record, in which case the publisher
#   starts from its oldest available record and logs a warning.
# ---------------------------------------------------------------------------
message TopicSubscribeAck (id=108)
    i64 accepted_from_seq_no
end

# ---------------------------------------------------------------------------
# TopicPage -- publisher -> subscriber.
#
# record_count:  number of populated entries in records[] (1..16).
# page_number:   1-based index of this page within the current delivery cycle
#                (X in "page X of Y").
# total_pages:   total pages in this delivery cycle (Y in "page X of Y").
#                Calculated at the start of each delivery cycle and held
#                fixed for that cycle even if new records arrive mid-cycle.
#                During live delivery (subscriber caught up), page_number
#                and total_pages are both 1.
# records:       the payload. Only records[0..record_count-1] are populated.
#
# Flow control: subscriber sends TopicAck after processing each page.
# If page_number < total_pages, publisher sends next page immediately on
# receiving the ack (catch-up path). If page_number == total_pages,
# publisher sends next page when new records arrive (live path).
# ---------------------------------------------------------------------------
message TopicPage (id=109)
    i16    record_count
    i16    page_number
    i16    total_pages
    array<TopicRecord>[16] records
end

# ---------------------------------------------------------------------------
# TopicAck -- subscriber -> publisher.
#
# Sent after the subscriber has processed a TopicPage.
# last_seq_no: the seq_no of the last record the subscriber processed in
#              the page just acknowledged. Publisher advances this subscriber's
#              cursor to last_seq_no + 1 and considers WAL truncation.
# ---------------------------------------------------------------------------
message TopicAck (id=110)
    i64 last_seq_no
end

# ---------------------------------------------------------------------------
# TopicNotLeader -- MEP secondary -> subscriber.
#
# Sent by a passive (non-leader) MEP instance immediately after receiving
# a TopicSubscribeRequest. Signals to TopicSubscriberChannel that this
# endpoint is not the current leader and it should try the other endpoint.
# The connection is closed by MEP after sending this PDU.
# ---------------------------------------------------------------------------
message TopicNotLeader (id=111)
end
```

**Note:** `TopicRecord` (id=109) and `TopicPage` (id=109) cannot share the same PDU ID.
`TopicRecord` is an inner message type (not a PDU). Only `TopicPage` carries an id.
Remove `id=109` from `TopicRecord`'s declaration; `TopicRecord` is a plain message
used as an array element inside `TopicPage`.

**Corrected PDU ID table:**

| ID | Message | Direction |
|---|---|---|
| 107 | `TopicSubscribeRequest` | subscriber → publisher |
| 108 | `TopicSubscribeAck` | publisher → subscriber |
| 109 | `TopicPage` | publisher → subscriber |
| 110 | `TopicAck` | subscriber → publisher |
| 111 | `TopicNotLeader` | MEP secondary → subscriber |

---

## 4. Additions to `leader_follower.dsl`

Two new messages extend the WAL replication protocol to support external WAL subscribers
(MEP primary and MEP secondary connecting to the sequencer's new external listener).

The existing `WalRecord` (id=103) and `WalAck` (id=104) PDUs are reused unchanged for
streaming records and acks between sequencer and MEP.

```
# ---------------------------------------------------------------------------
# WalSubscribeRequest -- external WAL follower -> sequencer.
#
# Sent immediately after the TCP connection to the sequencer's external WAL
# subscriber listener is established.
#
# subscriber_id: stable identity (e.g. "mep_primary", "mep_secondary").
# from_seq_no:   cursor. Semantics identical to TopicSubscribeRequest.
# ---------------------------------------------------------------------------
message WalSubscribeRequest (id=105)
    string subscriber_id
    i64    from_seq_no
end

# ---------------------------------------------------------------------------
# WalSubscribeAck -- sequencer -> external WAL follower.
# ---------------------------------------------------------------------------
message WalSubscribeAck (id=106)
    i64 accepted_from_seq_no
end
```

---

## 5. Sequencer Changes (Slice 10)

Slice 10 generalises the sequencer's WAL replication from "one follower (the secondary
sequencer)" to "N followers, each with their own cursor". MEP primary and MEP secondary are
the first two external subscribers.

### 5.1 New inbound listener

The sequencer gains a second inbound PDU listener, separate from the existing peer replication
port (7003/7004) used between the two sequencer instances.

| Port | Instance | Purpose |
|---|---|---|
| 7030 | sequencer primary | external WAL subscriber listener |
| 7031 | sequencer secondary | external WAL subscriber listener |

Both MEP primary and MEP secondary connect to both ports (keeping connections warm for
failover), exactly as the ME and gateway connect to both sequencer instances.

### 5.2 New PDU handlers in `SequencerThread`

- **`WalSubscribeRequest` (105):** Register the subscriber by `subscriber_id`. Record the
  starting cursor (`from_seq_no`). If `from_seq_no` refers to a position before the oldest
  available WAL record, start from the oldest available and reply with the adjusted cursor in
  `WalSubscribeAck`. Begin streaming `WalRecord` (103) PDUs from that position over this
  connection. Track the connection ID → subscriber state in a new map.

- **`WalAck` (104) from external subscriber:** Update that subscriber's confirmed cursor.
  This cursor participates in WAL truncation (see below).

### 5.3 WAL truncation update

Current truncation target: `min(secondary_cursor, snapshot_anchor)`.

New truncation target: `min(secondary_cursor, mep_primary_cursor, mep_secondary_cursor, ..., snapshot_anchor)`.

In general: `min over all known subscriber cursors ∧ snapshot_anchor`. The sequencer tracks
one cursor per registered external subscriber. A subscriber that has not yet sent any `WalAck`
holds the truncation point at its last known cursor (or at its `accepted_from_seq_no` on
first connect).

### 5.4 `SequencerConfiguration` additions

New section `[wal_subscriber]`:

```toml
[wal_subscriber]
listen_host = "${sequencer_primary_wal_subscriber_listen_host}"
listen_port = 7030
```

(The secondary sequencer's TOML gets port 7031.)

---

## 6. Matching Engine Publisher (MEP)

### 6.1 Role

MEP is the topic publisher for the framework's order-flow event streams. It sits between
the sequencer (source of truth) and downstream consumers that do not participate in the
order pipeline. It is the first component in the system to use the framework's topic pub/sub
primitive.

MEP has no business logic. It receives WAL records from the sequencer, writes them to its
own WAL, and fans them out to connected topic subscribers. It does not decode the message
payloads — it routes by `pdu_id`.

**The topic subscriber API is open.** Any process that can reach MEP's listener ports and
speaks the topic protocol may subscribe. There is no pre-registration, no whitelist, and no
configuration change required when a new subscriber type is added. TAP and market data are
the named first consumers, but the API is a library interface: any component can link against
it, connect to MEP, and begin receiving records. `subscriber_id` is a label used by MEP for
logging and per-connection cursor tracking; it is not a registry key and is not validated
against any list of known subscribers.

### 6.2 Input: WAL follower on the sequencer

MEP connects outbound to the sequencer's external WAL subscriber listener (ports 7030/7031).
This is a standard framework outbound PDU connection. On connection established, MEP sends
`WalSubscribeRequest` with its `subscriber_id` and its current WAL write cursor. It then
receives `WalRecord` PDUs and sends `WalAck` PDUs, identical to the existing sequencer
replication protocol.

MEP connects to both sequencer instances at startup and keeps both connections open. Only the
leader streams records; the follower silently holds the connection. On sequencer failover, the
new leader's connection becomes live.

### 6.3 MEP's own WAL

MEP maintains a write-ahead log of every `WalRecord` it receives from the sequencer. The WAL
uses the same format and segment structure as the sequencer's WAL (`SequencerWal`). The shared
implementation will be extracted into a library class (`Wal`) reused by both.

Key properties:
- **Single WAL, all record types.** NOS, OCR, and ER records all go into the same WAL.
  Topic filtering (by `pdu_id`) happens at delivery time, not at write time.
- **Truncation gated on connected subscriber cursors and a minimum retention window.**
  MEP tracks the last acked seq_no for each *currently connected* subscriber. Truncation
  target: `min(connected_subscriber_cursors) ∧ oldest_record_within_retention_window`.
  The retention window (configurable, e.g. 24 hours or N records) provides a floor: MEP
  never truncates records newer than the window even when no subscribers are connected,
  so a subscriber reconnecting after a short outage can always replay from its cursor.
  Because the subscriber API is open, MEP does not retain cursor anchors for disconnected
  subscribers — that would allow an anonymous subscriber that connects once and never
  returns to hold back truncation indefinitely. The subscriber is responsible for
  persisting its own cursor; MEP serves from the oldest available record if the cursor
  has been truncated past, logging a warning.
- **Lag threshold per subscriber.** If a subscriber's cursor falls more than a configurable
  number of records behind MEP's WAL head, MEP disconnects it. The subscriber must
  reconnect and replay from its persisted cursor. The threshold is configured with a default
  and can be overridden per `subscriber_id`.
- **Retention on MEP secondary.** The MEP secondary has no topic subscribers while passive.
  To ensure it can serve subscribers after a failover, it retains its full WAL indefinitely
  (no truncation while passive). The WAL segment files are small relative to disk; this is
  the chosen policy.
- **Recovery on restart.** On restart, MEP reads its WAL to recover its write cursor, then
  reconnects to the sequencer with `from_seq_no = write_cursor`. It resumes streaming from
  the next unwritten record.

### 6.4 Two topics

| Topic name | PDU IDs included | Consumers |
|---|---|---|
| `orders` | 1000 (NOS), 1001 (OCR) | TAP, market data |
| `execution_reports` | 1002 (ER) | TAP (for L3 book), market data |

MEP listens on separate ports per topic (see port table). A subscriber subscribes to exactly
one topic per connection. TAP connects twice (once per topic); market data connects twice.
Any other process may also connect to either port and subscribe — no configuration change
to MEP is required.

### 6.5 Topic delivery protocol

On a topic subscriber connection:
1. Subscriber sends `TopicSubscribeRequest{subscriber_id, topic_name, from_seq_no}`.
2. MEP validates the cursor and sends `TopicSubscribeAck{accepted_from_seq_no}`.
3. MEP starts a delivery cycle: reads records from its WAL matching the topic's pdu_id filter,
   groups them into pages of up to 16, and sends `TopicPage` PDUs.
4. Each `TopicPage` carries `page_number` (X) and `total_pages` (Y). `total_pages` is
   calculated at the start of the delivery cycle from the number of pending records divided
   by page size (ceiling). It does not change mid-cycle even if new records arrive.
5. Subscriber sends `TopicAck{last_seq_no}` after processing each page.
6. If `page_number < total_pages`, MEP sends the next page immediately on receiving the ack.
7. If `page_number == total_pages`, MEP sends the next page when new matching records arrive
   (live delivery). Both fields equal 1 during steady-state live delivery.

Each subscriber has its own independent delivery cycle. A slow subscriber does not block a
fast one. Per-connection isolation is the same discipline used throughout the framework.

### 6.6 High availability — transparent to subscribers

MEP primary and MEP secondary both independently follow the sequencer WAL. Each maintains its
own WAL. Leader election uses the same arbiter-mediated lease+epoch pattern as all other
component pairs.

**Failover is completely transparent to subscriber application code.** The subscriber
application registers callbacks and sees a logical record stream. It never handles connections,
reconnects, or leader discovery. All of that is owned by `TopicSubscriberChannel` (see
section 6.9), the client-side library component that applications link against.

**MEP secondary behaviour on subscriber connection.** The secondary's topic listener ports are
bound and accept connections. On receiving a `TopicSubscribeRequest`, the secondary replies
with a new PDU `TopicNotLeader` (id=111) and closes the connection. `TopicSubscriberChannel`
treats this as a signal to try the other endpoint. This is cleaner than silently closing the
connection because it allows `TopicSubscriberChannel` to fail over immediately rather than
waiting for a connect timeout.

**Failover sequence (invisible to the application):**
1. MEP primary dies. `TopicSubscriberChannel` detects `ConnectionLost`.
2. `TopicSubscriberChannel` tries the primary endpoint — connect fails (no server).
3. `TopicSubscriberChannel` tries the secondary endpoint — connect succeeds.
4. Secondary has been promoted to leader (arbiter-mediated). It accepts the
   `TopicSubscribeRequest` and replies with `TopicSubscribeAck`.
5. Delivery resumes from the subscriber's last acked cursor.
6. Application code observes only a brief pause in record delivery.

Subscriber cursors are maintained by `TopicSubscriberChannel` itself, updated on each
`TopicAck` it sends. Both MEP instances have independent WAL copies from the same sequencer
stream, so the new leader can always serve from the cursor the channel presents.

**New PDU:**

| ID | Name | Direction | Fields |
|---|---|---|---|
| 111 | `TopicNotLeader` | MEP secondary → subscriber | _(no fields)_ |

Added to `topics.dsl`.

### 6.7 Component structure

```
applications/matching_engine_publisher/
├── CMakeLists.txt
├── matching_engine_publisher.toml
├── matching_engine_publisher_secondary.toml
├── MatchingEnginePublisher.hpp
├── MatchingEnginePublisher.cpp
├── MatchingEnginePublisherThread.hpp
├── MatchingEnginePublisherThread.cpp
├── MatchingEnginePublisherConfiguration.hpp
├── MatchingEnginePublisherConfigurationLoader.hpp
└── MatchingEnginePublisherConfigurationLoader.cpp
```

All direct topic subscribers will be low-latency C++ applications. There is therefore no
need for a standalone client library, a C API shim, or any other-language mechanism.
Applications that need further downstream consumption at lower-latency requirements (Java
services, analytics, settlement) subscribe to the enterprise bus via TAP, not to MEP
directly.

`TopicSubscriberChannel`, `TopicSubscriberChannelConfig`, and `TopicSubscriberThread` all
live in the framework library:

```
libraries/pubsub_itc_fw/include/pubsub_itc_fw/
├── TopicSubscriberChannel.hpp       # for framework components (TAP, market data)
├── TopicSubscriberChannelConfig.hpp
└── TopicSubscriberThread.hpp        # pre-built thread for external C++ apps

libraries/pubsub_itc_fw/src/
├── TopicSubscriberChannel.cpp
└── TopicSubscriberThread.cpp
```

Any C++ application that wants to consume a MEP topic links against `pubsub_itc_fw` and
uses either `TopicSubscriberChannel` (if it is already an `ApplicationThread` subclass) or
`TopicSubscriberThread` (if it is an external application that does not subclass
`ApplicationThread`). No separate subscriber library is needed.

**`TopicSubscriberThread`** is a pre-built `ApplicationThread` subclass that owns the full
subscriber boilerplate — connection management, `TopicSubscriberChannel` delegation,
failover — and exposes only a callback interface and a Reactor to the external application:

```cpp
// External C++ application — no subclassing required
TopicSubscriberChannelConfig config;
config.subscriber_id   = "risk_engine";
config.topic_name      = "orders";
config.primary_host    = "mep-primary.internal";
config.primary_port    = 7040;
config.secondary_host  = "mep-secondary.internal";
config.secondary_port  = 7042;
config.from_seq_no     = load_cursor();

QuillLogger logger(...);
ReactorConfiguration reactor_config;  // CPU pinning, pool sizes, etc.

auto thread = ApplicationThread::make<TopicSubscriberThread>(
    config, logger,
    [](const TopicRecordView& record) { /* process record */ },
    [](int64_t seq_no) { save_cursor(seq_no); }
);

Reactor reactor(reactor_config, logger);
reactor.register_thread(thread);
reactor.run();  // put in its own std::thread if the app has its own event loop
```

The external application writes no protocol code and no connection management.
`TopicSubscriberThread` and `TopicSubscriberChannel` handle everything.

`MatchingEnginePublisher` follows the same pattern as `MatchingEngine` and `Sequencer`:
top-level class that wires reactor, registers listeners, creates the thread, and calls
`reactor.run()`.

### 6.8 Thread design (`MatchingEnginePublisherThread`)

Inherits `ApplicationThread`. ThreadID 1.

State:
- `wal_` — MEP's own WAL instance.
- `sequencer_conn_id_`, `sequencer_secondary_conn_id_` — outbound WAL follower connections.
- `role_` — `Role::unknown / leader / follower` (same leader-follower state machine as sequencer).
- `subscribers_` — map from `ConnectionID` to per-subscriber state (subscriber_id, topic_name,
  cursor, current delivery cycle position).
- `orders_conn_ids_`, `er_conn_ids_` — sets of inbound topic subscriber connections per topic.

Key callbacks:
- `on_initial_event()`: open WAL, recover write cursor.
- `on_app_ready_event()`: connect to both sequencer WAL listeners; begin leader-follower
  election (if `ha_enabled`).
- `on_connection_established(id)`: if sequencer connection — send `WalSubscribeRequest`.
  If topic subscriber connection — await `TopicSubscribeRequest`.
- `on_framework_pdu_message(msg)`:
  - `WalSubscribeAck` (106): log accepted cursor.
  - `WalRecord` (103) from sequencer: append to own WAL; fan out to matching topic subscribers.
  - `WalAck` (104): not expected inbound (MEP sends acks, not receives them from sequencer).
  - `TopicSubscribeRequest` (107): register subscriber, send `TopicSubscribeAck`, start delivery cycle.
  - `TopicAck` (110): advance subscriber cursor, consider WAL truncation, send next page if
    `page_number < total_pages`.
  - Leader-follower PDUs (100–106): handled by the same state machine as the sequencer.
- `on_connection_lost(id, reason)`: if sequencer — schedule reconnect. If subscriber —
  retain cursor anchor, log.
- `on_timer_event("housekeeping")`: check subscriber lag thresholds; disconnect slow subscribers.

### 6.9 `TopicSubscriberChannel` and `TopicSubscriberThread` — client-side components

`TopicSubscriberChannel` is the client-side half of the topic pub/sub primitive, for use by
framework components (TAP, market data) that are already `ApplicationThread` subclasses.
`TopicSubscriberThread` is a pre-built `ApplicationThread` subclass for external C++
applications that do not want to write their own.

All direct MEP topic subscribers are C++ applications — low-latency is the reason they
connect directly rather than via the enterprise bus. There is therefore no standalone client
library, no C API, and no other-language mechanism. Applications needing downstream
consumption at lower-latency requirements subscribe to the enterprise bus via TAP.

**`TopicSubscriberChannel` API (sketch):**

```cpp
class TopicSubscriberChannel {
  public:
    using RecordCallback = std::function<void(const TopicRecordView&)>;

    TopicSubscriberChannel(const TopicSubscriberChannelConfig& config,
                           QuillLogger& logger);

    void on_record(RecordCallback callback);

    // Wire into the owning ApplicationThread.
    void attach(Reactor& reactor, ApplicationThread& owner);

    // Delegate these from the owning ApplicationThread:
    void handle_connection_established(ConnectionID id);
    void handle_connection_lost(ConnectionID id, const std::string& reason);
    void handle_pdu(const EventMessage& message);

    // Last acked seq_no — application uses this for cursor persistence.
    [[nodiscard]] int64_t cursor() const;
};
```

**`TopicSubscriberChannelConfig` fields:**
- `subscriber_id` — label for logging; not validated against any list.
- `topic_name` — `"orders"` or `"execution_reports"`.
- `primary_host` / `primary_port` — MEP primary topic listener.
- `secondary_host` / `secondary_port` — MEP secondary topic listener.
- `from_seq_no` — initial cursor (0 for cold start; load from cursor file on restart).

**Internal behaviour:**
- Connects to primary first. On `TopicNotLeader` or `ConnectionLost`, tries secondary. On
  secondary failure, retries primary after a backoff interval. Cycle repeats until a leader
  is found.
- Tracks `current_cursor_`, updated on each `TopicAck` sent.
- Invokes `RecordCallback` for each record in each received `TopicPage`, in seq_no order.
- Never exposes connection IDs, PDU types, or failover state to the application.

**Application pattern (TAP example using `TopicSubscriberChannel`):**

```cpp
// In TapThread constructor:
orders_channel_ = std::make_unique<TopicSubscriberChannel>(orders_config, logger_);
orders_channel_->on_record([this](const TopicRecordView& r) { handle_order(r); });

// In TapThread::on_app_ready_event():
orders_channel_->attach(reactor_, *this);

// In TapThread::on_connection_established / on_connection_lost / on_framework_pdu_message:
orders_channel_->handle_connection_established(id);
// etc.
```

**External C++ application using `TopicSubscriberThread` (no subclassing required):**

```cpp
TopicSubscriberChannelConfig config;
config.subscriber_id  = "risk_engine";
config.topic_name     = "orders";
config.primary_host   = "mep-primary.internal";
config.primary_port   = 7040;
config.secondary_host = "mep-secondary.internal";
config.secondary_port = 7042;
config.from_seq_no    = load_cursor();

QuillLogger logger(...);

auto thread = ApplicationThread::make<TopicSubscriberThread>(
    config, logger,
    [](const TopicRecordView& record) { /* process record */  },
    [](int64_t seq_no)               { save_cursor(seq_no);  }
);

ReactorConfiguration reactor_config;
Reactor reactor(reactor_config, logger);
reactor.register_thread(thread);
reactor.run();  // run in its own std::thread if the app has its own event loop
```

### 6.10 Configuration (`MatchingEnginePublisherConfiguration`)

```toml
[network]
listen_host = "${matching_engine_publisher_network_listen_host}"

[sequencer_wal]
host = "${matching_engine_publisher_sequencer_wal_host}"
port = 7030

[sequencer_wal_secondary]
host = "${matching_engine_publisher_sequencer_wal_secondary_host}"
port = 7031

[topics.orders]
listen_port = 7040

[topics.execution_reports]
listen_port = 7041

[ha]
ha_enabled           = ${matching_engine_publisher_ha_enabled}
instance_id          = ${matching_engine_publisher_instance_id}
peer_host            = "${matching_engine_publisher_peer_host}"
peer_port            = 7044
arbiter_primary_host = "${matching_engine_publisher_arbiter_primary_host}"
arbiter_port         = 7100

[wal]
directory = "${matching_engine_publisher_wal_directory}"

[subscriber_lag]
# Per-subscriber lag threshold in number of records. A subscriber whose
# cursor falls this far behind MEP's WAL head is disconnected.
# Configure per subscriber_id if needed; this is the default.
default_max_lag_records = 100000

[wal_retention]
# Minimum retention window. MEP never truncates records newer than this
# window even when no subscribers are connected. Allows a subscriber that
# briefly disconnects to replay from its persisted cursor on reconnect.
# Set to cover the expected maximum reconnect time for your operational
# recovery objectives. Value is a duration string.
min_retention = "24h"

[logging]
applog_level = "info"
syslog_level = "critical"

[reactor]
cpu_pinning_enabled        = true
cpu_pinning_reserve_cpu0   = ${shared_reactor_cpu_pinning_reserve_cpu0}
cpu_registry_lock_file     = "${shared_reactor_cpu_registry_lock_file}"
connect_retry_warning_interval = "15m"

[event_queue_pool]
objects_per_slab = 1024
initial_slabs    = 1

[command_queue_pool]
objects_per_slab = 1024
initial_slabs    = 1
```

The secondary TOML is identical except `instance_id` and `peer_host`/`peer_port` are swapped,
and `topics.orders.listen_port = 7042`, `topics.execution_reports.listen_port = 7043`.

---

## 7. TAP (Trade Activity Publisher)

### 7.1 Role

TAP is a framework subscriber to MEP's two topics. It maintains an L3 order book (all live
orders tracked individually) and publishes order events to an enterprise bus. The enterprise
bus implementation (Kafka, Pulsar, or other) is a compile-time choice behind a `BusPublisher`
abstract interface. For framework validation purposes, a `StubBusPublisher` logs and counts
records without connecting to any external system.

TAP is HA (primary/secondary pair). Cursor persistence across restarts and failovers is
achieved by the subscriber maintaining its last acked cursor in a small local state file (one
file per topic subscription), combined with the MEP WAL that holds records for replay.

### 7.2 L3 book and enterprise bus interaction

TAP subscribes to both MEP topics:

**Orders topic (NOS + OCR):**
- On NOS: add the order to the L3 book; publish the order event to the enterprise bus.
- On OCR: update the L3 book to record a pending cancel; publish the cancel event.
- TAP does not remove an order from the L3 book until it receives confirmation from both:
  (a) the enterprise bus that it has acknowledged the published event; and
  (b) the ER confirming the order is fully terminal (filled or cancelled).
- This mirrors the behaviour of the equivalent component at the work system.

**Execution reports topic (ER):**
- On ER with a terminal status (Filled, Canceled, Rejected): mark the corresponding order
  in the L3 book as terminal. If the enterprise bus has already acked the order event, remove
  it from the book immediately. Otherwise, retain it and remove it on the subsequent bus ack.
- ERs are **not** published to the enterprise bus by TAP.

### 7.3 `BusPublisher` abstraction

```cpp
class BusPublisher {
  public:
    virtual ~BusPublisher() = default;
    virtual void publish_order_event(const pubsub_itc_fw_app::NewOrderSingleView& view,
                                     int64_t seq_no) = 0;
    virtual void publish_cancel_event(const pubsub_itc_fw_app::OrderCancelRequestView& view,
                                      int64_t seq_no) = 0;
};
```

Concrete implementations:
- `StubBusPublisher` — logs the event at Info level and counts it. Immediately calls back
  "ack" so the L3 book can clean up synchronously in test scenarios.
- `KafkaBusPublisher` — compiled when `USE_KAFKA=ON` in CMake. Uses the librdkafka C++ API.
  Async publish; ack delivered via librdkafka delivery callback.
- `PulsarBusPublisher` — compiled when `USE_PULSAR=ON`. Uses the Pulsar C++ client.
  Async publish; ack via producer callback.

Only one implementation is compiled into any given binary. The framework validation build
uses `StubBusPublisher`. Kafka and Pulsar implementations are future work; the interface is
defined now so they can be added without changing `TapThread`.

### 7.4 Cursor persistence

TAP maintains two cursor files (one per topic subscription):
- `tap_cursor_orders.bin` — last `TopicAck.last_seq_no` sent for the orders topic.
- `tap_cursor_execution_reports.bin` — last `TopicAck.last_seq_no` for the ER topic.

On restart, TAP reads these files and presents the cursors in `TopicSubscribeRequest`. If no
cursor file exists (cold start), TAP uses `from_seq_no = 0` (full replay from MEP's oldest
WAL record), so no order events are lost from the enterprise bus.

### 7.5 High availability

TAP primary/secondary pair, arbiter-mediated election. Only the TAP leader subscribes to MEP
and publishes to the enterprise bus. The secondary connects to MEP's topic listeners but
sends `TopicSubscribeRequest` with the cold-start cursor and immediately closes the connection
(it does not consume while passive). On failover, the new leader reconnects with its persisted
cursor and resumes.

### 7.6 Component structure

```
applications/tap/
├── CMakeLists.txt
├── tap.toml
├── tap_secondary.toml
├── Tap.hpp
├── Tap.cpp
├── TapThread.hpp
├── TapThread.cpp
├── TapConfiguration.hpp
├── TapConfigurationLoader.hpp
├── TapConfigurationLoader.cpp
├── BusPublisher.hpp                (abstract interface)
├── StubBusPublisher.hpp
├── StubBusPublisher.cpp
├── KafkaBusPublisher.hpp           (compiled with USE_KAFKA=ON)
├── KafkaBusPublisher.cpp
├── PulsarBusPublisher.hpp          (compiled with USE_PULSAR=ON)
└── PulsarBusPublisher.cpp
```

### 7.7 Configuration (`TapConfiguration`)

```toml
[mep_orders]
host = "${tap_mep_orders_host}"
port = 7040

[mep_execution_reports]
host = "${tap_mep_execution_reports_host}"
port = 7041

[bus]
# "stub", "kafka", or "pulsar" — must match the compiled BusPublisher.
type = "stub"

[cursors]
directory = "${tap_cursors_directory}"

[ha]
ha_enabled           = ${tap_ha_enabled}
instance_id          = ${tap_instance_id}
peer_host            = "${tap_peer_host}"
peer_port            = 7046
arbiter_primary_host = "${tap_arbiter_primary_host}"
arbiter_port         = 7100

[logging]
applog_level = "info"
syslog_level = "critical"

[reactor]
cpu_pinning_enabled        = true
cpu_pinning_reserve_cpu0   = ${shared_reactor_cpu_pinning_reserve_cpu0}
cpu_registry_lock_file     = "${shared_reactor_cpu_registry_lock_file}"
connect_retry_warning_interval = "15m"

[event_queue_pool]
objects_per_slab = 256
initial_slabs    = 1

[command_queue_pool]
objects_per_slab = 256
initial_slabs    = 1
```

---

## 8. Complete Port Table (updated)

| Port | Usage |
|---|---|
| 9879 | FIX client → gateway (RawBytes inbound) |
| 7001 | gateway → sequencer primary (order PDUs) |
| 7002 | gateway → sequencer secondary (order PDUs) |
| 7003 | sequencer peer-to-peer WAL replication (primary listens) |
| 7004 | sequencer peer-to-peer WAL replication (secondary listens) |
| 7010 | sequencer → gateway ER forwarding (gateway inbound) |
| 7020 | sequencer → ME order PDUs (ME inbound) |
| 7021 | ME → sequencer ER listener (sequencer inbound) |
| 7022 | ME → sequencer secondary ER listener |
| 7030 | sequencer primary external WAL subscriber listener (MEP connects here) |
| 7031 | sequencer secondary external WAL subscriber listener |
| 7040 | MEP primary "orders" topic inbound listener |
| 7041 | MEP primary "execution_reports" topic inbound listener |
| 7042 | MEP secondary "orders" topic inbound listener |
| 7043 | MEP secondary "execution_reports" topic inbound listener |
| 7044 | MEP primary leader-follower peer port |
| 7045 | MEP secondary leader-follower peer port |
| 7046 | TAP primary leader-follower peer port |
| 7047 | TAP secondary leader-follower peer port |
| 7070 | gateway → authentication_service primary |
| 7071 | gateway → authentication_service secondary |
| 7100 | arbiter inbound (all components connect here) |

---

## 9. `environments/dev.toml` Additions

New substitution sections needed:

```toml
[matching_engine_publisher_primary]
network_listen_host             = "127.0.0.1"
sequencer_wal_host              = "127.0.0.1"
sequencer_wal_secondary_host    = "127.0.0.1"
ha_enabled                      = true
instance_id                     = 1
peer_host                       = "127.0.0.1"
arbiter_primary_host            = "127.0.0.1"
wal_directory                   = "/var/tmp/pubsub/mep_primary_wal"

[matching_engine_publisher_secondary]
# same fields, instance_id = 2, peer_host points to primary
...

[tap_primary]
mep_orders_host              = "127.0.0.1"
mep_execution_reports_host   = "127.0.0.1"
ha_enabled                   = true
instance_id                  = 1
peer_host                    = "127.0.0.1"
arbiter_primary_host         = "127.0.0.1"
cursors_directory            = "/var/tmp/pubsub/tap_primary_cursors"

[tap_secondary]
# same fields, instance_id = 2
...
```

`devenv.py` and `deploy.py` require matching updates to include MEP and TAP in the startup
order and component lists (MEP starts after the sequencer pair; TAP starts after MEP).

---

## 10. Implementation Order

Dependencies flow in this direction:

```
DSL extension (array<MessageType>[N])
    └── topics.dsl can be written
        └── leader_follower.dsl additions (WalSubscribeRequest/Ack)
            └── Sequencer slice 10 (external WAL subscriber listener)
                └── MEP component (WAL follower + topic publisher)
                    └── TAP component (topic subscriber + BusPublisher)
```

Each step is a separate session. None should begin before the preceding step has been
verified by tests and build.

The `SequencerWal` → shared `Wal` extraction (so MEP can reuse the WAL implementation)
should be done as the first task within the sequencer slice 10 session, before adding the
external subscriber listener.
