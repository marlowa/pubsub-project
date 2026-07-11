"""Loader: expands `include` directives into a single merged DslFile.

The DSL parser handles one file at a time. The loader sits above it: given a root
`.dsl` file it parses that file, then recursively parses every `include "path"` it
finds, merging all declarations into one :class:`DslFile`. The existing
whole-program validator and the generators then run unchanged -- includes are a
loader concern, not a validator rewrite.

Rules:
  * Include paths are resolved relative to the directory of the *including* file.
  * Each file is loaded at most once (dedup by canonical real path), so a diamond
    of includes contributes its declarations only once, in first-seen order.
  * A cycle of includes is a hard error, reported with the offending chain.
"""

from __future__ import annotations
from pathlib import Path
from typing import List, Optional, Set

from .parser import Parser
from .errors import ParseError
from .ast import DslFile, Declaration, IncludeDecl


class Loader:
    """Resolves the transitive include closure of a root DSL file into one AST."""

    def __init__(self):
        self._loading: List[Path] = []  # DFS stack of canonical paths (cycle detection)
        self._loaded: Set[Path] = set()  # canonical paths already merged (dedup)

    def load(self, root_path) -> DslFile:
        """Parse ``root_path`` and expand all transitive includes into one DslFile."""
        declarations: List[Declaration] = []
        self._expand(Path(root_path), declarations, including=None)
        return DslFile(declarations)

    def _expand(self, path: Path, declarations: List[Declaration], including: Optional[Path]) -> None:
        try:
            canonical = path.resolve(strict=True)
        except FileNotFoundError:
            if including is None:
                raise ParseError(f"DSL file not found: {path}") from None
            raise ParseError(f"Included file not found: {path} (included by {including})") from None

        if canonical in self._loaded:
            return  # diamond dedup: this file's declarations are already merged

        if canonical in self._loading:
            cycle = self._loading[self._loading.index(canonical):] + [canonical]
            chain = " -> ".join(str(entry) for entry in cycle)
            raise ParseError(f"Cyclic include detected: {chain}")

        self._loading.append(canonical)
        try:
            text = canonical.read_text()
        except OSError as error:
            raise ParseError(f"Cannot read DSL file {canonical}: {error}") from error

        for decl in Parser(text).parse().declarations:
            if isinstance(decl, IncludeDecl):
                self._expand(canonical.parent / decl.path, declarations, including=canonical)
            else:
                declarations.append(decl)

        self._loading.pop()
        self._loaded.add(canonical)


def load(root_path) -> DslFile:
    """Parse ``root_path``, expanding all transitive includes into one DslFile."""
    return Loader().load(root_path)
