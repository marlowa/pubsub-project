\page dsl_grammar DSL Grammar Specification

# DSL Grammar Specification

This document defines the schema language used to describe all binary message payloads in the system. The DSL is the single source of truth for message structure, field types, enums, nested messages, lists, optional fields, and metadata. A Python-based code generator consumes DSL files and produces self‑contained C++17 headers containing encode/decode logic, validation, and type definitions.

The DSL is designed to be explicit, deterministic, allocator‑friendly, and suitable for both fixed‑size control‑plane messages and complex variable‑length application‑level PDUs.

---

## Language Overview

The DSL supports:

- Messages with named fields, IDs, and optional version metadata.
- Enums with explicit signed integer underlying types.
- Primitive types (`i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`).
- UTF‑8 strings (`string`) with length-prefix encoding.
- Lists (`list<T>`) of primitives, strings, enums, or nested messages.
- Nested messages (referencing another message type inline).
- Optional fields (`optional T name`) encoded as a presence flag plus value.
- Fixed-size arrays of primitive types (`i8[32]`).
- Comments using `#`.

All integers are signed. The only unsigned value in the entire system is the magic constant in the framing header, which is not part of the DSL.

---

## Wire Format Summary

The DSL describes payloads only. Packets on the wire begin with a fixed framing header:


