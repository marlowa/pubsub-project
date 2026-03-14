from __future__ import annotations
from typing import Dict, Set, List

from .ast import (
    DslFile,
    Declaration,
    EnumDecl,
    EnumEntry,
    MessageDecl,
    Field,
    PrimitiveType,
    StringType,
    ListType,
    ArrayType,
    ReferenceType,
)


class ValidationError(Exception):
    pass


class Validator:
    def __init__(self, ast: DslFile):
        self.ast = ast

        # Maps for quick lookup
        self.enums: Dict[str, EnumDecl] = {}
        self.messages: Dict[str, MessageDecl] = {}

    # -----------------------------
    # Entry point
    # -----------------------------

    def validate(self):
        self._collect_declarations()
        self._validate_enums()
        self._validate_messages()
        self._validate_no_cycles()

    # -----------------------------
    # Collect declarations
    # -----------------------------

    def _collect_declarations(self):
        for decl in self.ast.declarations:
            if isinstance(decl, EnumDecl):
                if decl.name in self.enums or decl.name in self.messages:
                    raise ValidationError(f"Duplicate declaration name: {decl.name}")
                self.enums[decl.name] = decl

            elif isinstance(decl, MessageDecl):
                if decl.name in self.enums or decl.name in self.messages:
                    raise ValidationError(f"Duplicate declaration name: {decl.name}")
                self.messages[decl.name] = decl

            else:
                raise ValidationError(f"Unknown declaration type: {decl}")

    # -----------------------------
    # Enum validation
    # -----------------------------

    def _validate_enums(self):
        for enum in self.enums.values():
            # Underlying type must be signed integer
            if enum.underlying_type not in {"i8", "i16", "i32", "i64"}:
                raise ValidationError(
                    f"Enum {enum.name} has invalid underlying type: {enum.underlying_type}"
                )

            seen_values = set()
            for entry in enum.entries:
                if entry.value in seen_values:
                    raise ValidationError(
                        f"Enum {enum.name} has duplicate value: {entry.value}"
                    )
                seen_values.add(entry.value)

    # -----------------------------
    # Message validation
    # -----------------------------

    def _validate_messages(self):
        used_ids = set()

        for msg in self.messages.values():
            # Metadata must contain id
            if "id" not in msg.metadata:
                raise ValidationError(f"Message {msg.name} missing required metadata 'id'")

            msg_id = msg.metadata["id"]
            if msg_id in used_ids:
                raise ValidationError(f"Duplicate message id: {msg_id}")
            used_ids.add(msg_id)

            # Validate fields
            for field in msg.fields:
                self._validate_field(field)

    def _validate_field(self, field: Field):
        self._validate_type(field.type)

    # -----------------------------
    # Type validation
    # -----------------------------

    def _validate_type(self, t):
        if isinstance(t, PrimitiveType):
            return

        if isinstance(t, StringType):
            return

        if isinstance(t, ListType):
            self._validate_type(t.element_type)
            return

        if isinstance(t, ArrayType):
            if not isinstance(t.element_type, PrimitiveType):
                raise ValidationError(
                    f"Array element type must be primitive, got {t.element_type}"
                )
            if t.length <= 0:
                raise ValidationError("Array length must be positive")
            return

        if isinstance(t, ReferenceType):
            if t.name not in self.messages and t.name not in self.enums:
                raise ValidationError(f"Unknown type reference: {t.name}")
            return

        raise ValidationError(f"Unknown type node: {t}")

    # -----------------------------
    # Cycle detection
    # -----------------------------

    def _validate_no_cycles(self):
        # Build adjacency: message -> referenced messages
        graph: Dict[str, Set[str]] = {name: set() for name in self.messages}

        for msg in self.messages.values():
            for field in msg.fields:
                self._collect_message_refs(msg.name, field.type, graph[msg.name])

        # DFS cycle detection
        visited = set()
        stack = set()

        def dfs(node: str):
            if node in stack:
                raise ValidationError(f"Cyclic message reference detected at {node}")
            if node in visited:
                return

            visited.add(node)
            stack.add(node)

            for neighbor in graph[node]:
                dfs(neighbor)

            stack.remove(node)

        for name in graph:
            dfs(name)

    def _collect_message_refs(self, msg_name: str, t, out: Set[str]):
        if isinstance(t, ReferenceType) and t.name in self.messages:
            out.add(t.name)

        elif isinstance(t, ListType):
            self._collect_message_refs(msg_name, t.element_type, out)

        elif isinstance(t, ArrayType):
            pass  # arrays cannot contain messages

        elif isinstance(t, PrimitiveType) or isinstance(t, StringType):
            pass

        else:
            # Nested message types (ReferenceType) handled above
            pass
