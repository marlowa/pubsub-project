"""Emit serialisation-DSL text from a parsed FIX dictionary and a selection spec.

The DD gives every field its type and (for enumerated fields) its values; the spec
curates which messages and fields become PDUs. Field *types* and enum *values* are the
DD's authoritative contribution. Enum member *names* are only cosmetic -- they are
derived from the DD ``description`` but sanitised (a description may be English prose,
especially for exchange custom tags), with a value-based fallback and a per-value spec
override, so a messy description can never produce an invalid identifier.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, List, NamedTuple, Set

from fix_dictionary.model import Dictionary, FieldDef
from fix_dictionary.parser import parse_dictionaries

from dd_to_dsl.spec import Spec


class GeneratorError(Exception):
    """Raised when the spec references something the DD does not define."""


# FIX field type -> DSL type. QTY/PRICE/AMT stay strings to avoid decimal-point
# representation issues on the wire; timestamps become datetime_ns; plain integers
# become i32. Anything not listed (STRING, CHAR, MULTIPLE*, CURRENCY, EXCHANGE, DATA,
# COUNTRY, ...) falls through to string.
_TYPE_MAP = {
    "QTY": "string",
    "PRICE": "string",
    "AMT": "string",
    "FLOAT": "string",
    "PRICEOFFSET": "string",
    "PERCENTAGE": "string",
    "UTCTIMESTAMP": "datetime_ns",
    "INT": "i32",
    "SEQNUM": "i32",
    "LENGTH": "i32",
    "NUMINGROUP": "i32",
    "DAYOFMONTH": "i32",
    "BOOLEAN": "bool",
    # A non-enumerated CHAR field maps to the DSL `char` type, not string, so it keeps
    # the FIX int-vs-char distinction: the generated to_string dump renders a `char` as a
    # character and an `i32` (INT) as a number. (A CHAR field WITH a <value> set becomes a
    # char enum instead, which dumps as the member name.) MULTIPLECHARVALUE stays a string
    # -- it may hold several space-separated values.
    "CHAR": "char",
}

# An enum represents an integer (or char, which fits in an integer) with a bounded set of
# choices, so a value-bearing field of one of these types becomes a DSL enum. The DSL enum
# base is chosen to match the FIX type: an INT field -> `enum : i32` (displays as a number),
# a CHAR field -> `enum : char` (displays as a character, never its integer code). Types
# outside these stay their base type even when the DD lists <value> entries: STRING /
# MULTIPLESTRINGVALUE hold multi-character values (SymbolSfx 'CD'/'WI'); MULTIPLECHARVALUE
# (e.g. ExecInst) can hold several space-separated values, so one enum cannot represent it;
# BOOLEAN stays bool; NUMINGROUP is a group counter (becomes a list<>), never a scalar enum.
_INT_ENUM_TYPES = {"INT", "SEQNUM", "LENGTH", "DAYOFMONTH"}
_CHAR_ENUM_TYPES = {"CHAR"}

# First numeric id assigned to a generated repeating-group body message. Group bodies are
# never sent as standalone PDUs, so their ids only need to be unique within the file and
# clear of the real PDU ids (in the PduId enum); this base sits comfortably above those.
_GROUP_ID_BASE = 2000

# Cap on how many words of a (possibly long, prose) DD description become an enum
# member name. Prose beyond this is dropped; use a spec override for a clean name.
_MAX_ENUM_NAME_WORDS = 6


def _snake_case(name: str) -> str:
    """Convert a FIX field name (e.g. ClOrdID, SecurityIDSource) to snake_case."""
    step1 = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    step2 = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", step1)
    return step2.lower()


def _sanitize_identifier(text: str, max_words: int = 0) -> str:
    """Turn arbitrary text into a PascalCase identifier fragment (may be empty).

    Non-alphanumeric runs -- spaces, punctuation, parentheses, slashes -- are dropped.
    ``max_words`` (when > 0) caps the number of words kept, so a long prose description
    cannot produce a monstrous identifier; the rest is discarded (use a spec override
    when a specific name is wanted).
    """
    words = re.findall(r"[A-Za-z0-9]+", text)
    if max_words > 0:
        words = words[:max_words]
    return "".join(word.capitalize() for word in words)


def _enum_member_name(description: str, value: str, override: str, used: Set[str]) -> str:
    """Pick a unique, valid DSL enum member name for one enumerated value.

    Preference: an explicit spec override, else a (length-capped) sanitised description.
    If neither yields a valid, letter-led identifier -- e.g. the description is pure
    prose/punctuation -- fall back to a value-based name. Collisions within the enum are
    broken with a numeric suffix.
    """
    base = override or _sanitize_identifier(description, _MAX_ENUM_NAME_WORDS)
    if not base or not base[0].isalpha():
        base = "Value" + _sanitize_identifier(value)
    if not base or not base[0].isalpha():
        base = "Value"
    candidate = base
    counter = 1
    while candidate in used:
        counter += 1
        candidate = f"{base}_{counter}"
    used.add(candidate)
    return candidate


def _dsl_type(field_def: FieldDef, enum_fields: Set[str], type_overrides: Dict[str, str]) -> str:
    """The DSL type for a field: enum name if enumerated here, else a spec override, else the type map."""
    if field_def.name in enum_fields:
        return field_def.name
    if field_def.name in type_overrides:
        return type_overrides[field_def.name]
    return _TYPE_MAP.get(field_def.type, "string")


def _field_line(dsl_type: str, dsl_name: str, optional: bool, comment: str) -> str:
    prefix = "optional " if optional else ""
    line = f"    {prefix}{dsl_type} {dsl_name}"
    if comment:
        line += f"  # {comment}"
    return line


def _emit_enum(field_def: FieldDef, overrides: Dict[str, str]) -> List[str]:
    if not field_def.values:
        raise GeneratorError(f"field '{field_def.name}' is listed as an enum but the DD gives it no values")
    is_int = field_def.type in _INT_ENUM_TYPES
    base = "i32" if is_int else "char"
    lines = [f"# {field_def.name} (tag {field_def.number})", f"enum {field_def.name} : {base} {{"]
    used: Set[str] = set()
    for value in field_def.values:
        # The FIX type guarantees the value fits the enum base; enforce it so a malformed
        # DD (a char field with a multi-char value, or an int field with a non-integer)
        # fails loudly here instead of emitting an invalid DSL literal.
        if is_int and not _is_int_literal(value.enum):
            raise GeneratorError(f"field '{field_def.name}' is {field_def.type} but value '{value.enum}' is not an integer")
        if not is_int and len(value.enum) != 1:
            raise GeneratorError(f"field '{field_def.name}' is {field_def.type} but value '{value.enum}' is not a single character")
        member = _enum_member_name(value.description, value.enum, overrides.get(value.enum, ""), used)
        literal = value.enum if is_int else f"'{value.enum}'"
        lines.append(f"    {member} = {literal}")
    lines.append("}")
    return lines


def _is_int_literal(text: str) -> bool:
    """True if `text` is a plain integer literal (an int-enum value)."""
    try:
        int(text)
        return True
    except ValueError:
        return False


def _is_enum_field(field_def: FieldDef) -> bool:
    """True if the field should become a DSL enum: it carries <value> entries and its FIX
    type is an int-enum type (-> `enum : i32`) or a char-enum type (-> `enum : char`).
    Decided by the DD type; _emit_enum picks the matching base and enforces the value fit."""
    return bool(field_def.values) and field_def.type in (_INT_ENUM_TYPES | _CHAR_ENUM_TYPES)


def _group_message_name(counter_name: str) -> str:
    """PascalCase name for a repeating-group body message, from its NUMINGROUP counter
    field name. The counter name is already a valid identifier (e.g. NoUnderlyings), so
    just drop a leading 'No' -- NoUnderlyings -> Underlyings, NoPartySubIDs -> PartySubIDs
    -- preserving its internal casing. Falls back to the counter name if stripping the
    prefix would leave nothing."""
    if counter_name.startswith("No") and len(counter_name) > 2 and counter_name[2].isupper():
        return counter_name[2:]
    return counter_name


def _unique_name(base: str, used: Set[str]) -> str:
    """Return `base`, or `base_2`, `base_3`, ... until it is not already in `used`; records it."""
    candidate = base
    counter = 1
    while candidate in used:
        counter += 1
        candidate = f"{base}_{counter}"
    used.add(candidate)
    return candidate


class _GroupInfo(NamedTuple):
    """A generated repeating-group body message: its DSL message name, numeric id, the
    NUMINGROUP counter field, and the DD member list of the group body."""

    message_name: str
    message_id: int
    counter: FieldDef
    body: list


# generate_dsl is a cohesive multi-pass assembler (resolve messages, collect enums,
# catalogue groups, emit) whose passes share the parsed dictionary and lookups; keeping
# them together as local closures is clearer than scattering that state across module
# helpers, so the local/statement counts are expected.
def generate_dsl(spec: Spec, root: Path) -> str:  # pylint: disable=too-many-locals,too-many-statements
    """Return the ``.dsl`` text for ``spec`` by expanding each selected FIX message in full.

    The whole DD message body is emitted -- every field in DD order, with the DD's
    required/optional flags and types -- with components inlined and repeating groups
    turned into a nested body message plus a ``list<>`` field on the parent. Enums are
    auto-derived (any field carrying DD <value> entries, except BOOLEAN). The only
    non-DD inputs are the PDU ids (from the spec) and optional type/enum-name overrides.
    """
    dictionary: Dictionary = parse_dictionaries([root / path for path in spec.data_dictionary])
    by_name: Dict[str, FieldDef] = {field_def.name: field_def for field_def in dictionary.fields.values()}
    by_msg_name = {message_def.name: message_def for message_def in dictionary.messages.values()}

    # Resolve each selected message name to its DD definition.
    selected = []
    for message_spec in spec.messages:
        message_def = by_msg_name.get(message_spec.name)
        if message_def is None:
            raise GeneratorError(f"message '{message_spec.name}' is not defined in the data dictionary")
        selected.append((message_spec, message_def))

    # ---- Pass 1: collect the enum fields used anywhere in the selected bodies/groups.
    # Enums share the DSL type namespace with messages, so their names must be known
    # before repeating-group body messages are named (Pass 2), to avoid a collision such
    # as a NoMatchInst group ('MatchInst') clashing with a 'MatchInst' enum. ----
    enum_fields: Set[str] = set()

    def collect_enums(members: list, visited_components: Set[str]) -> None:
        for member in members:
            if member.kind == "field":
                field_def = by_name.get(member.name)
                if field_def is not None and _is_enum_field(field_def):
                    enum_fields.add(field_def.name)
            elif member.kind == "component":
                if member.name in visited_components:
                    continue
                collect_enums(dictionary.components.get(member.name, []), visited_components | {member.name})
            elif member.kind == "group":
                collect_enums(dictionary.components.get(member.name, []), visited_components)

    for _, message_def in selected:
        collect_enums(message_def.members, set())

    # ---- Pass 2: catalogue repeating groups (post-order: a nested group is registered,
    # and thus emitted, before the group that contains it -- the DSL needs a message
    # defined before it is referenced by list<>). Group body message names are made unique
    # against the top-level message names, the enum names, and PduId (one type namespace). ----
    group_registry: Dict[str, _GroupInfo] = {}
    used_message_names: Set[str] = {"PduId"} | {message_spec.name for message_spec in spec.messages} | set(enum_fields)
    next_group_id = _GROUP_ID_BASE

    def register_groups(members: list, visited_components: Set[str], visiting_groups: Set[str]) -> None:
        nonlocal next_group_id
        for member in members:
            if member.kind == "component":
                if member.name in visited_components:
                    continue
                register_groups(dictionary.components.get(member.name, []), visited_components | {member.name}, visiting_groups)
            elif member.kind == "group":
                if member.name in group_registry or member.name in visiting_groups:
                    continue
                body = dictionary.components.get(member.name, [])
                register_groups(body, visited_components, visiting_groups | {member.name})  # nested groups first
                counter = by_name.get(member.name)
                if counter is None:
                    raise GeneratorError(f"repeating group '{member.name}' has no counter field in the data dictionary")
                message_name = _unique_name(_group_message_name(member.name), used_message_names)
                group_registry[member.name] = _GroupInfo(message_name, next_group_id, counter, body)
                next_group_id += 1

    for _, message_def in selected:
        register_groups(message_def.members, set(), set())

    # ---- Emit a message body's members (shared by top-level messages and group bodies). ----
    def emit_members(members: list, visited_components: Set[str]) -> List[str]:
        lines: List[str] = []
        for member in members:
            if member.kind == "field":
                field_def = by_name.get(member.name)
                if field_def is None:
                    raise GeneratorError(f"field '{member.name}' referenced by a message is not in the data dictionary")
                dsl_type = _dsl_type(field_def, enum_fields, spec.type_overrides)
                lines.append(_field_line(dsl_type, _snake_case(field_def.name), not member.required, f"tag {field_def.number} ({field_def.name})"))
            elif member.kind == "component":
                if member.name in visited_components:
                    continue
                lines.extend(emit_members(dictionary.components.get(member.name, []), visited_components | {member.name}))
            elif member.kind == "group":
                info = group_registry[member.name]
                # A repeating group is a list of its body message; an empty list means no
                # instances, so the list is never marked optional. The NUMINGROUP counter
                # is implicit in the list length.
                lines.append(f"    list<{info.message_name}> {_snake_case(member.name)}  # tag {info.counter.number} ({member.name}) repeating group")
        return lines

    # ---- Assemble the file. ----
    out: List[str] = []
    out.append("# ---------------------------------------------------------------------------")
    out.append("# GENERATED FILE -- do not edit by hand.")
    out.append("# Produced by python/dd_to_dsl: each message is expanded in full from the FIX")
    out.append("# data dictionary (all fields, DD order, DD required/optional, components")
    out.append("# inlined, repeating groups as nested messages + list<>). Enums are the DD's")
    out.append("# <value> sets; only the PDU ids come from the spec. Regenerate after changes.")
    out.append("# ---------------------------------------------------------------------------")
    out.append("")

    out.append("enum PduId : i16 {")
    for message_spec in spec.messages:
        out.append(f"    {message_spec.name} = {message_spec.pdu_id}")
    out.append("}")
    out.append("")

    for field_name in sorted(enum_fields, key=lambda name: by_name[name].number):
        out.extend(_emit_enum(by_name[field_name], spec.enum_name_overrides.get(field_name, {})))
        out.append("")

    # Group body messages, in registration order (nested groups precede their parents).
    for info in group_registry.values():
        out.append(f"message {info.message_name} (id={info.message_id})  # repeating group '{info.counter.name}' (tag {info.counter.number})")
        out.extend(emit_members(info.body, set()))
        out.append("end")
        out.append("")

    # Top-level PDUs.
    for message_spec, message_def in selected:
        out.append(f"message {message_spec.name} (id=PduId.{message_spec.name})")
        out.extend(emit_members(message_def.members, set()))
        out.append("end")
        out.append("")

    return "\n".join(out).rstrip("\n") + "\n"
