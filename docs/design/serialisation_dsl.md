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

Heap allocation is banned from all generated code. The generated headers depend only on
the standard library and `BumpAllocator.hpp`.

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

All integers are signed. There are no unsigned integer types in the DSL — the only unsigned
value in the system is the `0xC0FFEE00` canary in the PDU framing header, which is outside
the DSL.

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
value if present.

### Enums

Enums have an explicit signed underlying type (`i8`, `i16`, `i32`, `i64`, or `char`).
Each entry has an explicit integer value. Generated code includes `to_string()` and
`validate()` constexpr helpers. Enums are fixed-size on the wire and require no arena
allocation. Generated as `enum class` to prevent name collisions across enums in the same
namespace.

### Messages

Each message has a mandatory numeric `id` in its metadata, which maps to `pdu_id` in the
PDU framing header. An optional `version` field may also appear. Fields are listed in
declaration order, which determines wire order.

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
// Wire size of msg.
std::size_t encoded_size(const Foo& msg);

// Encode msg into out_buffer. Returns false if buffer too small.
// encode_arena is scratch only (not part of the wire output).
bool encode(const Foo& msg, uint8_t* out_buffer, std::size_t out_size,
            std::size_t& bytes_written, std::size_t& bytes_needed);

// Fixed-size messages only: no arena, no size check.
// Use on the hot path when buffer size is already known sufficient.
bool encode_fast(const Foo& msg, uint8_t* out_buffer);

// Decode from wire buffer into a FooView.
// arena_bytes_needed is always set (snprintf contract) even when
// the buffer is too small, allowing two-pass sizing.
bool decode(FooView& out, const uint8_t* buffer, std::size_t bytes_available,
            std::size_t& bytes_consumed,
            pubsub_itc_fw::BumpAllocator& decode_arena,
            std::size_t& arena_bytes_needed);

// Skip over a Foo in a wire buffer without decoding it.
bool skip_Foo(const uint8_t*& read_cursor, std::size_t& bytes_remaining);

// Compile-time upper bound on arena bytes needed to decode Foo,
// for a given maximum list element count.
constexpr std::size_t max_decode_arena_bytes_Foo(std::size_t max_elements = 256);
```

### Decode Arena

Decoding variable-length fields (strings, lists of non-primitives) needs memory for the
decoded view objects. This comes from a `BumpAllocator` arena passed to `decode`. The
arena is used for view objects (e.g. `std::string_view` entries in a decoded list) but
**not** for primitive list elements on little-endian hardware — those zero-copy directly
from the wire buffer via `reinterpret_cast`. On big-endian hardware (not the primary
target) the generator emits a byte-swap loop and does use the arena.

`max_decode_arena_bytes_Foo()` gives a compile-time upper bound, letting the application
pre-size the arena without runtime measurement.

### BumpAllocator Two-Pass Pattern for Variable-Length Encode

When the encode buffer is not pre-sized, use the BumpAllocator's measuring mode:

```cpp
// Pass 1: measure required wire size
BumpAllocator measuring(nullptr, 0);
encode(msg, wire_buf, measuring);
std::size_t needed = measuring.bytes_used();

// Pass 2: allocate real storage and encode
auto [slab_id, ptr] = allocator.allocate(needed);
BumpAllocator real(static_cast<uint8_t*>(ptr), needed);
encode(msg, wire_buf, real);
```

For fixed-size messages, use `encode_fast()` directly with a pre-allocated slab chunk —
no measuring pass needed.

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
| `dsl/validator.py` | Semantic validation: unknown types, duplicate IDs, cycles |
| `dsl/generator_cpp.py` | C++17 code emitter |
| `tools/generate_cpp_from_dsl.py` | Command-line entry point |

### Command-Line Interface

```
generate_cpp_from_dsl.py <input.dsl> <output.hpp> [--namespace NS] [--topics]
```

- Input is a `.dsl` file path; output is a `.hpp` **file path** (not a directory).
- `--namespace NS` sets the C++ namespace for the generated code.
- `--topics` enables generation of additional topic-registry glue code.

### CMake Integration

DSL files are discovered automatically via `file(GLOB_RECURSE *.dsl)`. Generated headers
are placed in `build/libraries/pubsub_itc_fw/dsl/` (framework-internal DSL) and
`build/generated_dsl/` (application-level DSL). All codegen targets are marked `ALL` so
they run at the very start of the build, before any C++ compilation begins.

Pylint runs on `python/dsl/` before CMake during every build. Pytest (133 Python roundtrip
tests) runs by default and can be suppressed with `--no-pytest`.

---

## DSL Files in the Project

| File | Namespace | Contents |
|------|-----------|---------|
| `libraries/pubsub_itc_fw/…/*.dsl` | `pubsub_itc_fw` | Framework-internal PDUs: command queue, events, internal protocols |
| `applications/fix_equity_orders.dsl` | `pubsub_itc_fw_app` | `NewOrderSingle` (1000), `OrderCancelRequest` (1001), `ExecutionReport` (1002); prices/quantities as `string`; `TransactTime` as `datetime_ns`; conditionally-required fields as `optional` |
| `applications/authentication.dsl` | `pubsub_itc_fw_app` | SCRAM PDUs 500–503 (`AuthenticationRequest`, `AuthenticationChallenge`, `AuthenticationProof`, `AuthenticationResult`); plus `SetCredentialRequest/Result` (510/511), `RemoveCredentialRequest/Result` (512/513), `RestoreCredentialRequest/Result` (514/515) |
| `applications/leader_follower.dsl` | `pubsub_itc_fw_app` | Leader-follower protocol PDUs: `StatusQuery` (100), `StatusResponse` (101), `Heartbeat` (102), `ArbitrationReport` (200), `ArbitrationDecision` (201); also `WalSubscribeRequest` (105), `WalSubscribeAck` (106) |
| `applications/topics.dsl` | `pubsub_itc_fw_app` | Topic pub/sub protocol: `TopicSubscribeRequest` (107), `TopicSubscribeAck` (108), `TopicPage` (109), `TopicAck` (110), inner type `TopicRecord` |

---

## Benchmarks

Measured on the primary development machine. Encode/decode times per message in nanoseconds
(encode / decode):

| Message | Encode | Decode |
|---------|--------|--------|
| `SmallMessage` | 17 ns | 15 ns |
| `MediumMessage` | 40 ns | 56 ns |
| `LargeMessage` | 51 ns | 44 ns |

---

## Test Status

- 133 Python roundtrip tests passing (pytest)
- Coverage: 90 %
- Pylint: 10/10

---

## See Also

- [Allocators](allocators.md) — `BumpAllocator` used as encode/decode scratch arena
- [Socket Comms](socket_comms.md) — how encoded PDU payloads move through `PduFramer` and `PduParser`
- [WAL and HA](wal_and_ha.md) — DSL-defined `WalRecord` and `WalAck` used on the replication channel
