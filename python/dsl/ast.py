"""AST node definitions for the pubsub_itc_fw DSL code generator."""

from __future__ import annotations
from dataclasses import dataclass
from typing import List, Dict


@dataclass
class DslFile:
    """Top-level container holding all declarations in a DSL source file."""

    declarations: List[Declaration]


class Declaration:  # pylint: disable=too-few-public-methods
    """Base class for all top-level DSL declarations (enums and messages)."""


@dataclass
class EnumEntry:
    """A single name/value pair within an enum declaration."""

    name: str
    value: int
    line: int = 0


@dataclass
class EnumDecl(Declaration):
    """An enum declaration with an underlying integer type and a list of entries."""

    name: str
    underlying_type: str  # "i8", "i16", "i32", "i64"
    entries: List[EnumEntry]
    line: int = 0


@dataclass
class MessageDecl(Declaration):
    """A message declaration with metadata (e.g. id, version) and a list of fields."""

    name: str
    metadata: Dict[str, int]  # e.g. {"id": 10, "version": 1}
    fields: List[Field]
    line: int = 0


@dataclass
class Field:
    """A single field within a message declaration."""

    name: str
    type: Type
    optional: bool = False
    line: int = 0


class Type:  # pylint: disable=too-few-public-methods
    """Base class for all DSL type nodes."""


@dataclass
class PrimitiveType(Type):
    """A primitive scalar type: i8, i16, i32, i64, bool, or datetime_ns."""

    name: str


@dataclass
class StringType(Type):
    """A variable-length UTF-8 string type."""


@dataclass
class ListType(Type):
    """A variable-length list of elements of a given type."""

    element_type: Type


@dataclass
class ArrayType(Type):
    """A fixed-length array of primitive elements."""

    element_type: PrimitiveType
    length: int
    line: int = 0


@dataclass
class ReferenceType(Type):
    """A reference to a named message or enum declaration."""

    name: str
    line: int = 0
