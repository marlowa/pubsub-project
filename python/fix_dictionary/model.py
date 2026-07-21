"""In-memory model of a merged FIX data dictionary."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Set, Tuple


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
class MemberRef:
    """A reference to a field, component, or group from a message or component body.

    ``kind`` is one of ``"field"``, ``"component"`` or ``"group"``. ``required`` is
    the ``required='Y'`` flag on the reference (whether the referenced member must
    be present in a conforming message), not a property of the member's definition.
    """

    kind: str
    name: str
    required: bool


@dataclass
class MessageDef:
    """A FIX message definition (``<message name= msgtype= msgcat=>``).

    ``members`` is the ordered list of field/component/group references in the
    message body, carrying their ``required`` flags. It is empty for dictionaries
    that define messages without a body (as some minimal test fixtures do).
    """

    name: str
    msgtype: str
    category: str
    members: List[MemberRef] = field(default_factory=list)


@dataclass
class Dictionary:
    """A merged view of one or more FIX XML dictionaries.

    Fields are keyed by tag number and messages by message-type string, so that
    session-layer definitions (FIXT11) and application definitions (FIX50SP2)
    combine into a single catalogue with duplicates collapsed. ``header_members``
    and ``trailer_members`` hold the session-level fields shared by every message
    (BeginString, MsgType, CheckSum, and so on); ``components`` maps a component
    name to its member list so that a required component in a message can be
    expanded into the concrete tags it makes mandatory.
    """

    fields: Dict[int, FieldDef] = field(default_factory=dict)
    messages: Dict[str, MessageDef] = field(default_factory=dict)
    components: Dict[str, List[MemberRef]] = field(default_factory=dict)
    header_members: List[MemberRef] = field(default_factory=list)
    trailer_members: List[MemberRef] = field(default_factory=list)

    def fields_sorted(self) -> List[FieldDef]:
        """Return fields ordered by tag number for deterministic output."""
        return [self.fields[number] for number in sorted(self.fields)]

    def messages_sorted(self) -> List[MessageDef]:
        """Return messages ordered by message-type string for deterministic output."""
        return [self.messages[msgtype] for msgtype in sorted(self.messages)]

    def _tag_for_name(self, name: str) -> int:
        """Return the tag number for a field name, or 0 if the name is unknown."""
        for field_def in self.fields.values():
            if field_def.name == name:
                return field_def.number
        return 0

    def _expand_required(self, members: List[MemberRef], visited: Set[str], out: Set[int]) -> None:
        """Accumulate the tags that ``members`` make mandatory into ``out``.

        A required field contributes its own tag. A required group contributes its
        counter tag (the NUMINGROUP field named after the group) but not its inner
        members -- a group member cannot be "missing" when the group is absent or
        empty. A required component is expanded recursively; ``visited`` guards
        against a component that references itself directly or transitively.
        """
        for member in members:
            if not member.required:
                continue
            if member.kind in ("field", "group"):
                tag = self._tag_for_name(member.name)
                if tag != 0:
                    out.add(tag)
            elif member.kind == "component":
                if member.name in visited:
                    continue
                visited.add(member.name)
                self._expand_required(self.components.get(member.name, []), visited, out)

    def required_tags(self, msgtype: str) -> List[int]:
        """Return the sorted tags a conforming message of ``msgtype`` must carry.

        The set is the union of the session header's required fields, the message
        body's required fields (with required components expanded), and the
        trailer's required fields. An unknown message type yields an empty list.
        """
        message = self.messages.get(msgtype)
        if message is None:
            return []
        out: Set[int] = set()
        self._expand_required(self.header_members, set(), out)
        self._expand_required(message.members, set(), out)
        self._expand_required(self.trailer_members, set(), out)
        return sorted(out)

    def _collect_permitted(self, members: List[MemberRef], visited: Set[str], out: Set[int]) -> None:
        """Accumulate every tag that may legitimately appear, descending into groups.

        Unlike required-tag expansion, this follows repeating groups into their
        bodies: a group member tag is permitted in the message even though it is not
        required. ``visited`` guards against components or groups that reference one
        another.
        """
        for member in members:
            if member.kind in ("field", "group"):
                tag = self._tag_for_name(member.name)
                if tag != 0:
                    out.add(tag)
            if member.kind in ("component", "group") and member.name not in visited:
                visited.add(member.name)
                self._collect_permitted(self.components.get(member.name, []), visited, out)

    def permitted_tags(self, msgtype: str) -> List[int]:
        """Return the sorted tags a message of ``msgtype`` is allowed to carry.

        This is the union of the session header, the message body (with components
        and repeating groups fully expanded), and the trailer. A tag outside this
        set is not defined for the message type (FIX SessionRejectReason 2). An
        unknown message type yields an empty list.
        """
        if msgtype not in self.messages:
            return []
        out: Set[int] = set()
        visited: Set[str] = set()
        self._collect_permitted(self.header_members, visited, out)
        self._collect_permitted(self.messages[msgtype].members, visited, out)
        self._collect_permitted(self.trailer_members, visited, out)
        return sorted(out)

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
