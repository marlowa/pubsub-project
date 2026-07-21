"""Parse QuickFIX-style FIX XML data dictionaries into a merged model."""

from __future__ import annotations

from pathlib import Path
from xml.etree import ElementTree
from typing import Iterable, List

from .model import Dictionary, FieldDef, FieldValue, MemberRef, MessageDef


class DictionaryError(Exception):
    """Raised when a dictionary file is malformed or conflicts on merge."""


def parse_dictionaries(paths: Iterable[Path]) -> Dictionary:
    """Parse and merge every dictionary in ``paths`` into one :class:`Dictionary`.

    Later files add to earlier ones. A tag number (or message type) defined more
    than once must agree on its name; otherwise a :class:`DictionaryError` is
    raised. Enumerated values from every occurrence of a field are unioned.
    """
    merged = Dictionary()
    for path in paths:
        _merge_file(merged, path)
    return merged


def _merge_file(merged: Dictionary, path: Path) -> None:
    """Parse a single dictionary file and merge it into ``merged``."""
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError) as error:
        raise DictionaryError(f"{path}: {error}") from error

    for field_def in _parse_fields(path, root):
        _merge_field(merged, path, field_def)
    for message in _parse_messages(root):
        _merge_message(merged, path, message)
    _merge_section(merged.header_members, _parse_member_list(root.find("header")))
    _merge_section(merged.trailer_members, _parse_member_list(root.find("trailer")))
    _merge_components(merged, root)
    messages_container = root.find("messages")
    if messages_container is not None:
        for message_element in messages_container.findall("message"):
            _register_nested_groups(merged, message_element)


def _parse_member_list(element) -> List[MemberRef]:
    """Return the field/component/group references directly under ``element``.

    ``element`` may be ``None`` (a section absent from this dictionary), in which
    case the result is empty. Only direct children are read; a group's own body is
    followed later, when the group is expanded, via the components map.
    """
    members: List[MemberRef] = []
    if element is None:
        return members
    for child in element:
        if child.tag not in ("field", "component", "group"):
            continue
        name = child.get("name")
        if name is None:
            continue
        members.append(MemberRef(kind=child.tag, name=name, required=child.get("required") == "Y"))
    return members


def _merge_section(existing: List[MemberRef], parsed: List[MemberRef]) -> None:
    """Append newly seen member references from ``parsed`` into ``existing``.

    The header and trailer are defined once (in FIXT11) but a later file may carry
    an empty section; appending only unseen references keeps the merge idempotent.
    """
    seen = set(existing)
    for member in parsed:
        if member not in seen:
            existing.append(member)
            seen.add(member)


def _merge_components(merged: Dictionary, root) -> None:
    """Merge every ``<component>`` definition under ``<components>`` into ``merged``.

    A component defines both a group counter and a body; its member list is the
    references directly beneath it (including any nested ``<group>`` whose own body
    is itself registered as a component-like entry keyed by the group name).
    """
    container = root.find("components")
    if container is None:
        return
    for component in container.findall("component"):
        name = component.get("name")
        if name is None:
            continue
        merged.components.setdefault(name, _parse_member_list(component))
        _register_nested_groups(merged, component)


def _register_nested_groups(merged: Dictionary, element) -> None:
    """Register each ``<group>`` body under its group name so it can be expanded.

    A group is keyed like a component: its name (the NUMINGROUP counter field name)
    maps to the references in its body. Groups nest inside messages, components, and
    other groups, so recurse. Only the permitted-tag traversal follows these; the
    required-tag traversal stops at a group's counter and never descends.
    """
    for group in element.findall("group"):
        name = group.get("name")
        if name is not None:
            merged.components.setdefault(name, _parse_member_list(group))
        _register_nested_groups(merged, group)


def _parse_fields(path: Path, root: ElementTree.Element) -> List[FieldDef]:
    """Extract every ``<field>`` under the ``<fields>`` section."""
    fields: List[FieldDef] = []
    container = root.find("fields")
    if container is None:
        return fields
    for element in container.findall("field"):
        number_text = element.get("number")
        name = element.get("name")
        if number_text is None or name is None:
            raise DictionaryError(f"{path}: <field> missing number or name")
        try:
            number = int(number_text)
        except ValueError as error:
            raise DictionaryError(f"{path}: field '{name}' has non-integer number '{number_text}'") from error
        values = [
            FieldValue(enum=value.get("enum", ""), description=value.get("description", ""))
            for value in element.findall("value")
        ]
        fields.append(FieldDef(number=number, name=name, type=element.get("type", "STRING"), values=values))
    return fields


def _parse_messages(root: ElementTree.Element) -> List[MessageDef]:
    """Extract every ``<message>`` under the ``<messages>`` section."""
    messages: List[MessageDef] = []
    container = root.find("messages")
    if container is None:
        return messages
    for element in container.findall("message"):
        name = element.get("name")
        msgtype = element.get("msgtype")
        if name is None or msgtype is None:
            continue
        messages.append(MessageDef(name=name, msgtype=msgtype, category=element.get("msgcat", ""),
                                   members=_parse_member_list(element)))
    return messages


def _merge_field(merged: Dictionary, path: Path, field_def: FieldDef) -> None:
    """Insert ``field_def`` into ``merged``, unioning values on a repeat tag."""
    existing = merged.fields.get(field_def.number)
    if existing is None:
        merged.fields[field_def.number] = field_def
        return
    if existing.name != field_def.name:
        raise DictionaryError(
            f"{path}: tag {field_def.number} is '{field_def.name}' but was already defined as '{existing.name}'"
        )
    known = {value.enum for value in existing.values}
    existing.values.extend(value for value in field_def.values if value.enum not in known)


def _merge_message(merged: Dictionary, path: Path, message: MessageDef) -> None:
    """Insert ``message`` into ``merged``, rejecting a conflicting name."""
    existing = merged.messages.get(message.msgtype)
    if existing is None:
        merged.messages[message.msgtype] = message
        return
    if existing.name != message.name:
        raise DictionaryError(
            f"{path}: msgtype '{message.msgtype}' is '{message.name}' but was already defined as '{existing.name}'"
        )
