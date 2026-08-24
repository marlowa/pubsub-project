"""Generators for the pub/sub topic catalog: a C++ registry header and a
human-readable Markdown catalog.

Both artifacts are derived from the `topic` groupings in a *validated* DSL AST
(so every member message's `id` metadata is already an integer). They are the
generated "identity" layer of the topic design (see
docs/superseded/dsl_topic_catalog.md): the single source of truth for which topics
exist and which pdu ids belong to each. Policy (retention, ports, ...) is
hand-written TOML validated against this registry, and is not emitted here.

A message may belong to more than one topic, so the pdu-id/topic relationship is
emitted as a flat membership table (one row per membership) rather than a plain
map.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Dict, List

from .ast import DslFile, MessageDecl, TopicDecl


def _topics(ast: DslFile) -> List[TopicDecl]:
    return [decl for decl in ast.declarations if isinstance(decl, TopicDecl)]


def _messages(ast: DslFile) -> Dict[str, MessageDecl]:
    return {decl.name: decl for decl in ast.declarations if isinstance(decl, MessageDecl)}


@dataclass
class TopicsGenerator:
    """Emits the topic registry header and catalog from a validated DSL AST."""

    namespace: str

    # ------------------------------------------------------------------
    # C++ registry header
    # ------------------------------------------------------------------

    def emit_registry(self, ast: DslFile, source_name: str) -> str:
        """Generate the `topics_registry.hpp` contents as a string."""
        topics = _topics(ast)
        messages = _messages(ast)

        lines: List[str] = []
        w = lines.append

        self._emit_cpp_banner(w, source_name)
        w("#pragma once")
        w("")
        w("#include <array>")
        w("#include <cstdint>")
        w("#include <string_view>")
        w("")
        w(f"namespace {self.namespace} {{")
        w("")

        self._emit_topic_enum(w, topics)
        self._emit_name_lookups(w, topics)
        self._emit_membership(w, topics, messages)

        w(f"}} // namespace {self.namespace}")
        w("")
        return "\n".join(lines)

    def _emit_topic_enum(self, w, topics):
        """Emit the Topic enum (0-based, declaration order) and topic_count."""
        w("enum class Topic : std::uint16_t {")
        for index, topic in enumerate(topics):
            w(f"    {topic.name} = {index},")
        w("};")
        w("")
        w(f"inline constexpr std::size_t topic_count = {len(topics)};")
        w("")
        # all_topics lets callers iterate every topic by enum value (range-for), with no
        # index arithmetic and no cast from an integer back to Topic.
        w(f"inline constexpr std::array<Topic, {len(topics)}> all_topics{{{{")
        for topic in topics:
            w(f"    Topic::{topic.name},")
        w("}};")
        w("")

    def _emit_name_lookups(self, w, topics):
        """Emit to_string(Topic) and topic_from_name(name, out)."""
        w("inline constexpr std::string_view to_string(Topic topic) {")
        w("    switch (topic) {")
        for topic in topics:
            w(f'    case Topic::{topic.name}: return std::string_view("{topic.name}");')
        w('    default: return std::string_view("<unknown>");')
        w("    }")
        w("}")
        w("")

        # topic_from_name: validate a wire topic name against the catalog.
        w("// Resolve a wire topic name to its Topic. Returns false (leaving out")
        w("// untouched) if the name is not a recognised topic.")
        w("inline bool topic_from_name(std::string_view name, Topic& out) {")
        for topic in topics:
            w(f'    if (name == std::string_view("{topic.name}")) {{ out = Topic::{topic.name}; return true; }}')
        w("    return false;")
        w("}")
        w("")

    def _emit_membership(self, w, topics, messages):
        """Emit the flat TopicMember table and the pdu_in_topic predicate."""
        # One (pdu_id, topic) row per membership.
        rows = [
            (messages[member].metadata["id"], topic.name, member)
            for topic in topics
            for member in topic.members
        ]
        w("// One (pdu_id, topic) row per membership. A message may belong to more")
        w("// than one topic, so its pdu id may appear in more than one row.")
        w("struct TopicMember {")
        w("    std::int64_t pdu_id;")
        w("    Topic topic;")
        w("};")
        w("")
        w(f"inline constexpr std::array<TopicMember, {len(rows)}> topic_members{{{{")
        for pdu_id, topic_name, member in rows:
            w(f"    TopicMember{{{pdu_id}, Topic::{topic_name}}},  // {member}")
        w("}};")
        w("")

        # Membership predicate: is this pdu id a member of this topic?
        w("// Return true if the given pdu id belongs to the given topic.")
        w("inline constexpr bool pdu_in_topic(std::int64_t pdu_id, Topic topic) {")
        w("    for (const TopicMember& member : topic_members) {")
        w("        if (member.pdu_id == pdu_id && member.topic == topic) {")
        w("            return true;")
        w("        }")
        w("    }")
        w("    return false;")
        w("}")
        w("")

    # ------------------------------------------------------------------
    # Markdown catalog
    # ------------------------------------------------------------------

    def emit_catalog(self, ast: DslFile, source_name: str) -> str:
        """Generate the human-readable `topics_catalog.md` contents as a string."""
        topics = _topics(ast)
        messages = _messages(ast)

        lines: List[str] = []
        w = lines.append

        self._emit_md_banner(w, source_name)
        w("# Topic catalog")
        w("")
        w(f"Recognised pub/sub topics, generated from `{source_name}`. "
          f"This file is generated -- do not edit.")
        w("")

        for index, topic in enumerate(topics):
            w(f"## `{topic.name}` (Topic = {index})")
            w("")
            w("| message | pdu_id |")
            w("| --- | --- |")
            for member in topic.members:
                w(f"| `{member}` | {messages[member].metadata['id']} |")
            w("")

        return "\n".join(lines)

    # ------------------------------------------------------------------
    # Banners
    # ------------------------------------------------------------------

    def _emit_cpp_banner(self, w, source_name: str):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        w("// -----------------------------------------------------------------------------")
        w("//  Topic registry generated by the DSL topics generator (generator_topics.py).")
        w("//  Do NOT edit this file manually.")
        w("//")
        w(f"//  Source:       {source_name}")
        w(f"//  Generated on: {timestamp}")
        w("// -----------------------------------------------------------------------------")
        w("")

    def _emit_md_banner(self, w, source_name: str):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        w("<!--")
        w("  Topic catalog generated by the DSL topics generator (generator_topics.py).")
        w("  Do NOT edit this file manually.")
        w(f"  Source:       {source_name}")
        w(f"  Generated on: {timestamp}")
        w("-->")
        w("")
