"""The selection spec: which messages and fields become PDUs, read from TOML.

The spec is the *human* part of DD-driven PDU generation. The FIX DD supplies field
types and enumerated values; the spec curates the subset that matters and, where a DD
``description`` is prose rather than a clean token (common with exchange custom tags),
supplies explicit enum member names.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List

try:  # Python 3.11+
    import tomllib as _toml
except ModuleNotFoundError:  # Python 3.8-3.10 (the RHEL8 / dev interpreter)
    import tomli as _toml


class SpecError(Exception):
    """Raised when the selection spec is malformed."""


@dataclass(frozen=True)
class ExtraField:
    """A PDU field that is not in the FIX DD (an internal pipeline field).

    ``name`` is already the DSL (snake_case) field name; ``type`` is a DSL type.
    """

    name: str
    type: str
    optional: bool = True
    comment: str = ""


@dataclass
class MessageSpec:
    """One message to emit: its PDU id and its curated FIX field lists."""

    name: str  # FIX message name, e.g. "NewOrderSingle"
    pdu_id: int
    required: List[str] = field(default_factory=list)  # FIX field names, in order
    optional: List[str] = field(default_factory=list)  # FIX field names, in order
    extra: List[ExtraField] = field(default_factory=list)  # trailing internal fields


@dataclass
class Spec:
    """A whole selection spec."""

    output: str
    data_dictionary: List[str]
    enums: List[str]  # FIX field names to emit as DSL enums
    messages: List[MessageSpec]
    # FIX field name -> DSL type, overriding the default DD-type mapping for that field.
    type_overrides: Dict[str, str] = field(default_factory=dict)
    # field name -> { enum-value : explicit DSL member name }, for prose descriptions.
    enum_name_overrides: Dict[str, Dict[str, str]] = field(default_factory=dict)


def _extra_from_toml(raw: dict) -> ExtraField:
    if "name" not in raw or "type" not in raw:
        raise SpecError(f"extra field requires 'name' and 'type': {raw!r}")
    return ExtraField(
        name=str(raw["name"]),
        type=str(raw["type"]),
        optional=bool(raw.get("optional", True)),
        comment=str(raw.get("comment", "")),
    )


def _message_from_toml(raw: dict) -> MessageSpec:
    if "name" not in raw or "pdu_id" not in raw:
        raise SpecError(f"message requires 'name' and 'pdu_id': {raw!r}")
    return MessageSpec(
        name=str(raw["name"]),
        pdu_id=int(raw["pdu_id"]),
        required=[str(name) for name in raw.get("required", [])],
        optional=[str(name) for name in raw.get("optional", [])],
        extra=[_extra_from_toml(item) for item in raw.get("extra", [])],
    )


def load_spec(path: Path) -> Spec:
    """Load and validate a selection spec from a TOML file."""
    with open(path, "rb") as handle:
        raw = _toml.load(handle)

    for key in ("output", "data_dictionary", "messages"):
        if key not in raw:
            raise SpecError(f"spec {path} is missing required key '{key}'")

    overrides = {
        str(field_name): {str(value): str(member) for value, member in mapping.items()}
        for field_name, mapping in raw.get("enum_name_overrides", {}).items()
    }

    return Spec(
        output=str(raw["output"]),
        data_dictionary=[str(item) for item in raw["data_dictionary"]],
        enums=[str(item) for item in raw.get("enums", [])],
        messages=[_message_from_toml(item) for item in raw["messages"]],
        type_overrides={str(name): str(dsl_type) for name, dsl_type in raw.get("type_overrides", {}).items()},
        enum_name_overrides=overrides,
    )
