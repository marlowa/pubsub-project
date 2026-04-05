from __future__ import annotations
from dataclasses import dataclass
from typing import List, Dict

# -----------------------------
# Top-level AST container
# -----------------------------

@dataclass
class DslFile:
    declarations: List[Declaration]


# -----------------------------
# Base class for all declarations
# -----------------------------

class Declaration:
    pass


# -----------------------------
# Enum declarations
# -----------------------------

@dataclass
class EnumEntry:
    name: str
    value: int
    line: int = 0


@dataclass
class EnumDecl(Declaration):
    name: str
    underlying_type: str  # "i8", "i16", "i32", "i64"
    entries: List[EnumEntry]
    line: int = 0


# -----------------------------
# Message declarations
# -----------------------------

@dataclass
class MessageDecl(Declaration):
    name: str
    metadata: Dict[str, int]  # e.g. {"id": 10, "version": 1}
    fields: List[Field]
    line: int = 0


# -----------------------------
# Field declarations
# -----------------------------

@dataclass
class Field:
    name: str
    type: Type
    optional: bool = False
    line: int = 0


# -----------------------------
# Type system
# -----------------------------

class Type:
    pass


@dataclass
class PrimitiveType(Type):
    name: str  # "i8", "i16", "i32", "i64", "bool", "datetime_ns"


@dataclass
class StringType(Type):
    pass


@dataclass
class ListType(Type):
    element_type: Type


@dataclass
class ArrayType(Type):
    element_type: PrimitiveType
    length: int
    line: int = 0


@dataclass
class ReferenceType(Type):
    name: str  # refers to a MessageDecl or EnumDecl by name
    line: int = 0
