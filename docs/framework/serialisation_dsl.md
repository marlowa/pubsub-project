# Serialisation DSL

## Design Goals

The framework uses a purpose-built schema description language to define all binary message
payloads. DSL files are the single source of truth for message structure. A Python code
generator reads them and emits self-contained C++17 headers.

The design competes with SBE (Simple Binary Encoding), prioritising:
- Zero-copy decode on little-endian hardware
- Deterministic wire sizes
- No external dependencies in the generated code
- Allocator-friendly decoding via a `BumpAllocator` arena
- Sub-100 ns encode/decode on the hot path

Heap allocation is banned from the generated encode and decode code. The one generated
function that allocates is `to_string()` on a decoded view, which builds a `std::string` for
logging and diagnostics and is not for the hot path. The generated headers depend only on the
standard library and `BumpAllocator.hpp`.

The same DSL files also generate Java classes, used by the Java components, and the topic
registry described below.

---

## DSL Language

### Primitive Types

| DSL type | C++ type | Wire size |
|----------|----------|-----------|
| `i8` | `int8_t` | 1 byte |
| `char` | `char` | 1 byte — for FIX protocol char fields; distinct from `i8` |
| `i16` | `int16_t` | 2 bytes, little-endian |
| `i32` | `int32_t` | 4 bytes, little-endian |
| `i64` | `int64_t` | 8 bytes, little-endian |
| `bool` | `bool` | 1 byte (0 or 1) |
| `datetime_ns` | `int64_t` | 8 bytes, little-endian nanoseconds since Unix epoch |
| `string` | `std::string_view` | 4-byte byte-count + UTF-8 bytes |
| `bytes` | `BytesView` | 4-byte byte-count + raw bytes; zero-copy decode |

All integers are signed. There are no unsigned integer types in the DSL. The one unsigned
value on the wire is the `0xC0FFEE00` canary in the PDU framing header, which is part of the
framing rather than of any message; a `framing` block (below) can declare it as a constant for
the generated Java code.

`char` is used for FIX protocol single-character fields (e.g. `OrdStatus`, `Side`). It
accepts character literals (`'A'`, `'1'`) in enum entry values and generates `char` in C++.
`i8` maps to `int8_t` and is for numeric byte values.

`bytes` is for binary content. The decode-side C++ type is `BytesView` — a non-owning
`{data, size}` pair that points directly into the wire buffer (zero-copy). Use `bytes`
instead of `string` when the content is binary rather than UTF-8 text (e.g. SCRAM nonces,
`StoredKey`, `ServerKey`).

### Compound Types

**`list<T>`** — variable-length sequence. Encoded as a 4-byte element count followed by
the encoded elements. `T` may be any DSL type including another `list` or a message
reference. On little-endian hardware, `list<primitive>` decode is zero-copy
(`reinterpret_cast` directly into the wire buffer); no arena allocation is used.

**`T[N]`** — fixed-length array. No length prefix. Encoded as N consecutive elements.

**`optional T`** — presence flag (1 byte: 0=absent, 1=present) followed by the encoded
value if present. A message whose bytes end exactly where an optional field would begin
decodes with that field absent. That is what lets an optional field be added at the end of
a message while encodings made before it existed are still read, as the records already in a
write-ahead log are.

### Enums

Enums have an explicit signed underlying type (`i8`, `i16`, `i32`, `i64`, or `char`).
Each entry has an explicit integer value. Generated code includes `to_string()` and
`validate()` constexpr helpers. Enums are fixed-size on the wire and require no arena
allocation. Generated as `enum class` to prevent name collisions across enums in the same
namespace.

### Messages

Each message has a mandatory numeric `id` in its metadata, which maps to `pdu_id` in the
PDU framing header. An optional `version` field may also appear. Fields are listed in
declaration order, which determines wire order. A message with `id=0` is an inner type: it is
only ever carried inside another message, and is never sent as a PDU of its own (for example
`TopicRecord` inside `TopicPage`).

### Other Declarations

- **`include "file.dsl"`** brings in the declarations of another DSL file. The loader replaces
  the directive with those declarations before validation. `--include-dir` adds directories to
  search, which is how a source DSL file includes one generated into the build tree.
- **`framing { name = value ... }`** declares wire-level constants such as the PDU header size
  and canary. The Java generator emits them as constants for the Java side's framing code.
- **`topic name { Message, ... }`** declares a pub/sub topic and the messages that belong to it.
  It is read only when the topic registry is generated (see below). A message may belong to
  more than one topic.
- **The pdu-id enum.** With `--pdu-id-enum`, the file must contain an enum named `PduId`, and
  every message's id must be written as a member of it (`id=PduId.NewOrderSingle`) rather than
  as a number. `fix_orders.dsl` is generated this way.

### Example DSL

```
enum OrderSide : i8 {
    Buy  = 1
    Sell = 2
}

message NewOrder(id=10, version=1)
    OrderSide    side
    i64          quantity
    i64          price_tenths
    string       symbol
    optional i32 client_order_id
end
```

---

## Generated C++ API

For each message `Foo`, the generator produces two structs and a set of free functions in
a single `.hpp` header.

### Owning and View Structs

| Struct | Purpose | Field types |
|--------|---------|-------------|
| `Foo` | Encode-side (owning) | Value types: `int32_t`, `std::string_view`, `ListView<T>`, etc. Populated by the application before encoding. |
| `FooView` | Decode-side (view) | Non-owning views into the wire buffer or arena. Populated by the decode function. |

### Generated Functions

```cpp
// Wire size of message.
std::size_t encoded_size(const Foo& message);

// Encode message into out_buffer. bytes_needed is always set, even when out_buffer is too
// small, in which case nothing is written and false is returned.
bool encode(const Foo& message, uint8_t* out_buffer, std::size_t out_size,
            std::size_t& bytes_written, std::size_t& bytes_needed);

// Fixed-size messages only: no size check. For the hot path, when the buffer is already
// known to be large enough.
void encode_fast(const Foo& message, uint8_t* out_buffer);
std::size_t fixed_encoded_size(const Foo& message);   // fixed-size messages only

// Decode one Foo starting at read_cursor, advancing read_cursor and reducing
// bytes_remaining by the bytes consumed. arena_bytes_needed is always set (the snprintf
// contract), even when decode_arena is too small, allowing two-pass sizing.
bool decode_Foo(FooView& out, const uint8_t*& read_cursor, std::size_t& bytes_remaining,
                pubsub_itc_fw::BumpAllocator& decode_arena, std::size_t& arena_bytes_needed);

// Skip over a Foo in a wire buffer without decoding it.
bool skip_Foo(const uint8_t*& read_cursor, std::size_t& bytes_remaining);

// Upper bounds on the arena bytes needed to decode, or to build for encoding, a Foo whose
// lists have at most max_elements_per_list elements each.
constexpr std::size_t max_decode_arena_bytes_Foo(std::size_t max_elements_per_list = 256);
constexpr std::size_t max_encode_arena_bytes_Foo(std::size_t max_elements_per_list = 256);

// The first field of a decoded message whose value the definition does not allow -- an
// enumerated field holding a value the enum does not have, or an empty required string --
// or an empty string_view if there is none. The decoder itself only checks that each
// field's bytes are present; a component taking messages from outside calls this to refuse
// a message the definition does not allow.
std::string_view first_invalid_field(const FooView& message);

// A readable rendering of a decoded message, for logs and diagnostics. Allocates.
std::string to_string(const FooView& view);
```

Enums get the same `encoded_size`, `encode`, `decode_` and `skip_` functions, as well as
`to_string()` and `validate()`.

An optional field appears in both structs as the field itself plus a `bool has_<name>` flag.

### Decode Arena

Decoding variable-length fields (strings, lists of non-primitives) needs memory for the
decoded view objects. This comes from a `BumpAllocator` arena passed to `decode`. The
arena is used for view objects (e.g. `std::string_view` entries in a decoded list) but
**not** for primitive list elements on little-endian hardware — those zero-copy directly
from the wire buffer via `reinterpret_cast`. On big-endian hardware (not the primary
target) the generator emits a byte-swap loop and does use the arena.

`max_decode_arena_bytes_Foo()` gives a compile-time upper bound, letting the application
pre-size the arena without runtime measurement.

### Encode Arena and Sizing the Output

`encode()` writes directly into the caller's buffer and needs no arena of its own. A caller
that has to build `ListView` arrays to fill in a message before encoding it can take that
scratch space from a `BumpAllocator`, sized with `max_encode_arena_bytes_Foo()`.

To size the output buffer, call `encoded_size(message)`, or call `encode()` once with a buffer
that may be too small: it sets `bytes_needed` either way. For fixed-size messages,
`fixed_encoded_size()` is known in advance and `encode_fast()` can write straight into a
pre-allocated slab chunk.

---

## Wire Format

All multi-byte integers are little-endian. The DSL describes payload bytes only; the PDU
framing header (magic, length, message ID) precedes every payload on the wire but is not
part of the DSL.

| Type | Wire encoding |
|------|---------------|
| `i8`, `char`, `bool` | 1 byte |
| `i16` | 2 bytes LE |
| `i32`, string byte-count, list element-count | 4 bytes LE |
| `i64`, `datetime_ns` | 8 bytes LE |
| `string` | 4-byte byte-count + UTF-8 bytes |
| `bytes` | 4-byte byte-count + raw bytes |
| `list<T>` | 4-byte element-count + N encoded elements |
| `T[N]` | N consecutive elements, no length prefix |
| `optional T` | 1-byte flag (0=absent, 1=present) + encoded T if present |

---

## Code Generator

### Python Package Structure

Lives under `python/`:

| Module | Role |
|--------|------|
| `dsl/lexer.py` | Tokeniser |
| `dsl/parser.py` | Recursive-descent parser producing an AST |
| `dsl/ast.py` | AST node dataclasses |
| `dsl/loader.py` | Reads a DSL file and resolves its `include` directives |
| `dsl/validator.py` | Semantic validation: unknown types, duplicate IDs, cycles, the pdu-id enum rules |
| `dsl/errors.py` | The error type every stage raises |
| `dsl/generator_cpp.py` | C++17 code emitter |
| `dsl/generator_java.py` | Java code emitter |
| `dsl/generator_topics.py` | Topic registry and topic catalog emitter |
| `dsl/generator_pybind11.py` | Python bindings over the generated C++, used by the round-trip tests |
| `tools/generate_cpp_from_dsl.py` | Command-line entry point |

`fix_orders.dsl` is not a source file. It is generated at build time from the FIX data
dictionary `applications/fix_orders.dd.xml` by `tools/generate_dd_to_dsl.py` (the
`dd_to_dsl` package); see [PDU generation](../fix/pdu_generation.md).

### Command-Line Interface

```
generate_cpp_from_dsl.py <input.dsl>
    [--cpp OUTPUT.hpp --namespace NS [--pdu-id-enum]]
    [--java OUTPUT.java [--package PKG]]
    [--topics-registry OUTPUT.hpp --namespace NS] [--topics-catalog OUTPUT.md]
    [--include-dir DIR ...]
```

- At least one output is required. Each output is a **file path**, not a directory.
- `--cpp` writes the C++ header and requires `--namespace`. `--pdu-id-enum` turns on the
  pdu-id enum rules described above.
- `--java` writes a Java source file, in package `--package` if given.
- `--topics-registry` writes a C++ header with a `Topic` enum and the table of which pdu ids
  belong to which topic; `--topics-catalog` writes a readable Markdown catalog of the same.
- `--include-dir` adds a directory to search when resolving `include` (may be repeated).

### CMake Integration

There are two kinds of rule.

- `libraries/pubsub_itc_fw/CMakeLists.txt` finds every `.dsl` file under the framework library
  with `file(GLOB_RECURSE ...)` and generates each into
  `build/libraries/pubsub_itc_fw/dsl/`, in namespace `pubsub_itc_fw`.
- The top-level `CMakeLists.txt` has an explicit rule for each application protocol, writing
  into `build/generated_dsl/` in namespace `pubsub_itc_fw_app`. It also generates
  `fix_orders.dsl` from the data dictionary first, and the topic registry and catalog from
  `applications/pubsub.dsl`.

Every generation target is marked `ALL`, and each rule depends on the generator's own source as
well as on the `.dsl` file, so a change to the generator regenerates the headers.

`build.py` runs pylint on `python/dsl` and `python/fix_dictionary` (failing on errors) before
the build, and runs the Python test suite in `python/` after it. `--no-pytest` skips the
Python tests only. The round-trip tests compile each generated header into a Python extension
with pybind11, so they need pybind11 available to CMake.

---

## DSL Files in the Project

| File | Namespace | Contents |
|------|-----------|---------|
| `libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl` | `pubsub_itc_fw_app` (and `pubsub_itc_fw` through the framework glob) | The messages between instances of a pair and their voters: `StatusQuery` (100), `StatusResponse` (101), `LeaseRequest` (130), `LeaseGrant` (131), `LeaseRefusal` (132), `ArbiterStateRecord` (400); the log's `WalRecord` (103), `WalAck` (104), `WalSubscribeRequest` (105), `WalSubscribeAck` (106), `LogPositionRequest` (107) and `LogPositionReply` (108); the matching engine's catch-up (`MePositionRequest` 115, `MePositionAck` 116), `RoleAnnouncement` (117) and `EnginePositionQuery`/`EnginePosition` (118/119); the session messages (120-126), `OrderAcceptance` (127) and `UndeliveredReportsRequest` (128) |
| `fix_orders.dsl`, generated in `build/generated_dsl/` | `pubsub_itc_fw_app` | `NewOrderSingle` (1000), `OrderCancelRequest` (1001), `ExecutionReport` (1002), `OrderCancelReject` (1003), with ids from the `PduId` enum |
| `applications/authentication.dsl` | `pubsub_itc_fw_app` | SCRAM PDUs 500–503 (`AuthenticationRequest`, `AuthenticationChallenge`, `AuthenticationProof`, `AuthenticationResult`); plus `SetCredentialRequest/Result` (510/511), `RemoveCredentialRequest/Result` (512/513), `RestoreCredentialRequest/Result` (514/515) |
| `applications/binary_session.dsl` | `pubsub_itc_fw_app` | The binary gateway's session messages: `Logon` (700), `LogonAck` (701) |
| `applications/matching_engine/matching_engine_replication.dsl` | `pubsub_itc_fw_app` | `BookUpdate` (600), from the matching engine primary to its secondary |
| `applications/topics.dsl` | `pubsub_itc_fw_app` | Topic pub/sub protocol: `TopicSubscribeRequest` (107), `TopicSubscribeAck` (108), `TopicPage` (109), `TopicAck` (110), `TopicNotLeader` (111), `TopicLagged` (112), inner type `TopicRecord` |
| `applications/pubsub.dsl` | — | The topic catalog: includes `fix_orders.dsl` and declares the topics `orders` and `execution_reports`. Generates the topic registry and catalog, not message code |
| `libraries/pubsub_itc_fw/integration_tests/variable_length_test_protocol.dsl`, `libraries/pubsub_itc_fw/performance/DslBenchProtocol.dsl` | `pubsub_itc_fw` | Test and benchmark messages only |

Pdu ids are not unique across files: 107 and 108 are both `LogPositionRequest`/`LogPositionReply`
in `leader_follower.dsl` and `TopicSubscribeRequest`/`TopicSubscribeAck` in `topics.dsl`. A
receiver interprets a pdu id according to the protocol it expects on that connection; the
sequencer handles the first pair and the topic publisher and subscriber classes the second.

---

## Benchmarks

Measured on the primary development machine with `DslBenchProtocol.dsl`. Encode/decode times
per message in nanoseconds:

| Message | Encode | Decode |
|---------|--------|--------|
| `SmallMessage` | 17 ns | 15 ns |
| `MediumMessage` | 40 ns | 56 ns |
| `LargeMessage` | 51 ns | 44 ns |

---

## See Also

- [Allocators](allocators.md) — `BumpAllocator` used as encode/decode scratch arena
- [Socket Comms](socket_comms.md) — how encoded PDU payloads move through `PduFramer` and `PduParser`
- [WAL and HA](../availability/wal_and_ha.md) — DSL-defined `WalRecord` and `WalAck` used on the replication channel
