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
    (BeginString, MsgType, Checksum, and so on); ``components`` maps a component
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

    def _flatten_body(self, members: List[MemberRef], visited: Set[str]) -> List[Tuple[str, str, bool]]:
        """Flatten a member list into ordered ``(kind, name, required)`` entries.

        Components are inlined (their members spliced in place, recursively);
        fields and groups are kept as-is. ``kind`` in the result is only ever
        ``"field"`` or ``"group"``. ``visited`` guards against a component that
        references itself directly or transitively.
        """
        out: List[Tuple[str, str, bool]] = []
        for member in members:
            if member.kind == "field":
                out.append(("field", member.name, member.required))
            elif member.kind == "group":
                out.append(("group", member.name, member.required))
            elif member.kind == "component":
                if member.name in visited:
                    continue
                out.extend(self._flatten_body(self.components.get(member.name, []), visited | {member.name}))
        return out

    def repeating_groups(self) -> List[Tuple[int, int, List[Tuple[int, bool, int]]]]:
        """Return the global repeating-group catalogue, keyed by counter tag.

        A NUMINGROUP counter tag (for example NoUnderlyings=711) always introduces
        the same group body wherever it appears, so groups are catalogued globally
        rather than per message. Each entry is
        ``(counter_tag, delimiter_tag, [(member_tag, required, nested_counter_tag), ...])``
        where the members are the flattened group body in FIX order, the delimiter
        is the first member's tag (the tag that marks each new instance), and
        ``nested_counter_tag`` is the counter tag of a member that is itself a
        repeating group (0 for a plain field). The result is sorted by counter tag
        for deterministic output. Every group reachable from any message body
        (through components and nested groups) is included.
        """
        groups: Dict[int, Tuple[int, List[Tuple[int, bool, int]]]] = {}

        def discover(members: List[MemberRef]) -> None:
            for kind, name, _required in self._flatten_body(members, set()):
                if kind != "group":
                    continue
                counter_tag = self._tag_for_name(name)
                if counter_tag == 0 or counter_tag in groups:
                    continue
                body = self._flatten_body(self.components.get(name, []), set())
                member_entries: List[Tuple[int, bool, int]] = []
                for member_kind, member_name, member_required in body:
                    member_tag = self._tag_for_name(member_name)
                    nested_counter = member_tag if member_kind == "group" else 0
                    member_entries.append((member_tag, member_required, nested_counter))
                delimiter_tag = member_entries[0][0] if member_entries else 0
                groups[counter_tag] = (delimiter_tag, member_entries)
                # Descend so nested groups are catalogued too.
                discover(self.components.get(name, []))

        for message in self.messages.values():
            discover(message.members)
        return [(counter_tag, groups[counter_tag][0], groups[counter_tag][1]) for counter_tag in sorted(groups)]

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
