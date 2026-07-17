"""In-memory model of a merged FIX data dictionary."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Tuple


@dataclass(frozen=True)
class FieldValue:
    """A single enumerated value of a FIX field (``<value enum= description=>``)."""

    enum: str
    description: str


@dataclass
class FieldDef:
    """A FIX field definition (``<field number= name= type=>``).

    ``values`` is empty for non-enumerated fields.
    """

    number: int
    name: str
    type: str
    values: List[FieldValue] = field(default_factory=list)


@dataclass(frozen=True)
class MessageDef:
    """A FIX message definition (``<message name= msgtype= msgcat=>``)."""

    name: str
    msgtype: str
    category: str


@dataclass
class Dictionary:
    """A merged view of one or more FIX XML dictionaries.

    Fields are keyed by tag number and messages by message-type string, so that
    session-layer definitions (FIXT11) and application definitions (FIX50SP2)
    combine into a single catalogue with duplicates collapsed.
    """

    fields: Dict[int, FieldDef] = field(default_factory=dict)
    messages: Dict[str, MessageDef] = field(default_factory=dict)

    def fields_sorted(self) -> List[FieldDef]:
        """Return fields ordered by tag number for deterministic output."""
        return [self.fields[number] for number in sorted(self.fields)]

    def messages_sorted(self) -> List[MessageDef]:
        """Return messages ordered by message-type string for deterministic output."""
        return [self.messages[msgtype] for msgtype in sorted(self.messages)]

    def data_length_pairs(self) -> List[Tuple[int, int]]:
        """Return ``(length_tag, data_tag)`` pairs, sorted by length tag.

        A DATA field may itself contain the SOH delimiter, so a FIX parser must
        read it by the byte count given in the preceding LENGTH field rather
        than scanning for the next SOH. The pairing is resolved by name -- a
        DATA field ``X`` pairs with the LENGTH field named ``XLen`` or
        ``XLength`` -- because numeric adjacency is not reliable (for example
        Signature is tag 89 but SignatureLength is tag 93). LENGTH fields with
        no matching DATA field (such as BodyLength and MaxMessageSize) are
        excluded.
        """
        by_name = {field_def.name: field_def for field_def in self.fields.values()}
        pairs: List[Tuple[int, int]] = []
        for field_def in self.fields.values():
            if field_def.type != "DATA":
                continue
            for suffix in ("Len", "Length"):
                length_field = by_name.get(field_def.name + suffix)
                if length_field is not None and length_field.type == "LENGTH":
                    pairs.append((length_field.number, field_def.number))
                    break
        pairs.sort()
        return pairs
