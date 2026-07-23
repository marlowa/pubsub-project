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
from typing import Dict, List, Set

from fix_dictionary.model import Dictionary, FieldDef
from fix_dictionary.parser import parse_dictionaries

from dd_to_dsl.spec import ExtraField, MessageSpec, Spec


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
}

# Enumerated fields whose values are integers rather than single characters.
_INT_ENUM_TYPES = {"INT", "SEQNUM", "LENGTH", "NUMINGROUP", "DAYOFMONTH"}

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
        member = _enum_member_name(value.description, value.enum, overrides.get(value.enum, ""), used)
        literal = value.enum if is_int else f"'{value.enum}'"
        lines.append(f"    {member} = {literal}")
    lines.append("}")
    return lines


def _emit_message(message: MessageSpec, by_name: Dict[str, FieldDef], enum_fields: Set[str], type_overrides: Dict[str, str]) -> List[str]:
    lines = [f"message {message.name} (id=PduId.{message.name})"]

    def append_fix_field(fix_name: str, optional: bool) -> None:
        field_def = by_name.get(fix_name)
        if field_def is None:
            raise GeneratorError(f"message '{message.name}' references field '{fix_name}' which is not in the data dictionary")
        lines.append(_field_line(_dsl_type(field_def, enum_fields, type_overrides), _snake_case(fix_name), optional, f"tag {field_def.number} ({fix_name})"))

    for fix_name in message.required:
        append_fix_field(fix_name, optional=False)
    for fix_name in message.optional:
        append_fix_field(fix_name, optional=True)
    for extra in message.extra:
        _append_extra(lines, extra)
    lines.append("end")
    return lines


def _append_extra(lines: List[str], extra: ExtraField) -> None:
    lines.append(_field_line(extra.type, extra.name, extra.optional, extra.comment or "internal field (not from the FIX DD)"))


def generate_dsl(spec: Spec, root: Path) -> str:
    """Return the full ``.dsl`` text for ``spec``, reading its DDs relative to ``root``."""
    dictionary: Dictionary = parse_dictionaries([root / path for path in spec.data_dictionary])
    by_name: Dict[str, FieldDef] = {field_def.name: field_def for field_def in dictionary.fields.values()}
    enum_fields: Set[str] = set(spec.enums)

    out: List[str] = []
    out.append("# ---------------------------------------------------------------------------")
    out.append("# GENERATED FILE -- do not edit by hand.")
    out.append("# Produced by python/dd_to_dsl from a FIX data dictionary and a selection spec.")
    out.append("# Field types and enum values come from the FIX DD; the field selection and")
    out.append("# PDU ids come from the spec. Regenerate after changing either.")
    out.append("# ---------------------------------------------------------------------------")
    out.append("")

    out.append("enum PduId : i16 {")
    for message in spec.messages:
        out.append(f"    {message.name} = {message.pdu_id}")
    out.append("}")
    out.append("")

    for fix_name in spec.enums:
        field_def = by_name.get(fix_name)
        if field_def is None:
            raise GeneratorError(f"enum field '{fix_name}' is not in the data dictionary")
        out.extend(_emit_enum(field_def, spec.enum_name_overrides.get(fix_name, {})))
        out.append("")

    for message in spec.messages:
        out.extend(_emit_message(message, by_name, enum_fields, spec.type_overrides))
        out.append("")

    return "\n".join(out).rstrip("\n") + "\n"
