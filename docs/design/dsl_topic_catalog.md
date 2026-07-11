# DSL topic catalog + include mechanism — design & plan {#dsl_topic_catalog}

**Status:** Decided 2026-07-11. **Step 1 (rename) DONE** (commit `d0ecfca`).
**Step 2 (include loader) DONE** (uncommitted at time of writing). Resume at
**step 3 (`topic` construct)** in the Implementation Plan below.

## Context — why this exists

The MEP first cut (committed `9ff45c6`, "Added first version of MEP") represents topics
as **free, unvalidated strings**, with two hardwired topics (`orders`, `execution_reports`)
on two ports. Per the requirements-first stance in `pubsub_requirements.md`, that is a
"draft to challenge."

The work system settles the requirement: it keeps an **ASCII file listing all recognised
topic names**. So topics are a *recognised catalog*, not free-form. To avoid the catalog
drifting from the PDU ids, the catalog is **generated from the DSL** (the schema already
owns the pdu ids).

## Decisions

1. **Recognised topic catalog**, not an open free-string API. (Resolves requirements
   dimensions 1 "granularity" and 6 "open vs registered" toward *registered*.)
2. **Identity vs policy split:**
   - *Identity* (topic name + member messages → pdu_ids): declared in the **DSL**,
     **generated** into `topics_registry.hpp` (+ a human-readable catalog). Single source
     of truth; cannot drift from the pdu ids.
   - *Policy* (per-topic retention window, lag threshold, page size, listener port):
     hand-written **TOML**, validated at load against the generated registry (a policy
     entry for an unknown topic is a hard error).
   - **File-type rule:** generated artifacts are `.hpp`/`.md` (banner'd); **TOML is always
     hand-written config**. So "is this file generated?" is answered by its extension.
3. **Rename the existing DSL `Topics` enum → `PduId`.** It is a per-message id registry
   (`NewOrderSingle = 1000`, …), a misnomer. Renaming frees the word `topic` for the
   grouping construct. The `--topics` generator flag (which really means "emit the id
   enum") is renamed to match.
4. **New `topic` grouping construct:** `topic orders { NewOrderSingle, OrderCancelRequest }`
   — a named stream listing member messages. A message may belong to **multiple** topics.
   Topic names are unique.
5. **DSL `include` mechanism:** transitive (any file may include any file, nesting allowed),
   at declaration-level position, with **dedup** (by canonical path) and **cycle detection**.
   A loader merges the transitive closure into one AST; the existing validator/generator run
   unchanged.
6. **Generated files carry a DO-NOT-EDIT banner** naming the generator, the source `.dsl`,
   and a timestamp.

## Key facts (verified 2026-07-11 — the grounding for the plan)

- DSL tooling lives in `python/dsl/{lexer,parser,ast,validator,generator_cpp,generator_java,
  generator_pybind11}.py`; CLI wrapper `python/tools/generate_cpp_from_dsl.py` takes a
  **single** input `.dsl` (`--cpp`/`--java`/`--namespace`/`--topics`).
- **The lexer has no string-literal token** (only char literals, `'A'`). `include "path"`
  needs a new `STRING` token.
- The parser is single-file recursive descent; `parse()` loops top-level declarations
  (`enum`/`framing`/`message`) to EOF.
- **The validator is already whole-program:** `_collect_declarations` gathers *all*
  declarations first, then validates references; it checks **global duplicate names and
  duplicate pdu_ids** and has message-reference cycle detection. => merging included files
  into one `DslFile` makes every global check work **unchanged** — includes are a *loader*
  concern, not a validator rewrite.
- Existing `Topics` enum is in `applications/fix_equity_orders.dsl`; `--topics` mode requires
  every message `id` to be `Topics.X` and emits the enum via `_emit_topics_enum_class`.
  `applications/topics.dsl` (the wire protocol) uses bare ids (107–111), not the enum.

## Implementation plan (landable steps)

1. **[DONE — `d0ecfca`] Rename `Topics` → `PduId`.** `fix_equity_orders.dsl` (enum name + every `id=Topics.X`
   → `id=PduId.X`), the validator's `Topics` special-casing + error text (`_validate_topics`,
   the `ref.enum_name == "Topics"` check), `generator_cpp._emit_topics_enum_class` + the
   `decl.name == "Topics"` guard, and rename the `--topics` flag (+ the CMake invocation).
   Keep the pytest round-trip suite green.
2. **[DONE] Include loader.** Lexer `STRING` token (`_lex_string_lit`) + `include` keyword;
   `IncludeDecl(path, line)` AST node; parser `_parse_include` at declaration level; new
   `python/dsl/loader.py` (`Loader` / `load()`) resolving includes transitively (paths relative
   to the *including* file's dir; dedup by `Path.resolve()` real path; cycle detection via a
   "currently loading" DFS stack + a "fully loaded" set, erroring with the `a -> b -> a` chain);
   merges into one `DslFile`; validator/generator unchanged. Both CLI wrappers
   (`generate_cpp_from_dsl.py`, `generate_java_from_dsl.py`) now call `load()` and catch the
   `DslError` base. `python/tests/test_include.py` covers: STRING token, unterminated string,
   include-decl parse, empty path, no-include passthrough, single/nested/diamond include,
   relative-path resolution, missing include, missing root, self-cycle, mutual cycle, cycle
   chain text, duplicate-id-across-includes (validator), valid-across-includes. 220 pytest pass.
3. **`topic` construct.** Lexer `topic` keyword; `TopicDecl(name, members)` AST node; parse
   `topic NAME { Msg, ... }`; validate (members are known messages; names unique; multi-topic
   allowed); generator emits `topics_registry.hpp` (a `Topic` enum + the `pdu_id → topic`
   map) and `topics_catalog.md`, both banner'd. pytest for each.
4. **Wire-up.** Author an index `pubsub.dsl` (includes + `topic` blocks); make the generated
   output depend on **all** transitively-included `.dsl` files in CMake (depfile or explicit
   list); regenerate; point MEP at the generated registry instead of free strings, and have
   MEP validate a subscribe's `topic_name` against the registry.

## Deferred / later

- **Per-topic policy TOML** (retention/lag/ports) — part of the later MEP-config step; it
  validates against the registry from step 3.
- **Reconcile the existing MEP first cut** (free-string topics, two ports) against the
  catalog: decide port-per-topic vs multiplex-several-topics-by-name on one connection.
