# FIX Codec (`fix_codec`)

## Overview

`fix_codec` is an **application-tier** FIX codec library. It sits *above* the
`pubsub_itc_fw` framework — never inside it — as the FIX counterpart to
`scram_crypto`. The framework itself stays protocol-agnostic and only mentions
FIX in comments as an example of an "alien" byte-stream protocol; anything that
actually understands FIX belongs at the application level.

The library has two halves that meet at a single generated header:

- A **pure-Python generator** that reads the QuickFIX-style FIX XML data
  dictionaries and emits a C++17 header of tag numbers, message types,
  enumerated values, and lookup tables.
- A **hand-written, header-only, zero-copy runtime** — a reader, a writer, a
  field accessor, and a checksum helper — that parses and builds FIX messages
  without allocating on any path.

It is the first of a likely small family of exchange-protocol codecs; a binary
protocol built on the framework's PDU framing is intended later as a sibling.

---

## Status

Complete and green:

- Generator, dictionary package, and runtime headers are done.
- 21 GoogleTest cases (`libraries/fix_codec/tests/`) and 6 pytest cases
  (`python/tests/test_fix_dictionary.py`) pass.
- `fix_codec_tests` is wired into `build.py`'s C++ test run; the `fix_dictionary`
  Python package is in the pylint gate (10.00/10); `check_standards` and
  clang-format are clean.

**In use inbound, not outbound.** The `fix_order_gateway` frames and validates what it
receives with this library, and takes its `Tag::` / `MsgType::` from the generated
dictionary. What it *sends* is still hand-written by `FixSerialiser` / `FixErEncoder`;
swapping the writer is a later pass, and would also remove the per-message `std::string`
allocation `validate_checksum` used to incur. What was migrated, what was not, and the
knock-on effect on how much of a large message like NewOrderSingle the system exercises,
are in [Order Gateway → Migration to `fix_codec`](../venue/fix_order_gateway.md#gw_fix_codec_migration).

---

## Why not depend on hffix

The design borrows the *ideas* of the [hffix](https://github.com/jamesdbrock/hffix)
library — zero-copy, no heap allocation on any path, and field/tag metadata
generated from the FIX data dictionary — **without depending on it**. hffix is
12+ years old, drives its code generation from a Haskell generator, and pulls in
Boost. None of that fits this project, which wants a small dependency surface,
Python for its tooling, and a pylint-10/10 gate. Notably hffix's own generator
was Python before 2018, so a pure-Python generator is faithful to its history.

The concrete hffix ideas reused here are called out where they appear: the lazy
typed field accessors (`field_value`), the `find_with_hint` search, and the
`push_back_header` / `push_back_trailer` in-place header/trailer construction.

---

## Part 1 — The dictionary generator (Python)

### Pipeline

The generator is a three-stage pipeline, one module per stage, so each concern is
independently testable:

| Stage | Module | Responsibility |
|---|---|---|
| Parse | `python/fix_dictionary/parser.py` | Read each XML file into model objects; merge |
| Model | `python/fix_dictionary/model.py` | The in-memory `Dictionary` and its derived views |
| Emit | `python/fix_dictionary/emitter.py` | Render the merged model as a C++ header string |

The CLI wrapper `python/tools/generate_fix_dictionary.py` wires them together:
`parse_dictionaries(inputs) → emit_header(dictionary, namespace) → write file`.

```
python3 python/tools/generate_fix_dictionary.py \
    libraries/fix_codec/data_dictionary/FIXT11.xml \
    libraries/fix_codec/data_dictionary/FIX50SP2.xml \
    --namespace fix_codec \
    --output <build>/generated_fix/fix_codec/fix_dictionary.hpp
```

### Merging two dictionaries

FIX 5.0 splits its definitions across two files: `FIXT11.xml` (the session /
transport layer — Logon, Heartbeat, and the tags they use) and `FIX50SP2.xml`
(the application messages — NewOrderSingle, ExecutionReport, and so on). The
generator merges them into a single catalogue:

- **Fields are keyed by tag number, messages by message-type string.** A tag or
  message type that appears in both files collapses to one entry.
- **A conflicting redefinition is a hard error.** If tag *N* is one name in one
  file and a different name in the other, `parse_dictionaries` raises
  `DictionaryError` rather than silently picking one. The same applies to a
  message type mapping to two different names.
- **Enumerated values are unioned.** When the same field carries enum values in
  both files, the values are merged and de-duplicated by their `enum` code.

Merge order is "later files add to earlier ones", so passing `FIXT11.xml` first
and `FIX50SP2.xml` second is the intended invocation.

### What the generated header contains

The emitter produces a single `#pragma once` header in the requested namespace
(default `fix_codec`) with a do-not-edit banner. Its sections:

| Section | Form | Purpose |
|---|---|---|
| `namespace tag` | `inline constexpr int` per field | Tag numbers keyed by canonical FIX field name, e.g. `tag::ClOrdID` |
| `namespace msg_type` | `inline constexpr std::string_view` per message | MsgType (tag 35) values keyed by message name; a `string_view` because multi-character msgtypes exist |
| `namespace enum_values` | nested namespace per enumerated field | Field values grouped by field; emitted as `char` when every value is a single character, otherwise `std::string_view` |
| `namespace detail` | two sorted `std::array` tables | `field_name_table` (tag → name) and `data_length_pairs` (length tag → data tag) |
| free functions | `constexpr` | `tag_name(int)`, `data_field_for_length_tag(int)`, `is_data_length_tag(int)` — binary search over the tables |

Every identifier is run through a sanitiser (non-alphanumeric characters become
`_`, a leading digit is prefixed with `_`) and a disambiguator (a name already
taken is suffixed with the tag number / msgtype, then with trailing `_` until
unique), so an arbitrary dictionary string always becomes a valid, unique C++
identifier.

### The DATA / LENGTH pairing (the subtle part)

Most FIX fields are delimited by the SOH (`0x01`) byte. Binary **DATA** fields
are the exception: their bytes can themselves contain SOH, so a parser must read
them by the exact byte count given in the **LENGTH** field that immediately
precedes them, not by scanning for the next SOH.

The generator therefore emits a table pairing each DATA field with its LENGTH
field, and the runtime reader consults it. The pairing is resolved **by name**,
not by tag adjacency: a DATA field `X` pairs with the field named `XLen` or
`XLength`. Numeric adjacency is unreliable — `Signature` is tag 89 but
`SignatureLength` is tag 93. LENGTH fields with no matching DATA field (such as
`BodyLength` and `MaxMessageSize`) are excluded. See
`Dictionary.data_length_pairs()` in `model.py`.

### Determinism

The output is deterministic: fields are emitted in tag order, messages in
msgtype order, and no timestamp is embedded. Regenerating from unchanged inputs
produces a byte-identical header, which keeps the build reproducible and avoids
spurious diffs.

---

## Part 2 — The zero-copy runtime (C++, header-only)

Four headers under `libraries/fix_codec/`, all in namespace `fix_codec`. None
allocates on any path; all conversions go through `std::from_chars` /
`std::to_chars` with no locale and no exceptions.

### `FixField.hpp`

`FixField` is one parsed field: a tag number and a `std::string_view` value that
points **directly into the caller's byte buffer**. It is valid only while that
buffer is alive and unmodified. Typed accessors convert on demand (hffix's
`field_value` idea):

- `as_int` / `as_int64` / `as_uint`, `as_char`, `as_bool` (FIX boolean: `Y` is
  true).
- `as_decimal(mantissa, exponent)` — parses a fixed-point decimal such as
  `-123.45` into `mantissa = -12345, exponent = -2`. **No float is produced**, so
  no precision is lost; scientific notation is rejected. This matters for the
  framework's integer-only price/quantity rule.
- `as_utc_timestamp_ns` — parses a FIX `UTCTimestamp`
  (`YYYYMMDD-HH:MM:SS` with an optional fractional part up to nanoseconds) to
  nanoseconds since the Unix epoch, using Howard Hinnant's days-from-civil
  algorithm rather than `timegm()`, so it is portable and free of global state.

### `FixMessageReader.hpp`

Constructed over a borrowed `(const char*, size_t)` window — deliberately the
same shape `FixParser::feed` receives from a `MirroredBuffer`, so a later gateway
migration is a drop-in. It frames **exactly one** message at the window start and
reports a `Status`:

| Status | Meaning |
|---|---|
| `Valid` | Complete message, checksum correct |
| `Incomplete` | The window does not yet hold the whole message |
| `Malformed` | Framing is wrong (not a FIX message boundary) |
| `ChecksumError` | Complete message, but the checksum is wrong |

Framing is driven by **BodyLength (tag 9)** — the reader reads tag 8, then tag 9,
computes exactly where the `10=` checksum field must start, and validates it
there. It **never scans for the checksum tag**. `message_size()` is non-zero once
the message is fully present (`Valid` *or* `ChecksumError`), so a stream driver
can advance its read position even over a message whose checksum failed.

The fields are exposed as a forward range of `FixField` (`begin()`/`end()`,
`find(tag)`, `find(tag, hint)`). `find(tag, hint)` is hffix's `find_with_hint`:
because FIX fields arrive in a known order, resuming a search from the previous
result avoids rescanning the header. The iterator honours the generated
data-length tags — when it reads a LENGTH tag it reads the following DATA field
by exact byte count, so a DATA value may contain SOH.

Stream framing above one message — skipping leading garbage, resynchronising
after a corrupt message — is the caller's responsibility; the reader assumes the
window starts on a message boundary.

### `FixMessageWriter.hpp`

Builds one outbound message directly into a caller-supplied buffer (for example a
slab chunk) with no intermediate `std::string`. The caller pushes every session
and application field; the writer owns tags 8, 9, and 10.

The mechanism is hffix's `push_back_header` / `push_back_trailer`: body fields
are written first into a region that leaves a reserved prefix in front. `finish()`
then writes the BeginString (tag 8) and BodyLength (tag 9) **backward into that
prefix** so the header ends exactly where the body begins, appends the Checksum
(tag 10), and returns a `string_view` over the whole wire message — computing
both length and checksum in place. The default BeginString is `FIXT.1.1` (the
required FIX 5.0 SP2 preamble). If any write would exceed the buffer capacity the
writer records an overflow and `finish()` returns an empty view.

### `FixChecksum.hpp`

`compute_checksum` sums every byte modulo 256 over the range from tag 8 up to
(not including) the `10=` field. `checksum_matches` parses the three received
digits numerically with `from_chars` and compares — no formatting, no allocation.

---

## Usage

The generated header and the runtime are always used together: you address fields
by their generated names and let the reader/writer do the wire work. The examples
below are drawn from the library's own tests (`libraries/fix_codec/tests/`).

The core idea is that **you never write a magic tag number or a bare message-type
string.** `tag::OrderQty` and `msg_type::NewOrderSingle` resolve, at compile time,
to whatever the FIX dictionary defines.

### Writing a message

`FixMessageWriter` builds into a caller buffer (e.g. a slab chunk). It owns tags 8,
9 and 10; you push everything else. `push_back_field` is overloaded for
`string_view`, `int`, and `char`, so nothing is formatted by hand:

```cpp
using namespace fix_codec;

char buffer[512];
FixMessageWriter writer(buffer, sizeof(buffer));            // default BeginString "FIXT.1.1"

writer.push_back_field(tag::MsgType,  msg_type::NewOrderSingle);  // 35=D
writer.push_back_field(tag::ClOrdID,  std::string_view("A-1"));   // 11=A-1
writer.push_back_field(tag::OrderQty, 250);                       // 38=250  (int overload)
writer.push_back_field(tag::Side,     enum_values::Side::SELL);   // 54=2    (char overload)

const std::string_view wire = writer.finish();   // writes 8/9 header + 10 checksum
if (writer.overflowed()) { /* buffer too small; finish() returned {} */ }
```

### Reading a message

`FixMessageReader` frames one message by BodyLength and validates its checksum.
`find(tag::X)` returns a `FixField` whose value is a `string_view` **into the
original buffer** — no copy — with `as_*` accessors that convert on demand:

```cpp
FixMessageReader reader(wire.data(), wire.size());

if (reader.is_valid()) {                                  // framed + checksum OK
    reader.msg_type();                                    // "D"
    const int  qty  = reader.find(tag::OrderQty).as_int();  // 250
    const char side = reader.find(tag::Side).as_char();     // '2'
    const std::string_view id = reader.find(tag::ClOrdID).as_string_view();  // "A-1"

    if (reader.find(tag::Price).empty()) { /* optional field absent */ }
}
```

`status()` distinguishes `Valid` / `Incomplete` / `Malformed` / `ChecksumError`
so a stream driver knows whether to wait for more bytes, resynchronise, or advance.

Because FIX fields are ordered, `find(tag, hint)` resumes a scan from a previous
result rather than re-walking the header (hffix's `find_with_hint`):

```cpp
auto it = reader.begin();
const FixField sender = reader.find(tag::SenderCompID, it);  // scans from the front
const FixField target = reader.find(tag::TargetCompID, it);  // resumes after the previous hit
```

### Binary DATA fields (the generated pairing at work)

A `RawData` value may itself contain the SOH delimiter, so it must be read by the
count in the preceding `RawDataLength` field. The reader knows the pairing because
the generator emitted it — there is no hand-maintained list:

```cpp
writer.push_back_field(tag::RawDataLength, static_cast<int>(raw.size()));  // 95=<n>
writer.push_back_field(tag::RawData,       std::string_view(raw));         // 96=<n bytes, may contain SOH>
// ...
const FixField field = reader.find(tag::RawData);
field.as_string_view() == raw;   // read by exact byte count, not by scanning for SOH
```

Internally the reader's iterator consults the generated `is_data_length_tag(95)` /
`data_field_for_length_tag(95)` to decide this.

### What the gateway migration did

The [FIX order gateway migration](../venue/fix_order_gateway.md#gw_fix_codec_migration)
is precisely this reader replacing the hand-written parser. Populating the order
PDU from an inbound NewOrderSingle is:

```cpp
FixMessageReader reader(window.data(), window.size());
if (reader.msg_type() == msg_type::NewOrderSingle) {
    nos_pdu.symbol    = reader.find(tag::Symbol).as_string_view();
    nos_pdu.side      = reader.find(tag::Side).as_char();
    nos_pdu.order_qty = reader.find(tag::OrderQty).as_string_view();
    // a further optional (MinQty, ExpireTime, ...) is one more find() — no table edit
}
```

That last line is the payoff: every FIX 5.0 SP2 tag is already in the generated
dictionary, so widening field coverage costs a `find()`, not a table entry.

---

## The no-allocation, zero-copy invariant

This is the whole point of the library, and it holds end to end:

- The reader never copies message bytes; every `FixField::value` is a view into
  the caller's buffer.
- The writer never builds an intermediate string; it writes into the caller's
  buffer and returns a view of it.
- All numeric conversion is `std::from_chars` / `std::to_chars` — no locale, no
  `std::stringstream`, no exceptions.
- The generated dictionary is entirely `constexpr` — tables and lookups resolve
  at compile time.

The lifetime rule that falls out of this: **a `FixField` (or any reader view) is
only valid while the underlying buffer is alive and unmodified.** A consumer that
needs the bytes beyond that window must copy them itself.

---

## Build integration

The generated header is produced into the build tree, never committed:

- `CMakeLists.txt` (top level) defines `FIX_DICT_INCLUDE_DIR =
  <build>/generated_fix` and generates
  `generated_fix/fix_codec/fix_dictionary.hpp` via an `add_custom_command`. The
  command re-runs when either the XML dictionaries **or** the generator's Python
  modules change (the module list is globbed into the command's `DEPENDS`).
- The `fix_dictionary_generated` target is `ALL` and **depends on
  `check_standards`**, so the coding-standards gate runs before generation and a
  violation fails the build early.
- `libraries/fix_codec/CMakeLists.txt` defines `fix_codec` as an `INTERFACE`
  (header-only) library whose include directories cover both the hand-written
  headers (`libraries/`, so they include as `<fix_codec/...>`) and the generated
  header (`FIX_DICT_INCLUDE_DIR`). `fix_codec_tests` links it and
  `add_dependencies(fix_codec_tests fix_dictionary_generated)` guarantees the
  generated header exists before the tests compile.

Consumers therefore write `#include <fix_codec/FixMessageReader.hpp>` and
`#include <fix_codec/fix_dictionary.hpp>` uniformly, unaware that one is
hand-written and the other generated.

## Regenerating after a dictionary change

The generated header is a build artefact, so there is normally nothing to do — a
normal build regenerates it when the XML or the generator changes. To regenerate
by hand (for inspection), run the CLI shown in the **Pipeline** section above. To
change *what* is generated, edit the emitter and add a pytest case in
`python/tests/test_fix_dictionary.py`; the pylint gate must stay at 10.00/10.

---

## Testing

- **C++** (`libraries/fix_codec/tests/`): `FixChecksumTest`, `FixFieldTest`,
  `FixMessageReaderTest`, `FixMessageWriterTest` — 21 cases covering framing
  statuses, the data-length SOH-in-DATA case, decimal/timestamp parsing, and the
  writer's backward-header construction and overflow handling.
- **Python** (`python/tests/test_fix_dictionary.py`): 6 cases covering the merge
  rules (conflict detection, enum union), the DATA/LENGTH-by-name pairing, and
  identifier sanitisation/disambiguation.

---

## See also

- [Serialisation DSL](../framework/serialisation_dsl.md) — the framework's own binary codec;
  `fix_codec` is the FIX-specific application-tier counterpart, not a replacement.
- [Secure Communications](../operations/secure_comms.md) — `scram_crypto`, the sibling
  application-tier library.
- [Order Gateway](../venue/fix_order_gateway.md) — the consumer that will be
  migrated onto this library.
