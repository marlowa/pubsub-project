from __future__ import annotations
from typing import Dict, Set, List

from .ast import (
    DslFile,
    Declaration,
    EnumDecl,
    MessageDecl,
    Field,
    PrimitiveType,
    StringType,
    ListType,
    ArrayType,
    ReferenceType,
)
from .errors import ValidationError


class Validator:
    def __init__(self, ast: DslFile):
        self.ast = ast
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
                    raise ValidationError(
                        f"line {decl.line}: duplicate declaration name '{decl.name}'"
                    )
                self.enums[decl.name] = decl

            elif isinstance(decl, MessageDecl):
                if decl.name in self.enums or decl.name in self.messages:
                    raise ValidationError(
                        f"line {decl.line}: duplicate declaration name '{decl.name}'"
                    )
                self.messages[decl.name] = decl

            else:
                raise ValidationError(f"Unknown declaration type: {decl}")

    # -----------------------------
    # Enum validation
    # -----------------------------

    def _validate_enums(self):
        for enum in self.enums.values():
            if enum.underlying_type not in {"i8", "i16", "i32", "i64"}:
                raise ValidationError(
                    f"line {enum.line}: enum '{enum.name}' has invalid "
                    f"underlying type '{enum.underlying_type}' "
                    f"(must be i8, i16, i32, or i64)"
                )

            seen_values: Set[int] = set()
            for entry in enum.entries:
                if entry.value in seen_values:
                    raise ValidationError(
                        f"line {entry.line}: enum '{enum.name}' has duplicate "
                        f"value {entry.value} (on entry '{entry.name}')"
                    )
                seen_values.add(entry.value)

    # -----------------------------
    # Message validation
    # -----------------------------

    def _validate_messages(self):
        seen_ids: Set[int] = set()

        for msg in self.messages.values():
            if "id" not in msg.metadata:
                raise ValidationError(
                    f"line {msg.line}: message '{msg.name}' is missing "
                    f"required metadata 'id'"
                )

            message_id = msg.metadata["id"]
            if message_id in seen_ids:
                raise ValidationError(
                    f"line {msg.line}: message '{msg.name}' has duplicate "
                    f"message id {message_id}"
                )
            seen_ids.add(message_id)

            for field in msg.fields:
                self._validate_field(field, msg.name)

    def _validate_field(self, field: Field, message_name: str):
        self._validate_type(field.type, field, message_name)

    # -----------------------------
    # Type validation
    # -----------------------------

    def _validate_type(self, type_node, field: Field, message_name: str):
        if isinstance(type_node, PrimitiveType):
            return

        if isinstance(type_node, StringType):
            return

        if isinstance(type_node, ListType):
            self._validate_type(type_node.element_type, field, message_name)
            return

        if isinstance(type_node, ArrayType):
            if not isinstance(type_node.element_type, PrimitiveType):
                raise ValidationError(
                    f"line {field.line}: in message '{message_name}', "
                    f"field '{field.name}': array element type must be primitive"
                )
            if type_node.length <= 0:
                raise ValidationError(
                    f"line {field.line}: in message '{message_name}', "
                    f"field '{field.name}': array length must be greater than zero"
                )
            return

        if isinstance(type_node, ReferenceType):
            if type_node.name not in self.messages and type_node.name not in self.enums:
                raise ValidationError(
                    f"line {field.line}: in message '{message_name}', "
                    f"field '{field.name}': unknown type '{type_node.name}'"
                )
            return

        raise ValidationError(
            f"line {field.line}: in message '{message_name}', "
            f"field '{field.name}': unknown type node {type_node}"
        )

    # -----------------------------
    # Cycle detection
    # -----------------------------

    def _validate_no_cycles(self):
        graph: Dict[str, Set[str]] = {name: set() for name in self.messages}

        for msg in self.messages.values():
            for field in msg.fields:
                self._collect_message_refs(field.type, graph[msg.name])

        visited: Set[str] = set()
        path: List[str] = []

        def dfs(node: str):
            if node in path:
                cycle = " -> ".join(path[path.index(node):] + [node])
                raise ValidationError(
                    f"line {self.messages[node].line}: "
                    f"cyclic message reference detected: {cycle}"
                )
            if node in visited:
                return

            visited.add(node)
            path.append(node)

            for neighbour in graph[node]:
                dfs(neighbour)

            path.pop()

        for name in graph:
            dfs(name)

    def _collect_message_refs(self, type_node, out: Set[str]):
        if isinstance(type_node, ReferenceType) and type_node.name in self.messages:
            out.add(type_node.name)
        elif isinstance(type_node, ListType):
            self._collect_message_refs(type_node.element_type, out)
