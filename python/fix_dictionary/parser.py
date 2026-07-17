"""Parse QuickFIX-style FIX XML data dictionaries into a merged model."""

from __future__ import annotations

from pathlib import Path
from xml.etree import ElementTree
from typing import Iterable, List

from .model import Dictionary, FieldDef, FieldValue, MessageDef


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
        messages.append(MessageDef(name=name, msgtype=msgtype, category=element.get("msgcat", "")))
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
