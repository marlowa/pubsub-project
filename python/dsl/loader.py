"""Loader: expands `include` directives into a single merged DslFile.

The DSL parser handles one file at a time. The loader sits above it: given a root
`.dsl` file it parses that file, then recursively parses every `include "path"` it
finds, merging all declarations into one :class:`DslFile`. The existing
whole-program validator and the generators then run unchanged -- includes are a
loader concern, not a validator rewrite.

Rules:
  * Include paths are resolved relative to the directory of the *including* file
    first, then against any include search directories supplied to the loader. This
    lets a source `.dsl` include a file that is generated into the build tree, so no
    generated `.dsl` needs to live in the source tree.
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


class Loader:  # pylint: disable=too-few-public-methods
    """Resolves the transitive include closure of a root DSL file into one AST."""

    def __init__(self, include_dirs=None):
        self._loading: List[Path] = []  # DFS stack of canonical paths (cycle detection)
        self._loaded: Set[Path] = set()  # canonical paths already merged (dedup)
        self._include_dirs: List[Path] = [Path(directory) for directory in (include_dirs or [])]

    def load(self, root_path) -> DslFile:
        """Parse ``root_path`` and expand all transitive includes into one DslFile."""
        declarations: List[Declaration] = []
        self._expand(Path(root_path), declarations, including=None)
        return DslFile(declarations)

    def _resolve_include(self, rel_path: Path, including_dir: Path) -> Path:
        """Resolve an include: the including file's directory first, then the search dirs.

        Returns the first candidate that exists, or the including-relative candidate
        (which does not exist) so _expand raises a clear "not found" error.
        """
        candidates = [including_dir / rel_path] + [directory / rel_path for directory in self._include_dirs]
        for candidate in candidates:
            if candidate.exists():
                return candidate
        return candidates[0]

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
                resolved = self._resolve_include(Path(decl.path), canonical.parent)
                self._expand(resolved, declarations, including=canonical)
            else:
                declarations.append(decl)

        self._loading.pop()
        self._loaded.add(canonical)


def load(root_path, include_dirs=None) -> DslFile:
    """Parse ``root_path``, expanding all transitive includes into one DslFile.

    ``include_dirs`` are extra directories searched (after the including file's own
    directory) when resolving `include` directives -- e.g. the build directory that
    holds a generated `.dsl`.
    """
    return Loader(include_dirs).load(root_path)
